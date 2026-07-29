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
/*
 * PCIe2 core registers. BAR0 page 2 is a fixed aperture onto them, so these are
 * raw BAR0 offsets. Names follow brcmfmac's BRCMF_PCIE_64_PCIE2REG_*.
 *
 * MAILBOXINT is status and is cleared by writing ones, so it must not be plain
 * storage: the driver writes 0xffffffff to clear and would read 0xffffffff back,
 * i.e. see every D2H doorbell and mailbox event asserted at once. Measured, the
 * driver unmasks 0x10100 -- D2H doorbell 0 (0x10000) and FN0 mailbox data
 * (0x0100) -- so those are the two bits the device has to be able to raise.
 */
#define APPLE_WLAN_PCIE2_MAILBOXINT (0x2c30)
#define APPLE_WLAN_PCIE2_MAILBOXMASK (0x2c34)
#define APPLE_WLAN_MB_INT_D2H_DB0 (0x10000)
#define APPLE_WLAN_MB_INT_FN0_0 (0x0100)
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
/*
 * How much of the structure to trace reads within, for layout discovery. Wide
 * enough to cover the pointer chain the driver walks: the structure itself, then
 * ring_info at +0x100, then the ring memory ring_info points at.
 */
#define APPLE_WLAN_FW_SHARED_WINDOW (4 * KiB)
/*
 * Field offsets within the structure, established by tracing which words the
 * driver actually reads. Over a whole attempt it touches exactly three:
 *
 *   +0x00  flags          (version + device-wake bits)
 *   +0x30  ring_info      read once, immediately before the failure
 *   +0x50  capabilities   the deviceShared+80 word: 0x4 BT streaming log,
 *                         0x8 core dump, 0x100 extended TX status, and
 *                         bits 0xC0 select the control ring item counts
 *
 * +0x30 is the ring-info pointer: it is validated the same way as the shared
 * address itself, and the check demands at least 80 bytes of room --
 *
 *     if (ramSize <= off || off + 80 > ramSize || (off & 3) != 0)
 *         -> "Failed to map common ring memory @ %#X"
 *
 * Reading 0 there puts it below ramBase, which is the second source of
 * "BCMWLAN FW provide bad address". Point it just past the structure; the whole
 * 16 KiB backoff region sits in the driver's own free space.
 */
#define APPLE_WLAN_FW_SHARED_RING_INFO_OFF (0x30)
#define APPLE_WLAN_FW_RING_INFO_BACKOFF (0x100)
/*
 * ring_info's own first word is another address -- the ring memory holding the
 * ring descriptors -- and a zero there fails the same validation. Traced: the
 * driver reads ring_info+0x00 once and then fails. Give it a further slot in the
 * same free-space region.
 */
#define APPLE_WLAN_FW_RING_MEM_BACKOFF (0x200)
/*
 * The mailbox-data words, brcmfmac's BRCMF_SHARED_{HTOD,DTOH}_MB_DATA_ADDR.
 * These hold a TCM address each, and the FN0 bit in MAILBOXINT is what tells
 * the driver to go read the D2H one. Left at zero, as the firmware image
 * leaves them, that read lands at TCM offset 0.
 */
#define APPLE_WLAN_FW_SHARED_HTOD_MB_DATA_OFF (0x24)
#define APPLE_WLAN_FW_SHARED_DTOH_MB_DATA_OFF (0x28)
#define APPLE_WLAN_FW_DTOH_MB_DATA_BACKOFF (0x300)
#define APPLE_WLAN_FW_HTOD_MB_DATA_BACKOFF (0x304)
/*
 * ring_info's ring counts. The layout is pinned by what the driver itself writes
 * rather than by trusting the public struct: it fills four 64-bit host index
 * addresses at ring_info+0x14, +0x1c, +0x24 and +0x2c (v236[5..12] in
 * createFirmwarePCIeIPC), which runs to exactly 0x34 -- and brcmfmac's
 * ring_info_t has max_tx_flowrings at 0x34 and max_submission_queues at 0x36
 * immediately after those same four fields. Traced, the driver reads both as
 * 16-bit quantities and they are the LAST device accesses it ever makes:
 *
 *     fw shared+0x134 size 2 -> 0x0
 *     fw shared+0x136 size 2 -> 0x0
 *     (no further MMIO for the remaining ~250 s of a 300 s run)
 *
 * Both must be non-zero, and there is an ordering constraint between them that is
 * the opposite of what the arithmetic reads like. The driver asserts, and the
 * panic prints the *violated* condition:
 *
 *   panic(cpu 1 caller 0xfffffff0095cbfa4):
 *     "AppleBCMWLANBusInterfacePCIe::createFirmwarePCIeIPC(): "
 *     "maxNbrOfDynamicSubmissionRings %u <= maxNbrOfTxFlowRings %u"
 *
 * Measured across three boots, which also pins which field is which:
 *
 *   published +0x34 / +0x36     panic text
 *   0 / 0                       "0 <= 0"
 *   8 / 2                       "2 <= 8"      <- values consumed exactly
 *
 * so +0x34 is maxNbrOfTxFlowRings and +0x36 is maxNbrOfDynamicSubmissionRings,
 * and since "2 <= 8" is arithmetically true yet still panics, the requirement is
 * the strict inequality the other way round: the dynamic submission ring count
 * must be GREATER than the TX flowring count. Sensible in hindsight -- every TX
 * flowring is drawn from the dynamic submission ring pool, so the pool has to be
 * larger than the flowrings carved out of it.
 */
#define APPLE_WLAN_FW_RING_INFO_MAX_FLOWRINGS_OFF (0x34)
#define APPLE_WLAN_FW_RING_INFO_MAX_SUBMIT_OFF (0x36)
#define APPLE_WLAN_FW_RING_MAX_FLOWRINGS (8)
#define APPLE_WLAN_FW_RING_MAX_DYN_SUBMIT (16)

/*
 * msgbuf. The layout below is not guessed: it is what the driver itself wrote
 * into the TCM, observed through apple_wlan_fw_shared_write, and it agrees
 * field for field with brcmfmac's brcmf_pcie_ringinfo and its ring-mem
 * accessors. ring_info carries the host DMA addresses of four u16 index arrays
 * (one entry per ring); ringmem is an array of 0x10-byte descriptors giving
 * each ring's item count, item size and host base address.
 */
#define APPLE_WLAN_RING_INFO_H2D_W_IDX_OFF (0x14)
#define APPLE_WLAN_RING_INFO_H2D_R_IDX_OFF (0x1C)
#define APPLE_WLAN_RING_INFO_D2H_W_IDX_OFF (0x24)
#define APPLE_WLAN_RING_INFO_D2H_R_IDX_OFF (0x2C)
#define APPLE_WLAN_RING_DESC_SIZE (0x10)
#define APPLE_WLAN_RING_DESC_MAX_ITEM_OFF (0x04)
#define APPLE_WLAN_RING_DESC_ITEM_SIZE_OFF (0x06)
#define APPLE_WLAN_RING_DESC_BASE_OFF (0x08)
// Ring ids, in the order the driver populates ringmem.
#define APPLE_WLAN_RING_H2D_CONTROL_SUBMIT (0)
#define APPLE_WLAN_RING_D2H_CONTROL_COMPLETE (2)
/*
 * Five rings come from ringmem, and the driver creates more at runtime with
 * H2D_RING_CREATE / D2H_RING_CREATE. Those live past the common ones in this
 * id space; a dynamic ring carries its own slot number, which is the index it
 * uses in its direction's arrays -- id 2 from submitH2DRingCreateMsg really is
 * h2d_w_idx[2].
 */
