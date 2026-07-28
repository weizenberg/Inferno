/*
 * ARM AARCH64 Fallback Emulation.
 *
 * Copyright (c) 2025-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef TARGET_ARM_EMULATE_AARCH64_H
#define TARGET_ARM_EMULATE_AARCH64_H

#include "qemu/osdep.h"

typedef uint64_t (*ArmAarch64FallbackEmuGetReg)(CPUState *cpu, int rt);
typedef void (*ArmAarch64FallbackEmuSetReg)(CPUState *cpu, int rt,
                                            uint64_t val);
/*
 * SIMD&FP register access, little-endian, 16 bytes. A guest copying a buffer
 * through a trapping window does it with `str q0` and friends, not with integer
 * stores, so without these the copy fails and the guest takes a data abort it
 * cannot explain. Optional: leave both NULL and the SIMD&FP forms are refused,
 * which is the behaviour this emulator had before they existed.
 */
typedef void (*ArmAarch64FallbackEmuGetVReg)(CPUState *cpu, int rt,
                                             uint8_t val[16]);
typedef void (*ArmAarch64FallbackEmuSetVReg)(CPUState *cpu, int rt,
                                             const uint8_t val[16]);

/*
 * Make `env` readable for the duration of one emulation. The emulator only ever
 * reads it -- the PC to fetch the instruction from, and whatever the software
 * page-table walk consults -- and returns register results through the
 * callbacks above, so an accelerator that can offer a cheaper read-only
 * snapshot than a full bidirectional state sync should do so here. Optional;
 * cpu_synchronize_state() is used when it is NULL.
 */
typedef void (*ArmAarch64FallbackEmuSnapshotState)(CPUState *cpu);

typedef struct {
    ArmAarch64FallbackEmuGetReg get_reg;
    ArmAarch64FallbackEmuSetReg set_reg;
    ArmAarch64FallbackEmuGetVReg get_vreg;
    ArmAarch64FallbackEmuSetVReg set_vreg;
    ArmAarch64FallbackEmuSnapshotState snapshot_state;
} ArmAarch64FallbackEmuOps;

bool arm_aarch64_fallback_emu_single(CPUState *cpu, AddressSpace *as,
                                     const ArmAarch64FallbackEmuOps *ops);
#endif /* TARGET_ARM_EMULATE_AARCH64_H */
