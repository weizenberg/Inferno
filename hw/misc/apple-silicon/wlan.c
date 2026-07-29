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
 * On Broadcom PCIe parts BAR0 is a set of 4 KiB pages, and several of those pages
 * are independently moveable windows onto the chip's backplane. Each such page
 * has its own base register in PCI config space. Counted over one boot of the
 * iOS 14 driver, four are used:
 *
 *   BAR0 + 0x0000  <-  config 0x80   (332 writes)   "BAR0_WINDOW"
 *   BAR0 + 0x1000  <-  config 0x70   ( 60 writes)
 *   BAR0 + 0x4000  <-  config 0x74   ( 93 writes)   the "core 2" window
 *   BAR0 + 0x5000  <-  config 0x78   ( 61 writes)
 *
 * All of them must be modelled. The driver keeps track of which core each window
 * currently maps and reaches for whichever one is not already pointing at a core
 * it still needs, so which window a given access arrives through varies from boot
 * to boot for identical driver code. A window that is not modelled silently loses
 * every access made through it: the access either lands beyond the end of a
 * too-small BAR0 and is absorbed by whatever the guest mapped next, or falls
 * through to the flat register array, and in both cases reads back as zero. That
 * is what made the OTP read — and hence the whole hardware-identity publish —
 * fail on some boots and succeed on others with no change in the binary.
 *
 * Pages 2 and 3 also see traffic (424 and 60 accesses) but no config write was
 * observed selecting a base for them, so they are presumed to be fixed apertures
 * rather than windows — the `bar0 + 0x21e8` write looks like a PCIe2 core
 * register block. They are left falling through to bar0_regs until identified.
 *
 * The chip identity is the ChipCommon `chipid` register at backplane 0x18000000
 * offset 0, encoded as id | rev << 16 | package << 20 | type << 28.
 */
#define APPLE_WLAN_BAR0_WINDOW_SIZE (4 * KiB)
#define APPLE_WLAN_BAR0_PAGES (APPLE_WLAN_DEVICE_BAR0_SIZE / APPLE_WLAN_BAR0_WINDOW_SIZE)
/*
 * Broadcom's own config-space registers, 0x70..0xa7: the four window bases
 * (0x70, 0x74, 0x78, 0x80) plus 0x88, 0xa0 and 0xa4. The PCI Express capability
 * is pushed clear of them; a version-2 capability is 0x3c bytes, so 0xc0 keeps it
 * below the 0x100 start of extended config space where AER lives.
 */
#define APPLE_WLAN_BCM_CFG_BASE (0x70)
#define APPLE_WLAN_BCM_CFG_SIZE (0x38)
#define APPLE_WLAN_EXP_CAP_OFFSET (0xC0)
// The reset value the driver expects to find in the core-2 window register.
#define APPLE_WLAN_BAR0_CORE2_WINDOW_RESET (0x18002000)

