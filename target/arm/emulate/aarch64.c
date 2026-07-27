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

/* Widest access this emulator handles: one 128-bit SIMD&FP register. */
#define EMU_MAX_ACCESS 16

// TODO: Protection checks? lol
static hwaddr arm_aarch64_fallback_emu_vtop(CPUState *cpu, vaddr addr)
{
    return cpu_get_phys_page_debug(cpu, addr & TARGET_PAGE_MASK) +
           (addr & ~TARGET_PAGE_MASK);
}

/*
 * Rn == 31 encodes SP, but the register callbacks map 31 to XZR, which is what
 * it means for Rt and is all hvf_get_reg()/hvf_set_reg() implement. Nothing
 * addresses MMIO through SP, so refuse the instruction rather than silently
 * using zero as the base address.
 */
static bool emu_base(CPUState *cpu, const ArmAarch64FallbackEmuOps *ops,
                     uint32_t rn, uint64_t *base)
{
    if (rn == 31) {
        qemu_log_mask(LOG_UNIMP, "%s: SP-relative access unsupported\n",
                      __func__);
        return false;
    }

    *base = ops->get_reg(cpu, rn);
    return true;
}

/* size is the access width in bytes: 1, 2, 4 or 8 for the integer forms. */
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
 * SIMD&FP transfers. The register is always written in full: a 64-bit load into
 * Dn zeroes bits 127:64, which falls out of starting from a zeroed buffer.
 */
static bool emu_vload(CPUState *cpu, AddressSpace *as,
                      const ArmAarch64FallbackEmuOps *ops, uint32_t rt,
                      uint32_t size, hwaddr pa)
{
    uint8_t v[EMU_MAX_ACCESS] = { 0 };

    if (address_space_read(as, pa, MEMTXATTRS_UNSPECIFIED, v, size) !=
        MEMTX_OK) {
        return false;
    }

    ops->set_vreg(cpu, rt, v);
    return true;
}

static bool emu_vstore(CPUState *cpu, AddressSpace *as,
                       const ArmAarch64FallbackEmuOps *ops, uint32_t rt,
                       uint32_t size, hwaddr pa)
{
    uint8_t v[EMU_MAX_ACCESS] = { 0 };

    ops->get_vreg(cpu, rt, v);
    return address_space_write(as, pa, MEMTXATTRS_UNSPECIFIED, v, size) ==
           MEMTX_OK;
}

static bool emu_have_simd(const ArmAarch64FallbackEmuOps *ops)
{
    if (ops->get_vreg == NULL || ops->set_vreg == NULL) {
        qemu_log_mask(LOG_UNIMP, "%s: SIMD&FP access unsupported\n", __func__);
        return false;
    }

    return true;
}

/*
 * LDP/STP/LDNP/STNP, integer and SIMD&FP. imm7 is signed and scaled by the
 * access size, and the two halves are translated separately so that a pair
 * straddling a page boundary still lands in the right places.
 */
static bool emu_ldst_pair(CPUState *cpu, AddressSpace *as,
                          const ArmAarch64FallbackEmuOps *ops, uint32_t inst)
{
    uint32_t opc = extract32(inst, 30, 2);
    bool is_simd = extract32(inst, 26, 1);
    uint32_t mode = extract32(inst, 23, 3);
    bool is_load = extract32(inst, 22, 1);
    int64_t imm7 = sextract32(inst, 15, 7);
    uint32_t rt2 = extract32(inst, 10, 5);
    uint32_t rn = extract32(inst, 5, 5);
    uint32_t rt = extract32(inst, 0, 5);
    uint32_t size;
    uint64_t base, addr;
    bool post, writeback;

    if (is_simd) {
        /* opc: 00 = 32-bit S, 01 = 64-bit D, 10 = 128-bit Q, 11 unallocated. */
        if (opc == 3) {
            return false;
        }
        if (!emu_have_simd(ops)) {
            return false;
        }
        size = 4u << opc;
    } else {
        // opc 01 is LDPSW and 11 is unallocated.
        if (opc != 0 && opc != 2) {
            return false;
        }
        size = opc == 0 ? 4 : 8;
    }

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

    if (!emu_base(cpu, ops, rn, &base)) {
        return false;
    }

    addr = base + (post ? 0 : imm7 * size);

    if (is_simd) {
        hwaddr pa1 = arm_aarch64_fallback_emu_vtop(cpu, addr);
        hwaddr pa2 = arm_aarch64_fallback_emu_vtop(cpu, addr + size);

        if (is_load) {
            if (!emu_vload(cpu, as, ops, rt, size, pa1) ||
                !emu_vload(cpu, as, ops, rt2, size, pa2)) {
                return false;
            }
        } else {
            if (!emu_vstore(cpu, as, ops, rt, size, pa1) ||
                !emu_vstore(cpu, as, ops, rt2, size, pa2)) {
                return false;
            }
        }
    } else if (is_load) {
        uint64_t v1, v2;

        if (!emu_load(as, arm_aarch64_fallback_emu_vtop(cpu, addr), size,
                      &v1) ||
            !emu_load(as, arm_aarch64_fallback_emu_vtop(cpu, addr + size), size,
                      &v2)) {
            return false;
        }
        ops->set_reg(cpu, rt, v1);
        ops->set_reg(cpu, rt2, v2);
    } else {
        if (!emu_store(as, arm_aarch64_fallback_emu_vtop(cpu, addr), size,
                       ops->get_reg(cpu, rt)) ||
            !emu_store(as, arm_aarch64_fallback_emu_vtop(cpu, addr + size),
                       size, ops->get_reg(cpu, rt2))) {
            return false;
        }
    }

    if (writeback) {
        ops->set_reg(cpu, rn, base + imm7 * size);
    }

    return true;
}

