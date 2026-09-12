/*
 * Inferno guest-to-host Metal execution bridge.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "coordinator.h"
#include "batch-client.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct ImtlCoordinator {
    pthread_mutex_t mutex;
    ImtlUserClient *client;
    ImtlCoordinatorConfig config;
    uint64_t next_sequence;
    bool needs_drain;
};

static uint32_t wireGet32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint64_t monotonicNS(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) {
        return 0;
    }
    return (uint64_t)value.tv_sec * 1000000000ULL + value.tv_nsec;
}

static bool makeDeadline(uint64_t duration, uint64_t *deadline)
{
    uint64_t now = monotonicNS();
    if (!now || duration > UINT64_MAX - now) {
        return false;
    }
    *deadline = now + duration;
    return true;
}

static void clearError(ImtlCoordinatorError *error)
{
    if (error) {
        memset(error, 0, sizeof(*error));
    }
}

static bool fail(ImtlCoordinatorError *error, uint32_t kind, IOReturn io,
                 const ImtlUserStatus *status, uint64_t sequence,
                 IOReturn cleanup)
{
    if (error) {
        error->kind = kind;
        error->io = io;
        error->cleanup_io = cleanup;
        error->sequence = sequence;
        if (status) {
            error->state = status->state;
            error->completion_error = status->completion_error;
            error->timer_error = status->timer_error;
        }
    }
    return false;
}

static bool pauseUntil(uint64_t deadline, uint64_t *delay)
{
    uint64_t now = monotonicNS();
    if (!now || now >= deadline) {
        return false;
    }
    uint64_t remaining = deadline - now;
    uint64_t amount = *delay < remaining ? *delay : remaining;
    struct timespec sleep = { .tv_sec = amount / 1000000000ULL,
                              .tv_nsec = amount % 1000000000ULL };
    while (nanosleep(&sleep, &sleep) && monotonicNS() < deadline) {
    }
    if (*delay < 2000000ULL) {
        *delay *= 2;
        if (*delay > 2000000ULL) {
            *delay = 2000000ULL;
        }
    }
    return monotonicNS() < deadline;
}

static bool drainLocked(ImtlCoordinator *c, uint64_t deadline,
                        ImtlCoordinatorError *error)
{
    uint64_t delay = 100000;
    while (c->needs_drain) {
        ImtlUserStatus status;
        IOReturn io = imtl_user_client_status(c->client, &status);
        if (io != kIOReturnSuccess) {
            return fail(error, IMTL_COORDINATOR_ERROR_TRANSPORT, io, NULL, 0,
                        0);
        }
        if (status.state == INFERNO_METAL_USER_IDLE) {
            c->needs_drain = false;
            return true;
        }
        if (status.state == INFERNO_METAL_USER_COMPLETED ||
            status.state == INFERNO_METAL_USER_FAULTED ||
            status.state == INFERNO_METAL_USER_DRAINED) {
            io = imtl_user_client_reset(c->client);
            if (io != kIOReturnSuccess) {
                return fail(error, IMTL_COORDINATOR_ERROR_TRANSPORT, io,
                            &status, status.sequence, io);
            }
        }
        if (!pauseUntil(deadline, &delay)) {
            return fail(error, IMTL_COORDINATOR_ERROR_TIMEOUT, kIOReturnTimeout,
                        &status, status.sequence, 0);
        }
    }
    return true;
}

void imtl_coordinator_config_default(ImtlCoordinatorConfig *config)
{
    if (config) {
        *config = (ImtlCoordinatorConfig){
            .timeout_ns = IMTL_COORDINATOR_DEFAULT_TIMEOUT_NS,
            .close_budget_ns = IMTL_COORDINATOR_DEFAULT_CLOSE_BUDGET_NS,
            .initial_output_bytes =
                IMTL_COORDINATOR_DEFAULT_INITIAL_OUTPUT_BYTES,
            .max_growth_rounds = IMTL_COORDINATOR_DEFAULT_MAX_GROWTH_ROUNDS,
        };
    }
}

IOReturn imtl_coordinator_create(ImtlUserClient *client,
                                 const ImtlCoordinatorConfig *config,
                                 ImtlCoordinator **out)
{
    if (out) {
        *out = NULL;
    }
    if (!client || !config || !out || !config->timeout_ns ||
        !config->close_budget_ns ||
        config->initial_output_bytes < INFERNO_METAL_COMPILER_MIN_OUTPUT ||
        config->initial_output_bytes > INFERNO_METAL_COMPILER_MAX_OUTPUT) {
        return kIOReturnBadArgument;
    }
    const ImtlUserCaps *caps = imtl_user_client_caps(client);
    if (!caps || caps->max_output_size < INFERNO_METAL_COMPILER_MIN_OUTPUT) {
        return kIOReturnUnsupported;
    }
    uint64_t deadline;
    if (!makeDeadline(config->timeout_ns, &deadline) ||
        !makeDeadline(config->close_budget_ns, &deadline)) {
        return kIOReturnBadArgument;
    }
    ImtlCoordinator *result = calloc(1, sizeof(*result));
    if (!result) {
        return kIOReturnNoMemory;
    }
    if (pthread_mutex_init(&result->mutex, NULL)) {
        free(result);
        return kIOReturnNoResources;
    }
    result->client = client;
    result->config = *config;
    result->next_sequence = 1;
    *out = result;
    return kIOReturnSuccess;
}

static bool queryLocked(ImtlCoordinator *c, uint32_t opcode, const void *source,
                        size_t source_size, const char *name, uint32_t width,
                        uint32_t height, uint32_t depth, ImtlQueryReply *out,
                        ImtlCoordinatorError *error, uint64_t deadline)
{
    if (!c->client) {
        return fail(error, IMTL_COORDINATOR_ERROR_CLOSED, kIOReturnNotOpen,
                    NULL, 0, 0);
    }
    if (!drainLocked(c, deadline, error)) {
        return false;
    }
    const ImtlUserCaps *caps = imtl_user_client_caps(c->client);
    uint32_t output_size = opcode == INFERNO_METAL_QUERY_LIBRARY ?
                               c->config.initial_output_bytes :
                               INFERNO_METAL_COMPILER_MIN_OUTPUT;
    if (output_size > caps->max_output_size) {
        output_size = caps->max_output_size;
    }
    for (uint32_t attempt = 0;; attempt++) {
        if (c->next_sequence == UINT64_MAX) {
            IOReturn close_io = imtl_user_client_close(&c->client);
            return fail(error, IMTL_COORDINATOR_ERROR_CLOSED, close_io, NULL,
                        UINT64_MAX, close_io);
        }
        uint64_t sequence = c->next_sequence++;
        IOReturn io;
    retry_submit:
        if (opcode == INFERNO_METAL_QUERY_LIBRARY) {
            io = imtl_compiler_submit_library(c->client, sequence, source,
                                              source_size, output_size);
        } else if (opcode == INFERNO_METAL_QUERY_PIPELINE) {
            io = imtl_compiler_submit_pipeline(c->client, sequence, source,
                                               source_size, name);
        } else {
            io = imtl_compiler_submit_imageblock(c->client, sequence, source,
                                                 source_size, name, width,
                                                 height, depth);
        }
        bool uncertain = io != kIOReturnSuccess;
        uint64_t delay = 100000;
        ImtlUserStatus status = { 0 };
        while (true) {
            IOReturn status_io = imtl_user_client_status(c->client, &status);
            if (status_io != kIOReturnSuccess) {
                if (uncertain && pauseUntil(deadline, &delay)) {
                    continue;
                }
                c->needs_drain = true;
                return fail(error,
                            uncertain ? IMTL_COORDINATOR_ERROR_UNCERTAIN :
                                        IMTL_COORDINATOR_ERROR_TRANSPORT,
                            uncertain ? io : status_io, NULL, sequence, 0);
            }
            if (status.state == INFERNO_METAL_USER_IDLE && uncertain) {
                if (io == kIOReturnBusy) {
                    uncertain = false;
                    if (!pauseUntil(deadline, &delay)) {
                        return fail(error, IMTL_COORDINATOR_ERROR_BUSY,
                                    kIOReturnBusy, &status, sequence, 0);
                    }
                    goto retry_submit;
                }
                return fail(error, IMTL_COORDINATOR_ERROR_TRANSPORT, io,
                            &status, sequence, 0);
            }
            if (status.state == INFERNO_METAL_USER_COMPLETED) {
                if (status.sequence != sequence) {
                    c->needs_drain = true;
                    return fail(error, IMTL_COORDINATOR_ERROR_PROTOCOL,
                                kIOReturnBadMessageID, &status, sequence, 0);
                }
                if (status.transport_result != INFERNO_METAL_USER_RESULT_OK ||
                    status.completion_error) {
                    IOReturn cleanup = imtl_user_client_ack(c->client);
                    c->needs_drain = cleanup != kIOReturnSuccess;
                    return fail(error, IMTL_COORDINATOR_ERROR_DEVICE_FAULT,
                                kIOReturnError, &status, sequence, cleanup);
                }
                break;
            }
            if (status.state == INFERNO_METAL_USER_FAULTED ||
                status.state == INFERNO_METAL_USER_STOPPED) {
                c->needs_drain = true;
                return fail(error, IMTL_COORDINATOR_ERROR_DEVICE_FAULT,
                            kIOReturnError, &status, sequence, 0);
            }
            if (!pauseUntil(deadline, &delay)) {
                IOReturn cleanup = imtl_user_client_reset(c->client);
                c->needs_drain = true;
                return fail(error, IMTL_COORDINATOR_ERROR_TIMEOUT,
                            kIOReturnTimeout, &status, sequence, cleanup);
            }
        }
        uint8_t *bytes = calloc(1, output_size);
        if (!bytes) {
            IOReturn cleanup = imtl_user_client_reset(c->client);
            c->needs_drain = true;
            return fail(error, IMTL_COORDINATOR_ERROR_TRANSPORT,
                        kIOReturnNoMemory, &status, sequence, cleanup);
        }
        size_t read_size = 0;
        while ((io = imtl_user_client_read(c->client, 0, bytes, output_size,
                                           &read_size)) != kIOReturnSuccess) {
            memset(bytes, 0, output_size);
            if (!pauseUntil(deadline, &delay)) {
                break;
            }
        }
        ImtlCompilerResult result;
        if (io != kIOReturnSuccess || read_size != output_size ||
            !imtl_compiler_decode_result(bytes, output_size, opcode, sequence,
                                         &result)) {
            free(bytes);
            IOReturn cleanup = imtl_user_client_ack(c->client);
            c->needs_drain = cleanup != kIOReturnSuccess;
            return fail(error,
                        io == kIOReturnSuccess ?
                            IMTL_COORDINATOR_ERROR_PROTOCOL :
                            IMTL_COORDINATOR_ERROR_TRANSPORT,
                        io == kIOReturnSuccess ? kIOReturnBadMessageID : io,
                        &status, sequence, cleanup);
        }
        IOReturn cleanup = imtl_user_client_ack(c->client);
        if (cleanup != kIOReturnSuccess) {
            c->needs_drain = true;
        }
        if (result.outcome == INFERNO_METAL_COMPILER_OUTCOME_OUTPUT_TOO_SMALL) {
            uint32_t required = result.required_output_size;
            free(bytes);
            if (cleanup != kIOReturnSuccess ||
                attempt >= c->config.max_growth_rounds ||
                required <= output_size || required > caps->max_output_size ||
                required > INFERNO_METAL_COMPILER_MAX_OUTPUT) {
                return fail(error, IMTL_COORDINATOR_ERROR_PROTOCOL,
                            kIOReturnBadMessageID, &status, sequence, cleanup);
            }
            output_size = required;
            if (monotonicNS() >= deadline) {
                return fail(error, IMTL_COORDINATOR_ERROR_TIMEOUT,
                            kIOReturnTimeout, &status, sequence, cleanup);
            }
            continue;
        }
        *out = (ImtlQueryReply){ .bytes = bytes,
                                 .size = output_size,
                                 .result = result,
                                 .timer_error = status.timer_error,
                                 .cleanup_io = cleanup,
                                 .sequence = sequence };
        return true;
    }
}

static bool query(ImtlCoordinator *c, uint32_t opcode, const void *source,
                  size_t source_size, const char *name, uint32_t width,
                  uint32_t height, uint32_t depth, ImtlQueryReply *out,
                  ImtlCoordinatorError *error)
{
    clearError(error);
    bool named = opcode != INFERNO_METAL_QUERY_LIBRARY;
    size_t name_size = named && name ? strnlen(name, 64) : 0;
    if (!c || !out || !source || !source_size ||
        source_size > INFERNO_METAL_MAX_SOURCE ||
        (named && (!name_size || name_size == 64)) ||
        (opcode == INFERNO_METAL_QUERY_IMAGEBLOCK &&
         (!width || width > 65536 || !height || height > 65536 || !depth ||
          depth > 65536))) {
        return fail(error, IMTL_COORDINATOR_ERROR_INVALID_ARGUMENT,
                    kIOReturnBadArgument, NULL, 0, 0);
    }
    uint64_t deadline;
    if (!makeDeadline(c->config.timeout_ns, &deadline)) {
        return fail(error, IMTL_COORDINATOR_ERROR_INVALID_ARGUMENT,
                    kIOReturnBadArgument, NULL, 0, 0);
    }
    pthread_mutex_lock(&c->mutex);
    bool result = monotonicNS() < deadline &&
                  queryLocked(c, opcode, source, source_size, name, width,
                              height, depth, out, error, deadline);
    if (!result && error && error->kind == IMTL_COORDINATOR_ERROR_NONE) {
        fail(error, IMTL_COORDINATOR_ERROR_TIMEOUT, kIOReturnTimeout, NULL, 0,
             0);
    }
    pthread_mutex_unlock(&c->mutex);
    return result;
}

bool imtl_coordinator_query_library(ImtlCoordinator *c, const void *source,
                                    size_t size, ImtlQueryReply *out,
                                    ImtlCoordinatorError *error)
{
    return query(c, INFERNO_METAL_QUERY_LIBRARY, source, size, NULL, 1, 1, 1,
                 out, error);
}

bool imtl_coordinator_query_pipeline(ImtlCoordinator *c, const void *source,
                                     size_t size, const char *name,
                                     ImtlQueryReply *out,
                                     ImtlCoordinatorError *error)
{
    return query(c, INFERNO_METAL_QUERY_PIPELINE, source, size, name, 1, 1, 1,
                 out, error);
}

bool imtl_coordinator_query_imageblock(ImtlCoordinator *c, const void *source,
                                       size_t size, const char *name,
                                       uint32_t width, uint32_t height,
                                       uint32_t depth, ImtlQueryReply *out,
                                       ImtlCoordinatorError *error)
{
    return query(c, INFERNO_METAL_QUERY_IMAGEBLOCK, source, size, name, width,
                 height, depth, out, error);
}

void imtl_query_reply_free(ImtlQueryReply *reply)
{
    if (reply) {
        free(reply->bytes);
        memset(reply, 0, sizeof(*reply));
    }
}

bool imtl_coordinator_execute_batch(ImtlCoordinator *c, const void *manifest,
                                    size_t manifest_size, uint32_t buffer_count,
                                    uint32_t images_size,
                                    const ImtlBatchObserver *observer,
                                    ImtlBatchReply *out,
                                    ImtlCoordinatorError *error)
{
    clearError(error);
    if (!c || !manifest || !out ||
        manifest_size < INFERNO_METAL_BATCH_HEADER_SIZE ||
        manifest_size > INFERNO_METAL_MAX_BUFFER ||
        buffer_count > INFERNO_METAL_BATCH_MAX_BUFFERS ||
        images_size > INFERNO_METAL_BATCH_MAX_IMAGES ||
        (observer && !observer->scheduled)) {
        return fail(error, IMTL_COORDINATOR_ERROR_INVALID_ARGUMENT,
                    kIOReturnBadArgument, NULL, 0, 0);
    }
    memset(out, 0, sizeof(*out));
    uint64_t deadline;
    if (!makeDeadline(c->config.timeout_ns, &deadline)) {
        return fail(error, IMTL_COORDINATOR_ERROR_INVALID_ARGUMENT,
                    kIOReturnBadArgument, NULL, 0, 0);
    }
    pthread_mutex_lock(&c->mutex);
    if (!c->client) {
        pthread_mutex_unlock(&c->mutex);
        return fail(error, IMTL_COORDINATOR_ERROR_CLOSED, kIOReturnNotOpen,
                    NULL, 0, 0);
    }
    if (!drainLocked(c, deadline, error)) {
        pthread_mutex_unlock(&c->mutex);
        return false;
    }
    if (c->next_sequence == UINT64_MAX) {
        IOReturn close_io = imtl_user_client_close(&c->client);
        pthread_mutex_unlock(&c->mutex);
        return fail(error, IMTL_COORDINATOR_ERROR_CLOSED, close_io, NULL,
                    UINT64_MAX, close_io);
    }
    uint64_t sequence = c->next_sequence++;
    out->sequence = sequence;
    IOReturn submit_io;
    uint64_t delay = 100000;
retry_submit:
    submit_io = imtl_batch_submit(c->client, sequence, manifest, manifest_size,
                                  images_size);
    bool uncertain = submit_io != kIOReturnSuccess;
    /* Until an IDLE status proves rejection, the kernel may own the request. */
    c->needs_drain = true;
    ImtlUserStatus status = { 0 };
    bool observed = false;
    while (true) {
        IOReturn status_io = imtl_user_client_status(c->client, &status);
        if (status_io != kIOReturnSuccess) {
            c->needs_drain = true;
            if (pauseUntil(deadline, &delay)) {
                continue;
            }
            pthread_mutex_unlock(&c->mutex);
            return fail(error,
                        uncertain ? IMTL_COORDINATOR_ERROR_UNCERTAIN :
                                    IMTL_COORDINATOR_ERROR_TRANSPORT,
                        uncertain ? submit_io : status_io, NULL, sequence, 0);
        }
        if (status.state == INFERNO_METAL_USER_COMPLETED &&
            status.sequence != sequence) {
            c->needs_drain = true;
            pthread_mutex_unlock(&c->mutex);
            return fail(error, IMTL_COORDINATOR_ERROR_PROTOCOL,
                        kIOReturnBadMessageID, &status, sequence, 0);
        }
        if ((status.state == INFERNO_METAL_USER_SUBMITTED ||
             status.state == INFERNO_METAL_USER_COMPLETED) &&
            (status.progress & INFERNO_METAL_USER_PROGRESS_SCHEDULED) &&
            !observed) {
            observed = true;
            out->scheduled_observed = true;
            if (observer) {
                observer->scheduled(observer->opaque, sequence);
            }
        }
        if (status.state == INFERNO_METAL_USER_IDLE && uncertain) {
            if (submit_io == kIOReturnBusy) {
                uncertain = false;
                c->needs_drain = false;
                if (!pauseUntil(deadline, &delay)) {
                    pthread_mutex_unlock(&c->mutex);
                    return fail(error, IMTL_COORDINATOR_ERROR_BUSY,
                                kIOReturnBusy, &status, sequence, 0);
                }
                goto retry_submit;
            }
            c->needs_drain = false;
            pthread_mutex_unlock(&c->mutex);
            return fail(error, IMTL_COORDINATOR_ERROR_TRANSPORT, submit_io,
                        &status, sequence, 0);
        }
        if (status.state == INFERNO_METAL_USER_COMPLETED) {
            if (status.transport_result != INFERNO_METAL_USER_RESULT_OK ||
                status.completion_error) {
                IOReturn cleanup = imtl_user_client_ack(c->client);
                c->needs_drain = cleanup != kIOReturnSuccess;
                pthread_mutex_unlock(&c->mutex);
                return fail(error, IMTL_COORDINATOR_ERROR_DEVICE_FAULT,
                            kIOReturnError, &status, sequence, cleanup);
            }
            break;
        }
        if (status.state == INFERNO_METAL_USER_FAULTED ||
            status.state == INFERNO_METAL_USER_STOPPED) {
            c->needs_drain = true;
            pthread_mutex_unlock(&c->mutex);
            return fail(error, IMTL_COORDINATOR_ERROR_DEVICE_FAULT,
                        kIOReturnError, &status, sequence, 0);
        }
        if (!pauseUntil(deadline, &delay)) {
            IOReturn cleanup = imtl_user_client_reset(c->client);
            c->needs_drain = true;
            pthread_mutex_unlock(&c->mutex);
            return fail(error, IMTL_COORDINATOR_ERROR_TIMEOUT, kIOReturnTimeout,
                        &status, sequence, cleanup);
        }
    }
    size_t output_size = INFERNO_METAL_BATCH_RESULT_SIZE + images_size;
    uint8_t *bytes = calloc(1, output_size);
    if (!bytes) {
        IOReturn cleanup = imtl_user_client_reset(c->client);
        c->needs_drain = true;
        pthread_mutex_unlock(&c->mutex);
        return fail(error, IMTL_COORDINATOR_ERROR_TRANSPORT, kIOReturnNoMemory,
                    &status, sequence, cleanup);
    }
    IOReturn read_io;
    size_t read_size = 0;
    while ((read_io = imtl_user_client_read(c->client, 0, bytes, output_size,
                                            &read_size)) != kIOReturnSuccess) {
        memset(bytes, 0, output_size);
        if (!pauseUntil(deadline, &delay)) {
            break;
        }
    }
    ImtlBatchResult result;
    bool valid = read_io == kIOReturnSuccess && read_size == output_size &&
                 imtl_batch_decode_result(bytes, output_size, sequence,
                                          buffer_count, images_size, &result);
    if (valid && result.outcome != INFERNO_METAL_BATCH_OUTCOME_MALFORMED) {
        const uint8_t *wire = manifest;
        uint32_t pipeline_count =
            wireGet32(wire + INFERNO_METAL_BATCH_PIPELINE_COUNT_OFFSET);
        uint32_t dispatch_count =
            wireGet32(wire + INFERNO_METAL_BATCH_DISPATCH_COUNT_OFFSET);
        uint32_t binding_count =
            wireGet32(wire + INFERNO_METAL_BATCH_BINDING_COUNT_OFFSET);
        if ((result.failed_record_kind == INFERNO_METAL_BATCH_RECORD_PIPELINE &&
             result.failed_record_index >= pipeline_count) ||
            (result.failed_record_kind == INFERNO_METAL_BATCH_RECORD_DISPATCH &&
             result.failed_record_index >= dispatch_count) ||
            (result.failed_record_kind == INFERNO_METAL_BATCH_RECORD_BINDING &&
             result.failed_record_index >= binding_count)) {
            valid = false;
        }
    }
    IOReturn cleanup = imtl_user_client_ack(c->client);
    c->needs_drain = cleanup != kIOReturnSuccess;
    if (!valid) {
        free(bytes);
        uint32_t kind = read_io != kIOReturnSuccess ?
                            IMTL_COORDINATOR_ERROR_TRANSPORT :
                            IMTL_COORDINATOR_ERROR_PROTOCOL;
        IOReturn primary =
            read_io == kIOReturnSuccess ? kIOReturnBadMessageID : read_io;
        pthread_mutex_unlock(&c->mutex);
        return fail(error, kind, primary, &status, sequence, cleanup);
    }
    *out = (ImtlBatchReply){
        .bytes = bytes,
        .size = output_size,
        .result = result,
        .timer_error = status.timer_error,
        .cleanup_io = cleanup,
        .sequence = sequence,
        .scheduled_observed = observed,
    };
    pthread_mutex_unlock(&c->mutex);
    return true;
}

