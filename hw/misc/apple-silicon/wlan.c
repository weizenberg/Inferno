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
 * On Broadcom PCIe parts BAR0 carries *two* independently moveable 4 KiB windows
 * onto the chip's backplane, each with its own base register in PCI config
 * space:
 *
 *   BAR0 + 0x0000  <-  config 0x80 (BAR0_WINDOW)
 *   BAR0 + 0x4000  <-  config 0x74 (the "core 2" window)
 *
 * Both must be modelled. The driver keeps track of which core each window
 * currently maps and reaches for whichever one is not already pointing at a core
 * it still needs, so which window a given access arrives through varies from
 * boot to boot for identical driver code. Modelling only BAR0_WINDOW silently
 * loses every access made through the other window: they land beyond the end of
 * a too-small BAR0 and are absorbed by whatever the guest mapped next (here,
 * BAR2), which reads back as zeroes. That is what made the OTP read — and hence
 * the whole hardware-identity publish — fail on some boots and succeed on
 * others with no change in the binary.
 *
 * The chip identity is the ChipCommon `chipid` register at backplane 0x18000000
 * offset 0, encoded as id | rev << 16 | package << 20 | type << 28.
 */
#define APPLE_WLAN_BAR0_WINDOW_CFG (0x80)
#define APPLE_WLAN_BAR0_CORE2_WINDOW_CFG (0x74)
#define APPLE_WLAN_BAR0_WINDOW_SIZE (4 * KiB)
#define APPLE_WLAN_BAR0_CORE2_WINDOW_OFFSET (16 * KiB)
// The reset value the driver expects to find in the core-2 window register.
#define APPLE_WLAN_BAR0_CORE2_WINDOW_RESET (0x18002000)
#define APPLE_WLAN_CHIPCOMMON_BASE (0x18000000)
#define APPLE_WLAN_CHIPID_OFFSET (0x0)
// n104 ships a BCM4378 stepping B1; the guest picks its firmware directory from
// this (`/usr/share/firmware/wifi/C-4378__s-B1`).
#define APPLE_WLAN_CHIP_ID (0x4378)
#define APPLE_WLAN_CHIP_REV (0x3)
#define APPLE_WLAN_CHIP_PKG (0x0)
#define APPLE_WLAN_CHIP_TYPE (0x1) // AXI backplane

/*
 * ChipCommon's indirect SROM interface, which is how the guest actually reads
 * the chip's provisioning data. Derived from the driver, not guessed:
 * AppleBCMWLANChipManagerPCIe::readChipProvisioningData() maps core 0
 * (ChipCommon) and then only touches offsets 0x04, 0x190, 0x194 and 0x198 --
 * the public Broadcom ChipCommon capabilities, sromcontrol, sromaddress and
 * sromdata registers. It validates the result with
 * AppleBCMWLANUtil::getcrc8(buf, len, 0xFF) and compares against 0x9F, i.e.
 * Broadcom's CRC8_INIT_VALUE / CRC8_GOOD_VALUE. The CRC table the driver uses
 * (at 0xfffffff007308f30) is a reflected CRC8 with polynomial 0xAB.
 */
#define APPLE_WLAN_CC_CAPABILITIES (0x004)
#define APPLE_WLAN_CC_CAP_SPROM_PRESENT (1U << 30)
#define APPLE_WLAN_CC_SROM_CONTROL (0x190)
#define APPLE_WLAN_CC_SROM_CONTROL_BUSY (1U << 31)
#define APPLE_WLAN_CC_SROM_ADDRESS (0x194)
#define APPLE_WLAN_CC_SROM_DATA (0x198)
#define APPLE_WLAN_CRC8_POLY (0xAB)
#define APPLE_WLAN_CRC8_INIT (0xFF)
#define APPLE_WLAN_CRC8_GOOD (0x9F)
// Provisioning blob: body plus one trailing byte chosen so the CRC lands on
// CRC8_GOOD_VALUE. Deliberately empty for now -- an empty-but-valid blob tests
// whether the driver's parse merely needs to succeed, without guessing at key
// names we have not yet identified.
#define APPLE_WLAN_SROM_BYTES (64)