// PCI config register -> which 4 KiB page of BAR0 its base applies to.
static const struct {
    uint32_t cfg;
    uint32_t page;
} apple_wlan_bar0_windows[] = {
    { 0x80, 0 },
    { 0x70, 1 },
    { 0x74, 4 },
    { 0x78, 5 },
};
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
 * BAR0 carries the backplane windows; BAR2 is the chip's TCM (tightly-coupled
 * memory) aperture, which is where the firmware image is actually written.
 * BAR0 is 32 KiB on real parts (brcmfmac's BRCMF_PCIE_REG_MAP_SIZE), which is
 * what makes room for the core-2 window at +0x4000. Declaring it any smaller
 * moves BAR2 on top of that window — see the comment on the window registers.
 *
 * BAR2 has to be big enough for the image the driver lays out. With the real
 * n104 firmware it reports:
 *
 *   loadImage: Chip RAM: 0 [ 0< firmware{1317617} >1317617
 *       | 1317624 ~ 1317624< free{565456} >1883080
 *       | 1883344< nvram{8998} >1892342 | ... ] 1892352
 *
 * i.e. 1,892,352 bytes of content. Sizing the BAR to just fit that (2 MiB) is
 * NOT enough: the driver maps the whole TCM aperture up front, and if the map is
 * larger than the BAR it fails, leaving a null base. AppleBCMWLANChipMemory::write
 * then stores straight through it —
 *
 *     v17 = (_QWORD *)(a1 + 24LL * a2 + 48);        // region descriptor
 *     *(_QWORD *)(*v17 + 8 * v20) = ...;            // no null check
 *
 * — so the guest takes a kernel data abort on the first 64-bit store. That is
 * what a too-small BAR2 looks like from outside: a panic in ChipMemory::write,
 * not a short write.
 *
 * The size is set by where in the aperture the driver actually writes, not by the
 * size of the content. Measured, the firmware write region begins at
 * bar2+0x351fdc (3.48 MiB), so with the 1,892,352-byte layout on top it reaches
 * 0x51ffdc, i.e. 5.12 MiB. 2 MiB paniced on the first store and 4 MiB paniced
 * after 524,288 bytes with the last write at bar2+0x3d1fdc, both at this same
 * instruction. 8 MiB is the next power of two that contains it.
 */
#define APPLE_WLAN_DEVICE_BAR0_SIZE (32 * KiB)
#define APPLE_WLAN_DEVICE_BAR2_SIZE (8 * MiB)
// One trace line per this many bytes written to the TCM, instead of per access.
#define APPLE_WLAN_TCM_LOG_STRIDE (256 * KiB)
/*
 * AI (AXI interconnect) wrapper registers, at the wrapper's 4 KiB page. Wrapper
 * space begins at 0x18100000 — every core the driver visits is below that.
 */
#define APPLE_WLAN_WRAPPER_BASE (0x18100000)
#define APPLE_WLAN_AI_IOCTRL (0x408)
#define APPLE_WLAN_AI_RESETCTRL (0x800)
#define APPLE_WLAN_AI_RESETSTATUS (0x804)
/*
 * The word at the end of chip RAM is not a liveness flag: createFirmwarePCIeIPC()
 * reads it as the *backplane address of the firmware's shared-memory structure*
 * and validates it against the chip RAM window --
 *
 *     v114 = ramBase; v115 = addr - v114; v116 = ramSize;
 *     if (addr == -1)                       -> "Failed to read shared memory address"
 *     if (addr < v114 || v114 + v116 <= addr
 *         || v116 <= v115 || v115 + 120 > v116
 *         || (v115 & 3) != 0)               -> bad address / "Failed to map shared memory"
 *
 * so it must sit inside RAM, be 4-byte aligned, and leave at least 120 bytes. A
 * plain 0 is below ramBase, which is what produced "BCMWLAN FW provide bad
 * address".
 *
 * For this aperture a BAR2 offset *is* the chip address: the chip table gives
 * ramBase 0x352000 / ramSize 0x1ce000, and the marker word (ramBase + ramSize - 4)
 * was observed at bar2+0x51fffc = 0x352000 + 0x1cdffc. So the address can be
 * derived from where the driver polls rather than by hardcoding that constant.
 *
 * Place the structure 16 KiB below the marker. The driver's own layout puts its
 * free region at 1317624..1883080 and NVRAM at 1883344..1892342 with the marker at
 * 1892348, so 16 KiB back lands in free space and clear of NVRAM.
 */
#define APPLE_WLAN_FW_SHARED_BACKOFF (16 * KiB)
/*
 * First word of that structure is `flags`, whose low byte is the protocol version
 * (brcmfmac's BRCMF_PCIE_SHARED_VERSION_MASK). The driver checks it immediately:
 *
 *   createFirmwarePCIeIPC@7045: Host requires version 7, firmware supports 0
 */
#define APPLE_WLAN_FW_SHARED_VERSION (7)
/*
 * Device-wake capability bits in the same word. createFirmwarePCIeIPC() reads
 *
 *     if ((flags & 0x20000000) != 0 && (flags & 0x40000000) == 0)
 *         -> "device does not support oob or inband device wake, bailing"
 *     log OOB as (flags & 0x20000000) ? "No" : "Has"
 *     log DS  as (flags & 0x40000000) ? "has" : "no"
 *     if ((flags & 0x40000000) == 0) -> needs a device-wake GPIO
 *
 * so 0x20000000 means *no* out-of-band device wake and 0x40000000 means inband
 * deep sleep is supported. Leaving both clear advertises OOB-only, and the driver
 * then bails with "device wake GPIO not available, and inband device wake not
 * supported by endpoint" because this platform has no such GPIO. Declaring no-OOB
 * plus inband takes the path that needs no GPIO.
 */
#define APPLE_WLAN_FW_SHARED_NO_OOB_DW (0x20000000)
#define APPLE_WLAN_FW_SHARED_INBAND_DS (0x40000000)
#define APPLE_WLAN_FW_SHARED_FLAGS                                   \
    (APPLE_WLAN_FW_SHARED_VERSION | APPLE_WLAN_FW_SHARED_NO_OOB_DW | \
     APPLE_WLAN_FW_SHARED_INBAND_DS)

struct AppleWLANDeviceState {
    PCIDevice parent_obj;

    AppleWLANState *root;
    ApplePCIEPort *port;
    MemoryRegion *dma_mr;
    AddressSpace *dma_as;

    MemoryRegion bar0;
    MemoryRegion bar2;

    /*
     * BAR2 and the parts of BAR0 outside the windows are flat RAM, so that
     * write-then-verify sequences behave sanely instead of reading back zero.
     * bar0_regs is indexed by raw BAR0 offset — the windowed ranges are simply
     * never routed here, which is cheaper than tracking the holes. bar2_backing
     * is the TCM and is heap-allocated rather than inline, being megabytes.
     */
    uint8_t bar0_regs[APPLE_WLAN_DEVICE_BAR0_SIZE];
    uint8_t *bar2_backing;
    uint64_t tcm_written_bytes;
    // See apple_wlan_bar2_ops_read(): identifying the firmware-alive marker from
    // the driver's own poll of it, rather than guessing its address.
    bool arm_core_released;
    bool tcm_marker_done;
    uint32_t tcm_marker_offset;
    // Where the stand-in told the driver its shared-memory structure lives.
    uint32_t fw_shared_offset;
    uint32_t tcm_last_read_offset;
    bool tcm_last_read_valid;

    // Backplane addresses currently mapped by each of BAR0's two windows, set by
    // the guest through PCI config space. Tracking them turns the otherwise
    // opaque BAR0 offsets into real backplane addresses.
    uint32_t bar0_window_base[APPLE_WLAN_BAR0_PAGES];
    bool bar0_page_windowed[APPLE_WLAN_BAR0_PAGES];

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
 *
 * Two further rules, from AppleBCMWLANCore::{generateFileName,copyKeys}, decide
 * how these strings become firmware paths:
 *
 *   - A slot string holds *several* pairs separated by spaces; copyKeys() breaks
 *     on 0x20. So one slot carries a whole group, not a single key.
 *   - copyKeys() selects by the *case* of the key letter: it is called twice,
 *     first taking only UPPERCASE keys and then only lowercase ones, and the two
 *     results are joined with a literal "__". That is what produces Apple's
 *     `C-4378__s-B1` (uppercase group `C=4378`, lowercase group `s=B1`) and
 *     `P-moana_M-GODF_V-m__m-4.3` (uppercase `P`/`M`/`V`, lowercase `m`).
 *
 * Which slot is which is not guesswork -- getModuleInfo() publishes them by
 * fixed offset:
 *
 *     setObject("ChipInfo",   *((_QWORD *)this + 25));   // 25*8 = 0xC8
 *     setObject("ModuleInfo", *((_QWORD *)this + 26));   // 26*8 = 0xD0
 *
 * and against the slot order above that makes ChipInfo the *third* string and
 * ModuleInfo the *fourth*. Confirmed: a tuple of "P=moana", "M=GODF", "V=m"
 * put "V=m" third and generated the directory `C-4378_V-m__`, and moving `s=B1`
 * into the third slot changed it to `C-4378__s-B1`, matching the image.
 *
 * Every slot is also parsed into the shared key/value dictionary, so a key may
 * be repeated across slots and stay available to publishHWIdentifiers().
 *
 * ModuleInfo (the fourth slot) supplies the NVRAM leaf, as copyKeys() over it:
 * uppercase group, "__", lowercase group. But it must **not** contain a `P=`
 * pair, because generateFileName() gates on exactly that:
 *
 *     if ( strnstr(ModuleInfo, "P=", 0xFF) != nullptr )
 *         goto LABEL_32;      // skips appending the module-instance
 *
 * and the skipped block is what appends the platform name to the *firmware*
 * buffer (this+3192). Put `P=` in ModuleInfo and the NVRAM name comes out right
 * while FW/CLM/Tx Cap lose their leaf entirely (`.trx`, `.clmb`, `.txcb`).
 * So the platform belongs in the module-instance property and only the module,
 * vendor and revision keys belong here.
 */
static const char apple_wlan_otp_identity[] =
    "\x01\x00" "P=moana\0M=GODF\0s=B1\0M=GODF V=m m=4.3\0";

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
    uint32_t page = (uint32_t)(offset / APPLE_WLAN_BAR0_WINDOW_SIZE);

    if (page < APPLE_WLAN_BAR0_PAGES && s->bar0_page_windowed[page]) {
        *backplane = s->bar0_window_base[page] +
                     (uint32_t)(offset & (APPLE_WLAN_BAR0_WINDOW_SIZE - 1));
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

/*
 * The chip's ARM core is taken out of reset by clearing bit 0 of the AI wrapper's
 * RESETCTRL. There is no core here to run the downloaded firmware, so note the
 * release and let the read path stand in for the one thing the driver checks to
 * decide whether it booted — see apple_wlan_bar2_ops_read().
 *
 * This is where the model stops being a passive device and starts impersonating
 * running firmware. Everything past it is protocol, not plumbing.
 */
static void apple_wlan_release_arm_core(AppleWLANDeviceState *s)
{
    if (s->tcm_written_bytes == 0 || s->arm_core_released) {
        return;
    }
    s->arm_core_released = true;
    s->tcm_last_read_valid = false;
    trace_apple_wlan_arm_core_released(s->tcm_written_bytes);
}

static void apple_wlan_bar0_ops_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    AppleWLANDeviceState *s = opaque;
    uint32_t backplane = 0;

    if (apple_wlan_bar0_to_backplane(s, offset, &backplane)) {
        /*
         * Wrapper space starts at 0x18100000; the cores the driver visits all sit
         * below it. A RESETCTRL deassert there is the core coming up.
         */
        if (backplane >= APPLE_WLAN_WRAPPER_BASE &&
            (backplane & (APPLE_WLAN_BACKPLANE_PAGE_SIZE - 1)) ==
                APPLE_WLAN_AI_RESETCTRL &&
            (value & 1) == 0) {
            apple_wlan_release_arm_core(s);
        }
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

    if (offset + size > APPLE_WLAN_DEVICE_BAR2_SIZE) {
        return 0;
    }
    /*
     * Stand in for firmware having booted. Once the ARM core has been released,
     * the driver polls the last word of chip RAM every 10 ms for ~4.8 s and
     * requires it to become something that is neither the signature it wrote there
     * nor 0xFFFFFFFF; otherwise it logs "last 4 bytes in WiFi chip RAM does not
     * change after init!" and fails with "Chip Init failure".
     *
     * The marker's address is not guessed: the poll identifies it. A repeated
     * 32-bit read of one offset after the core came up is that poll, so the second
     * such read is answered as a live firmware would have, once.
     */
    if (s->arm_core_released && !s->tcm_marker_done && size == 4) {
        if (s->tcm_last_read_valid && s->tcm_last_read_offset == offset &&
            offset > APPLE_WLAN_FW_SHARED_BACKOFF) {
            uint32_t shared = (uint32_t)offset - APPLE_WLAN_FW_SHARED_BACKOFF;

            stl_le_p(s->bar2_backing + offset, shared);
            s->tcm_marker_done = true;
            s->tcm_marker_offset = (uint32_t)offset;
            s->fw_shared_offset = shared;
            // Publish the structure the address points at, starting with `flags`.
            stl_le_p(s->bar2_backing + shared, APPLE_WLAN_FW_SHARED_FLAGS);
            trace_apple_wlan_fw_alive_marker((uint32_t)offset, shared,
                                             s->tcm_written_bytes);
        }
        s->tcm_last_read_offset = (uint32_t)offset;
        s->tcm_last_read_valid = true;
    }
    memcpy(&value, s->bar2_backing + offset, size);
    return value;
}

static void apple_wlan_bar2_ops_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    AppleWLANDeviceState *s = opaque;
    uint64_t before = s->tcm_written_bytes;

    if (offset + size > APPLE_WLAN_DEVICE_BAR2_SIZE) {
        return;
    }
    memcpy(s->bar2_backing + offset, &value, size);
    s->tcm_written_bytes += size;
    /*
     * The driver retries the whole bring-up on failure, rewriting the signature
     * and polling again, so the stand-in has to re-arm. A write covering the word
     * it last answered means a fresh attempt is under way.
     *
     * Forget the last-read offset at the same time. Two reads of this word want
     * opposite answers: loadImage() verifies it still holds the host's signature
     * ("NVRAM Location Mismatch @ %u: host 0x%X, chip 0x%X" if not), and only the
     * later poll in loadChipImage() wants it changed. Carrying a stale offset over
     * from the previous pass made the very first verify read look like a repeat and
     * answered it with the marker.
     */
    if (s->tcm_marker_done && offset <= s->tcm_marker_offset &&
        s->tcm_marker_offset < offset + size) {
        s->tcm_marker_done = false;
        s->tcm_last_read_valid = false;
    }
    /*
     * The firmware is over a megabyte, so trace progress rather than every
     * access — a per-access event here buries everything else in the log.
     */
    if (before / APPLE_WLAN_TCM_LOG_STRIDE !=
        s->tcm_written_bytes / APPLE_WLAN_TCM_LOG_STRIDE) {
        trace_apple_wlan_tcm_written(s->tcm_written_bytes, offset);
    }
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

    s->bar2_backing = g_malloc0(APPLE_WLAN_DEVICE_BAR2_SIZE);
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
    /*
     * The PCI Express capability must NOT be placed at 0x70. Broadcom's own
     * config registers live at 0x70, 0x74, 0x78, 0x80, 0x88, 0xa0 and 0xa4 --
     * the first four are the BAR0 backplane window bases -- and a version-2
     * express capability at 0x70 spans 0x70..0xab, swallowing all of them.
     *
     * That matters because the driver reads its window bases back:
     * AppleBCMWLANChipBackplane::validateWindow() does
     *
     *     readReg32(configSpace, window->cfg_offset, &v) == 0 && v == expected
     *
     * and validateCores()/validateWrappers() call it for every mapped core, so a
     * window register that does not read back what was written fails the whole
     * bring-up -- which is how loadChipImage() came to return kIOReturnIOError.
     * pci_default_write_config() honours wmask, and capability-owned bytes are
     * read-only or write-1-to-clear, so the bases were being dropped.
     */
    pcie_endpoint_cap_init(dev, APPLE_WLAN_EXP_CAP_OFFSET);
    msi_init(dev, 0x50, 1, true, false, &error_fatal);
    pci_pm_init(dev, 0x40, &error_fatal);

    // Make Broadcom's config registers plain read/write storage.
    for (uint32_t off = APPLE_WLAN_BCM_CFG_BASE;
         off < APPLE_WLAN_BCM_CFG_BASE + APPLE_WLAN_BCM_CFG_SIZE; off += 4) {
        pci_set_long(dev->wmask + off, 0xFFFFFFFFU);
    }

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
    memset(s->bar0_window_base, 0, sizeof(s->bar0_window_base));
    memset(s->bar0_page_windowed, 0, sizeof(s->bar0_page_windowed));
    for (size_t i = 0; i < ARRAY_SIZE(apple_wlan_bar0_windows); i++) {
        s->bar0_page_windowed[apple_wlan_bar0_windows[i].page] = true;
    }
    // Only the core-2 window has a non-zero base out of reset.
    s->bar0_window_base[4] = APPLE_WLAN_BAR0_CORE2_WINDOW_RESET;
}

/*
 * Track every BAR0 window base register so BAR0 accesses can be resolved to
 * backplane addresses. The value itself is stored by pci_default_write_config()
 * now that the Broadcom range is writable and the express capability has been
 * moved clear of it, which is what lets the driver read its own window bases back
 * (see validateWindow() in the realize path); this hook only mirrors them into
 * the resolver's array.
 */
static void apple_wlan_device_config_write(PCIDevice *dev, uint32_t addr,
                                           uint32_t val, int len)
{
    AppleWLANDeviceState *s = APPLE_WLAN_DEVICE(dev);

    pci_default_write_config(dev, addr, val, len);

    if (len == 4) {
        for (size_t i = 0; i < ARRAY_SIZE(apple_wlan_bar0_windows); i++) {
            if (addr != apple_wlan_bar0_windows[i].cfg) {
                continue;
            }
            s->bar0_window_base[apple_wlan_bar0_windows[i].page] = val;
            trace_apple_wlan_bar0_window(apple_wlan_bar0_windows[i].cfg,
                                         apple_wlan_bar0_windows[i].page, val);
            break;
        }
    }
}

static void apple_wlan_device_pci_uninit(PCIDevice *dev)
{
    AppleWLANDeviceState *s = APPLE_WLAN_DEVICE(dev);

    g_clear_pointer(&s->backplane, g_hash_table_unref);
    g_clear_pointer(&s->bar2_backing, g_free);
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