void imtl_batch_reply_free(ImtlBatchReply *reply)
{
    if (reply) {
        free(reply->bytes);
        memset(reply, 0, sizeof(*reply));
    }
}

IOReturn imtl_coordinator_invalidate(ImtlCoordinator *c)
{
    if (!c) {
        return kIOReturnBadArgument;
    }
    pthread_mutex_lock(&c->mutex);
    uint64_t deadline;
    if (c->client && c->needs_drain &&
        makeDeadline(c->config.close_budget_ns, &deadline)) {
        ImtlCoordinatorError ignored;
        (void)drainLocked(c, deadline, &ignored);
    }
    IOReturn io = imtl_user_client_close(&c->client);
    c->needs_drain = false;
    pthread_mutex_unlock(&c->mutex);
    return io;
}

void imtl_coordinator_destroy(ImtlCoordinator **ptr)
{
    if (ptr && *ptr) {
        ImtlCoordinator *c = *ptr;
        *ptr = NULL;
        imtl_coordinator_invalidate(c);
        pthread_mutex_destroy(&c->mutex);
        free(c);
    }
}

IOReturn imtl_coordinator_close(ImtlCoordinator **ptr)
{
    if (!ptr || !*ptr) {
        return kIOReturnSuccess;
    }
    ImtlCoordinator *c = *ptr;
    IOReturn io = imtl_coordinator_invalidate(c);
    imtl_coordinator_destroy(ptr);
    return io;
}
