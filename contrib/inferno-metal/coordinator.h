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

void imtl_query_reply_free(ImtlQueryReply *reply);

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
