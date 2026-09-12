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

#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/misc/apple-silicon/metal-bridge.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "migration/blocker.h"
#include "qemu/bswap.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "qemu/thread.h"
#include "system/address-spaces.h"
#include "trace.h"

#include <math.h>

OBJECT_DECLARE_SIMPLE_TYPE(InfernoMetalState, INFERNO_METAL_BRIDGE)

typedef struct InfernoMetalWork {
    InfernoMetalState *device;
    InfernoMetalCommand command;
    uint64_t generation;
    uint8_t *source;
    uint8_t *input;
    uint8_t *output;
    uint32_t progress;
    bool success;
    char message[256];
} InfernoMetalWork;

struct InfernoMetalState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    uint64_t base;
    uint64_t descriptor;
    uint64_t completed;
    uint64_t generation;
    uint32_t status;
    uint32_t error;
    uint32_t irq_status;
    uint32_t irq_mask;
    uint32_t progress;
    InfernoMetalBackend *backend;
    InfernoMetalWork *work;
    QemuThread worker;
    QEMUBH *completion_bh;
    QEMUBH *progress_bh;
    Error *migration_blocker;
};

static bool allZero(const uint8_t *bytes, size_t size)
{
    while (size--) {
        if (*bytes++) {
            return false;
        }
    }
    return true;
}

/* Called with BQL. Only ordinary contiguous RAM is a valid DMA target. */
static bool metal_ram_copy(uint64_t address, void *buffer, size_t size,
                           bool write, bool check_only)
{
    MemoryRegion *mr;
    hwaddr offset, length = size;
    RCU_READ_LOCK_GUARD();

    if (!size || address > UINT64_MAX - size) {
        return false;
    }
    mr = address_space_translate(&address_space_memory, address, &offset,
                                 &length, write, MEMTXATTRS_UNSPECIFIED);
    if (!mr || length < size || !memory_region_is_ram(mr) ||
        !memory_access_is_direct(mr, write, MEMTXATTRS_UNSPECIFIED)) {
        return false;
    }
    if (!check_only) {
        uint8_t *host = memory_region_get_ram_ptr(mr) + offset;

        if (write) {
            /* Use the DMA path so TCG translations are invalidated too. */
            return address_space_write(&address_space_memory, address,
                                       MEMTXATTRS_UNSPECIFIED, buffer,
                                       size) == MEMTX_OK;
        } else {
            memcpy(buffer, host, size);
        }
    }
    return true;
}

static void metal_irq_update(InfernoMetalState *s)
{
    qemu_set_irq(s->irq, (s->irq_status & s->irq_mask & 1) != 0);
}

static void metal_complete(InfernoMetalState *s, uint64_t sequence,
                           uint32_t error)
{
    s->completed = sequence;
    s->error = error;
    s->status = error ? INFERNO_METAL_FAILED : INFERNO_METAL_DONE;
    s->irq_status = 1;
    metal_irq_update(s);
    trace_inferno_metal_complete(sequence, error);
}

static void metal_work_free(InfernoMetalWork *work)
{
    g_free(work->source);
    g_free(work->input);
    g_free(work->output);
    g_free(work);
}

static void metal_work_progress(void *opaque, uint32_t flags)
{
    InfernoMetalWork *work = opaque;

    qatomic_or(&work->progress, flags & INFERNO_METAL_PROGRESS_MASK);
    qemu_bh_schedule(work->device->progress_bh);
}

static void metal_worker_progress(void *opaque)
{
    InfernoMetalState *s = opaque;

    assert(bql_locked());
    if (s->work && s->work->generation == s->generation) {
        uint32_t observed =
            qatomic_read(&s->work->progress) & INFERNO_METAL_PROGRESS_MASK;
        uint32_t added = observed & ~s->progress;

        s->progress |= observed;
        if (added) {
            trace_inferno_metal_progress(s->work->command.sequence, added);
        }
    }
}

static void *metal_worker(void *opaque)
{
    InfernoMetalWork *work = opaque;

    if (work->command.opcode == INFERNO_METAL_BATCH) {
        work->success = inferno_metal_backend_batch(
            work->device->backend, &work->command, work->input, work->output,
            metal_work_progress, work, work->message, sizeof(work->message));
    } else if (work->command.opcode >= INFERNO_METAL_QUERY_LIBRARY) {
        work->success = inferno_metal_backend_query(
            work->device->backend, &work->command, work->source, work->output,
            work->message, sizeof(work->message));
    } else {
        work->success = inferno_metal_backend_execute(
            work->device->backend, &work->command, work->source, work->input,
            work->output, work->message, sizeof(work->message));
    }
    qemu_bh_schedule(work->device->completion_bh);
    return NULL;
}