/*
 * Measured: before publishing hardware identifiers the guest reads backplane
 * 0x18011120..0x180113fe as 16-bit words (880 accesses, immediately before
 * "publishHWIdentifiers: Bad argument"), i.e. the chip's OTP starting at offset
 * 0x120 -- the same OTP base the public brcmfmac driver uses for BCM4378, whose
 * contents are CIS/TLV tuples. It is not modelled yet, so the guest finds no
 * valid tuples and cannot publish identifiers. Serving a synthesised blank or a
 * bare SROM signature there was measured to change nothing.
 */
/*
 * OTP layout, read out of the driver rather than guessed:
 *
 *   AppleBCMWLANChipManagerPCIe::readChipProvisioningData() pulls 0x170 16-bit
 *   words starting at offset 0x120 of the GCI core -- the same base/size the
 *   public brcmfmac driver uses for BCM4378 -- and validates them with
 *   AppleBCMWLANUtil::getcrc8(buf, len, 0xFF), requiring Broadcom's
 *   CRC8_GOOD_VALUE of 0x9F. The CRC table it uses is a reflected CRC8 with
 *   polynomial 0xAB.
 *
 *   AppleBCMWLANBusInterface::parseOTPData() then walks it as CIS tuples:
 *   a type byte, 0x00 meaning "skip one byte" and 0xFF meaning end-of-list,
 *   otherwise a length byte at +1 and the payload at +2.
 *
 *   AppleBCMWLANBusInterface::parseOTPTuple() dispatches type 0x15 with
 *   length > 6 to parseVersion1Tuple(), which strlcpy()s identity strings into
 *   the bus interface. publishHWIdentifiers() then requires those strings to be
 *   non-empty, and FilesDB is keyed off them -- which is why an empty OTP ends
 *   as "publishHWIdentifiers: Bad argument" and no firmware is ever selected.
 */
#define APPLE_WLAN_OTP_BASE (0x18011000)
#define APPLE_WLAN_OTP_CIS_OFFSET (0x120)
#define APPLE_WLAN_OTP_WORDS (0x170) // matches brcmfmac's BCM4378 OTP size
#define APPLE_WLAN_OTP_SIZE (APPLE_WLAN_OTP_WORDS * 2)
#define APPLE_WLAN_CIS_TYPE_NULL (0x00)
#define APPLE_WLAN_CIS_TYPE_VERS1 (0x15)
#define APPLE_WLAN_CIS_TYPE_END (0xFF)
#define APPLE_WLAN_OTP_END \
    (APPLE_WLAN_OTP_CIS_OFFSET + APPLE_WLAN_OTP_WORDS * 2)

/*
 * Measured, for whoever models the OTP: the guest reads this region as 0x170
 * 16-bit words starting at offset 0x120 -- the same core/base/size the public
 * brcmfmac driver uses for BCM4378's OTP, whose payload is CIS/TLV tuples.
 * Tested and ruled out as the cause of "publishHWIdentifiers: Bad argument":
 * a bare SROM signature, a synthesised CIS vendor tuple naming the module, and
 * the driver's own `wlan.debug.module-instance` boot-arg override all left that
 * failure unchanged. What does satisfy it is a type-0x15 Version-1 tuple whose
 * payload is a 2-byte version followed by NUL-separated `key=value` strings.
 *
 * The separate "publishHWIdentifiers: media error" (kIOReturnBadMedia) failure,
 * which used to come and go across boots of an identical binary, was never an
 * OTP-content problem at all: on those boots the driver reached this region
 * through BAR0's *core-2* window, which the model did not implement, so every
 * read returned zero and the tuple's CRC could not check out.
 */

/*
 * BAR0 carries the backplane windows; BAR2 is the PCIe core register window.
 * BAR0 is 32 KiB on real parts (brcmfmac's BRCMF_PCIE_REG_MAP_SIZE), which is
 * what makes room for the core-2 window at +0x4000. Declaring it any smaller
 * moves BAR2 on top of that window — see the comment on the window registers.
 */
