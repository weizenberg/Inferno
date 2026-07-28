/*
 * QEMU Hypervisor.framework support
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "accel/accel-ops.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/hvf.h"
#include "system/hvf_int.h"
#include "hw/core/cpu.h"
#include "hw/boards.h"
#include "qemu/cacheflush.h"
#include "trace.h"

bool hvf_allowed;

struct mac_slot {
    int present;
    uint64_t size;
    uint64_t gpa_start;
    uint64_t gva;
};

struct mac_slot mac_slots[32];

void hvf_enable_sprr_compat(void)
{
    /*
     * Hypervisor.framework does not expose Apple private SPRR accesses as
     * system-register exits. Run executable pages through a one-time
     * compatibility pass so target/arm can replace the EL0 permission write
     * that HVF cannot execute. This only changes transient guest RAM.
     */
    hvf_state->sprr_compat = true;
}

const char *hvf_return_string(hv_return_t ret)
{
    switch (ret) {
    case HV_SUCCESS:      return "HV_SUCCESS";
    case HV_ERROR:        return "HV_ERROR";
    case HV_BUSY:         return "HV_BUSY";
    case HV_BAD_ARGUMENT: return "HV_BAD_ARGUMENT";
    case HV_NO_RESOURCES: return "HV_NO_RESOURCES";
    case HV_NO_DEVICE:    return "HV_NO_DEVICE";
    case HV_UNSUPPORTED:  return "HV_UNSUPPORTED";
    case HV_DENIED:       return "HV_DENIED";
    default:              return "[unknown hv_return value]";
    }
}

void assert_hvf_ok_impl(hv_return_t ret, const char *file, unsigned int line,
                        const char *exp)
{
    if (ret == HV_SUCCESS) {
        return;
    }

    error_report("Error: %s = %s (0x%x, at %s:%u)",
        exp, hvf_return_string(ret), ret, file, line);

    abort();
}

static int do_hvf_set_memory(hvf_slot *slot, hv_memory_flags_t flags)
{
    struct mac_slot *macslot;
    hv_return_t ret;

    macslot = &mac_slots[slot->slot_id];

    if (macslot->present) {
        if (macslot->size != slot->size) {
            macslot->present = 0;
            trace_hvf_vm_unmap(macslot->gpa_start, macslot->size);
            ret = hv_vm_unmap(macslot->gpa_start, macslot->size);
            assert_hvf_ok(ret);
        }
    }

    if (!slot->size) {
        g_clear_pointer(&slot->exec_bitmap, g_free);
        slot->exec_page_count = 0;
        slot->flags &= ~HVF_SLOT_SPRR_EXEC;
        return 0;
    }

    slot->memory_flags = flags;
    if (hvf_state->sprr_compat && (flags & HV_MEMORY_EXEC)) {
        size_t page_size = qemu_real_host_page_size();

        slot->exec_page_count = slot->size / page_size;
        slot->exec_bitmap = g_new0(uint8_t, slot->exec_page_count);
        slot->flags |= HVF_SLOT_SPRR_EXEC;
        flags &= ~HV_MEMORY_EXEC;
    } else {
        slot->flags &= ~HVF_SLOT_SPRR_EXEC;
    }

    macslot->present = 1;
    macslot->gpa_start = slot->start;
    macslot->size = slot->size;
    trace_hvf_vm_map(slot->start, slot->size, slot->mem, flags,
                     flags & HV_MEMORY_READ ?  'R' : '-',
                     flags & HV_MEMORY_WRITE ? 'W' : '-',
                     flags & HV_MEMORY_EXEC ?  'X' : '-');
    ret = hv_vm_map(slot->mem, slot->start, slot->size, flags);
    assert_hvf_ok(ret);
    return 0;
}