static void metal_worker_done(void *opaque)
{
    InfernoMetalState *s = opaque;
    InfernoMetalWork *work = s->work;
    InfernoMetalCommand *c;
    uint32_t error = INFERNO_METAL_OK;

    assert(bql_locked());
    if (!work) {
        return;
    }
    c = &work->command;
    qemu_thread_join(&s->worker);
    s->work = NULL;
    if (work->generation != s->generation) {
        /* Reset invalidates both DMA writeback and the old completion IRQ. */
        s->status = INFERNO_METAL_IDLE;
    } else {
        uint32_t observed =
            qatomic_read(&work->progress) & INFERNO_METAL_PROGRESS_MASK;
        uint32_t added = observed & ~s->progress;

        s->progress |= observed;
        if (added) {
            trace_inferno_metal_progress(c->sequence, added);
        }
        if (!work->success) {
            error = INFERNO_METAL_BACKEND_ERROR;
            trace_inferno_metal_error(c->sequence, work->message);
        } else if (!metal_ram_copy(c->output_gpa, work->output, c->output_size,
                                   true, false)) {
            error = INFERNO_METAL_BAD_MEMORY;
        }
        smp_wmb();
        metal_complete(s, c->sequence, error);
    }
    metal_work_free(work);
}

static bool metal_decode_command(const uint8_t *raw, InfernoMetalCommand *c)
{
    c->opcode = ldl_le_p(raw + 4);
    c->sequence = ldq_le_p(raw + 8);
    c->source_gpa = ldq_le_p(raw + 16);
    c->source_size = ldl_le_p(raw + 24);
    c->input_size = ldl_le_p(raw + 28);
    c->input_gpa = ldq_le_p(raw + 32);
    c->output_gpa = ldq_le_p(raw + 40);
    c->output_size = ldl_le_p(raw + 48);
    c->width = ldl_le_p(raw + 52);
    c->height = ldl_le_p(raw + 56);
    c->depth = ldl_le_p(raw + 60);
    c->options = ldl_le_p(raw + INFERNO_METAL_DESCRIPTOR_OPTIONS_OFFSET);
    memcpy(c->function, raw + 64, sizeof(c->function));
    memcpy(c->fragment, raw + 128, sizeof(c->fragment));

    for (unsigned i = 0; i < INFERNO_METAL_DESCRIPTOR_RESERVED_SIZE; i++) {
        if (raw[INFERNO_METAL_DESCRIPTOR_RESERVED_OFFSET + i]) {
            return false;
        }
    }
    if (ldl_le_p(raw) != INFERNO_METAL_VERSION || c->options ||
        !c->output_size || c->output_size > INFERNO_METAL_MAX_BUFFER ||
        c->input_size > INFERNO_METAL_MAX_BUFFER || !c->width || !c->height ||
        !c->depth) {
        return false;
    }
    if (c->opcode == INFERNO_METAL_BATCH) {
        if (c->source_size || c->input_size < INFERNO_METAL_BATCH_HEADER_SIZE ||
            c->output_size < INFERNO_METAL_BATCH_RESULT_SIZE || c->width != 1 ||
            c->height != 1 || c->depth != 1 ||
            !allZero((const uint8_t *)c->function, sizeof(c->function)) ||
            !allZero((const uint8_t *)c->fragment, sizeof(c->fragment))) {
            return false;
        }
        return true;
    } else if (c->opcode >= INFERNO_METAL_QUERY_LIBRARY &&
               c->opcode <= INFERNO_METAL_QUERY_IMAGEBLOCK) {
        bool library = c->opcode == INFERNO_METAL_QUERY_LIBRARY;
        bool imageblock = c->opcode == INFERNO_METAL_QUERY_IMAGEBLOCK;

        if (!c->source_size || c->source_size > INFERNO_METAL_MAX_SOURCE ||
            c->input_size ||
            (!imageblock &&
             (c->width != 1 || c->height != 1 || c->depth != 1)) ||
            (imageblock &&
             (!c->width || c->width > 65536 || !c->height ||
              c->height > 65536 || !c->depth || c->depth > 65536)) ||
            c->output_size < INFERNO_METAL_COMPILER_MIN_OUTPUT ||
            c->output_size > INFERNO_METAL_COMPILER_MAX_OUTPUT ||
            (!library && c->output_size != INFERNO_METAL_COMPILER_MIN_OUTPUT)) {
            return false;
        }
        for (unsigned i = 0; i < sizeof(c->fragment); i++) {
            if (c->fragment[i]) {
                return false;
            }
        }
        if (library) {
            for (unsigned i = 0; i < sizeof(c->function); i++) {
                if (c->function[i]) {
                    return false;
                }
            }
            return true;
        }
        return c->function[0] && memchr(c->function, 0, sizeof(c->function));
    } else if (c->opcode == INFERNO_METAL_COMPUTE) {
        if (c->width > INFERNO_METAL_MAX_THREADS ||
            c->height > INFERNO_METAL_MAX_THREADS / c->width ||
            c->depth > INFERNO_METAL_MAX_THREADS / c->width / c->height) {
            return false;
        }
    } else if (c->opcode == INFERNO_METAL_RENDER ||
               c->opcode == INFERNO_METAL_CLEAR) {
        if (c->width > 4096 || c->height > 4096 ||
            (uint64_t)c->width * c->height * 4 != c->output_size) {
            return false;
        }
        if (c->opcode == INFERNO_METAL_CLEAR) {
            return c->input_size == 16 && c->source_size == 0 && c->depth == 1;
        }
        if (c->depth > 65535 || c->depth % 3 || !c->fragment[0] ||
            !memchr(c->fragment, 0, sizeof(c->fragment))) {
            return false;
        }
    } else {
        return false;
    }
    return c->source_size && c->source_size <= INFERNO_METAL_MAX_SOURCE &&
           c->function[0] && memchr(c->function, 0, sizeof(c->function));
}

