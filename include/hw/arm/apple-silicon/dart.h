/*
 * Apple Device Address Resolution Table.
 *
 * Copyright (c) 2024-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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

#ifndef HW_ARM_APPLE_SILICON_DART_H
#define HW_ARM_APPLE_SILICON_DART_H

#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dt.h"
#include "qom/object.h"

typedef struct AppleDARTState AppleDARTState;

#define TYPE_APPLE_DART "apple-dart"
OBJECT_DECLARE_SIMPLE_TYPE(AppleDARTState, APPLE_DART)

#define TYPE_APPLE_DART_IOMMU_MEMORY_REGION "apple-dart-iommu"
OBJECT_DECLARE_SIMPLE_TYPE(AppleDARTIOMMUMemoryRegion,
                           APPLE_DART_IOMMU_MEMORY_REGION)

#define DART_DART_FORCE_ACTIVE "dart-dart_force_active"
#define DART_DART_REQUEST_SID "dart-dart_request_sid"
#define DART_DART_RELEASE_SID "dart-dart_release_sid"
#define DART_DART_SELF "dart-dart_self"

IOMMUMemoryRegion *apple_dart_iommu_mr(AppleDARTState *dart, uint32_t sid);
IOMMUMemoryRegion *apple_dart_instance_iommu_mr(AppleDARTState *s,
                                                uint32_t instance,
                                                uint32_t sid);
AppleDARTState *apple_dart_from_node(AppleDTNode *node);

/*
 * Walk one IOVA with no guest-visible side effects (no error_status latch, no
 * IRQ). For debug taps that must be invisible to the guest; see
 * apple_dart_mirror_probe().
 */
bool apple_dart_probe_iova(IOMMUMemoryRegion *iommu, hwaddr addr, hwaddr *pa,
                           AddressSpace **target_as);

/*
 * Choose the AddressSpace this DART's translations resolve into. Defaults to
 * address_space_memory. A machine can point a DART at a private downstream view
 * so that the memory behind it is reachable only through translation, which is
 * what an HVF vCPU requires: Hypervisor.framework maps one VM-wide address
 * space, so anything also present in the global map is walked by hardware and
 * bypasses the IOMMU entirely.
 */
void apple_dart_set_target_as(AppleDARTState *dart, AddressSpace *as);

/*
 * Publish this DART's translation of [base, base + size) into `into` as a plain
 * RAM alias, so an accelerator that walks page tables in hardware reaches the
 * right memory without exiting.
 *
 * This exists purely as an HVF optimisation and is the counterpart to
 * apple_dart_set_target_as(): pointing a DART at a private downstream view is
 * what makes its translations *correct* under HVF, but it also means every
 * access by the DMA master traps, since HVF caches no IOMMU translations. When
 * the guest's mapping of the window turns out to be one contiguous run of RAM
 * -- which is what a coprocessor carveout normally is -- the same bytes can be
 * exposed directly, costing one memory slot and zero exits.
 *
 * Deliberately all-or-nothing: the mirror is installed only while the *entire*
 * window is mapped, contiguous, and backed by a single RAM region. Anything
 * else (partial mapping, scattered pages, MMIO behind the DART) leaves the
 * window trapping, which is slower but always correct -- in particular an
 * access to an unmapped IOVA keeps faulting instead of silently reading RAM.
 * The mirror is re-evaluated whenever the guest invalidates the DART TLB.
 *
 * `into` is normally get_system_memory(); `iommu_mr` must belong to `dart`.
 */
void apple_dart_install_dma_mirror(AppleDARTState *dart,
                                   IOMMUMemoryRegion *iommu_mr,
                                   MemoryRegion *into, hwaddr base,
                                   uint64_t size);

#endif /* HW_ARM_APPLE_SILICON_DART_H */