bool hvf_sprr_exec_fault(hwaddr physical_address, bool patch_sprr)
{
    const uint32_t sprr_el0br0_write = 0xd51ef1a0;
    const uint32_t sysreg_write_mask = 0xffffffe0;
    const uint32_t nop = 0xd503201f;
    size_t page_size = qemu_real_host_page_size();
    hwaddr page = QEMU_ALIGN_DOWN(physical_address, page_size);
    hvf_slot *slot = hvf_find_overlap_slot(page, page_size);
    size_t page_index;
    uint8_t *host_page;
    unsigned int patched = 0;
    hv_return_t ret;

    if (!slot || !(slot->flags & HVF_SLOT_SPRR_EXEC) ||
        page < slot->start || page + page_size > slot->start + slot->size) {
        return false;
    }

    page_index = (page - slot->start) / page_size;
    if (page_index >= slot->exec_page_count ||
        slot->exec_bitmap[page_index]) {
        return false;
    }

    host_page = slot->mem + page - slot->start;
    if (patch_sprr) {
        for (size_t offset = 0; offset < page_size; offset += sizeof(uint32_t)) {
            uint32_t *insn = (uint32_t *)(host_page + offset);

            if ((ldl_le_p(insn) & sysreg_write_mask) ==
                sprr_el0br0_write) {
                stl_le_p(insn, nop);
                patched++;
            }
        }
        if (patched) {
            flush_idcache_range((uintptr_t)host_page,
                                (uintptr_t)host_page, page_size);
            info_report("HVF: replaced %u unsupported SPRR write%s in "
                        "executable guest page 0x%" HWADDR_PRIx,
                        patched, patched == 1 ? "" : "s", page);
        }
    }

    ret = hv_vm_protect(page, page_size, slot->memory_flags);
    assert_hvf_ok(ret);
    slot->exec_bitmap[page_index] = 1;
    return true;
}

static void hvf_protect_slot(hvf_slot *slot, hv_memory_flags_t flags)
{
    size_t page_size = qemu_real_host_page_size();
    size_t first = 0;

    slot->memory_flags = flags;
    if (!(slot->flags & HVF_SLOT_SPRR_EXEC)) {
        hv_return_t ret = hv_vm_protect(slot->start, slot->size, flags);

        assert_hvf_ok(ret);
        return;
    }

    while (first < slot->exec_page_count) {
        bool executable = slot->exec_bitmap[first];
        size_t last = first + 1;
        hv_memory_flags_t range_flags = flags;
        hv_return_t ret;

        while (last < slot->exec_page_count &&
               !!slot->exec_bitmap[last] == executable) {
            last++;
        }
        if (!executable) {
            range_flags &= ~HV_MEMORY_EXEC;
        }
        ret = hv_vm_protect(slot->start + first * page_size,
                            (last - first) * page_size, range_flags);
        assert_hvf_ok(ret);
        first = last;
    }
}

static void hvf_set_phys_mem(MemoryRegionSection *section, bool add)
{
    hvf_slot *mem;
    MemoryRegion *area = section->mr;
    bool writable = !area->readonly && !area->rom_device;
    hv_memory_flags_t flags;
    uint64_t page_size = qemu_real_host_page_size();

    if (!memory_region_is_ram(area)) {
        if (writable) {
            return;
        } else if (!memory_region_is_romd(area)) {
            /*
             * If the memory device is not in romd_mode, then we actually want
             * to remove the hvf memory slot so all accesses will trap.
             */
             add = false;
        }
    }

    if (!QEMU_IS_ALIGNED(int128_get64(section->size), page_size) ||
        !QEMU_IS_ALIGNED(section->offset_within_address_space, page_size)) {
        /* Not page aligned, so we can not map as RAM */
        add = false;
    }

    mem = hvf_find_overlap_slot(
            section->offset_within_address_space,
            int128_get64(section->size));

    if (mem && add) {
        if (mem->size == int128_get64(section->size) &&
            mem->start == section->offset_within_address_space &&
            mem->mem == (memory_region_get_ram_ptr(area) +
            section->offset_within_region)) {
            return; /* Same region was attempted to register, go away. */
        }
    }

    /* Region needs to be reset. set the size to 0 and remap it. */
    if (mem) {
        mem->size = 0;
        if (do_hvf_set_memory(mem, 0)) {
            error_report("Failed to reset overlapping slot");
            abort();
        }
    }

    if (!add) {
        return;
    }

    if (area->readonly ||
        (!memory_region_is_ram(area) && memory_region_is_romd(area))) {
        flags = HV_MEMORY_READ | HV_MEMORY_EXEC;
    } else {
        flags = HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC;
    }

    /* Now make a new slot. */
    int x;

    for (x = 0; x < hvf_state->num_slots; ++x) {
        mem = &hvf_state->slots[x];
        if (!mem->size) {
            break;
        }
    }

    if (x == hvf_state->num_slots) {
        error_report("No free slots");
        abort();
    }

    mem->size = int128_get64(section->size);
    mem->mem = memory_region_get_ram_ptr(area) + section->offset_within_region;
    mem->start = section->offset_within_address_space;
    mem->region = area;

    if (do_hvf_set_memory(mem, flags)) {
        error_report("Error registering new memory slot");
        abort();
    }
}