static void metal_submit(InfernoMetalState *s)
{
    uint8_t raw[INFERNO_METAL_DESCRIPTOR_SIZE];
    InfernoMetalWork *work = g_new0(InfernoMetalWork, 1);
    InfernoMetalCommand *c = &work->command;
    uint32_t error = INFERNO_METAL_BAD_MEMORY;

    if (!metal_ram_copy(s->descriptor, raw, sizeof(raw), false, false)) {
        goto fail;
    }
    if (!metal_decode_command(raw, c)) {
        error = INFERNO_METAL_BAD_DESCRIPTOR;
        goto fail;
    }
    if (!metal_ram_copy(c->output_gpa, NULL, c->output_size, true, true)) {
        goto fail;
    }
    if (c->source_size) {
        work->source = g_malloc(c->source_size);
        if (!metal_ram_copy(c->source_gpa, work->source, c->source_size, false,
                            false)) {
            goto fail;
        }
    }
    if (c->input_size) {
        work->input = g_malloc(c->input_size);
        if (!metal_ram_copy(c->input_gpa, work->input, c->input_size, false,
                            false)) {
            goto fail;
        }
    }
    if (c->opcode == INFERNO_METAL_CLEAR) {
        for (unsigned i = 0; i < 4; i++) {
            uint32_t bits = ldl_le_p(work->input + i * 4);
            float color;

            memcpy(&color, &bits, sizeof(color));
            if (!isfinite(color) || color < 0 || color > 1) {
                error = INFERNO_METAL_BAD_DESCRIPTOR;
                goto fail;
            }
        }
    }
    work->output = g_malloc(c->output_size);
    work->device = s;
    work->generation = s->generation;
    s->work = work;
    s->status = INFERNO_METAL_BUSY;
    s->error = INFERNO_METAL_OK;
    s->progress = 0;
    trace_inferno_metal_submit(c->sequence, c->opcode, c->output_size);
    qemu_thread_create(&s->worker, "inferno-metal", metal_worker, work,
                       QEMU_THREAD_JOINABLE);
    return;

fail:
    s->progress = 0;
    metal_complete(s, c->sequence, error);
    metal_work_free(work);
}

static void metal_reset(DeviceState *dev)
{
    InfernoMetalState *s = INFERNO_METAL_BRIDGE(dev);

    ++s->generation;
    s->descriptor = 0;
    s->completed = 0;
    s->error = 0;
    s->irq_status = 0;
    s->irq_mask = 0;
    s->progress = 0;
    /* Do not reuse the worker/backend until the old submission retires. */
    s->status = s->work ? INFERNO_METAL_BUSY : INFERNO_METAL_IDLE;
    metal_irq_update(s);
}