#define APPLE_WLAN_RING_COMMON_COUNT (5)
#define APPLE_WLAN_RING_DYNAMIC_MAX (8)
#define APPLE_WLAN_RING_COUNT \
    (APPLE_WLAN_RING_COMMON_COUNT + APPLE_WLAN_RING_DYNAMIC_MAX)
/*
 * The two index arrays each cover only their own direction, and the driver
 * hands out slots sequentially as it walks the rings (brcmfmac does the same in
 * brcmf_pcie_init_ringbuffers). So a D2H ring's slot is its id minus the number
 * of H2D rings, not its id: ring 2's write index lives at d2h_w_idx[0], and
 * writing it to d2h_w_idx[2] lands in ring 4's slot instead -- the driver then
 * sees w_idx == r_idx for the control ring and reports
 * "Primary Interrupt Pending 0" however many interrupts it has taken.
 */
#define APPLE_WLAN_RING_H2D_COUNT (2)
// The H2D doorbell. brcmfmac calls this BRCMF_PCIE_64_PCIE2REG_H2D_MAILBOX_0.
#define APPLE_WLAN_PCIE2_H2D_MAILBOX_0 (0x2140)
#define APPLE_WLAN_PCIE2_H2D_MAILBOX_1 (0x2144)
// Longest submission item seen (control submit is 40 bytes).
#define APPLE_WLAN_RING_ITEM_MAX (64)

/*
 * msgbuf message types, Broadcom's names (brcmfmac's MSGBUF_TYPE_*). Every one
 * of these was observed being posted by the driver except the *_CMPLT replies,
 * which are what it is waiting for.
 */
#define APPLE_WLAN_MSGBUF_IOCTLPTR_REQ (0x09)
#define APPLE_WLAN_MSGBUF_IOCTLPTR_REQ_ACK (0x0A)
#define APPLE_WLAN_MSGBUF_IOCTLRESP_BUF_POST (0x0B)
#define APPLE_WLAN_MSGBUF_IOCTL_CMPLT (0x0C)
#define APPLE_WLAN_MSGBUF_EVENT_BUF_POST (0x0D)
#define APPLE_WLAN_MSGBUF_H2D_RING_CREATE (0x1B)
#define APPLE_WLAN_MSGBUF_D2H_RING_CREATE (0x1C)
#define APPLE_WLAN_MSGBUF_H2D_RING_CREATE_CMPLT (0x1D)
#define APPLE_WLAN_MSGBUF_D2H_RING_CREATE_CMPLT (0x1E)
// Control completions are 24 bytes, matching ring 2's item size.
#define APPLE_WLAN_MSGBUF_CMPLT_SIZE (24)

// Offsets within a submission/completion item, from the layouts above.
#define APPLE_WLAN_MSGBUF_REQUEST_ID_OFF (0x04)
#define APPLE_WLAN_MSGBUF_STATUS_OFF (0x08)
#define APPLE_WLAN_MSGBUF_RING_ID_OFF (0x0A)
/*
 * The phase bit in the common header flags. The driver walks a completion ring
 * checking this rather than trusting an index alone, so it has to flip every
 * time the ring wraps: it expects 1 on the first pass through a fresh ring,
 * then 0, and complains "Unexpected phaseBit ... got=N expect=M" otherwise.
 * It happened to be right for a while only because the requests being answered
 * carry flags 0x81, which already has this bit set.
 */
#define APPLE_WLAN_MSGBUF_PHASE_BIT (0x80)

#define APPLE_WLAN_BCME_OK (0)
#define APPLE_WLAN_BCME_UNSUPPORTED (-23)

#define APPLE_WLAN_WLC_GET_VAR (262)
#define APPLE_WLAN_WLC_SET_VAR (263)
/*
 * A plain WLC command rather than an iovar, so it arrives with no name at all.
 * This is what updateFWAPIVerFromHW reads; refusing it fails setupFirmware with
 * "Unable to get FW API version". 2 is the current ioctl interface version --
 * brcmfmac accepts only 1 or 2.
 */
#define APPLE_WLAN_WLC_GET_VERSION (1)
#define APPLE_WLAN_IOCTL_VERSION (2)

/*
 * WLC_GET_COUNTRY_LIST, what populateCountryList reads. Its layout is taken
 * from that function, which zeroes count before the call and then reads it
 * back:
 *
 *   v10 = *(_DWORD *)(v4 + 12);                       // count, clamped to 256
 *   ... = *(_DWORD *)(v4 + 16 + 4 * v11);             // 4-byte abbreviations
 *
 * so it is wl_country_list_t: buflen, band_set, band, count, then count
 * four-byte country abbreviations. A count of zero is tolerated there, but a
 * refusal is fatal -- the list has to exist even if it is short. One entry is
 * published as the regulatory domain to operate under; it is a configuration
 * default, not anything read off a device.
 */
#define APPLE_WLAN_WLC_GET_COUNTRY_LIST (261)
#define APPLE_WLAN_COUNTRY_LIST_COUNT_OFF (12)
#define APPLE_WLAN_COUNTRY_LIST_ENTRY_OFF (16)
#define APPLE_WLAN_COUNTRY_ABBREV_SIZE (4)
#define APPLE_WLAN_COUNTRY_DEFAULT "US"
/*
 * WLC_GET_COUNTRY. The driver asks for four bytes, not the twelve of a full
 * wl_country_t, so this is the abbreviation alone -- worth measuring rather than
 * assuming, since the struct would have been the obvious guess.
 */
#define APPLE_WLAN_WLC_GET_COUNTRY (83)
/*
 * The regulatory table version, read straight after the blob is loaded. Same
 * shape a real CLM reports, with a synthetic build stamp. Refusing this fails
 * the whole download from setupFirmware's point of view, even though the
 * transfer itself succeeded.
 */
#define APPLE_WLAN_CLM_VERSION_STRING                          \
    "API: 12.2 Data: 9.10.39 Compiler: 1.29.4 ClmImport: 1.36.3 " \
    "Creation: 2020-01-01 00:00:00"

/*
 * The chip capability list. processChipCaps compares this against a set of
 * optional feature tokens -- ap, he, rsdb, sc, hp2p, ecounters, nap, psbw,
 * txhist and friends -- and enables the matching feature for each one present.
 * None of them are listed here on purpose: there is no radio behind this, so
 * every gated feature should stay off. Refusing the iovar outright is what
 * fails, not the shortness of the answer.
 */
#define APPLE_WLAN_CHIP_CAPS_STRING "802.11d 802.11h"

/*
 * The channel list. handleGetChanSpecs reads a wl_uint32_list_t -- a count
 * followed by that many u32 entries, each stored back as a u16:
 *
 *   v10 = v5 + 1;                                   // entries follow the count
 *   *(_WORD *)(result + 7262 + 2 * v9) = v10[v9];
 *   *(_WORD *)(result + 7260) = v6;                 // count
 *
 * A chanspec is band | bandwidth | channel. Band 2G is zero and 0x1000 is the
 * 20 MHz bandwidth field (brcmfmac's BRCMU_CHSPEC_D11AC_BND_2G and
 * _BW_20), so channel N at 20 MHz is 0x1000 | N.
 *
 * Channels 1-11 are the 2.4 GHz set the "US" regulatory default allows, which
 * is what the country list reports. With no radio behind this, the list says
 * what the stand-in claims to support and nothing more -- but refusing it stops
 * getSupportedChannelsMatching, and the driver cannot scan without it.
 */