static void hvf_set_dirty_tracking(MemoryRegionSection *section, bool on)
{
    hvf_slot *slot;

    slot = hvf_find_overlap_slot(
            section->offset_within_address_space,
            int128_get64(section->size));

    /* protect region against writes; begin tracking it */
    if (on) {
        slot->flags |= HVF_SLOT_LOG;
        hvf_protect_slot(slot, HV_MEMORY_READ | HV_MEMORY_EXEC);
    /* stop tracking region*/
    } else {
        slot->flags &= ~HVF_SLOT_LOG;
        hvf_protect_slot(slot, HV_MEMORY_READ | HV_MEMORY_WRITE |
                        HV_MEMORY_EXEC);
    }
}

static void hvf_log_start(MemoryListener *listener,
                          MemoryRegionSection *section, int old, int new)
{
    if (old != 0) {
        return;
    }

    hvf_set_dirty_tracking(section, 1);
}

static void hvf_log_stop(MemoryListener *listener,
                         MemoryRegionSection *section, int old, int new)
{
    if (new != 0) {
        return;
    }

    hvf_set_dirty_tracking(section, 0);
}

static void hvf_log_sync(MemoryListener *listener,
                         MemoryRegionSection *section)
{
    /*
     * sync of dirty pages is handled elsewhere; just make sure we keep
     * tracking the region.
     */
    hvf_set_dirty_tracking(section, 1);
}

static void hvf_region_add(MemoryListener *listener,
                           MemoryRegionSection *section)
{
    hvf_set_phys_mem(section, true);
}

static void hvf_region_del(MemoryListener *listener,
                           MemoryRegionSection *section)
{
    hvf_set_phys_mem(section, false);
}

static MemoryListener hvf_memory_listener = {
    .name = "hvf",
    .priority = MEMORY_LISTENER_PRIORITY_ACCEL,
    .region_add = hvf_region_add,
    .region_del = hvf_region_del,
    .log_start = hvf_log_start,
    .log_stop = hvf_log_stop,
    .log_sync = hvf_log_sync,
};

static int hvf_accel_init(AccelState *as, MachineState *ms)
{
    int x;
    hv_return_t ret;
    HVFState *s = HVF_STATE(as);
    int pa_range = 36;
    MachineClass *mc = MACHINE_GET_CLASS(ms);

    if (mc->hvf_get_physical_address_range) {
        pa_range = mc->hvf_get_physical_address_range(ms);
        if (pa_range < 0) {
            return -EINVAL;
        }
    }

    ret = hvf_arch_vm_create(ms, (uint32_t)pa_range);
    if (ret == HV_DENIED) {
        error_report("Could not access HVF. Is the executable signed"
                     " with com.apple.security.hypervisor entitlement?");
        exit(1);
    }
    assert_hvf_ok(ret);

    s->num_slots = ARRAY_SIZE(s->slots);
    for (x = 0; x < s->num_slots; ++x) {
        s->slots[x].size = 0;
        s->slots[x].slot_id = x;
    }

    QTAILQ_INIT(&s->hvf_sw_breakpoints);

    hvf_state = s;
    memory_listener_register(&hvf_memory_listener, &address_space_memory);

    return hvf_arch_init();
}

static int hvf_gdbstub_sstep_flags(AccelState *as)
{
    return SSTEP_ENABLE | SSTEP_NOIRQ;
}

static void hvf_accel_class_init(ObjectClass *oc, const void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);
    ac->name = "HVF";
    ac->init_machine = hvf_accel_init;
    ac->allowed = &hvf_allowed;
    ac->gdbstub_supported_sstep_flags = hvf_gdbstub_sstep_flags;
}

static const TypeInfo hvf_accel_type = {
    .name = TYPE_HVF_ACCEL,
    .parent = TYPE_ACCEL,
    .instance_size = sizeof(HVFState),
    .class_init = hvf_accel_class_init,
};

static void hvf_type_init(void)
{
    type_register_static(&hvf_accel_type);
}

type_init(hvf_type_init);