#define APPLE_WLAN_DEVICE_BAR0_SIZE (32 * KiB)
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
     * BAR2 and the parts of BAR0 outside the two windows are flat RAM, so that
     * write-then-verify sequences behave sanely instead of reading back zero.
     * bar0_regs is indexed by raw BAR0 offset — the windowed ranges are simply
     * never routed here, which is cheaper than tracking the holes.
     */
    uint8_t bar0_regs[APPLE_WLAN_DEVICE_BAR0_SIZE];
    uint8_t bar2_backing[APPLE_WLAN_DEVICE_BAR2_SIZE];

    // Backplane addresses currently mapped by each of BAR0's two windows, set by
    // the guest through PCI config space. Tracking them turns the otherwise
    // opaque BAR0 offsets into real backplane addresses.
    uint32_t bar0_window;
    uint32_t bar0_core2_window;

    /*
     * Backplane storage, sparse and keyed by *backplane* address rather than by
     * BAR offset. A flat BAR-indexed buffer is wrong here: the low 4 KiB of BAR0
     * is a moveable window, so every window base would alias onto the same
     * bytes, and the megabyte-scale firmware image the driver pushes through it
     * would collapse into 4 KiB of garbage.
     */
    GHashTable *backplane;
    uint64_t written_bytes;
    uint32_t written_hash; // FNV-1a over everything written, for image identity

    // ChipCommon indirect SROM interface state and its backing provisioning blob.
    uint32_t cc_srom_control;
    uint32_t cc_srom_address;
    uint8_t srom[APPLE_WLAN_SROM_BYTES];
    uint8_t otp[APPLE_WLAN_OTP_SIZE];
};

// The identity strings the guest image expects: its firmware directory is
// C-4378__s-B1 and the NVRAM file there is P-moana_M-GODF_V-m__m-4.3.txt, i.e.
// platform "moana", module "GODF", vendor "m". These are model identity, not any
// real device's calibration or MAC.
/*
 * Version-1 tuple payload, per AppleBCMWLANBusInterfacePCIe::parseVersion1Tuple()
 * as decompiled:
 *
 *   v13 = len - 3;                            // effective payload length
 *   if (v13 == 0) { all four slots = "" }
 *   do {
 *       b = data[off + 2];                    // strings start at data + 2
 *       if (idx > 3 || b == 0xFF) break;
 *       off += strlcpy(buf, data + off + 2, 256, 256) + 1;
 *       slot[idx++] = OSString::withCString(buf);
 *   } while (off < v13);
 *   if (idx <= 3) fill the remaining slots with "";
 *
 * so: two header bytes, then NUL-separated strings, and the slot order is
 * this->+0xD8, +0xE0, +0xC8, +0xD0 (not address order). Unfilled slots get
 * empty strings, which is why all four pointers are non-NULL yet publishing
 * still failed: publishHWIdentifiers() feeds each string to
 * AppleBCMWLANUtil::appendParsedKeyValuePairsToDictionary(), so the strings
 * have to be "key=value" text. The guest image names the keys for us --
 * its NVRAM file is P-moana_M-GODF_V-m__m-4.3.txt, i.e. P=platform,
 * M=module, V=vendor. Model identity only; no calibration or MAC.
 */
static const char apple_wlan_otp_identity[] =
    "\x01\x00" "P=moana\0M=GODF\0V=m\0";

static void apple_wlan_build_otp(AppleWLANDeviceState *s, const uint8_t *table)
{
    uint8_t crc = APPLE_WLAN_CRC8_INIT;
    uint32_t i, n = 0;

    memset(s->otp, APPLE_WLAN_CIS_TYPE_NULL, sizeof(s->otp));
    s->otp[n++] = APPLE_WLAN_CIS_TYPE_VERS1;
    s->otp[n++] = (uint8_t)(sizeof(apple_wlan_otp_identity) - 1);
    memcpy(&s->otp[n], apple_wlan_otp_identity,
           sizeof(apple_wlan_otp_identity) - 1);
    n += sizeof(apple_wlan_otp_identity) - 1;
    s->otp[n++] = APPLE_WLAN_CIS_TYPE_END;
    g_assert_cmpuint(n, <, sizeof(s->otp));

    // Trailing byte chosen so getcrc8(otp, sizeof(otp), 0xFF) == CRC8_GOOD_VALUE.
    for (i = 0; i < sizeof(s->otp) - 1; i++) {
        crc = table[(s->otp[i] ^ crc) & 0xFF];
    }
    for (i = 0; i < 256; i++) {
        if (table[(i ^ crc) & 0xFF] == APPLE_WLAN_CRC8_GOOD) {
            s->otp[sizeof(s->otp) - 1] = (uint8_t)i;
            return;
        }
    }
    g_assert_not_reached();
}

