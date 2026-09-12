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

#ifndef STANDARD_HEADERS_INFERNO_METAL_H
#define STANDARD_HEADERS_INFERNO_METAL_H

#include <stdint.h>

#define INFERNO_METAL_MAGIC 0x4c544d49U
#define INFERNO_METAL_VERSION 1U
#define INFERNO_METAL_DESCRIPTOR_SIZE 192U
#define INFERNO_METAL_MAX_SOURCE (64U * 1024U)
#define INFERNO_METAL_MAX_BUFFER (16U * 1024U * 1024U)
#define INFERNO_METAL_MAX_THREADS (16U * 1024U * 1024U)

enum {
    INFERNO_METAL_COMPUTE = 1,
    INFERNO_METAL_RENDER = 2,
    INFERNO_METAL_CLEAR = 3,
};

enum {
    INFERNO_METAL_IDLE = 0,
    INFERNO_METAL_BUSY = 1,
    INFERNO_METAL_DONE = 2,
    INFERNO_METAL_FAILED = 3,
};

enum {
    INFERNO_METAL_OK = 0,
    INFERNO_METAL_BAD_DESCRIPTOR = 1,
    INFERNO_METAL_BAD_MEMORY = 2,
    INFERNO_METAL_BACKEND_ERROR = 3,
};

/* Native field representation; serialize explicitly as little endian. */
typedef struct InfernoMetalCommand {
    uint32_t opcode;
    uint64_t sequence;
    uint64_t source_gpa;
    uint32_t source_size;
    uint32_t input_size;
    uint64_t input_gpa;
    uint64_t output_gpa;
    uint32_t output_size;
    uint32_t width;
    uint32_t height;
    /* Compute depth, or render vertex count. */
    uint32_t depth;
    char function[64];
    char fragment[64];
} InfernoMetalCommand;

/* Byte offsets of aligned 32-bit little-endian device registers. */
enum {
    INFERNO_METAL_REG_MAGIC = 0x00,
    INFERNO_METAL_REG_VERSION = 0x04,
    INFERNO_METAL_REG_STATUS = 0x08,
    INFERNO_METAL_REG_ERROR = 0x0c,
    INFERNO_METAL_REG_DESCRIPTOR_LO = 0x10,
    INFERNO_METAL_REG_DESCRIPTOR_HI = 0x14,
    INFERNO_METAL_REG_SUBMIT = 0x18,
    INFERNO_METAL_REG_SEQUENCE_LO = 0x20,
    INFERNO_METAL_REG_SEQUENCE_HI = 0x24,
    INFERNO_METAL_REG_PENDING = 0x28,
    INFERNO_METAL_REG_IRQ_ENABLE = 0x2c,
    INFERNO_METAL_REG_ACK = 0x30,
    INFERNO_METAL_REG_RESET = 0x34,
    INFERNO_METAL_REGISTER_SIZE = 0x38,
};

#endif
