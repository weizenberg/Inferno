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

#include "qemu/osdep.h"
#include "exec/target_page.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "target/arm/emulate/aarch64.h"
#include "cpu-qom.h"
#include "cpu.h"

// TODO: Protection checks? lol
static hwaddr arm_aarch64_fallback_emu_vtop(CPUState *cpu, vaddr addr)
{
    return cpu_get_phys_page_debug(cpu, addr & TARGET_PAGE_MASK) +
           (addr & ~TARGET_PAGE_MASK);
}

/*
 * Rn == 31 encodes SP, but the register callbacks map 31 to XZR, which is what
 * it means for Rt. Under HVF there is no way to reach SP through them either:
 * hvf_reg_match has no SP entry and SP_EL0/SP_EL1 are ARM_CP_ALIAS, so they are
 * left out of the cpreg list the sync walks. Nothing addresses MMIO through SP,
 * so refuse the instruction instead of silently using zero as the base.
 */
static bool emu_base(CPUState *cpu, ArmAarch64FallbackEmuGetReg get_reg,
                     uint32_t rn, uint64_t *base)
{
    if (rn == 31) {
        qemu_log_mask(LOG_UNIMP, "%s: SP-relative access unsupported\n",
                      __func__);
        return false;
    }

    *base = get_reg(cpu, rn);
    return true;
}

/* size is the access width in bytes, one of 1, 2, 4 or 8. */
static bool emu_load(AddressSpace *as, hwaddr pa, uint32_t size, uint64_t *out)
{
    uint8_t data[8] = { 0 };

    if (address_space_read(as, pa, MEMTXATTRS_UNSPECIFIED, data, size) !=
        MEMTX_OK) {
        return false;
    }

    switch (size) {
    case 1:
        *out = data[0];
        break;
    case 2:
        *out = lduw_le_p(data);
        break;
    case 4:
        *out = ldl_le_p(data);
        break;
    default:
        *out = ldq_le_p(data);
        break;
    }

    return true;
}

static bool emu_store(AddressSpace *as, hwaddr pa, uint32_t size, uint64_t val)
{
    uint8_t data[8] = { 0 };

    switch (size) {
    case 1:
        data[0] = (uint8_t)val;
        break;
    case 2:
        stw_le_p(data, (uint16_t)val);
        break;
    case 4:
        stl_le_p(data, (uint32_t)val);
        break;
    default:
        stq_le_p(data, val);
        break;
    }

    return address_space_write(as, pa, MEMTXATTRS_UNSPECIFIED, data, size) ==
           MEMTX_OK;
}

/*
 * LDP/STP/LDNP/STNP. imm7 is signed and scaled by the access size, and the two
 * halves are translated separately so that a pair straddling a page boundary
 * still lands in the right places.
 */
static bool emu_ldst_pair(CPUState *cpu, AddressSpace *as,
                          ArmAarch64FallbackEmuGetReg get_reg,
                          ArmAarch64FallbackEmuSetReg set_reg, uint32_t inst)
{
    uint32_t opc = extract32(inst, 30, 2);
    uint32_t mode = extract32(inst, 23, 3);
    bool is_load = extract32(inst, 22, 1);
    int64_t imm7 = sextract32(inst, 15, 7);
    uint32_t rt2 = extract32(inst, 10, 5);
    uint32_t rn = extract32(inst, 5, 5);
    uint32_t rt = extract32(inst, 0, 5);
    uint32_t size;
    uint64_t base, addr;
    bool post, writeback;

    if (extract32(inst, 26, 1)) {
        return false; // SIMD&FP
    }

    // opc 01 is LDPSW and 11 is unallocated.
    if (opc != 0 && opc != 2) {
        return false;
    }
    size = opc == 0 ? 4 : 8;

    switch (mode) {
    case 0: // LDNP/STNP, addressed like the offset form
    case 2: // signed offset
        post = false;
        writeback = false;
        break;
    case 1: // post-index
        post = true;
        writeback = true;
        break;
    default: // pre-index
        post = false;
        writeback = true;
        break;
    }

    if (!emu_base(cpu, get_reg, rn, &base)) {
        return false;
    }

    addr = base + (post ? 0 : imm7 * size);

    if (is_load) {
        uint64_t v1, v2;

        if (!emu_load(as, arm_aarch64_fallback_emu_vtop(cpu, addr), size,
                      &v1) ||
            !emu_load(as, arm_aarch64_fallback_emu_vtop(cpu, addr + size), size,
                      &v2)) {
            return false;
        }
        set_reg(cpu, rt, v1);
        set_reg(cpu, rt2, v2);
    } else {
        if (!emu_store(as, arm_aarch64_fallback_emu_vtop(cpu, addr), size,
                       get_reg(cpu, rt)) ||
            !emu_store(as, arm_aarch64_fallback_emu_vtop(cpu, addr + size),
                       size, get_reg(cpu, rt2))) {
            return false;
        }
    }

    if (writeback) {
        set_reg(cpu, rn, base + imm7 * size);
    }

    return true;
}