static void apple_wlan_build_provisioning(AppleWLANDeviceState *s)
{
    uint8_t table[256];
    uint8_t crc = APPLE_WLAN_CRC8_INIT;
    uint32_t i;

    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        uint32_t bit;

        for (bit = 0; bit < 8; bit++) {
            c = (c >> 1) ^ ((c & 1) ? APPLE_WLAN_CRC8_POLY : 0);
        }
        table[i] = (uint8_t)c;
    }

    // Body stays zeroed; solve the last byte so getcrc8() yields CRC8_GOOD_VALUE.
    for (i = 0; i < sizeof(s->srom) - 1; i++) {
        crc = table[(s->srom[i] ^ crc) & 0xFF];
    }
    for (i = 0; i < 256; i++) {
        if (table[(i ^ crc) & 0xFF] == APPLE_WLAN_CRC8_GOOD) {
            s->srom[sizeof(s->srom) - 1] = (uint8_t)i;
            break;
        }
    }
    g_assert_cmpuint(i, <, 256);

    apple_wlan_build_otp(s, table);
}

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

#define APPLE_WLAN_BACKPLANE_PAGE_SIZE (4 * KiB)
#define APPLE_WLAN_FNV_OFFSET (2166136261u)
#define APPLE_WLAN_FNV_PRIME (16777619u)
// Log download progress this often, so a multi-MiB push is visible but not spammy.
#define APPLE_WLAN_DL_LOG_STRIDE (256 * KiB)

static uint8_t *apple_wlan_backplane_page(AppleWLANDeviceState *s, uint32_t addr,
                                          bool allocate)
{
    uint32_t base = addr & ~(APPLE_WLAN_BACKPLANE_PAGE_SIZE - 1);
    uint8_t *page = g_hash_table_lookup(s->backplane, GUINT_TO_POINTER(base));

    if (page == NULL && allocate) {
        page = g_malloc0(APPLE_WLAN_BACKPLANE_PAGE_SIZE);
        g_hash_table_insert(s->backplane, GUINT_TO_POINTER(base), page);
    }
    return page;
}

/*
 * Resolve a BAR0 offset to a backplane address, through whichever of the two
 * windows covers it. Returns false for the flat register parts of BAR0.
 */
static bool apple_wlan_bar0_to_backplane(AppleWLANDeviceState *s, hwaddr offset,
                                         uint32_t *backplane)
{
    if (offset < APPLE_WLAN_BAR0_WINDOW_SIZE) {
        *backplane = s->bar0_window + (uint32_t)offset;
        return true;
    }
    if (offset >= APPLE_WLAN_BAR0_CORE2_WINDOW_OFFSET &&
        offset < APPLE_WLAN_BAR0_CORE2_WINDOW_OFFSET +
                     APPLE_WLAN_BAR0_WINDOW_SIZE) {
        *backplane = s->bar0_core2_window +
                     (uint32_t)(offset - APPLE_WLAN_BAR0_CORE2_WINDOW_OFFSET);
        return true;
    }
    return false;
}

