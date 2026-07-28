/*
 * Apple iPhone 11 WLAN (Broadcom BCM4378 PCIe)
 *
 * Copyright (c) 2026 ChefKiss Inc.
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
#include "hw/arm/apple-silicon/dt.h"
#include "hw/irq.h"
#include "hw/misc/apple-silicon/smc.h"
#include "hw/misc/apple-silicon/wlan.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "qemu/units.h"
#include "trace.h"

/*
 * Phase-1 stub: config space plus BAR windows that log every access.
 * The guest-visible identity follows the iOS 14 n104 DeviceTree
 * (`wlan-pcie,bcm4378` under `arm-io/apcie/pci-bridge2`); no kext in the
 * kernelcache matches the WiFi function by PCI ID, so the vendor/device
 * pair below only has to be plausible, not exact.
 */

#define TYPE_APPLE_WLAN_DEVICE "apple-wlan-device"
OBJECT_DECLARE_SIMPLE_TYPE(AppleWLANDeviceState, APPLE_WLAN_DEVICE)

#define TYPE_APPLE_WLAN "apple-wlan"
OBJECT_DECLARE_SIMPLE_TYPE(AppleWLANState, APPLE_WLAN)

#define APPLE_WLAN_VENDOR_ID (0x14e4) // Broadcom
#define APPLE_WLAN_DEVICE_ID (0x4425) // BCM4378 WiFi function (guess)
#define APPLE_WLAN_SUBVENDOR_ID (0x106b) // Apple
#define APPLE_WLAN_SUBDEVICE_ID (0x4378)

// BAR0 is the backplane window; BAR2 is the PCIe core register window.
#define APPLE_WLAN_DEVICE_BAR0_SIZE (16 * KiB)
#define APPLE_WLAN_DEVICE_BAR2_SIZE (4 * KiB)

struct AppleWLANDeviceState {
    PCIDevice parent_obj;

    AppleWLANState *root;
    ApplePCIEPort *port;
    MemoryRegion *dma_mr;
    AddressSpace *dma_as;

    MemoryRegion bar0;
    MemoryRegion bar2;

    /*
     * Until the backplane is modelled, the windows are flat RAM so that
     * write-then-verify sequences (backplane window register, OTP block,
     * mailbox indices) behave sanely instead of reading back zero.
     */
    uint8_t bar0_backing[APPLE_WLAN_DEVICE_BAR0_SIZE];
    uint8_t bar2_backing[APPLE_WLAN_DEVICE_BAR2_SIZE];
};

struct AppleWLANState {
    SysBusDevice parent_obj;

    PCIBus *pci_bus;
    AppleWLANDeviceState *device;
    uint32_t gp11;
};

// The /amfm node's function-reg_on points at SMC key gP11: the combo-chip
// power gate the AppleBCMWLANBusInterfacePCIe driver toggles through the
// AMFM platform function before it touches the PCIe function.
static SMCResult apple_wlan_smc_gp11_read(SMCKey *key, SMCKeyData *data,
                                          const void *in, uint8_t in_length)
{
    AppleWLANState *s = key->opaque;

    trace_apple_wlan_gp11_read(s->gp11);
    stl_le_p(data->data, s->gp11);
    return SMC_RESULT_SUCCESS;
}

static SMCResult apple_wlan_smc_gp11_write(SMCKey *key, SMCKeyData *data,
                                           const void *in, uint8_t in_length)
{
    AppleWLANState *s = key->opaque;

    if (in == NULL || in_length != key->info.size) {
        return SMC_RESULT_BAD_ARGUMENT_ERROR;
    }
    s->gp11 = ldl_le_p(in);
    trace_apple_wlan_gp11_write(s->gp11);
    return SMC_RESULT_SUCCESS;
}

static uint64_t apple_wlan_bar0_ops_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    AppleWLANDeviceState *s = opaque;
    uint64_t value = 0;

    if (offset + size <= sizeof(s->bar0_backing)) {
        memcpy(&value, s->bar0_backing + offset, size);
    }
    trace_apple_wlan_bar0_read(offset, size, value);
    return value;
}

static void apple_wlan_bar0_ops_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    AppleWLANDeviceState *s = opaque;

    if (offset + size <= sizeof(s->bar0_backing)) {
        memcpy(s->bar0_backing + offset, &value, size);
    }
    trace_apple_wlan_bar0_write(offset, size, value);
}

static uint64_t apple_wlan_bar2_ops_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    AppleWLANDeviceState *s = opaque;
    uint64_t value = 0;

    if (offset + size <= sizeof(s->bar2_backing)) {
        memcpy(&value, s->bar2_backing + offset, size);
    }
    trace_apple_wlan_bar2_read(offset, size, value);
    return value;
}

static void apple_wlan_bar2_ops_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    AppleWLANDeviceState *s = opaque;

    if (offset + size <= sizeof(s->bar2_backing)) {
        memcpy(s->bar2_backing + offset, &value, size);
    }
    trace_apple_wlan_bar2_write(offset, size, value);
}

#define APPLE_WLAN_BAR_OPS(_name) \
    static const MemoryRegionOps _name = {              \
        .read = _name##_read,                           \
        .write = _name##_write,                         \
        .endianness = DEVICE_LITTLE_ENDIAN,             \
        .valid = {                                      \
            .min_access_size = 1,                       \
            .max_access_size = 8,                       \
        },                                              \
        .impl = {                                       \
            .min_access_size = 1,                       \
            .max_access_size = 4,                       \
        },                                              \
    }