/*
 * Decode size and opc for the single-register forms.
 *
 * Integer (V=0): opc 00 stores, 01 loads zero-extending, 10 and 11 load
 * sign-extending to 64 and 32 bits.
 *
 * SIMD&FP (V=1): opc<0> is the load bit and opc<1> selects the 128-bit access,
 * which is only allowed with size == 00. So B/H/S/D come from size and Q comes
 * from opc<1>.
 */
static bool emu_ldst_op(bool is_simd, uint32_t size_bits, uint32_t opc,
                        uint32_t *size, bool *is_load, bool *sign_extend,
                        uint32_t *ext_size)
{
    if (is_simd) {
        if ((opc & 2) != 0 && size_bits != 0) {
            return false; // unallocated
        }
        *size = (opc & 2) != 0 ? 16 : (1u << size_bits);
        *is_load = (opc & 1) != 0;
        *sign_extend = false;
        *ext_size = 0;
        return true;
    }

    if (size_bits == 3 && opc >= 2) {
        return false; // PRFM and the unallocated encoding
    }
    if (size_bits == 2 && opc == 3) {
        return false; // unallocated
    }

    *size = 1u << size_bits;
    *is_load = opc != 0;
    *sign_extend = opc >= 2;
    *ext_size = opc == 3 ? 4 : 8;
    return true;
}

static bool emu_ldst_finish(CPUState *cpu, AddressSpace *as,
                            const ArmAarch64FallbackEmuOps *ops, uint32_t rt,
                            uint32_t size, bool is_simd, bool is_load,
                            bool sign_extend, uint32_t ext_size, uint64_t addr)
{
    hwaddr pa = arm_aarch64_fallback_emu_vtop(cpu, addr);

    if (is_simd) {
        return is_load ? emu_vload(cpu, as, ops, rt, size, pa) :
                         emu_vstore(cpu, as, ops, rt, size, pa);
    }

    if (!is_load) {
        return emu_store(as, pa, size, ops->get_reg(cpu, rt));
    }

    uint64_t val;

    if (!emu_load(as, pa, size, &val)) {
        return false;
    }

    if (sign_extend) {
        val = (uint64_t)sextract64(val, 0, size * 8);
        if (ext_size == 4) {
            val = (uint32_t)val;
        }
    }

    ops->set_reg(cpu, rt, val);
    return true;
}

/*
 * LDR/STR with an immediate: the unsigned-offset form scales imm12 by the
 * access size, while LDUR/STUR and the pre/post-index forms use an unscaled
 * signed imm9 at bits [20:12].
 */