static MemTxResult metal_read(void *opaque, hwaddr offset, uint64_t *data,
                              unsigned size, MemTxAttrs attrs)
{
    InfernoMetalState *s = opaque;

    switch (offset) {
    case 0x00:
        *data = INFERNO_METAL_MAGIC;
        break;
    case 0x04:
        *data = INFERNO_METAL_VERSION;
        break;
    case 0x08:
        *data = s->status;
        break;
    case 0x0c:
        *data = s->error;
        break;
    case 0x10:
        *data = (uint32_t)s->descriptor;
        break;
    case 0x14:
        *data = s->descriptor >> 32;
        break;
    case 0x20:
        *data = (uint32_t)s->completed;
        break;
    case 0x24:
        *data = s->completed >> 32;
        break;
    case 0x28:
        *data = s->irq_status;
        break;
    case 0x2c:
        *data = s->irq_mask;
        break;
    case 0x38:
        *data = s->progress;
        break;
    default:
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult metal_write(void *opaque, hwaddr offset, uint64_t data,
                               unsigned size, MemTxAttrs attrs)
{
    InfernoMetalState *s = opaque;

    switch (offset) {
    case 0x10:
        s->descriptor = (s->descriptor & 0xffffffff00000000ULL) | data;
        break;
    case 0x14:
        s->descriptor = (s->descriptor & 0xffffffffULL) | (data << 32);
        break;
    case 0x18:
        if (s->status != INFERNO_METAL_IDLE) {
            /* A busy doorbell cannot corrupt the pending completion. */
            break;
        }
        if (data != 1) {
            metal_complete(s, 0, INFERNO_METAL_BAD_DESCRIPTOR);
            break;
        }
        metal_submit(s);
        break;
    case 0x2c:
        s->irq_mask = data & 1;
        metal_irq_update(s);
        break;
    case 0x30:
        s->irq_status &= ~(data & 1);
        if (!s->work && (data & 1)) {
            s->status = INFERNO_METAL_IDLE;
            s->progress = 0;
        }
        metal_irq_update(s);
        break;
    case 0x34:
        if (data == 1) {
            metal_reset(DEVICE(s));
        }
        break;
    default:
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps metal_ops = {
    .read_with_attrs = metal_read,
    .write_with_attrs = metal_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void metal_realize(DeviceState *dev, Error **errp)
{
    InfernoMetalState *s = INFERNO_METAL_BRIDGE(dev);

    if (!s->base || s->base & 0xffff || s->base > UINT64_MAX - 0x10000) {
        error_setg(errp, "Inferno Metal bridge requires an aligned MMIO addr");
        return;
    }
    s->backend = inferno_metal_backend_new(errp);
    if (!s->backend) {
        return;
    }
    error_setg(&s->migration_blocker,
               "Inferno Metal bridge GPU state is not migratable");
    if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
        inferno_metal_backend_free(s->backend);
        s->backend = NULL;
        return;
    }
    s->completion_bh =
        qemu_bh_new_guarded(metal_worker_done, s, &dev->mem_reentrancy_guard);
    s->progress_bh = qemu_bh_new_guarded(metal_worker_progress, s,
                                         &dev->mem_reentrancy_guard);
    memory_region_init_io(&s->mmio, OBJECT(s), &metal_ops, s,
                          TYPE_INFERNO_METAL_BRIDGE, 0x10000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    sysbus_mmio_map(SYS_BUS_DEVICE(s), 0, s->base);
}

static void metal_unrealize(DeviceState *dev)
{
    InfernoMetalState *s = INFERNO_METAL_BRIDGE(dev);

    metal_reset(dev);
    if (s->work) {
        /* Worker has no BQL dependency; join before deleting its BH/backend. */
        qemu_thread_join(&s->worker);
        qemu_bh_cancel(s->completion_bh);
        qemu_bh_cancel(s->progress_bh);
        metal_work_free(s->work);
        s->work = NULL;
    }
    qemu_bh_delete(s->completion_bh);
    qemu_bh_delete(s->progress_bh);
    inferno_metal_backend_free(s->backend);
    s->backend = NULL;
    migrate_del_blocker(&s->migration_blocker);
}

static const Property metal_properties[] = {
    DEFINE_PROP_UINT64("addr", InfernoMetalState, base, 0),
};

static void metal_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = metal_realize;
    dc->unrealize = metal_unrealize;
    dc->hotpluggable = false;
    dc->desc = "Host Metal execution bridge; requires a matching guest driver";
    device_class_set_props(dc, metal_properties);
    device_class_set_legacy_reset(dc, metal_reset);
}

static const TypeInfo metal_type = {
    .name = TYPE_INFERNO_METAL_BRIDGE,
    .parent = TYPE_DYNAMIC_SYS_BUS_DEVICE,
    .instance_size = sizeof(InfernoMetalState),
    .class_init = metal_class_init,
};

static void metal_register_types(void)
{
    type_register_static(&metal_type);
}

type_init(metal_register_types)
