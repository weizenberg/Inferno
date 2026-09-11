# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

ChefKiss Inferno — a fork of QEMU that adds **Apple ARM (iOS) guest support**. It is not a general QEMU
checkout: the upstream test suite has been deleted, and the interesting code is the Apple silicon SoC,
coprocessor, and boot emulation under `hw/*/apple-silicon/` plus Apple-specific extensions to `target/arm`.

Remotes: `origin` = personal fork, `upstream` = `ChefKissInc/Inferno`. User docs live at
<https://chefkiss.dev/applehax/inferno/>; the FAQ/wiki still lives under the old `QEMUAppleSilicon` repo name.

Licensing matters here: preserve the license of inherited files; **new project code is AGPLv3-or-later**
(copy the canonical block from `include/hw/arm/apple-silicon/boot.h` or `patcher.h`). GPLv2-only code still
exists and its removal is an ongoing goal. Branding assets are restricted — see `ui/icons/CKBrandingNotice.md`.

## Build

Out-of-tree build, standard QEMU flow. The official build (Host Setup in the manual,
<https://chefkiss.dev/guides/inferno/host-setup/>) builds **two** targets — `aarch64-softmmu` for the iPhone
and `x86_64-softmmu` for the companion VM used during restore:

```bash
git submodule update --init
mkdir build && cd build
LIBTOOL="glibtool" ../configure --target-list=aarch64-softmmu,x86_64-softmmu \
    --disable-guest-agent --enable-lzfse --enable-slirp --enable-curses --enable-libssh \
    --enable-virtfs --enable-zstd --extra-cflags=-DNCURSES_WIDECHAR=1 \
    --disable-sdl --disable-gtk --enable-cocoa --enable-nettle --enable-gnutls \
    --extra-cflags="-I/opt/homebrew/include" --extra-ldflags="-L/opt/homebrew/lib" \
    --disable-werror --disable-qom-cast-debug --disable-debug-info
ninja
```

`LIBTOOL=glibtool` and the `/opt/homebrew` flags are macOS arm64 only; Linux drops those and uses
`--enable-gtk --enable-sdl`. Deps (brew): `libtool meson ninja pkgconf dtc glib gnutls jpeg-turbo libpng
libslirp libssh libusb lzo ncurses pixman snappy vde zstd libtasn1 lzfse`.

Non-obvious build requirements:
- **Drop `--disable-qom-cast-debug --disable-debug-info` when debugging Inferno itself** (not the guest) —
  the manual calls this out explicitly. They are only there to make normal user builds faster/smaller.
- `git submodule update --init` is mandatory: `util/mlib` (M\*LIB) is a submodule and the build fails without
  it. It is often left uninitialised in a fresh clone — check `git submodule status util/mlib` for a leading `-`.
- **liblzfse** is required for Apple image decompression; **libtasn1** is a global hard dependency used for
  IMG4 (`img4.asn1`) and SEP ART (`art.asn`) parsing. On Linux the package is usually `libtasn1-dev`.
- nettle ≥ 3.10.2 — Linux CI (and Debian users) must build it from source; then pass
  `PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig` to `configure`.
- Only rerun `configure` when options change; otherwise `ninja` in `build/` re-runs meson itself.
- On macOS `ninja` stops at `qemu-system-aarch64-unsigned`; `ninja qemu-system-aarch64` runs the codesign step
  that adds the HVF entitlement. The manual's commands invoke the `-unsigned` binary directly.

### Tests

There is **no test suite** — `tests/` was intentionally removed from git tracking (commit
"remove some tests that somehow stayed after removing all tests"), and `scripts/checkpatch.pl` is gone too.
An untracked `tests/` directory may contain leftover upstream tooling, but is not project source. CI
(`.github/workflows/build.yaml`) only compiles, on Linux, macOS arm64, and macOS x86_64. Verification means
*building and booting a guest*, not running `make check`. Do not add or wire up upstream QEMU tests unless
asked.

### Style

Enforced by `.clang-format` (LLVM-derived, 80 columns, 4-space indent, `AfterFunction: true` braces,
`SortIncludes: CaseSensitive` with `qemu/osdep.h` first). Run `clang-format` on files you touch. Apple code
uses `//` comments freely, unlike upstream QEMU. Devices log via per-directory `trace-events` + `trace.h`
(`hw/arm/apple-silicon/`, `hw/misc/apple-silicon/`, `hw/misc/apple-silicon/a7iop/`) — regenerate by rebuilding;
enable at runtime with `-trace apple_*`.

## Running a guest

Abridged from the manual (<https://chefkiss.dev/guides/inferno/running-restoring/>) — the real command also
passes seven `nvme-ns`/`apple-nvram` drives:

```bash
./build/qemu-system-aarch64 \
    -M t8030,trustcache=./Restore/Firmware/038-44135-124.dmg.trustcache,ticket=root_ticket.der,\
sep-fw=sep-firmware.n104.RELEASE.new.img4,sep-rom=AppleSEPROM-Cebu-B1,kaslr-off=true \
    -kernel ./Restore/kernelcache.research.iphone12b \
    -dtb ./Restore/Firmware/all_flash/DeviceTree.n104ap.im4p \
    -append "tlto_us=-1 mtxspin=-1 agm-genuine=1 agm-authentic=1 agm-trusted=1 serial=3 wdt=-1 -vm_compressor_wk_sw" \
    -smp 7 -m 4G -serial mon:stdio \
    -display cocoa,zoom-to-fit=on,zoom-interpolation=on,show-cursor=on \
    -drive file=sep_nvram,if=pflash,format=raw -drive file=sep_ssc,if=pflash,format=raw \
    -drive file=root,format=raw,if=none,id=root \
    -device nvme-ns,drive=root,bus=nvme-bus.0,nsid=1,nstype=1,logical_block_size=4096,physical_block_size=4096 \
    # ...nsid=2..7 for firmware, syscfg, ctrl_bits, nvram (apple-nvram), effaceable, panic_log
```

Notes that matter:
- `A13_MAX_CPU` is 6 AP cores; the seventh CPU runs SEP. The default build enables data encryption, so `t8030`
  requires both SEP files and cannot use the simulated SEP (see below) — hence `-smp 7`, not 6.
- The NVMe namespaces are positional: `nsid`/`nstype` 1=root, 2=firmware, 3=syscfg, 4=ctrl_bits, 5=nvram
  (uses `-device apple-nvram`, not `nvme-ns`), 6=effaceable, 7=panic_log (`nstype=8`).
- `-vm_compressor_wk_sw` forces iOS to use the *software* WKdm compressor. `-initrd <ramdisk>.dmg` is only for
  restore/upgrade (the smaller of the two RAM disks erases, the larger upgrades).
- Firmware comes from a real IPSW plus an AP ticket; restore is driven by `idevicerestore --erase
  --restore-mode -i 0x1122334455667788` from the companion VM (that ECID is the machine's default). Only
  **iOS 14.x** is supported today, on iPhone 11 (`t8030`); `s8000` boots but has no UI and cannot restore.
- After restore, filesystem patches are required for software rendering — there is no GPU emulation.

Machine properties are added in `<machine>_class_init` via `object_class_property_add*` (not `DEFINE_PROP`),
so they are `-M <machine>,<prop>=<val>` options, not `-device` options. Key `t8030` properties:
`trustcache`, `ticket`, `sep-rom`, `sep-fw`, `securerom`, `boot-mode` (`auto`/`enter_recovery`/`exit_recovery`),
`ecid`, `kaslr-off`, `force-dfu`, `hactivation` (default true; set false to skip the hactivation kernel patch
and omit the `allow-hactivation` DT property, requiring real device activation), `usb-conn-type` (`unix`/`ipv4`/`ipv6`), `usb-conn-addr`, `usb-conn-port`,
`model`, `region-info`, `config-number`, `serial-number`, `mlb`, `regulatory-model`, `disp-width`, `disp-height`.
`enable-wlan` (worktree branch `wlan-pcie`) keeps the WiFi DeviceTree nodes
(`apcie/pci-bridge2/wlan`, `arm-io/wlan`, `amfm`) and creates the stub BCM4378
PCIe endpoint on `pcie.bridge2`; its BAR accesses are traced via
`apple_wlan_*` events. The driver currently stops in the AMFM port power-up
path (SMC `gP11` / `pcie_port_control` not yet implemented).

Machines:
- `t8030` — Apple T8030 / A13 (iPhone 11), `hw/arm/apple-silicon/t8030.c`, DisplayPipe v4, 16K pages, 4 GiB default.
- `s8000` — Apple S8000 / A9 (iPhone 6s Plus), `hw/arm/apple-silicon/s8000.c`, DisplayPipe v2.

The two machine files are structurally parallel and heavily duplicated; a change to one usually needs the
same change to the other. Per-SoC register/DT constants live in `t8030-config.c.inc` / `s8000-config.c.inc`.

### Hardware acceleration

**On `master`, treat the Apple machines as TCG-only: `-accel hvf` on `t8030`/`s8000` does not get you a
booting guest.** On the `hvf-apple-gxf` branch it does — see the status section below before assuming either
way. The reasons are narrower than "the Apple features are TCG-only", and getting them right matters if you
touch this area:

- **PAC is the actual blocker, and it is a TCG/HVF asymmetry.** `pauth-noop` is a fork-added property
  defaulting to `true` (`cpu64.c:541`), and `aarch64_apple_gxf_initfn` sets it **only when `tcg_enabled()`**
  (`cpu64.c:754`). `TYPE_APPLE_A13` inherits that initfn (`.parent = ARM_CPU_TYPE_NAME("apple-gxf")`,
  `a13.c:865`). So under TCG every `PAC*`/`AUT*` is a no-op (`hflags.c:312`, "all insns are implemented as a
  nop"), while under HVF the guest gets **real** IMPDEF PAC. Measured on an M5 Pro / macOS 26.5.2 via
  `hv_vcpu_config_get_feature_reg`: `ID_AA64ISAR1_EL1 = 0x0010221110211502` → `API=0x5` (IMPDEF PAuth with
  FPAC+FPACCOMBINE), `GPI=0x1`, `APA=GPA=0`. Meanwhile Apple's key-management layer XNU programs
  (`APCTL_EL1`, `KERNELKEY_LO/HI`, the per-boot M-key XOR — `helper.c:5180`, `t8030.c:2647`) exists only in
  QEMU's shadow `CPUARMState`, which native PAC never consumes. That is the "stuck on PAC" in issue #68.
  Note the model forces `APA=PauthFeat_2` (`cpu64.c:751`) — the QARMA5 field — while the host implements the
  IMPDEF one.
- **The Apple sysreg definitions are *not* TCG-only.** `hvf_sysreg_read_cp`/`write_cp` (`hvf.c:1157`) fall back
  to QEMU's `cp_regs` table — the same one `define_arm_cp_regs` fills from `apple_a13_cp_reginfo_tcg`
  (`a13.c:517`, `:626`) — honouring `readfn`/`accessfn`. Added by `e2a56e45ad` (2025-06-07). The catch is that
  HVF only exits to userspace for registers *it* chooses to trap, there is no API to force-trap, and no
  `hv_sys_reg_t` IDs exist for Apple `S3_4_c15_*`/`S3_6_c15_*`. An emulator you cannot invoke is not an emulator.
- **SPRR/GXF genuinely is TCG-only**, because it lives in the software page-table walker
  (`pte_to_sprr_prot_is_guarded`, `ptw.c:1017`; `arm_is_sprr_enabled` at `ptw.c:2219`) plus `GENTER`/`GEXIT`
  banking (`helper.c:8749`, `helper-a64.c:796`). Under HVF the hardware walks page tables, so none of it runs.
- **The fast-IPI sysregs are the sleeper blocker**: `IPI_RR_LOCAL/GLOBAL`, `IPI_SR`, `IPI_CR`
  (`a13.c:349-515`) carry XNU's entire A13 SMP IPI path and HVF cannot trap them.
- **WKdm decode is ungated**: `disas_a64_legacy` calls `disas_apple_insn` for *all* AArch64 CPUs
  (`translate-a64.c:10186`) with no Apple feature check, so `-vm_compressor_wk_sw` is mandatory under HVF.
- HVF enablement is real, published, and ongoing — `e2a56e45ad`, `d590a682af` (adds
  `target/arm/emulate/aarch64.c` so QEMU can service faulting MMIO), `c1bfe91b1d`, `bd41bc7747`, active
  through March 2026. Tracked in issues #68 (HVF) and #87 (KVM), both milestone Future.

There is no guard that rejects `-accel hvf` for these machines — on `master`, `-M t8030 -accel hvf`
gets all the way into `t8030_init` and fails later on unrelated grounds, so a wrong `-accel` looks like a
firmware bug rather than an accel bug. Note also that upstream HVF only ever runs `-cpu host`, while these
machines instantiate `apple-a13-cpu`/`apple-a9-cpu` directly — one accelerator serves the whole VM, including
the SEP core, and `hvf_init_vcpu()` runs for every CPU.

#### HVF status, and the rules that are easy to get wrong

On `hvf-apple-gxf`, **`t8030` boots iOS 14.x under `-accel hvf` at parity with TCG**: the AP reaches `launchd`
and full userspace (USB stack, CoreAnalytics, APFS transactions), and the **real SEP boots SEPOS** with the
keystore serving xART. Measured side by side on the same disk image: 810 (TCG) vs 820 (HVF) serial lines, both
reaching `launchd`, 12 vs 12 SEP-xART fetches, 13 vs 13 xART Locker ops, no panics on either, and byte-identical
display-pipe register programming. The remaining `cannot unwrap d_key` appears on both and is not a divergence.
Details and A/B tables live in the `hvf:` commits; the facts you need before touching this code:

- **PAC is per-core: off for the AP, on for the SEP.** Clearing the `SCTLR_EL1` PAC enables in
  `hvf_arch_put_registers()` is how `pauth-noop` is approximated under HVF, and the AP *needs* it, because
  XNU's key management (`APCTL_EL1`, `KERNELKEY_LO/HI`, the per-boot M-key XOR) lives only in QEMU's shadow
  state and never reaches the hardware. The SEP is the opposite: SEPFW is never patched (only SEPROM is, via
  `ck_sep_seprom_patches()`) and SEPOS signs and authenticates constantly, so it must run **native** PAC.
  The masking is not stable — SCTLR is not trapped, so the guest's own writes stand in between — and on a
  host with FPAC a sign/auth mismatch *faults* rather than returning a corrupted pointer. That fault lands in
  SEPOS's own EL1 vector, invisible to HVF. Hence the `hvf-pauth-noop` property: default true, registered in
  `aarch64_apple_gxf_initfn()` so only the Apple CPUs are affected (a plain HVF guest on `-cpu host` keeps its
  PAC), and cleared for the SEP core in `sep.c`. Either extreme is self-consistent; only the flicker is fatal.
- **GENTER/GEXIT must be rewritten to HVC before the guest runs.** They are IMPDEF instructions the host does
  not implement, so under HVF `ck_kp_gxf_rewrite_all()` patches them into `HVC #imm` using the ABI in
  `include/hw/arm/apple-silicon/gxf-hvc.h`, and `target/arm/hvf/hvf.c` emulates guarded entry/exit in its
  `EC_AA64_HVC` handler. This runs on the **AP kernelcache only** (`kernel_text`/`kernel_ppltext`). SEPFW is
  not patched and does not need to be — the n104 SEPOS image contains zero GENTER/GEXIT.

One structural constraint explains a whole class of SEP bugs: **HVF has no per-vCPU address space.**
`hvf_accel_init()` registers its memory listener only on `address_space_memory`, so anything mapped solely
into a CPU-private `AddressSpace` — such as the SEP's 32 MiB `sep_dma` alias in `AppleA13State::memory` —
does not exist for an HVF vCPU. Trapping accesses are fine, because `hvf_handle_exception()` resolves MMIO
through `cpu_get_address_space()`; it is hardware-walked RAM that silently reads the wrong memory, with no
fault raised. The corollary is the rule that fixed the keystore: **a region a DMA master must reach through an
IOMMU must not also exist in the global map.** `t8030_create_sep()` therefore puts the SEP's DMA staging RAM in
its own `sep-dma-downstream` AddressSpace and points `dart-sep`'s `target_as` at it (`apple_dart_set_target_as`).

#### Where HVF's time actually goes (measured, and not where it looks)

HVF runs the boot **1.2–1.7× slower** than TCG — same snapshot, same harness (`InfernoData/bench.sh`):
`launchd` 14.5s vs 10.4s, first guest frame 51.9s vs 31.3s, SpringBoard 118.0s vs 97.3s.

The cost is **MMIO trap volume**: 2,621,288 data aborts in one boot, and they are ordinary device registers, so
trapping is correct and unavoidable. `-trace enable=hvf_data_abort` and a histogram of `pa=` gives:

| trapping region | aborts / boot |
|---|---|
| `apple-a7iop.SEP.regs` (mailbox, `0x242404xxx` + `0x242400xxx`) | 930,592 |
| `sep.aess_base` (SEP AES engine) | 439,634 |
| `apple-uart` — the serial console, i.e. `serial=3` in `-append` | 151,640 |
| `sep_i2c` | 50,962 |

**Do not attribute this to the trapping DART window.** That was the obvious theory and it is wrong: a
page-granular DMA mirror was built and measured (`apple_dart_install_dma_mirror()`, `-M t8030,sep-dma-mirror=`),
it installs all 17 mapped pages as RAM, and the fallback-emulation count moves 617,943 → 614,038 — **0.6%**. The
~610k software-decoded accesses are ~99% *not* DMA to that window; they are the SEP (`cpu 6`) hitting device
registers, 79% of them from one two-instruction `ldp`/`stp` loop at `0x24000464c`/`0x240004650`.

Two facts about the window that are worth keeping anyway: the SEP's DART maps only **17 of 2048** pages (the
shmbuf the AP advertises at `0x100C000`), and those pages are **not physically contiguous** — so no coarse
single-alias mirror of the window can ever apply.

The levers that follow from the table above are mailbox round-trips, a bulk path for the AES engine, and simply
being less chatty on the UART — not the IOMMU.

**That table describes the iOS 14 boot to SpringBoard. It is not the whole story for iOS 26 userspace.** An exit
histogram taken on the `ios26` worktree (2026-09-11, HVF, after `launchd`, all exit reasons traced) showed the
dominant cost was **system-register synchronisation, not MMIO**: `APCTL_EL1` reads and writes alone produced
~49,000 exits in three seconds, and nearly every AP sysreg trap paid a full `cpu_synchronize_state` round trip.
The mitigation is the shadow-only fast path in `hvf_sync_apple_shadow()` (`target/arm/hvf/hvf.c`): registers whose
state lives entirely in QEMU (`APCTL_EL1`, `GXF_STATUS_EL1`, `SPRR_EL1BR1_EL1`, `ASPSR_GL11`) skip the full sync on
the eligible path, writes and guarded aliases keep it. Measure the exit mix for the workload you care about before
picking a lever; the two boots have different bottlenecks.

Because those accesses land in `target/arm/emulate/aarch64.c`, that decoder has to cover whatever the guest
actually emits — which includes SIMD&FP (`str q0`, `ld1 {v0-v3.4s}, [x2], #64`). A form it does not decode
becomes a data abort the guest cannot explain, so it looks like a firmware bug. Check new decode logic against
assembled encodings, not against the manual alone.

#### The UI works under HVF; a blank screen means the image lacks the filesystem patches

**`t8030` reaches the iOS 14 SpringBoard lock screen under `-accel hvf`** — wallpaper, live clock, notifications,
all composited through `apple_displaypipe_v4.c`. It takes ~4–6 minutes of wall clock after the Apple logo on both
accelerators; the guest kicks a frame about once a minute while idle, so be patient before concluding anything.

The display behaves identically under TCG and HVF (byte-identical register streams, byte-identical captured
framebuffers). What decides whether you see anything is **whether the guest image has the filesystem patches**,
and the difference is visible in one register:

| | `pixel_format` | `GP_LAYER_0_STRIDE` | result |
|---|---|---|---|
| unpatched image | `0x457c8003` — bit 30 `GP_PIXEL_FORMAT_COMPRESSED` **set** | `0x34000` = 64× the byte stride | `adp_v4_gp_read()` drops the frame (explicit TODO, there is no decompressor) |
| patched image | `0x057c8000` — bit 30 clear | `0xD00` = 3328, the true byte stride | frame composites; Apple logo, then SpringBoard |

`InfernoFSPatcher` neutralises `___CADeviceSupportsCIF10_block_invoke` in QuartzCore, "which also neutralises
framebuffer compression" — that patch is what clears bit 30. So **a screen stuck on the ChefKiss splash means
the image lacks the fs patches, not that the display model is broken.** Both stride encodings are the guest's
own; the model's plain `src_height * stride` is right for the patched case and meaningless for the compressed one.

Two traps when debugging this:
- `adp_v4_gp_read()` bails on the COMPRESSED check **before** its `ADP_INFO` lines, so a missing
  `gp0: pixel format is …` does *not* mean `adp_v4_gp_draw()` bailed on the `RUN`/`ENABLED` gate. That gate is
  not a blocker — at the frame kick the pipe reads `config_control = 0x80070001` (RUN 1, ENABLED 1). The
  `<- 0x00040000` disable belongs to the *teardown* of the boot framebuffer, and shows up as a second draw with
  every register back to zero.
- The frame is pulled by DMA on each kick (`REG_0x4602C` bit 12), not by dirty tracking, so a static screen
  means no kicks — not a missed invalidate.

Enable the `#if 0` `ADP_INFO` block at the top of the file to see all of this. To capture the framebuffer
without a GUI session, run with `-display none -monitor unix:…` and use the monitor's `screendump` — that is how
the HVF lock-screen captures were taken, and it makes the display checkable from a script.

Always test the UI against a **post-migration, fs-patched** namespace set, cloned with `cp -c` so the snapshot
stays pristine. A copy taken before the patches boots just as far on the serial console and looks identical by
every metric in `inferno-hvf-metrics.sh`, but never renders — which reads as a display bug and is not one.

`scripts/inferno-hvf-metrics.sh` summarises a boot into the metrics these commits quote (serial lines, SEP
bring-up markers, endpoint traffic, exit counts) and has a `-c` compare mode. Use it: a change here can make
an error message disappear by stopping the guest *earlier*, so always read a positive progress metric next to
the failure strings.

Acceleration *is* supported for the **companion VM** (the Linux guest that runs `idevicerestore` and provides
USB/network to the main VM over `usb-tcp-remote`). The official guide's reference command uses
`qemu-system-x86_64`, which cannot use HVF on an Apple Silicon host — guest and host arch must match. On
Apple Silicon, run the companion as **aarch64 with `-accel hvf -cpu host`** instead:

```bash
qemu-system-aarch64 -M virt -accel hvf -cpu host -m 1G -smp 2 \
    -usb -device usb-ehci,id=ehci -device usb-tcp-remote,bus=ehci.0 \
    -drive file=Arch.qcow2,if=virtio \
    -nic user,model=virtio-net-pci,hostfwd=tcp::32222-:22
```

`CONFIG_USB_TCP=y` in `aarch64-softmmu-config-devices.mak`, so `usb-tcp-remote` is available in the aarch64
binary. Start the companion **before** the main VM; it is the socket server (defaults to
`/tmp/InfernoUSBRemote`, and leaves the socket file behind on exit — delete it if a later run complains).

For HVF the binary must carry the `com.apple.security.hypervisor` entitlement. `ninja` alone stops at
`qemu-system-aarch64-unsigned`; run `ninja qemu-system-aarch64` to get the codesigned binary
(`accel/hvf/entitlements.plist`), and verify with `codesign -d --entitlements - qemu-system-aarch64`.

## Architecture

### Boot pipeline (`hw/arm/apple-silicon/boot.c`, `dt.c`, `mem.c`)

`<machine>_init` runs at machine creation; the real memory setup happens later in `<machine>_init_done`
(an `init_done_notifier`) → `t8030_memory_setup()` → `t8030_cpu_reset()`.

1. `apple_boot_load_dt_file()` parses Apple's binary DeviceTree into an `AppleDTNode` tree (`dt.c` — Apple's
   format, *not* FDT; nodes are prop hash tables, `apple_dt_finalise`/`serialise` write it back for the guest).
2. `apple_boot_load_kernel()` parses the Mach-O kernelcache (fileset kexts, chained fixups) and
   `apple_boot_build_version()` gives the iOS major version, which selects protocol variants
   (e.g. `sio_protocol` 9 for iOS 13–16, 10 for 17/18/26). Adding OS support usually means extending these
   `switch (BUILD_VERSION_MAJOR(...))` sites.
3. Device inventory and creation order are explicit in each machine, while addresses, interrupts, and wiring
   come from the guest DeviceTree. Each `t8030_create_*`/`s8000_create_*` looks up its node (for example,
   `apple_dt_get_node(dt, "arm-io/uart0")`), reads `reg`/`interrupts`, maps MMIO, and wires GPIO/IRQ via
   `apple_dt_connect_function_prop_*`. If a device is missing, check both the creation sequence and DT name.
4. Memory is laid out with a **carveout allocator** (`mem.h`): `carveout_alloc_new` → `carveout_alloc_mem` per
   coprocessor region → `carveout_alloc_finalise` returns the kernel region size. `g_virt_base`/`g_phys_base`/
   `g_*_slide` back the `vtop_static`/`ptov_static`/`vtop_slid` helpers used everywhere.
5. `apple_boot_populate_dt` / `apple_boot_finalise_dt`, `apple_boot_setup_bootargs`, `apple_boot_load_ramdisk`,
   `apple_boot_load_raw_file` complete the boot; `apple_boot_allocate_segment_records` builds the memory map.

With `securerom=` the machine boots SecureROM/DFU instead of a kernelcache (different reset path).

### Kernel patching (`patcher.c`, `kernel_patches.c`)

`ck_patch_kernel(MachoHeader64 *)` (called from `t8030.c:247`) applies pattern-based binary patches. Use the
`ck_patcher_*` API: `ck_patcher_range_from_ptr` to define a searchable range, then
`ck_patcher_find_replace` / `ck_patcher_find_callback[_ctx]` with masked pattern+mask byte arrays, plus
`ck_patcher_find_next_insn` / `ck_patcher_find_prev_insn` for AArch64 instruction scanning. Patterns must be
masked. This is the mechanism for defeating kernel checks — add new patches here, not inline in `t8030.c`.

### Coprocessors: A7IOP / RTKit (`hw/misc/apple-silicon/a7iop/`)

Almost every Apple coprocessor is an A7IOP: a CPU-less mailbox block (`core.c`, `mailbox/`, `regs-v2.c` vs
`regs-v4.c` for the two register layouts, selected by `APPLE_A7IOP_V2`/`V4`) with the RTKit protocol on top
(`rtkit.c`). Consumers: `smc.c`, `aop.c`, `baseband.c`, `hw/dma/apple_sio.c`, `hw/block/apple-silicon/ans.c`.
When adding a coprocessor, subclass RTKit and register endpoint handlers rather than reimplementing mailboxes.

### SEP

- **`t8030` real SEP (`sep.c`)** runs SEPROM + SEPFW on an extra CPU core. It requires **both** `sep-rom=` and
  `sep-fw=`; specifying only one is fatal. `mc->max_cpus = A13_MAX_CPU + 1`, with the extra core assigned to SEP.
- **`t8030` simulated SEP (`sep-sim.c`)** is selected when neither file is given only if data encryption is
  disabled at compile time. The current default defines `ENABLE_DATA_ENCRYPTION` in
  `include/hw/arm/apple-silicon/boot.h`, so omitting the files is fatal and the simulated SEP is unavailable.
- **`s8000`** always creates the simulated SEP. Its exposed SEP file properties are not equivalent to the
  `t8030` real-SEP path; SEPFW loading there is still TODO.

Real SEP also allocates a long list of poorly-understood `SEP_UNKN*` RAM regions in `t8030_init`; those
comments are the reverse-engineering notes.

### CPU (`a13.c`, `a9.c`, `a13_gxf.c`, `target/arm/`)

`apple-a13-cpu` / `apple-a13-cluster` / `apple-a9-cpu` are custom ARM CPU types with Apple's `ARM64_REG_*HID*`
system registers (`A13_CPREG_VAR_DEF` macros in `a13.h`) and cluster-level IPI registers.

Apple-specific changes leak into upstream files — grep for `apple` there before assuming a file is vanilla:
- `target/arm/tcg/translate-a64.c` — `disas_apple_insn()` decodes `WKdmC`/`WKdmD` (WKdm compression) and
  `GENTER`/`GEXIT` (GXF guarded execution), hooked into `disas_a64_legacy`.
- `target/arm/cpu64.c` — `apple-gxf` CPU feature (`aarch64_apple_gxf_initfn`).
- `a13_gxf.c` implements GXF registers and guarded state; `target/arm/helper.c` handles `EXCP_GENTER`.

### IOMMU and other Apple devices

`dart.c` (Apple's IOMMU; `dart-stub.c` is compiled instead when `CONFIG_APPLE_DART` is off) and `sart.c`
(scatter-gather address relocation for ANS). Devices are spread across `hw/` by class, all prefixed `apple_`:
`hw/intc/apple_aic.c` (AIC), `hw/char/apple_uart.c`, `hw/i2c/apple_i2c.c`, `hw/spmi/apple_spmi*.c`,
`hw/ssi/apple_spi.c`, `hw/gpio/apple_gpio.c`, `hw/dma/apple_sio.c`, `hw/nvram/apple_nvram.c`,
`hw/watchdog/apple_wdt.c`, `hw/usb/apple_otg.c` + `apple_typec.c`, `hw/display/apple_displaypipe_v2|v4.c`,
`hw/misc/apple-silicon/` (SMC, AOP, AES, PMU/SPMI, buttons, temp sensors, regulators like `fan53740.c`),
`hw/audio/apple-silicon/` (MCA, AOP audio, CS35L27/CS42L77 codecs), `hw/block/apple-silicon/`
(ANS → NVMe via `nvme_mmu.c`), `hw/arm/apple-silicon/mt-spi.c` (multi-touch), `lm-backlight.c`.

Wiring is declared in `hw/arm/Kconfig` (`CONFIG_APPLE_SOC` selects all the `APPLE_*` symbols; it depends on
`TCG && ARM && AARCH64` — there is no KVM/HVF path for these machines) and the per-directory `meson.build`.

### USB over TCP (`hw/usb/tcp-usb.c`, `dev-tcp-remote.c`, `hcd-tcp.c`, `CONFIG_USB_TCP`)

`usb-tcp-remote` proxies the guest's USB device port to a host socket so tools like idevicerestore can talk to
the emulated device. Default UNIX socket `/tmp/InfernoUSBRemote`; the machine's `usb-conn-type`/`-addr`/`-port`
properties select unix/ipv4/ipv6.