/*
 * Decode opc for the single-register integer forms: 00 stores, 01 loads
 * zero-extending, 10 and 11 load sign-extending to 64 and 32 bits. Returns
 * false for the encodings that are not a plain integer load or store.
 */
static bool emu_ldst_op(uint32_t size_bits, uint32_t opc, bool *is_load,
                        bool *sign_extend, uint32_t *ext_size)
{
    if (size_bits == 3 && opc >= 2) {
        return false; // PRFM and the unallocated encoding
    }
    if (size_bits == 2 && opc == 3) {
        return false; // unallocated
    }

    *is_load = opc != 0;
    *sign_extend = opc >= 2;
    *ext_size = opc == 3 ? 4 : 8;
    return true;
}

static bool emu_ldst_finish(CPUState *cpu, AddressSpace *as,
                            ArmAarch64FallbackEmuGetReg get_reg,
                            ArmAarch64FallbackEmuSetReg set_reg, uint32_t rt,
                            uint32_t size, bool is_load, bool sign_extend,
                            uint32_t ext_size, uint64_t addr)
{
    if (!is_load) {
        return emu_store(as, arm_aarch64_fallback_emu_vtop(cpu, addr), size,
                         get_reg(cpu, rt));
    }

    uint64_t val;

    if (!emu_load(as, arm_aarch64_fallback_emu_vtop(cpu, addr), size, &val)) {
        return false;
    }

    if (sign_extend) {
        val = (uint64_t)sextract64(val, 0, size * 8);
        if (ext_size == 4) {
            val = (uint32_t)val;
        }
    }

    set_reg(cpu, rt, val);
    return true;
}

/*
 * LDR/STR with an immediate: the unsigned-offset form scales imm12 by the
 * access size, while LDUR/STUR and the pre/post-index forms use an unscaled
 * signed imm9 at bits [20:12].
 */
static bool emu_ldst_imm(CPUState *cpu, AddressSpace *as,
                         ArmAarch64FallbackEmuGetReg get_reg,
                         ArmAarch64FallbackEmuSetReg set_reg, uint32_t inst)
{
    uint32_t size_bits = extract32(inst, 30, 2);
    uint32_t opc = extract32(inst, 22, 2);
    uint32_t rn = extract32(inst, 5, 5);
    uint32_t rt = extract32(inst, 0, 5);
    uint32_t size = 1U << size_bits;
    uint32_t ext_size;
    int64_t off;
    uint64_t base, addr;
    bool is_load, sign_extend;
    bool post = false, writeback = false;

    if (!emu_ldst_op(size_bits, opc, &is_load, &sign_extend, &ext_size)) {
        return false;
    }

    if (extract32(inst, 24, 1)) {
        off = (int64_t)extract32(inst, 10, 12) * size;
    } else {
        off = sextract32(inst, 12, 9);
        switch (extract32(inst, 10, 2)) {
        case 0: // LDUR/STUR
            break;
        case 1: // post-index
            post = true;
            writeback = true;
            break;
        case 3: // pre-index
            writeback = true;
            break;
        default:
            return false; // LDTR/STTR
        }
    }

    if (!emu_base(cpu, get_reg, rn, &base)) {
        return false;
    }

    addr = base + (post ? 0 : off);

    if (!emu_ldst_finish(cpu, as, get_reg, set_reg, rt, size, is_load,
                         sign_extend, ext_size, addr)) {
        return false;
    }

    if (writeback) {
        set_reg(cpu, rn, base + off);
    }

    return true;
}