#define APPLE_WLAN_CHANSPEC_BW_20 (0x1000)
#define APPLE_WLAN_CHANNEL_FIRST (1)
#define APPLE_WLAN_CHANNEL_COUNT (11)
#define APPLE_WLAN_CHANSPEC_DEFAULT \
    (APPLE_WLAN_CHANSPEC_BW_20 | APPLE_WLAN_CHANNEL_FIRST)

// Likewise for the TX capability table; the driver only logs this one.
#define APPLE_WLAN_TXCAP_VERSION_STRING \
    "TxCap: 1.0 Creation: 2020-01-01 00:00:00"

/*
 * Blob downloads. Each is a set carrying the file in chunks, followed by a get
 * of a matching status. There is nothing here to load them into, so the
 * transfer is accepted and the status reported clean -- a zeroed status is
 * BCME_OK. Refusing either half fails setupFirmware outright ("Download clmb
 * failed", "Download txcap failed"), even though the transfer succeeded. The
 * status lengths are the out_len the driver asks for, as observed.
 */
static const struct {
    const char *load;
    const char *status;
    uint16_t status_len;
    const char *version_iovar;
    const char *version;
} apple_wlan_blob_loads[] = {
    { "clmload", "clmload_status", 20, "clmver",
      APPLE_WLAN_CLM_VERSION_STRING },
    { "txcapload", "txcapload_status", 24, "txcapver",
      APPLE_WLAN_TXCAP_VERSION_STRING },
};

/*
 * How many posted ioctl response buffers to remember. The driver posts one at a
 * time and reclaims it with each completion, so this only needs to absorb a
 * burst.
 */
#define APPLE_WLAN_IOCTL_RESP_BUFS (8)
/*
 * The MSI line apcie hands us is a level, and apcie never lowers it -- every
 * lowering call site in the tree is commented out, above a comment noting that
 * iOS storms if an interrupt stays asserted when it is not expected. It does:
 * leaving it up cost 869k acknowledgements of one vector in a single boot.
 *
 * A message-signalled interrupt is really an edge, so the line is held only
 * long enough to be seen. The AIC does not deliver on the edge; it polls
 * eir_state every kAICWT (64us), so the hold has to outlast a poll or the
 * interrupt is simply lost. If the guest still has not serviced the condition
 * once the line drops, it is re-asserted -- slowly. That is a deliberate
 * backstop rather than hardware behaviour: real firmware's driver clears
 * MAILBOXINT and the quiesce path below fires instead.
 */
#define APPLE_WLAN_INT_HOLD_NS (200 * 1000)
#define APPLE_WLAN_INT_RETRY_NS (10 * 1000 * 1000)

#define APPLE_WLAN_IOVAR_NAME_MAX (64)
#define APPLE_WLAN_IOVAR_PAYLOAD_MAX (512)

/*
 * wlc_ver is deliberately left refused. Answering it with brcmfmac's
 * brcmf_wlc_version_le layout made things worse, not better: the driver reads
 * its interface version out of that structure and reported 0, where refusing
 * the iovar leaves it at its own default of 3. The real layout is not known, and
 * a confidently wrong structure is harder to notice than a refusal.
 */
/*
 * Plain integer iovars. Each is read as a u32, and the driver sets some of them
 * straight back, so a set of any name listed here is accepted too. mpc is
 * Minimum Power Consumption, reported off -- nothing here sleeps.
 */
static const struct {
    const char *name;
    uint32_t value;
} apple_wlan_int_iovars[] = {
    { "event_log_max_sets", 8 },
    { "mpc", 0 },
    /*
     * Scan home-away time in milliseconds. initDefaultScanParametersFromChip
     * reads it into a plain int and only stores and logs it, but refusing it
     * fails setupDriver ("Failure to get default Home Away Time").
     */
    { "scan_home_away_time", 100 },
    // The channel currently sat on, read by getCHANNEL.
    { "chanspec", APPLE_WLAN_CHANSPEC_DEFAULT },
};

/*
 * The version banner an ioctl "ver" get answers with. This is the shape real
 * firmware emits, with a deliberately synthetic build stamp and FWID -- it
 * identifies the stand-in, not any real device.
 */
#define APPLE_WLAN_FW_VERSION_STRING \
    "wl0: Jan  1 2020 00:00:00 version 18.20.309.0.0.0.0 FWID 01-00000000"

#define APPLE_WLAN_FW_SHARED_NO_OOB_DW (0x20000000)
#define APPLE_WLAN_FW_SHARED_INBAND_DS (0x40000000)
/*
 * Ring indices are DMAed rather than read over the bus, and the driver supports
 * nothing else:
 *
 *     if ( (**((_DWORD **)this + 154) & 0x10000) == 0 ) {
 *         v11 = 3758097095LL;              // kIOReturnUnsupported
 *         logAlert("Driver only supports FW with bi-directional ring index DMA.");
 *     }
 *
 * (brcmfmac calls this bit BRCMF_PCIE_SHARED_DMA_INDEX.) Past it the driver fills
 * the host-side index array addresses into ring_info itself, so those need not be
 * published here.
 */
