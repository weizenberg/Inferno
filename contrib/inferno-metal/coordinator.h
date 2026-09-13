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

#ifndef INFERNO_METAL_COORDINATOR_H
#define INFERNO_METAL_COORDINATOR_H

#include "batch-wire.h"
#include "compiler-client.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMTL_COORDINATOR_DEFAULT_TIMEOUT_NS (30ULL * 1000 * 1000 * 1000)
#define IMTL_COORDINATOR_DEFAULT_CLOSE_BUDGET_NS (100ULL * 1000 * 1000)
#define IMTL_COORDINATOR_DEFAULT_INITIAL_OUTPUT_BYTES (64U * 1024U)
#define IMTL_COORDINATOR_DEFAULT_MAX_GROWTH_ROUNDS 2U

typedef struct ImtlCoordinator ImtlCoordinator;

typedef struct ImtlCoordinatorConfig {
    /* Relative monotonic duration. Its deadline starts before mutex
     * acquisition, so elapsed contention consumes the budget once acquired.
     * Mutex waits and synchronous IOKit calls are not interruptible and
     * therefore have no hard wall-clock bound here.
     */
    uint64_t timeout_ns;
    uint64_t close_budget_ns;
    uint32_t initial_output_bytes;
    uint32_t max_growth_rounds;
} ImtlCoordinatorConfig;

enum {
    IMTL_COORDINATOR_ERROR_NONE = 0,
    IMTL_COORDINATOR_ERROR_BUSY = 1,
    IMTL_COORDINATOR_ERROR_TIMEOUT = 2,
    IMTL_COORDINATOR_ERROR_TRANSPORT = 3,
    IMTL_COORDINATOR_ERROR_UNCERTAIN = 4,
    IMTL_COORDINATOR_ERROR_DEVICE_FAULT = 5,
    IMTL_COORDINATOR_ERROR_PROTOCOL = 6,
    IMTL_COORDINATOR_ERROR_CLOSED = 7,
    IMTL_COORDINATOR_ERROR_INVALID_ARGUMENT = 8,
};

typedef struct ImtlQueryReply {
    uint8_t *bytes;
    size_t size;
    ImtlCompilerResult result;
    IOReturn timer_error;
    IOReturn cleanup_io;
    uint64_t sequence;
} ImtlQueryReply;

typedef struct ImtlBatchObserver {
    void (*scheduled)(void *opaque, uint64_t sequence);
    void *opaque;
} ImtlBatchObserver;

typedef struct ImtlBatchReply {
    uint8_t *bytes;
    size_t size;
    ImtlBatchResult result;
    IOReturn timer_error;
    IOReturn cleanup_io;
    uint64_t sequence;
    bool scheduled_observed;
} ImtlBatchReply;

typedef struct ImtlBatch5Reply {
    uint8_t *bytes;
    size_t size;
    ImtlBatch5Result result;
    IOReturn timer_error;
    IOReturn cleanup_io;
    uint64_t sequence;
    bool scheduled_observed;
} ImtlBatch5Reply;

typedef struct ImtlCoordinatorError {
    uint32_t kind;
    IOReturn io;
    uint32_t phase;
    uint32_t state;
    uint32_t completion_error;
    IOReturn timer_error;
    IOReturn cleanup_io;
    uint64_t sequence;
} ImtlCoordinatorError;

void imtl_coordinator_config_default(ImtlCoordinatorConfig *config);

/* Takes ownership of client only on success. Calls on one coordinator are
 * serialized. The caller must keep the handle alive, stop admitting new calls,
 * and wait for already admitted/queued calls before final destroy.
 */
IOReturn imtl_coordinator_create(ImtlUserClient *client,
                                 const ImtlCoordinatorConfig *config,
                                 ImtlCoordinator **out);

bool imtl_coordinator_query_library(ImtlCoordinator *coordinator,
                                    const void *source, size_t source_size,
                                    ImtlQueryReply *out,
                                    ImtlCoordinatorError *error);
bool imtl_coordinator_query_pipeline(ImtlCoordinator *coordinator,
                                     const void *source, size_t source_size,
                                     const char *kernel_name,
                                     ImtlQueryReply *out,
                                     ImtlCoordinatorError *error);
bool imtl_coordinator_query_imageblock(ImtlCoordinator *coordinator,
                                       const void *source, size_t source_size,
                                       const char *kernel_name, uint32_t width,
                                       uint32_t height, uint32_t depth,
                                       ImtlQueryReply *out,
                                       ImtlCoordinatorError *error);
/* Typed query manifests and their referenced payloads are borrowed only until
 * return. Each call builds one immutable canonical copy before entering the
 * serialized submit/retry path. The returned reply owns its bytes; decoded
 * views remain valid until imtl_query_reply_free().
 */
bool imtl_coordinator_query_library_typed(
    ImtlCoordinator *coordinator, const ImtlTypedQueryManifest *manifest,
    ImtlQueryReply *out, ImtlCoordinatorError *error);
bool imtl_coordinator_query_pipeline_typed(
    ImtlCoordinator *coordinator, const ImtlTypedQueryManifest *manifest,
    ImtlQueryReply *out, ImtlCoordinatorError *error);
bool imtl_coordinator_query_render_pipeline(
    ImtlCoordinator *coordinator, const ImtlTypedQueryManifest *manifest,
    ImtlQueryReply *out, ImtlCoordinatorError *error);
bool imtl_coordinator_query_imageblock_typed(
    ImtlCoordinator *coordinator, const ImtlTypedQueryManifest *manifest,
    uint32_t width, uint32_t height, uint32_t depth, ImtlQueryReply *out,
    ImtlCoordinatorError *error);
bool imtl_coordinator_query_argument_layout(
    ImtlCoordinator *coordinator, const ImtlTypedQueryManifest *manifest,
    ImtlQueryReply *out, ImtlCoordinatorError *error);
/* True publishes a completely validated reply. timer_error or cleanup_io may
 * still be nonzero and makes provider execution fail without image writeback.
 * On false, scheduled_observed and sequence may describe an already accepted
 * request, but bytes remains NULL.
 */
bool imtl_coordinator_execute_batch(ImtlCoordinator *coordinator,
                                    const void *manifest, size_t manifest_size,
                                    uint32_t buffer_count, uint32_t images_size,
                                    const ImtlBatchObserver *observer,
                                    ImtlBatchReply *out,
                                    ImtlCoordinatorError *error);
/* manifest is borrowed and must remain immutable until return. True publishes
 * one fully validated owned result containing buffer images followed by
 * texture images. The caller must refuse all writeback when timer_error or
 * cleanup_io is nonzero.
 */
bool imtl_coordinator_execute_batch5(
    ImtlCoordinator *coordinator, const void *manifest, size_t manifest_size,
    uint32_t buffer_count, uint32_t texture_count, uint32_t images_size,
    const ImtlBatchObserver *observer, ImtlBatch5Reply *out,
    ImtlCoordinatorError *error);

void imtl_query_reply_free(ImtlQueryReply *reply);
void imtl_batch_reply_free(ImtlBatchReply *reply);
void imtl_batch5_reply_free(ImtlBatch5Reply *reply);

/* Invalidation serializes with calls and closes the owned connection once.
 * Destroy requires the external lifetime condition above and clears *ptr.
 */
IOReturn imtl_coordinator_invalidate(ImtlCoordinator *coordinator);
void imtl_coordinator_destroy(ImtlCoordinator **coordinator);
IOReturn imtl_coordinator_close(ImtlCoordinator **coordinator);

#ifdef __cplusplus
}
#endif
#endif