APPLE_WLAN_BAR_OPS(apple_wlan_bar0_ops);
APPLE_WLAN_BAR_OPS(apple_wlan_bar2_ops);

static void apple_wlan_device_pci_realize(PCIDevice *dev, Error **errp)
{
    AppleWLANDeviceState *s = APPLE_WLAN_DEVICE(dev);
    uint8_t *pci_conf = dev->config;

    pci_conf[PCI_INTERRUPT_PIN] = 1;
    pci_set_word(pci_conf + PCI_SUBSYSTEM_VENDOR_ID, APPLE_WLAN_SUBVENDOR_ID);
    pci_set_word(pci_conf + PCI_SUBSYSTEM_ID, APPLE_WLAN_SUBDEVICE_ID);

    memory_region_init_io(&s->bar0, OBJECT(dev), &apple_wlan_bar0_ops, s,
                          TYPE_APPLE_WLAN_DEVICE ".bar0",
                          APPLE_WLAN_DEVICE_BAR0_SIZE);
    memory_region_init_io(&s->bar2, OBJECT(dev), &apple_wlan_bar2_ops, s,
                          TYPE_APPLE_WLAN_DEVICE ".bar2",
                          APPLE_WLAN_DEVICE_BAR2_SIZE);

    assert_true(pci_is_express(dev));
    pcie_endpoint_cap_init(dev, 0x70);
    msi_init(dev, 0x50, 1, true, false, &error_fatal);
    pci_pm_init(dev, 0x40, &error_fatal);

    if (s->port->maximum_link_speed == 2) {
        pcie_cap_fill_link_ep_usp(dev, QEMU_PCI_EXP_LNK_X1,
                                  QEMU_PCI_EXP_LNK_5GT);
    }

    // The link is always up in this model; report Data Link Layer Active.
    pci_word_test_and_set_mask(dev->config + dev->exp.exp_cap + PCI_EXP_LNKSTA,
                               PCI_EXP_LNKSTA_DLLLA);

    // Broadcom WiFi uses a 0x3c-sized version-1 AER capability.
    pcie_aer_init(dev, 1, 0x100, 0x3c, &error_fatal);

    pci_register_bar(
        dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64,
        &s->bar0);
    pci_register_bar(
        dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64,
        &s->bar2);
}

static void apple_wlan_device_qdev_reset_hold(Object *obj, ResetType type)
{
    PCIDevice *dev = PCI_DEVICE(obj);

    pci_set_word(dev->config + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
}

static void apple_wlan_device_pci_uninit(PCIDevice *dev)
{
    pcie_aer_exit(dev);
    pcie_cap_exit(dev);
    msi_uninit(dev);
}

static void apple_wlan_device_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *c = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    c->realize = apple_wlan_device_pci_realize;
    c->exit = apple_wlan_device_pci_uninit;
    c->vendor_id = APPLE_WLAN_VENDOR_ID;
    c->device_id = APPLE_WLAN_DEVICE_ID;
    c->revision = 0x01;
    c->class_id = PCI_CLASS_NETWORK_OTHER;

    rc->phases.hold = apple_wlan_device_qdev_reset_hold;

    dc->desc = "Apple WLAN Device";
    dc->user_creatable = false;
    dc->hotpluggable = false;

    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static void apple_wlan_realize(DeviceState *dev, Error **errp)
{
    AppleWLANState *s = APPLE_WLAN(dev);

    qdev_realize(DEVICE(s->device), BUS(s->pci_bus), &error_fatal);

}

static const VMStateDescription vmstate_apple_wlan = {
    .name = "apple_wlan",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields =
        (const VMStateField[]){
            VMSTATE_END_OF_LIST(),
        }
};

static void apple_wlan_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = apple_wlan_realize;
    dc->desc = "Apple WLAN";
    dc->vmsd = &vmstate_apple_wlan;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

SysBusDevice *apple_wlan_create(AppleDTNode *node, PCIBus *pci_bus,
                                ApplePCIEPort *port)
{
    DeviceState *dev;
    AppleWLANState *s;
    PCIDevice *pci_dev;

    dev = qdev_new(TYPE_APPLE_WLAN);
    s = APPLE_WLAN(dev);

    s->pci_bus = pci_bus;
    pci_dev = pci_new(-1, TYPE_APPLE_WLAN_DEVICE);
    s->device = APPLE_WLAN_DEVICE(pci_dev);
    s->device->root = s;
    s->device->port = port;
    s->device->dma_mr = port->dma_mr;
    s->device->dma_as = &port->dma_as;

    object_property_add_child(OBJECT(s), "device", OBJECT(s->device));

    AppleSMCState *smc = APPLE_SMC_IOP(object_property_get_link(
        OBJECT(qdev_get_machine()), "smc", &error_fatal));
    apple_smc_add_key_func(smc, 'gP11', 4, SMC_KEY_TYPE_UINT32,
                           SMC_ATTR_LE | SMC_ATTR_UNK_0x20, s,
                           apple_wlan_smc_gp11_read, apple_wlan_smc_gp11_write);

    return SYS_BUS_DEVICE(dev);
}

static const TypeInfo apple_wlan_types[] = {
    {
        .name = TYPE_APPLE_WLAN_DEVICE,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(AppleWLANDeviceState),
        .class_init = apple_wlan_device_class_init,
        .interfaces = (InterfaceInfo[]){ { INTERFACE_PCIE_DEVICE }, {} },
    },
    {
        .name = TYPE_APPLE_WLAN,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(AppleWLANState),
        .class_init = apple_wlan_class_init,
    },
};

DEFINE_TYPES(apple_wlan_types)
