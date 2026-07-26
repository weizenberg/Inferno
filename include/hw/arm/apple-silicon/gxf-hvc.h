/*
 * Apple GXF over HVC: private ABI between the kernel patcher and HVF.
 *
 * Copyright (c) 2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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

#ifndef HW_ARM_APPLE_SILICON_GXF_HVC_H
#define HW_ARM_APPLE_SILICON_GXF_HVC_H

/*
 * Under HVF the host CPU does not implement Apple's GENTER/GEXIT, and there is
 * no way to make Hypervisor.framework trap them: the resulting UNDEF is taken
 * to the *guest's* own vector, so XNU panics with
 *
 *     "Undefined kernel instruction: pc=... instr=201420"
 *
 * before the hypervisor ever sees it. The kernel patcher therefore rewrites
 * both instructions to HVC with the reserved immediates below, which the
 * EC_AA64_HVC handler in target/arm/hvf/hvf.c services by emulating guarded
 * mode. Under TCG the real instructions are decoded by disas_apple_insn(), so
 * the rewrite must only happen when hvf_enabled().
 *
 * Both sides of this ABI must agree; that is why it lives in a shared header.
 */

/*
 * Instruction encodings. The Apple group is bit31 == 0, bits[28:25] == 0 and
 * bits[15:10] == 5 (see disas_apple_insn()); bits[9:5] select GENTER (1) or
 * GEXIT (0), and bits[4:0] are Rd. The mask covers everything but Rd, so a
 * single pattern matches every Rd.
 */
#define APPLE_GXF_INSN_MASK 0xFFFFFFE0U
#define APPLE_GXF_INSN_GENTER 0x00201420U
#define APPLE_GXF_INSN_GEXIT 0x00201400U
#define APPLE_GXF_INSN_RD(insn) ((insn) & 0x1FU)

/*
 * GENTER's Rd is reported to the guest through ESR_GL (see syn_aa64_genter()),
 * so it has to survive the rewrite. It rides in the low five bits of the HVC
 * immediate, giving GENTER the range 0xF000..0xF01F. GEXIT ignores Rd, so it
 * gets a single immediate -- placed outside the GENTER range, since 0xF001
 * would alias GENTER with Rd == 1.
 */
#define HVF_HVC_GXF_ENTER_BASE 0xF000U
#define HVF_HVC_GXF_ENTER_MASK 0xFFE0U
#define HVF_HVC_GXF_EXIT 0xF100U

#define HVF_HVC_IS_GXF_ENTER(imm) \
    (((imm) & HVF_HVC_GXF_ENTER_MASK) == HVF_HVC_GXF_ENTER_BASE)
#define HVF_HVC_GXF_ENTER_RD(imm) ((imm) & 0x1FU)

/* HVC #imm16 is 0xD4000002 | imm16 << 5 (checked against the assembler). */
#define APPLE_GXF_HVC_INSN(imm) \
    (0xD4000002U | ((uint32_t)((imm) & 0xFFFFU) << 5))

#endif /* HW_ARM_APPLE_SILICON_GXF_HVC_H */
