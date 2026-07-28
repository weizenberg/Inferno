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

/*
 * On Broadcom PCIe parts the low 4 KiB of BAR0 is a moveable window into the
 * chip's backplane; the window base lives in PCI config space (BAR0_WINDOW).
 * The chip identity is the ChipCommon `chipid` register at backplane 0x18000000
 * offset 0, encoded as id | rev << 16 | package << 20 | type << 28.
 */
#define APPLE_WLAN_BAR0_WINDOW_CFG (0x80)
#define APPLE_WLAN_BAR0_WINDOW_SIZE (4 * KiB)
#define APPLE_WLAN_CHIPCOMMON_BASE (0x18000000)
#define APPLE_WLAN_CHIPID_OFFSET (0x0)
// n104 ships a BCM4378 stepping B1; the guest picks its firmware directory from
// this (`/usr/share/firmware/wifi/C-4378__s-B1`).
#define APPLE_WLAN_CHIP_ID (0x4378)
#define APPLE_WLAN_CHIP_REV (0x3)
#define APPLE_WLAN_CHIP_PKG (0x0)
#define APPLE_WLAN_CHIP_TYPE (0x1) // AXI backplane

/*
 * Measured: before publishing hardware identifiers the guest reads backplane
 * 0x18011120..0x180113fe as 16-bit words (880 accesses, immediately before
 * "publishHWIdentifiers: Bad argument"), i.e. the chip's OTP starting at offset
 * 0x120 -- the same OTP base the public brcmfmac driver uses for BCM4378, whose
 * contents are CIS/TLV tuples. It is not modelled yet, so the guest finds no
 * valid tuples and cannot publish identifiers. Serving a synthesised blank or a
 * bare SROM signature there was measured to change nothing.
 */
#define APPLE_WLAN_OTP_BASE (0x18011000)
#define APPLE_WLAN_OTP_CIS_OFFSET (0x120)
#define APPLE_WLAN_OTP_WORDS (0x170) // matches brcmfmac's BCM4378 OTP size
#define APPLE_WLAN_OTP_END \
    (APPLE_WLAN_OTP_CIS_OFFSET + APPLE_WLAN_OTP_WORDS * 2)

/*
 * Measured, for whoever models the OTP: the guest reads this region as 0x170
 * 16-bit words starting at offset 0x120 -- the same core/base/size the public
 * brcmfmac driver uses for BCM4378's OTP, whose payload is CIS/TLV tuples.
 * Tested and ruled out as the cause of "publishHWIdentifiers: Bad argument":
 * a bare SROM signature, a synthesised CIS vendor tuple naming the module, and
 * the driver's own `wlan.debug.module-instance` boot-arg override all left that
 * failure unchanged -- and it also reproduces on boots where this region is
 * never read at all, so the failing check is upstream of the OTP contents.
 */

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

    // Backplane address currently mapped into the low 4 KiB of BAR0, set by
    // the guest through PCI config space (BAR0_WINDOW). Tracking it turns the
    // otherwise opaque BAR0 offsets into real backplane addresses.
    uint32_t bar0_window;
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

    /*
     * Measured: after this gate is written the driver logs "Power transition
     * before init" and then waits 120 s before failing with "AdjustBusy timeout
     * in 120000 ms!". It never re-reads gP11, so it waits on an event from the
     * device. Raising a bare PCI interrupt here was tested and is NOT enough --
     * the chip also has to present coherent mailbox/shared-memory state for the
     * handler to read, i.e. the boot/firmware-download interface has to exist.
     */
    return SMC_RESULT_SUCCESS;
}

static uint64_t apple_wlan_bar0_ops_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    AppleWLANDeviceState *s = opaque;
    uint64_t value = 0;
    bool windowed = offset < APPLE_WLAN_BAR0_WINDOW_SIZE;
    uint32_t backplane = s->bar0_window + (uint32_t)offset;

    if (windowed &&
        backplane == APPLE_WLAN_CHIPCOMMON_BASE + APPLE_WLAN_CHIPID_OFFSET) {
        value = APPLE_WLAN_CHIP_ID | (APPLE_WLAN_CHIP_REV << 16) |
                (APPLE_WLAN_CHIP_PKG << 20) | (APPLE_WLAN_CHIP_TYPE << 28);
    } else if (offset + size <= sizeof(s->bar0_backing)) {
        memcpy(&value, s->bar0_backing + offset, size);
    }
    if (windowed) {
        trace_apple_wlan_backplane_read(backplane, offset, size, value);
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

// Track BAR0_WINDOW so BAR0 accesses can be resolved to backplane addresses.
static void apple_wlan_device_config_write(PCIDevice *dev, uint32_t addr,
                                           uint32_t val, int len)
{
    AppleWLANDeviceState *s = APPLE_WLAN_DEVICE(dev);

    pci_default_write_config(dev, addr, val, len);

    if (addr == APPLE_WLAN_BAR0_WINDOW_CFG && len == 4) {
        s->bar0_window = val;
        trace_apple_wlan_bar0_window(val);
    }
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
    c->config_write = apple_wlan_device_config_write;
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