/* LDR/STR with a register offset, optionally extended and scaled. */
static bool emu_ldst_reg(CPUState *cpu, AddressSpace *as,
                         ArmAarch64FallbackEmuGetReg get_reg,
                         ArmAarch64FallbackEmuSetReg set_reg, uint32_t inst)
{
    uint32_t size_bits = extract32(inst, 30, 2);
    uint32_t opc = extract32(inst, 22, 2);
    uint32_t rm = extract32(inst, 16, 5);
    uint32_t option = extract32(inst, 13, 3);
    uint32_t rn = extract32(inst, 5, 5);
    uint32_t rt = extract32(inst, 0, 5);
    uint32_t size = 1U << size_bits;
    uint32_t ext_size;
    uint64_t base, index;
    bool is_load, sign_extend;

    if (extract32(inst, 10, 2) != 2) {
        return false;
    }
    if (!emu_ldst_op(size_bits, opc, &is_load, &sign_extend, &ext_size)) {
        return false;
    }

    index = get_reg(cpu, rm);

    switch (option) {
    case 2: // UXTW
        index = (uint32_t)index;
        break;
    case 6: // SXTW
        index = (uint64_t)(int64_t)(int32_t)index;
        break;
    case 3: // LSL
    case 7: // SXTX
        break;
    default:
        return false;
    }

    if (extract32(inst, 12, 1)) {
        index <<= size_bits;
    }

    if (!emu_base(cpu, get_reg, rn, &base)) {
        return false;
    }

    return emu_ldst_finish(cpu, as, get_reg, set_reg, rt, size, is_load,
                           sign_extend, ext_size, base + index);
}

bool arm_aarch64_fallback_emu_single(CPUState *cpu, AddressSpace *as,
                                     ArmAarch64FallbackEmuGetReg get_reg,
                                     ArmAarch64FallbackEmuSetReg set_reg)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    uint32_t inst = 0;
    bool success;

    cpu_synchronize_state(cpu);

    if (address_space_read(as, arm_aarch64_fallback_emu_vtop(cpu, env->pc),
                           MEMTXATTRS_UNSPECIFIED, &inst,
                           sizeof(inst)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: failed to read instruction at 0x%" PRIx64 "\n",
                      __func__, env->pc);
        return false;
    }

    inst = le32_to_cpu(inst);

    if (extract32(inst, 25, 1) != 0) {
        success = false;
    } else if (extract32(inst, 27, 3) == 5) {
        success = emu_ldst_pair(cpu, as, get_reg, set_reg, inst);
    } else if (extract32(inst, 27, 3) == 7 && extract32(inst, 26, 1) == 0) {
        if (extract32(inst, 24, 1) == 0 && extract32(inst, 21, 1)) {
            success = emu_ldst_reg(cpu, as, get_reg, set_reg, inst);
        } else {
            success = emu_ldst_imm(cpu, as, get_reg, set_reg, inst);
        }
    } else {
        success = false;
    }

    qemu_log_mask(CPU_LOG_INT,
                  "%s: cpu %d pc 0x%" PRIx64 " insn 0x%08x success=%s\n",
                  __func__, cpu->cpu_index, env->pc, inst,
                  success ? "true" : "false");

    return success;
}