static bool emu_ldst_imm(CPUState *cpu, AddressSpace *as,
                         const ArmAarch64FallbackEmuOps *ops, uint32_t inst)
{
    uint32_t size_bits = extract32(inst, 30, 2);
    bool is_simd = extract32(inst, 26, 1);
    uint32_t opc = extract32(inst, 22, 2);
    uint32_t rn = extract32(inst, 5, 5);
    uint32_t rt = extract32(inst, 0, 5);
    uint32_t size, ext_size;
    int64_t off;
    uint64_t base, addr;
    bool is_load, sign_extend;
    bool post = false, writeback = false;

    if (!emu_ldst_op(is_simd, size_bits, opc, &size, &is_load, &sign_extend,
                     &ext_size)) {
        return false;
    }
    if (is_simd && !emu_have_simd(ops)) {
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

    if (!emu_base(cpu, ops, rn, &base)) {
        return false;
    }

    addr = base + (post ? 0 : off);

    if (!emu_ldst_finish(cpu, as, ops, rt, size, is_simd, is_load, sign_extend,
                         ext_size, addr)) {
        return false;
    }

    if (writeback) {
        ops->set_reg(cpu, rn, base + off);
    }

    return true;
}

/* LDR/STR with a register offset, optionally extended and scaled. */
static bool emu_ldst_reg(CPUState *cpu, AddressSpace *as,
                         const ArmAarch64FallbackEmuOps *ops, uint32_t inst)
{
    uint32_t size_bits = extract32(inst, 30, 2);
    bool is_simd = extract32(inst, 26, 1);
    uint32_t opc = extract32(inst, 22, 2);
    uint32_t rm = extract32(inst, 16, 5);
    uint32_t option = extract32(inst, 13, 3);
    uint32_t rn = extract32(inst, 5, 5);
    uint32_t rt = extract32(inst, 0, 5);
    uint32_t size, ext_size;
    uint64_t base, index;
    bool is_load, sign_extend;

    if (extract32(inst, 10, 2) != 2) {
        return false;
    }
    if (!emu_ldst_op(is_simd, size_bits, opc, &size, &is_load, &sign_extend,
                     &ext_size)) {
        return false;
    }
    if (is_simd && !emu_have_simd(ops)) {
        return false;
    }

    index = ops->get_reg(cpu, rm);

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

    /* S scales by log2 of the access size, which for Q is 4, not size_bits. */
    if (extract32(inst, 12, 1)) {
        index <<= ctz32(size);
    }

    if (!emu_base(cpu, ops, rn, &base)) {
        return false;
    }

    return emu_ldst_finish(cpu, as, ops, rt, size, is_simd, is_load,
                           sign_extend, ext_size, base + index);
}

/*
 * Advanced SIMD load/store multiple structures, non-interleaved only: LD1/ST1
 * of 1 to 4 consecutive registers, with the no-offset and post-index forms.
 * This is what a bulk copy compiles to -- SEPOS moves the xART payload with
 * `ld1 {v0.4s-v3.4s}, [xN], #64` -- so without it a trapping DMA window cannot
 * carry a memcpy. The interleaved variants (LD2/LD3/LD4) need element
 * de-interleaving and are refused rather than silently mistranslated.
 */
static bool emu_ldst_multi(CPUState *cpu, AddressSpace *as,
                           const ArmAarch64FallbackEmuOps *ops, uint32_t inst)
{
    bool is_q = extract32(inst, 30, 1);
    bool post = extract32(inst, 23, 1);
    bool is_load = extract32(inst, 22, 1);
    uint32_t rm = extract32(inst, 16, 5);
    uint32_t opcode = extract32(inst, 12, 4);
    uint32_t rn = extract32(inst, 5, 5);
    uint32_t rt = extract32(inst, 0, 5);
    uint32_t regs, reg_size, i;
    uint64_t base, addr;

    switch (opcode) {
    case 0x7: // LD1/ST1, one register
        regs = 1;
        break;
    case 0xA: // LD1/ST1, two registers
        regs = 2;
        break;
    case 0x6: // LD1/ST1, three registers
        regs = 3;
        break;
    case 0x2: // LD1/ST1, four registers
        regs = 4;
        break;
    default:
        return false; // LD2/LD3/LD4 and friends: interleaved, not handled
    }

    if (!emu_have_simd(ops)) {
        return false;
    }

    reg_size = is_q ? 16 : 8;

    if (!emu_base(cpu, ops, rn, &base)) {
        return false;
    }
    addr = base;

    for (i = 0; i < regs; i++) {
        uint32_t reg = (rt + i) % 32;
        hwaddr pa = arm_aarch64_fallback_emu_vtop(cpu, addr + i * reg_size);

        if (is_load) {
            if (!emu_vload(cpu, as, ops, reg, reg_size, pa)) {
                return false;
            }
        } else {
            if (!emu_vstore(cpu, as, ops, reg, reg_size, pa)) {
                return false;
            }
        }
    }

    if (post) {
        /* Rm == 31 selects the immediate, which is exactly what was moved. */
        uint64_t step =
            rm == 31 ? (uint64_t)regs * reg_size : ops->get_reg(cpu, rm);

        ops->set_reg(cpu, rn, base + step);
    }

    return true;
}

bool arm_aarch64_fallback_emu_single(CPUState *cpu, AddressSpace *as,
                                     const ArmAarch64FallbackEmuOps *ops)
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

    if (extract32(inst, 31, 1) == 0 && extract32(inst, 24, 6) == 0x0C &&
        extract32(inst, 21, 1) == 0) {
        success = emu_ldst_multi(cpu, as, ops, inst);
    } else if (extract32(inst, 25, 1) != 0) {
        success = false;
    } else if (extract32(inst, 27, 3) == 5) {
        success = emu_ldst_pair(cpu, as, ops, inst);
    } else if (extract32(inst, 27, 3) == 7) {
        if (extract32(inst, 24, 1) == 0 && extract32(inst, 21, 1)) {
            success = emu_ldst_reg(cpu, as, ops, inst);
        } else {
            success = emu_ldst_imm(cpu, as, ops, inst);
        }
    } else {
        success = false;
    }

    qemu_log_mask(
        CPU_LOG_INT, "%s: cpu %d pc 0x%" PRIx64 " insn 0x%08x success=%s\n",
        __func__, cpu->cpu_index, env->pc, inst, success ? "true" : "false");

    return success;
}