#define APPLE_WLAN_FW_SHARED_DMA_INDEX (0x10000)
#define APPLE_WLAN_FW_SHARED_FLAGS                                   \
    (APPLE_WLAN_FW_SHARED_VERSION | APPLE_WLAN_FW_SHARED_DMA_INDEX | \
     APPLE_WLAN_FW_SHARED_NO_OOB_DW | APPLE_WLAN_FW_SHARED_INBAND_DS)

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
    // PCIe2 core mailbox status (write-1-to-clear) and its enable mask.
    uint32_t mailbox_int;
    uint32_t mailbox_mask;
    // Our side of each ring's index pair, mirrored to the host arrays.
    uint16_t ring_r_idx[APPLE_WLAN_RING_COUNT];
    uint16_t ring_w_idx[APPLE_WLAN_RING_COUNT];
    /*
     * The host base each ring last had. The driver tears the rings down and
     * rebuilds them on retry, zeroing its indices; noticing the base change is
     * what stops a stale read index from walking the whole ring as garbage.
     */
    uint64_t ring_base_seen[APPLE_WLAN_RING_COUNT];
    // Current phase for each ring, flipped on wrap. See the phase bit above.
    bool ring_phase[APPLE_WLAN_RING_COUNT];
    // Rings the driver created at runtime, past the common ones.
    struct {
        uint64_t base;
        uint16_t max_item;
        uint16_t item_size;
        unsigned slot;
        bool h2d;
        bool valid;
    } dyn_ring[APPLE_WLAN_RING_DYNAMIC_MAX];
    // Host buffers posted for ioctl responses, consumed oldest first.
    struct {
        uint64_t addr;
        uint16_t len;
        /*
         * The id the buffer was posted under. A completion has to name the
         * buffer it wrote into, not the request it answers -- the driver looks
         * the response up by it and otherwise reports "Rx IO not found for
         * resourceID N, bad IPC message ID".
         */
        uint32_t request_id;
    } ioctl_resp_buf[APPLE_WLAN_IOCTL_RESP_BUFS];
    unsigned ioctl_resp_head;
    unsigned ioctl_resp_count;
    // Interrupt moderation; see APPLE_WLAN_INT_HOLD_NS.
    QEMUTimer *int_timer;
    bool int_asserted;

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
    } else if (offset == APPLE_WLAN_PCIE2_MAILBOXINT) {
        /*
         * Status, not storage. The driver clears it by writing ones, so echoing
         * the written value back makes every D2H event look permanently pending:
         * it wrote 0xffffffff to clear and read 0xffffffff, i.e. all eight
         * doorbells plus everything else asserted at once.
         */
        value = s->mailbox_int;
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

/*
 * A ring's parameters, read back out of the TCM the driver populated. Reading
 * them rather than hardcoding them means the model follows whatever the driver
 * chose, and it fails loudly (false) if the driver has not set a ring up yet.
 */
typedef struct AppleWLANRing {
    uint64_t base;
    uint16_t max_item;
    uint16_t item_size;
    // Which pair of index arrays this ring uses, and its slot within them.
    bool h2d;
    unsigned slot;
} AppleWLANRing;

static uint32_t apple_wlan_ring_info_off(AppleWLANDeviceState *s)
{
    return s->fw_shared_offset + APPLE_WLAN_FW_RING_INFO_BACKOFF;
}

static uint64_t apple_wlan_tcm_addr64(AppleWLANDeviceState *s, uint32_t off)
{
    if ((uint64_t)off + 8 > APPLE_WLAN_DEVICE_BAR2_SIZE) {
        return 0;
    }
    return (uint64_t)ldl_le_p(s->bar2_backing + off) |
           ((uint64_t)ldl_le_p(s->bar2_backing + off + 4) << 32);
}

static bool apple_wlan_get_ring(AppleWLANDeviceState *s, unsigned id,
                                AppleWLANRing *ring)
{
    uint32_t ringmem;
    uint32_t desc;

    if (s->fw_shared_offset == 0 || id >= APPLE_WLAN_RING_COUNT) {
        return false;
    }
    if (id >= APPLE_WLAN_RING_COMMON_COUNT) {
        unsigned d = id - APPLE_WLAN_RING_COMMON_COUNT;

        if (!s->dyn_ring[d].valid) {
            return false;
        }
        ring->base = s->dyn_ring[d].base;
        ring->max_item = s->dyn_ring[d].max_item;
        ring->item_size = s->dyn_ring[d].item_size;
        ring->h2d = s->dyn_ring[d].h2d;
        ring->slot = s->dyn_ring[d].slot;
        return true;
    }
    ringmem = ldl_le_p(s->bar2_backing + apple_wlan_ring_info_off(s));
    if (ringmem == 0) {
        return false;
    }
    desc = ringmem + id * APPLE_WLAN_RING_DESC_SIZE;
    if ((uint64_t)desc + APPLE_WLAN_RING_DESC_SIZE >
        APPLE_WLAN_DEVICE_BAR2_SIZE) {
        return false;
    }
    ring->max_item =
        lduw_le_p(s->bar2_backing + desc + APPLE_WLAN_RING_DESC_MAX_ITEM_OFF);
    ring->item_size =
        lduw_le_p(s->bar2_backing + desc + APPLE_WLAN_RING_DESC_ITEM_SIZE_OFF);
    ring->base =
        apple_wlan_tcm_addr64(s, desc + APPLE_WLAN_RING_DESC_BASE_OFF);
    ring->h2d = id < APPLE_WLAN_RING_H2D_COUNT;
    ring->slot = ring->h2d ? id : id - APPLE_WLAN_RING_H2D_COUNT;
    return ring->base != 0 && ring->max_item != 0 && ring->item_size != 0 &&
           ring->item_size <= APPLE_WLAN_RING_ITEM_MAX;
}

// The index arrays are u16 per ring, at a host address published in ring_info.
static bool apple_wlan_read_ring_index(AppleWLANDeviceState *s,
                                       uint32_t ring_info_off, unsigned slot,
                                       uint16_t *out)
{
    uint64_t array = apple_wlan_tcm_addr64(s, apple_wlan_ring_info_off(s) +
                                                  ring_info_off);
    uint16_t raw;

    if (array == 0) {
        return false;
    }
    if (pci_dma_read(PCI_DEVICE(s),
                     array + slot * sizeof(uint16_t), &raw,
                     sizeof(raw)) != MEMTX_OK) {
        return false;
    }
    *out = le16_to_cpu(raw);
    return true;
}

// The index arrays live in host memory; the device owns d2h w and h2d r.
static bool apple_wlan_write_ring_index(AppleWLANDeviceState *s,
                                        uint32_t ring_info_off, unsigned slot,
                                        uint16_t value)
{
    uint64_t array =
        apple_wlan_tcm_addr64(s, apple_wlan_ring_info_off(s) + ring_info_off);
    uint16_t raw = cpu_to_le16(value);

    if (array == 0) {
        return false;
    }
    return pci_dma_write(PCI_DEVICE(s),
                         array + slot * sizeof(uint16_t), &raw,
                         sizeof(raw)) == MEMTX_OK;
}

/*
 * Raise the mailbox status the driver's ISR reads, and signal it. The MSI line
 * is level and apcie never lowers it on its own, so the pairing with the
 * write-1-to-clear of MAILBOXINT below is what keeps this from latching high
 * forever -- that clear is the quiesce point apcie.c asks for.
 */
static void apple_wlan_int_deassert(AppleWLANDeviceState *s)
{
    if (!s->int_asserted) {
        return;
    }
    s->int_asserted = false;
    if (s->port != NULL) {
        apple_pcie_port_temp_lower_msi_irq(s->port, 0);
    }
}

static void apple_wlan_int_assert(AppleWLANDeviceState *s)
{
    PCIDevice *dev = PCI_DEVICE(s);
    MSIMessage msg;

    if (!msi_enabled(dev) || s->int_asserted) {
        return;
    }
    msg = msi_get_message(dev, 0);
    trace_apple_wlan_msi_notify(msg.address, msg.data);
    msi_notify(dev, 0);
    s->int_asserted = true;
    timer_mod_ns(s->int_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                     APPLE_WLAN_INT_HOLD_NS);
}

static void apple_wlan_int_timer(void *opaque)
{
    AppleWLANDeviceState *s = opaque;
    bool pending = (s->mailbox_int & s->mailbox_mask) != 0;

    if (s->int_asserted) {
        apple_wlan_int_deassert(s);
        if (pending) {
            timer_mod_ns(s->int_timer,
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                             APPLE_WLAN_INT_RETRY_NS);
        }
        return;
    }
    if (pending) {
        trace_apple_wlan_int_retry(s->mailbox_int, s->mailbox_mask);
        apple_wlan_int_assert(s);
    }
}

static void apple_wlan_raise_int(AppleWLANDeviceState *s, uint32_t bits)
{
    s->mailbox_int |= bits;
    trace_apple_wlan_raise_int(s->mailbox_int, s->mailbox_mask,
                               msi_enabled(PCI_DEVICE(s)));
    if ((s->mailbox_int & s->mailbox_mask) == 0) {
        return;
    }
    apple_wlan_int_assert(s);
}

// The guest has acknowledged; drop the line and stop retrying.
static void apple_wlan_lower_int_if_quiesced(AppleWLANDeviceState *s)
{
    if ((s->mailbox_int & s->mailbox_mask) != 0) {
        return;
    }
    timer_del(s->int_timer);
    apple_wlan_int_deassert(s);
}

/*
 * A rebuilt ring starts over: fresh host memory, zeroed, indices back to 0 and
 * phase back to its first-pass value. The driver tears every ring down and
 * rebuilds it on retry, so this has to be checked for each ring that is used,
 * not just the one a doorbell arrives on -- a write index carried over from the
 * previous attempt points into a zeroed ring, which the driver reports as
 * "Unexpected phaseBit ... At {0 31}".
 */
static void apple_wlan_ring_check_rebuilt(AppleWLANDeviceState *s, unsigned id,
                                          const AppleWLANRing *ring)
{
    if (s->ring_base_seen[id] == ring->base) {
        return;
    }
    trace_apple_wlan_ring_rebuilt(id, s->ring_base_seen[id], ring->base);
    s->ring_base_seen[id] = ring->base;
    s->ring_r_idx[id] = 0;
    s->ring_w_idx[id] = 0;
    s->ring_phase[id] = true;
    if (id == APPLE_WLAN_RING_H2D_CONTROL_SUBMIT) {
        /*
         * The submission ring being rebuilt is the attempt boundary. Buffers
         * posted before it belong to the epoch that was just torn down, and
         * handing one back afterwards is a resource the driver no longer knows.
         */
        s->ioctl_resp_head = 0;
        s->ioctl_resp_count = 0;
    }
}

// Post one control completion. The device owns this ring's write index.
static bool apple_wlan_d2h_post(AppleWLANDeviceState *s, const uint8_t *item)
{
    unsigned id = APPLE_WLAN_RING_D2H_CONTROL_COMPLETE;
    AppleWLANRing ring;
    uint16_t w_idx;
    uint16_t next;
    uint16_t host_r_idx;
    uint8_t stamped[APPLE_WLAN_MSGBUF_CMPLT_SIZE];

    memcpy(stamped, item, sizeof(stamped));

    if (!apple_wlan_get_ring(s, id, &ring)) {
        trace_apple_wlan_d2h_no_ring(id);
        return false;
    }
    apple_wlan_ring_check_rebuilt(s, id, &ring);
    w_idx = s->ring_w_idx[id];
    next = (w_idx + 1) % ring.max_item;
    /*
     * Overrunning would silently destroy completions the driver has not read,
     * which would look like the firmware answering the wrong request.
     */
    if (apple_wlan_read_ring_index(s, APPLE_WLAN_RING_INFO_D2H_R_IDX_OFF,
                                   ring.slot, &host_r_idx) &&
        next == host_r_idx) {
        trace_apple_wlan_d2h_full(id, w_idx, host_r_idx);
        return false;
    }
    stamped[2] = s->ring_phase[id] ?
                     (uint8_t)(stamped[2] | APPLE_WLAN_MSGBUF_PHASE_BIT) :
                     (uint8_t)(stamped[2] & ~APPLE_WLAN_MSGBUF_PHASE_BIT);
    if (pci_dma_write(PCI_DEVICE(s), ring.base + w_idx * ring.item_size,
                      stamped,
                      MIN(ring.item_size, APPLE_WLAN_MSGBUF_CMPLT_SIZE)) !=
        MEMTX_OK) {
        trace_apple_wlan_ring_dma_fail(ring.base + w_idx * ring.item_size);
        return false;
    }
    s->ring_w_idx[id] = next;
    if (next == 0) {
        // Wrapped: the next pass through the ring carries the other phase.
        s->ring_phase[id] = !s->ring_phase[id];
    }
    if (!apple_wlan_write_ring_index(s, APPLE_WLAN_RING_INFO_D2H_W_IDX_OFF,
                                     ring.slot, next)) {
        return false;
    }
    trace_apple_wlan_d2h_post(stamped[0],
                              ldl_le_p(stamped +
                                       APPLE_WLAN_MSGBUF_REQUEST_ID_OFF),
                              w_idx, next, stamped[2]);
    return true;
}

// Every completion starts as the request's header with a new type and a status.
static void apple_wlan_cmplt_init(uint8_t *cmplt, const uint8_t *req,
                                  uint8_t msgtype, int16_t status,
                                  uint16_t ring_id)
{
    memset(cmplt, 0, APPLE_WLAN_MSGBUF_CMPLT_SIZE);
    cmplt[0] = msgtype;
    cmplt[1] = req[1];
    cmplt[2] = req[2];
    stl_le_p(cmplt + APPLE_WLAN_MSGBUF_REQUEST_ID_OFF,
             ldl_le_p(req + APPLE_WLAN_MSGBUF_REQUEST_ID_OFF));
    stw_le_p(cmplt + APPLE_WLAN_MSGBUF_STATUS_OFF, (uint16_t)status);
    stw_le_p(cmplt + APPLE_WLAN_MSGBUF_RING_ID_OFF, ring_id);
}

static void apple_wlan_handle_ring_create(AppleWLANDeviceState *s,
                                          const uint8_t *item)
{
    uint16_t ring_id = lduw_le_p(item + 8);
    uint16_t ring_type = lduw_le_p(item + 10);
    uint64_t ring_ptr = ldq_le_p(item + 16);
    uint16_t max_items = lduw_le_p(item + 24);
    uint16_t item_size = lduw_le_p(item + 26);
    uint8_t cmplt[APPLE_WLAN_MSGBUF_CMPLT_SIZE];
    uint8_t reply = item[0] == APPLE_WLAN_MSGBUF_D2H_RING_CREATE ?
                        APPLE_WLAN_MSGBUF_D2H_RING_CREATE_CMPLT :
                        APPLE_WLAN_MSGBUF_H2D_RING_CREATE_CMPLT;

    bool h2d = item[0] == APPLE_WLAN_MSGBUF_H2D_RING_CREATE;
    unsigned free_slot = APPLE_WLAN_RING_DYNAMIC_MAX;
    unsigned d;

    trace_apple_wlan_ring_create(item[0], ring_id, ring_type, ring_ptr,
                                 max_items, item_size);
    /*
     * Remember it, or the driver's submissions to it are never drained. Reuse
     * the entry if this ring is being created again, which happens on retry.
     */
    for (d = 0; d < APPLE_WLAN_RING_DYNAMIC_MAX; d++) {
        if (s->dyn_ring[d].valid && s->dyn_ring[d].h2d == h2d &&
            s->dyn_ring[d].slot == ring_id) {
            break;
        }
        if (!s->dyn_ring[d].valid && free_slot == APPLE_WLAN_RING_DYNAMIC_MAX) {
            free_slot = d;
        }
    }
    if (d == APPLE_WLAN_RING_DYNAMIC_MAX) {
        d = free_slot;
    }
    if (d < APPLE_WLAN_RING_DYNAMIC_MAX && ring_ptr != 0 && max_items != 0 &&
        item_size != 0 && item_size <= APPLE_WLAN_RING_ITEM_MAX) {
        s->dyn_ring[d].base = ring_ptr;
        s->dyn_ring[d].max_item = max_items;
        s->dyn_ring[d].item_size = item_size;
        s->dyn_ring[d].slot = ring_id;
        s->dyn_ring[d].h2d = h2d;
        s->dyn_ring[d].valid = true;
        trace_apple_wlan_dyn_ring_added(d, h2d, ring_id, ring_ptr, max_items,
                                        item_size);
    } else {
        trace_apple_wlan_dyn_ring_rejected(h2d, ring_id, ring_ptr, max_items,
                                           item_size);
    }
    apple_wlan_cmplt_init(cmplt, item, reply, APPLE_WLAN_BCME_OK, ring_id);
    apple_wlan_d2h_post(s, cmplt);
}

static void apple_wlan_handle_resp_buf_post(AppleWLANDeviceState *s,
                                            const uint8_t *item)
{
    uint16_t len = lduw_le_p(item + 8);
    uint64_t addr = ldq_le_p(item + 16);
    unsigned slot;

    if (addr == 0 || len == 0) {
        return;
    }
    if (s->ioctl_resp_count == APPLE_WLAN_IOCTL_RESP_BUFS) {
        // Drop the oldest rather than the newest; the driver reuses in order.
        s->ioctl_resp_head =
            (s->ioctl_resp_head + 1) % APPLE_WLAN_IOCTL_RESP_BUFS;
        s->ioctl_resp_count--;
    }
    slot = (s->ioctl_resp_head + s->ioctl_resp_count) %
           APPLE_WLAN_IOCTL_RESP_BUFS;
    s->ioctl_resp_buf[slot].addr = addr;
    s->ioctl_resp_buf[slot].len = len;
    s->ioctl_resp_buf[slot].request_id =
        ldl_le_p(item + APPLE_WLAN_MSGBUF_REQUEST_ID_OFF);
    s->ioctl_resp_count++;
    trace_apple_wlan_resp_buf_post(addr, len, s->ioctl_resp_count);
}

/*
 * Fill an ioctl's response payload. Only the iovars the driver actually asks
 * for are answered; anything else is refused explicitly, so an unimplemented
 * one shows up as a refusal in the log instead of as plausible-looking zeroes.
 */
static uint16_t apple_wlan_ioctl_response(uint32_t cmd, const char *name,
                                          uint16_t in_len, uint16_t out_len,
                                          uint8_t *buf, uint16_t cap,
                                          int16_t *status)
{
    *status = APPLE_WLAN_BCME_OK;

    if (cmd == APPLE_WLAN_WLC_GET_COUNTRY) {
        if (cap < APPLE_WLAN_COUNTRY_ABBREV_SIZE) {
            *status = APPLE_WLAN_BCME_UNSUPPORTED;
            return 0;
        }
        memcpy(buf, APPLE_WLAN_COUNTRY_DEFAULT,
               strlen(APPLE_WLAN_COUNTRY_DEFAULT));
        return APPLE_WLAN_COUNTRY_ABBREV_SIZE;
    }
    if (cmd == APPLE_WLAN_WLC_GET_COUNTRY_LIST) {
        uint16_t len = APPLE_WLAN_COUNTRY_LIST_ENTRY_OFF +
                       APPLE_WLAN_COUNTRY_ABBREV_SIZE;

        if (cap < len) {
            *status = APPLE_WLAN_BCME_UNSUPPORTED;
            return 0;
        }
        stl_le_p(buf + APPLE_WLAN_COUNTRY_LIST_COUNT_OFF, 1);
        // Zero-padded within its four bytes; the caller already zeroed buf.
        memcpy(buf + APPLE_WLAN_COUNTRY_LIST_ENTRY_OFF,
               APPLE_WLAN_COUNTRY_DEFAULT, strlen(APPLE_WLAN_COUNTRY_DEFAULT));
        return len;
    }
    if (cmd == APPLE_WLAN_WLC_GET_VERSION) {
        if (cap < sizeof(uint32_t)) {
            *status = APPLE_WLAN_BCME_UNSUPPORTED;
            return 0;
        }
        stl_le_p(buf, APPLE_WLAN_IOCTL_VERSION);
        return sizeof(uint32_t);
    }
    /*
     * Sets and gets are treated asymmetrically, on purpose.
     *
     * A set carries its payload in the input buffer and expects nothing back.
     * There is no radio and no firmware state here for one to land in, so
     * accepting it cannot corrupt anything, while refusing it stops setup dead
     * -- the driver pushes a long batch of tuning iovars (ampdu_rx_factor,
     * bw_cap, ldpc_cap, rxstreams, sgi_rx, txcapconfig ...) and treats a
     * refusal of any one of them as fatal. So every set is accepted.
     *
     * A get is the opposite: answering one wrongly is worse than refusing it.
     * wlc_ver showed why -- a plausible but wrong structure had the driver
     * report interface version 0 where a refusal left it at its own default of
     * 3. So gets stay explicit, and an unimplemented one shows up as a refusal
     * in the log rather than as a confident wrong answer.
     */
    if (cmd == APPLE_WLAN_WLC_SET_VAR) {
        trace_apple_wlan_ioctl_set_accepted(name, in_len);
        return 0;
    }
    /*
     * Plain commands split by whether they carry input, not by whether they want
     * output -- WLC_SET_RADIO passes four bytes in and echoes four back, so
     * keying on the output length would have refused it. A command with input,
     * or with no output at all, is an action: WLC_UP (in 0, out 0) and
     * WLC_SET_RADIO (in 4, out 4) are the two setup insists on, and bringupBCM
     * gives up entirely if either is refused. As with sets, there is no state
     * here for one to change, so taking it cannot be wrong.
     *
     * A command with no input and an output buffer is a pure query, and falls
     * through to be answered explicitly or refused -- WLC_GET_VERSION and
     * WLC_GET_COUNTRY are answered that way, and cmd 39 is still refused, which
     * is how it stays visible.
     */
    if (cmd != APPLE_WLAN_WLC_GET_VAR && (in_len != 0 || out_len == 0)) {
        trace_apple_wlan_ioctl_action_accepted(cmd);
        return 0;
    }
    if (cmd != APPLE_WLAN_WLC_GET_VAR) {
        *status = APPLE_WLAN_BCME_UNSUPPORTED;
        return 0;
    }
    if (strcmp(name, "chanspecs") == 0) {
        uint16_t len = sizeof(uint32_t) * (1 + APPLE_WLAN_CHANNEL_COUNT);

        if (len > cap) {
            *status = APPLE_WLAN_BCME_UNSUPPORTED;
            return 0;
        }
        stl_le_p(buf, APPLE_WLAN_CHANNEL_COUNT);
        for (unsigned i = 0; i < APPLE_WLAN_CHANNEL_COUNT; i++) {
            stl_le_p(buf + sizeof(uint32_t) * (1 + i),
                     APPLE_WLAN_CHANSPEC_BW_20 + APPLE_WLAN_CHANNEL_FIRST + i);
        }
        return len;
    }
    if (strcmp(name, "cap") == 0) {
        size_t len = strlen(APPLE_WLAN_CHIP_CAPS_STRING) + 1;

        if (len > cap) {
            *status = APPLE_WLAN_BCME_UNSUPPORTED;
            return 0;
        }
        memcpy(buf, APPLE_WLAN_CHIP_CAPS_STRING, len);
        return (uint16_t)len;
    }
    for (size_t i = 0; i < ARRAY_SIZE(apple_wlan_int_iovars); i++) {
        if (strcmp(name, apple_wlan_int_iovars[i].name) != 0) {
            continue;
        }
        if (cap < sizeof(uint32_t)) {
            *status = APPLE_WLAN_BCME_UNSUPPORTED;
            return 0;
        }
        stl_le_p(buf, apple_wlan_int_iovars[i].value);
        return sizeof(uint32_t);
    }
    if (strcmp(name, "ver") == 0) {
        size_t len = strlen(APPLE_WLAN_FW_VERSION_STRING) + 1;

        if (len > cap) {
            *status = APPLE_WLAN_BCME_UNSUPPORTED;
            return 0;
        }
        memcpy(buf, APPLE_WLAN_FW_VERSION_STRING, len);
        return (uint16_t)len;
    }
    for (size_t i = 0; i < ARRAY_SIZE(apple_wlan_blob_loads); i++) {
        if (strcmp(name, apple_wlan_blob_loads[i].status) == 0) {
            if (cap < apple_wlan_blob_loads[i].status_len) {
                *status = APPLE_WLAN_BCME_UNSUPPORTED;
                return 0;
            }
            // Already zeroed by the caller; zero is a clean load.
            return apple_wlan_blob_loads[i].status_len;
        }
        if (strcmp(name, apple_wlan_blob_loads[i].version_iovar) == 0) {
            size_t len = strlen(apple_wlan_blob_loads[i].version) + 1;

            if (len > cap) {
                *status = APPLE_WLAN_BCME_UNSUPPORTED;
                return 0;
            }
            memcpy(buf, apple_wlan_blob_loads[i].version, len);
            return (uint16_t)len;
        }
    }
    *status = APPLE_WLAN_BCME_UNSUPPORTED;
    return 0;
}

static void apple_wlan_handle_ioctl(AppleWLANDeviceState *s,
                                    const uint8_t *item)
{
    uint32_t cmd = ldl_le_p(item + 8);
    uint16_t trans_id = lduw_le_p(item + 12);
    uint16_t in_len = lduw_le_p(item + 14);
    uint16_t out_len = lduw_le_p(item + 16);
    uint64_t in_addr = ldq_le_p(item + 24);
    char name[APPLE_WLAN_IOVAR_NAME_MAX] = { 0 };
    uint8_t cmplt[APPLE_WLAN_MSGBUF_CMPLT_SIZE];
    uint8_t payload[APPLE_WLAN_IOVAR_PAYLOAD_MAX] = { 0 };
    int16_t status = APPLE_WLAN_BCME_OK;
    uint16_t resp_len;
    uint16_t cap;
    bool have_buf_request_id = false;
    uint32_t buf_request_id = 0;

    if (in_len != 0 && in_addr != 0) {
        pci_dma_read(PCI_DEVICE(s), in_addr, name,
                     MIN(in_len, sizeof(name) - 1));
    }
    trace_apple_wlan_ioctl(cmd, trans_id, in_len, out_len, name);

    // The driver expects the request acknowledged before its result arrives.
    apple_wlan_cmplt_init(cmplt, item, APPLE_WLAN_MSGBUF_IOCTLPTR_REQ_ACK,
                          APPLE_WLAN_BCME_OK, 0);
    apple_wlan_d2h_post(s, cmplt);

    cap = MIN(out_len, sizeof(payload));
    resp_len =
        apple_wlan_ioctl_response(cmd, name, in_len, out_len, payload, cap,
                                  &status);
    /*
     * Every completion consumes one posted response buffer, whatever the status.
     * The driver looks the buffer up by its resource id and removes it from the
     * table it was registered in, so a completion that names anything else --
     * the request's own resource id, say, which belongs to the Tx table -- is
     * rejected outright with "Rx IO not found for resourceID N". Refusing an
     * iovar is still an answer, and it still has to give the buffer back.
     */
    if (s->ioctl_resp_count != 0) {
        uint64_t addr = s->ioctl_resp_buf[s->ioctl_resp_head].addr;
        uint16_t len = MIN(resp_len, s->ioctl_resp_buf[s->ioctl_resp_head].len);

        buf_request_id = s->ioctl_resp_buf[s->ioctl_resp_head].request_id;
        have_buf_request_id = true;
        if (len != 0 &&
            pci_dma_write(PCI_DEVICE(s), addr, payload, len) != MEMTX_OK) {
            trace_apple_wlan_ring_dma_fail(addr);
            status = APPLE_WLAN_BCME_UNSUPPORTED;
            len = 0;
        }
        resp_len = len;
        s->ioctl_resp_head =
            (s->ioctl_resp_head + 1) % APPLE_WLAN_IOCTL_RESP_BUFS;
        s->ioctl_resp_count--;
    } else {
        // Nothing to answer into, and nothing valid to name.
        trace_apple_wlan_ioctl_no_buf(cmd);
        status = APPLE_WLAN_BCME_UNSUPPORTED;
        resp_len = 0;
    }

    apple_wlan_cmplt_init(cmplt, item, APPLE_WLAN_MSGBUF_IOCTL_CMPLT, status,
                          0);
    if (have_buf_request_id) {
        stl_le_p(cmplt + APPLE_WLAN_MSGBUF_REQUEST_ID_OFF, buf_request_id);
    }
    stw_le_p(cmplt + 12, resp_len);
    stw_le_p(cmplt + 14, trans_id);
    stl_le_p(cmplt + 16, cmd);
    trace_apple_wlan_ioctl_cmplt(cmd, status, resp_len, trans_id);
    apple_wlan_d2h_post(s, cmplt);
}

/*
 * Drain one H2D submission ring and answer what it holds. Returns whether
 * anything was posted back, so the caller knows to interrupt. Items live in
 * host memory reached through the WLAN DART, not in our TCM.
 */
static bool apple_wlan_drain_h2d_ring(AppleWLANDeviceState *s, unsigned id,
                                      const AppleWLANRing *ring)
{
    uint16_t w_idx;
    bool posted = false;

    apple_wlan_ring_check_rebuilt(s, id, ring);
    if (!apple_wlan_read_ring_index(s, APPLE_WLAN_RING_INFO_H2D_W_IDX_OFF,
                                   ring->slot, &w_idx)) {
        trace_apple_wlan_doorbell_no_index(ring->base);
        return false;
    }
    if (w_idx >= ring->max_item || s->ring_r_idx[id] == w_idx) {
        return false;
    }
    trace_apple_wlan_doorbell(id, ring->base, ring->max_item, ring->item_size,
                              s->ring_r_idx[id], w_idx);
    while (s->ring_r_idx[id] != w_idx) {
        uint8_t item[APPLE_WLAN_RING_ITEM_MAX] = { 0 };
        uint64_t addr = ring->base + s->ring_r_idx[id] * ring->item_size;

        if (pci_dma_read(PCI_DEVICE(s), addr, item, ring->item_size) !=
            MEMTX_OK) {
            trace_apple_wlan_ring_dma_fail(addr);
            return posted;
        }
        trace_apple_wlan_ring_item(id, s->ring_r_idx[id], item[0], item[1],
                                   item[2],
                                   ldl_le_p(item +
                                            APPLE_WLAN_MSGBUF_REQUEST_ID_OFF),
                                   ldq_le_p(item + 8), ldq_le_p(item + 16),
                                   ldq_le_p(item + 24), ldq_le_p(item + 32));
        switch (item[0]) {
        case APPLE_WLAN_MSGBUF_H2D_RING_CREATE:
        case APPLE_WLAN_MSGBUF_D2H_RING_CREATE:
            apple_wlan_handle_ring_create(s, item);
            posted = true;
            break;
        case APPLE_WLAN_MSGBUF_IOCTLRESP_BUF_POST:
            apple_wlan_handle_resp_buf_post(s, item);
            break;
        case APPLE_WLAN_MSGBUF_EVENT_BUF_POST:
            // Nothing to complete; there are no events to deliver yet.
            break;
        case APPLE_WLAN_MSGBUF_IOCTLPTR_REQ:
            apple_wlan_handle_ioctl(s, item);
            posted = true;
            break;
        default:
            trace_apple_wlan_msgbuf_unhandled(item[0], s->ring_r_idx[id]);
            break;
        }
        s->ring_r_idx[id] = (s->ring_r_idx[id] + 1) % ring->max_item;
    }
    apple_wlan_write_ring_index(s, APPLE_WLAN_RING_INFO_H2D_R_IDX_OFF,
                                ring->slot, s->ring_r_idx[id]);
    /*
     * Consuming submissions is itself something to signal, not just answering
     * them. The read index moving is how the driver learns its Tx commands were
     * taken and its ring space freed -- reportCompletedTxCommands is that path,
     * and it is what releases a command waiting in the pending queue. Returning
     * false here when a drain posted no completion left the driver with work
     * queued and nothing to wake it.
     */
    return true;
}

/*
 * The doorbell is one register for every submission ring, so a ring of the
 * driver's own making has to be drained too -- it creates one with
 * submitH2DRingCreateMsg and posts to it, and leaving that ring alone stalls
 * whatever was queued there whatever the control ring is doing.
 */
static void apple_wlan_h2d_doorbell(AppleWLANDeviceState *s)
{
    bool posted = false;
    bool any = false;

    for (unsigned id = 0; id < APPLE_WLAN_RING_COUNT; id++) {
        AppleWLANRing ring;

        if (!apple_wlan_get_ring(s, id, &ring) || !ring.h2d) {
            continue;
        }
        any = true;
        posted |= apple_wlan_drain_h2d_ring(s, id, &ring);
    }
    if (!any) {
        trace_apple_wlan_doorbell_no_ring(s->fw_shared_offset);
        return;
    }
    if (posted) {
        /*
         * Only the doorbell. The driver also unmasks FN0_0, but that bit means
         * mailbox data is waiting, and raising it with the D2H word at zero
         * changed nothing -- measured, no extra handler activity at all -- so
         * it is not asserted here.
         */
        apple_wlan_raise_int(s, APPLE_WLAN_MB_INT_D2H_DB0);
    }
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
    } else if (offset == APPLE_WLAN_PCIE2_MAILBOXINT) {
        // Write-1-to-clear.
        s->mailbox_int &= ~(uint32_t)value;
        trace_apple_wlan_mailbox_int(s->mailbox_int, (uint32_t)value,
                                     s->mailbox_mask);
        apple_wlan_lower_int_if_quiesced(s);
    } else if (offset == APPLE_WLAN_PCIE2_MAILBOXMASK) {
        s->mailbox_mask = (uint32_t)value;
        trace_apple_wlan_mailbox_mask(s->mailbox_mask);
    } else if (offset == APPLE_WLAN_PCIE2_H2D_MAILBOX_0 ||
               offset == APPLE_WLAN_PCIE2_H2D_MAILBOX_1) {
        apple_wlan_h2d_doorbell(s);
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
            stl_le_p(s->bar2_backing + shared +
                         APPLE_WLAN_FW_SHARED_RING_INFO_OFF,
                     shared + APPLE_WLAN_FW_RING_INFO_BACKOFF);
            stl_le_p(s->bar2_backing + shared +
                         APPLE_WLAN_FW_RING_INFO_BACKOFF,
                     shared + APPLE_WLAN_FW_RING_MEM_BACKOFF);
            stw_le_p(s->bar2_backing + shared +
                         APPLE_WLAN_FW_RING_INFO_BACKOFF +
                         APPLE_WLAN_FW_RING_INFO_MAX_FLOWRINGS_OFF,
                     APPLE_WLAN_FW_RING_MAX_FLOWRINGS);
            stw_le_p(s->bar2_backing + shared +
                         APPLE_WLAN_FW_RING_INFO_BACKOFF +
                         APPLE_WLAN_FW_RING_INFO_MAX_SUBMIT_OFF,
                     APPLE_WLAN_FW_RING_MAX_DYN_SUBMIT);
            /*
             * Give the mailbox-data words somewhere real to live, so that
             * raising FN0 makes the driver read a defined location rather than
             * whatever the firmware image left at TCM offset 0.
             */
            stl_le_p(s->bar2_backing + shared +
                         APPLE_WLAN_FW_SHARED_DTOH_MB_DATA_OFF,
                     shared + APPLE_WLAN_FW_DTOH_MB_DATA_BACKOFF);
            stl_le_p(s->bar2_backing + shared +
                         APPLE_WLAN_FW_SHARED_HTOD_MB_DATA_OFF,
                     shared + APPLE_WLAN_FW_HTOD_MB_DATA_BACKOFF);
            stl_le_p(s->bar2_backing + shared +
                         APPLE_WLAN_FW_DTOH_MB_DATA_BACKOFF,
                     0);
            stl_le_p(s->bar2_backing + shared +
                         APPLE_WLAN_FW_HTOD_MB_DATA_BACKOFF,
                     0);
            trace_apple_wlan_fw_alive_marker((uint32_t)offset, shared,
                                             s->tcm_written_bytes);
        }
        s->tcm_last_read_offset = (uint32_t)offset;
        s->tcm_last_read_valid = true;
    }
    memcpy(&value, s->bar2_backing + offset, size);
    /*
     * Trace reads inside the published structure only. A per-access event across
     * the whole 8 MiB TCM buries the log, but the driver's reads *here* are what
     * tell us which fields of the structure it consults, and in what order --
     * which is how its layout gets established instead of guessed.
     */
    if (s->fw_shared_offset != 0 && offset >= s->fw_shared_offset &&
        offset < s->fw_shared_offset + APPLE_WLAN_FW_SHARED_WINDOW) {
        trace_apple_wlan_fw_shared_read((uint32_t)(offset - s->fw_shared_offset),
                                        size, value);
    }
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
     * Trace writes inside the published structure. The msgbuf rings live in host
     * memory, so the driver writes their DMA addresses and sizes in here -- this
     * is how those get discovered rather than guessed.
     */
    if (s->fw_shared_offset != 0 && offset >= s->fw_shared_offset &&
        offset < s->fw_shared_offset + APPLE_WLAN_FW_SHARED_WINDOW) {
        trace_apple_wlan_fw_shared_write(
            (uint32_t)(offset - s->fw_shared_offset), size, value);
    }
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

    s->int_timer =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, apple_wlan_int_timer, s);
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
    if (s->int_timer != NULL) {
        timer_free(s->int_timer);
        s->int_timer = NULL;
    }
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