static uint64_t apple_wlan_bar0_ops_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    AppleWLANDeviceState *s = opaque;
    uint64_t value = 0;
    uint32_t backplane = 0;
    bool windowed = apple_wlan_bar0_to_backplane(s, offset, &backplane);

    if (windowed &&
        backplane >= APPLE_WLAN_OTP_BASE + APPLE_WLAN_OTP_CIS_OFFSET &&
        backplane < APPLE_WLAN_OTP_BASE + APPLE_WLAN_OTP_CIS_OFFSET +
                        APPLE_WLAN_OTP_SIZE) {
        uint32_t off =
            backplane - (APPLE_WLAN_OTP_BASE + APPLE_WLAN_OTP_CIS_OFFSET);

        for (unsigned i = 0; i < size && off + i < sizeof(s->otp); i++) {
            value |= (uint64_t)s->otp[off + i] << (i * 8);
        }
        trace_apple_wlan_otp_read(off, size, value);
    } else if (windowed && backplane == APPLE_WLAN_CHIPCOMMON_BASE +
                                     APPLE_WLAN_CC_CAPABILITIES) {
        value = APPLE_WLAN_CC_CAP_SPROM_PRESENT;
    } else if (windowed && backplane == APPLE_WLAN_CHIPCOMMON_BASE +
                                            APPLE_WLAN_CC_SROM_CONTROL) {
        // Operations complete immediately, so never report BUSY.
        value = s->cc_srom_control & ~APPLE_WLAN_CC_SROM_CONTROL_BUSY;
    } else if (windowed && backplane == APPLE_WLAN_CHIPCOMMON_BASE +
                                            APPLE_WLAN_CC_SROM_DATA) {
        uint32_t off = s->cc_srom_address;

        value = 0;
        for (unsigned i = 0; i < size; i++) {
            if (off + i < sizeof(s->srom)) {
                value |= (uint64_t)s->srom[off + i] << (i * 8);
            }
        }
        trace_apple_wlan_srom_read(off, size, value);
    } else if (windowed &&
        backplane == APPLE_WLAN_CHIPCOMMON_BASE + APPLE_WLAN_CHIPID_OFFSET) {
        value = APPLE_WLAN_CHIP_ID | (APPLE_WLAN_CHIP_REV << 16) |
                (APPLE_WLAN_CHIP_PKG << 20) | (APPLE_WLAN_CHIP_TYPE << 28);
    } else if (windowed) {
        uint8_t *page = apple_wlan_backplane_page(s, backplane, false);

        if (page != NULL) {
            memcpy(&value, page + (backplane & (APPLE_WLAN_BACKPLANE_PAGE_SIZE - 1)),
                   size);
        }
    } else {
        if (offset + size <= sizeof(s->bar0_regs)) {
            memcpy(&value, s->bar0_regs + offset, size);
        }
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
    uint32_t backplane = 0;

    if (apple_wlan_bar0_to_backplane(s, offset, &backplane)) {
        if (backplane == APPLE_WLAN_CHIPCOMMON_BASE +
                             APPLE_WLAN_CC_SROM_CONTROL) {
            s->cc_srom_control = (uint32_t)value;
        } else if (backplane == APPLE_WLAN_CHIPCOMMON_BASE +
                                    APPLE_WLAN_CC_SROM_ADDRESS) {
            s->cc_srom_address = (uint32_t)value;
        }
        uint8_t *page = apple_wlan_backplane_page(s, backplane, true);
        uint64_t before = s->written_bytes;

        memcpy(page + (backplane & (APPLE_WLAN_BACKPLANE_PAGE_SIZE - 1)), &value,
               size);
        for (unsigned i = 0; i < size; i++) {
            s->written_hash ^= (uint8_t)(value >> (i * 8));
            s->written_hash *= APPLE_WLAN_FNV_PRIME;
        }
        s->written_bytes += size;
        if (before / APPLE_WLAN_DL_LOG_STRIDE !=
            s->written_bytes / APPLE_WLAN_DL_LOG_STRIDE) {
            trace_apple_wlan_backplane_written(
                s->written_bytes, g_hash_table_size(s->backplane),
                s->written_hash);
        }
        trace_apple_wlan_backplane_write(backplane, offset, size, value);
    } else {
        if (offset + size <= sizeof(s->bar0_regs)) {
            memcpy(s->bar0_regs + offset, &value, size);
        }
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

    s->backplane = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                        g_free);
    s->written_hash = APPLE_WLAN_FNV_OFFSET;
    apple_wlan_build_provisioning(s);

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
    AppleWLANDeviceState *s = APPLE_WLAN_DEVICE(dev);

    pci_set_word(dev->config + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    s->bar0_window = 0;
    s->bar0_core2_window = APPLE_WLAN_BAR0_CORE2_WINDOW_RESET;
}

/*
 * Track both BAR0 window registers so BAR0 accesses can be resolved to backplane
 * addresses. The core-2 window register overlaps the PCI Express capability, so
 * pci_default_write_config() treats it as read-only and drops the value; capture
 * it here regardless, since the guest's window writes are what give the BAR0
 * offsets meaning.
 */
static void apple_wlan_device_config_write(PCIDevice *dev, uint32_t addr,
                                           uint32_t val, int len)
{
    AppleWLANDeviceState *s = APPLE_WLAN_DEVICE(dev);

    pci_default_write_config(dev, addr, val, len);

    if (len == 4) {
        switch (addr) {
        case APPLE_WLAN_BAR0_WINDOW_CFG:
            s->bar0_window = val;
            trace_apple_wlan_bar0_window(val);
            break;
        case APPLE_WLAN_BAR0_CORE2_WINDOW_CFG:
            s->bar0_core2_window = val;
            trace_apple_wlan_bar0_core2_window(val);
            break;
        default:
            break;
        }
    }
}

static void apple_wlan_device_pci_uninit(PCIDevice *dev)
{
    AppleWLANDeviceState *s = APPLE_WLAN_DEVICE(dev);

    g_clear_pointer(&s->backplane, g_hash_table_unref);
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
