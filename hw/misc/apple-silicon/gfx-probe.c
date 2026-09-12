/*
 * Apple GPU startup MMIO probe.
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
#include "hw/misc/apple-silicon/gfx-probe.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "trace.h"

#define TYPE_APPLE_GFX_PROBE "apple-gfx-probe"
OBJECT_DECLARE_SIMPLE_TYPE(AppleGFXProbeState, APPLE_GFX_PROBE)

struct AppleGFXProbeState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    char *name;
    uint64_t base;
    uint64_t size;
};

static MemTxResult apple_gfx_probe_read(void *opaque, hwaddr addr,
                                        uint64_t *data, unsigned size,
                                        MemTxAttrs attrs)
{
    AppleGFXProbeState *s = opaque;

    *data = 0;
    trace_apple_gfx_probe_fault(s->name, s->base + addr, size, false, 0);
    return MEMTX_ERROR;
}

static MemTxResult apple_gfx_probe_write(void *opaque, hwaddr addr,
                                         uint64_t data, unsigned size,
                                         MemTxAttrs attrs)
{
    AppleGFXProbeState *s = opaque;

    trace_apple_gfx_probe_fault(s->name, s->base + addr, size, true, data);
    return MEMTX_ERROR;
}

static const MemoryRegionOps apple_gfx_probe_ops = {
    .read_with_attrs = apple_gfx_probe_read,
    .write_with_attrs = apple_gfx_probe_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .impl.unaligned = true,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void apple_gfx_probe_realize(DeviceState *dev, Error **errp)
{
    AppleGFXProbeState *s = APPLE_GFX_PROBE(dev);

    if (!s->name || !s->size || s->base > UINT64_MAX - s->size) {
        error_setg(errp, "GPU probe requires a name and valid MMIO range");
        return;
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &apple_gfx_probe_ops, s,
                          s->name, s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const Property apple_gfx_probe_properties[] = {
    DEFINE_PROP_STRING("name", AppleGFXProbeState, name),
    DEFINE_PROP_UINT64("base", AppleGFXProbeState, base, 0),
    DEFINE_PROP_UINT64("size", AppleGFXProbeState, size, 0),
};

static void apple_gfx_probe_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = apple_gfx_probe_realize;
    device_class_set_props(dc, apple_gfx_probe_properties);
    dc->desc = "Diagnostic GPU MMIO ranges; every access faults";
}

static const TypeInfo apple_gfx_probe_type_info = {
    .name = TYPE_APPLE_GFX_PROBE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleGFXProbeState),
    .class_init = apple_gfx_probe_class_init,
};

static void apple_gfx_probe_register_types(void)
{
    type_register_static(&apple_gfx_probe_type_info);
}

void apple_gfx_probe_map(const char *name, hwaddr base, hwaddr size)
{
    DeviceState *dev = qdev_new(TYPE_APPLE_GFX_PROBE);

    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_uint64(dev, "base", base);
    qdev_prop_set_uint64(dev, "size", size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
}

type_init(apple_gfx_probe_register_types)
