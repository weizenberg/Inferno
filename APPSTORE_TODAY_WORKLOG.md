# App Store Today worklog

## Objective

Run the iOS App Store application successfully in the emulated `t8030` guest
and verify that the Today tab loads real content.

Success requires direct runtime evidence of all of the following:

1. The restored, filesystem-patched iOS guest reaches SpringBoard.
2. The guest has working network access to the services used by App Store.
3. Touch input can unlock the guest and launch App Store.
4. App Store reaches the Today tab and renders fetched Today content rather
   than a spinner, blank view, cached shell, or network-error page.
5. A framebuffer capture and relevant logs document the successful state.

## Repository and runtime state inspected

- Branch: `hvf-apple-gxf`
- Head: `33f28507b1` (`sep: stop the guest aborting QEMU from the AESS
  registers, and pin keywrap`)
- Build products are present in `build/`, including a codesigned
  `qemu-system-aarch64`.
- Runtime artifacts are under `/Users/weizenberg/InfernoData`.
- The pristine post-migration namespace set is `stage2-migrated/`.
- Existing writable clones include `play/`, `bench-a/`, and other benchmark
  directories.
- The current working tree contains active, unrelated HVF/DART performance
  experiments. They have not been overwritten, removed, staged, or reverted.

## Verified baseline

The guest can boot through userspace and render the lock screen under HVF.

Evidence:

- `/Users/weizenberg/InfernoData/s1-monitor-shot6.png` shows the iOS 14 lock
  screen at 828 x 1792 with wallpaper, live date/time, battery status, and a
  setup notification.
- The corresponding VM was confirmed as `running` through its HMP monitor.
- The VM used a cloned namespace (`bench-a/`), not the pristine
  `stage2-migrated/` source.
- A companion aarch64 Linux VM is running under HVF and owns the default
  `/tmp/InfernoUSBRemote` USB-over-TCP server socket.

The guest was subsequently driven through SpringBoard, Settings, Apple ID
sign-in UI, and App Store with QMP touch input. This proves boot, display, and
input. Today content is still not proven.

## Input-control decision

Use QMP `input-send-event` with absolute X/Y axes and a left-button
press/release to automate touch input.

Reasoning:

- `hw/arm/apple-silicon/mt-spi.c` registers the Apple multitouch controller as
  an absolute QEMU mouse handler.
- It scales QEMU's absolute range (`0..0x7fff`) into the modeled touch sensor
  surface and turns left-button transitions into touch make/break reports.
- QMP supports atomic absolute-axis plus button events. This is more
  reproducible than host GUI automation and works with a headless display.
- The existing HMP-only benchmark VM cannot receive QMP commands, so a
  dedicated run should expose a QMP Unix socket.

## Networking decision

Connect the dedicated main VM to the companion's existing
`/tmp/InfernoUSBRemote` socket, because the Apple machine has no generic QEMU
NIC and receives connectivity through its emulated USB device path.

The reverse-tethering path has now been verified end to end:

- the companion uplink, DNS, and Apple HTTPS access work;
- the companion uses `USBMUXD_DEFAULT_DEVICE_MODE=3`;
- the required USB network drivers are loaded;
- the iPhone enumerates with Apple USB Ethernet/NCM;
- pairing/trust was re-established after a companion restart; and
- guest DNS, TCP, and TLS traffic was observed reaching Apple services.

Therefore the App Store's current `401`/anisette behavior is not a generic
network-connectivity failure.

## Namespace and process-safety decision

- Do not boot `stage2-migrated/` directly.
- Use an APFS clone for the dedicated App Store run.
- Do not reuse benchmark namespace files while their VM is running.
- Do not terminate the active companion or benchmark jobs unless their exact
  ownership and purpose are resolved.
- Capture serial, QEMU, QMP, and framebuffer artifacts under a unique
  App Store run tag.

## Existing experiments intentionally left untouched

The worktree currently includes an uncommitted page-granular SEP DART DMA
mirror, firmware preflight checks, and a cheaper HVF fallback-state snapshot.
These changes target performance and startup diagnostics. There is not yet
evidence that they are required for App Store Today, so the App Store run will
first establish behavior with the currently built binary and will not rewrite
those experiments speculatively.

## Acceleration assessment

### Blocking compatibility gap

The App Store was initially a **TCG-only workload**, even though HVF booted the
device and reached SpringBoard.

An App Store crash report captured over the USB link shows JavaScriptCore
dying with `SIGILL` while executing:

```text
msr S3_6_c15_c1_5
```

This is an Apple private SPRR write-protection register used by JSC's JIT
write-protection path. TCG models the relevant Apple behavior; under HVF the
host does not implement the register and Hypervisor.framework provides no API
to force-trap its encoding into QEMU. The instruction reaches the guest as an
undefined instruction before QEMU can emulate it.

This was the highest-priority acceleration problem for App Store. Reducing HVF
boot time could not help Today while the app could not stay alive under HVF.

### SPRR/HVF decision and implementation

Investigation established the exact Hypervisor.framework behavior with the
standalone `scripts/hvf-sprr-probe.c`:

1. `hv_vcpu_get_sys_reg()` accepts the raw architectural ID for
   `SPRR_EL0BR0_EL1` and returns a saved value.
2. That does not make the register executable by the guest. An EL0
   `msr S3_6_c15_c1_5, x0` still enters the guest's lower-EL synchronous vector
   with `ESR_EL1.EC = 0x00` (`UNDEFINED`).
3. Hypervisor.framework's private
   `_hv_vcpu_config_set_fgt_enabled(..., true)` switch was also tested. Fine
   grained traps do not cover this implementation-defined encoding; behavior
   remained `UNDEFINED`.
4. Mapping the test code without stage-2 execute permission causes an HVF exit
   with syndrome `0x82000006` and the exact instruction IPA.
5. Replacing only instruction word `0xd51ef1a0` with AArch64 `NOP`, flushing
   the host instruction cache, and granting execute permission makes the same
   EL0 test reach `SVC` (`ESR_EL1.EC = 0x15`) instead of `UNDEFINED`.

The selected implementation is therefore a transient execute-page
compatibility pass:

- `t8030` and `s8000` enable SPRR compatibility only when HVF is active.
- HVF initially maps otherwise executable guest RAM without execute
  permission and tracks execution at host-page granularity.
- On a page's first instruction fetch, QEMU scans EL0 pages for the exact
  `SPRR_EL0BR0_EL1` write encoding (with any source register), replaces it with
  `NOP` in transient guest RAM, flushes the instruction cache, and grants that
  page its original execute permission.
- EL1 pages are granted execute permission without scanning or modification.
- A per-slot bitmap ensures only compatibility-generated first-execute faults
  are consumed; later genuine instruction aborts are still injected into the
  guest.
- Dirty-log permission changes retain the per-page execute state.

This does not alter or persist changes to any disk image, firmware, kernel, or
dyld-cache file. Treating the write as a no-op matches the effective HVF
memory model: HVF does not enforce the guest's SPRR permissions in its hardware
page-table walk, so changing the shadow permission value would not change
access rights.

Rejected alternatives:

- QEMU's existing system-register fallback cannot help because HVF never
  reports this access as `EC_SYSTEMREGISTERTRAP`.
- `HCR_EL2.TIDCP` does not apply to this EL0 access.
- Hypervisor.framework's private fine-grained-trap configuration does not trap
  this encoding.
- A fixed virtual-address hardware breakpoint would depend on the dyld shared
  cache slide and the exact iOS build.
- Persistently patching a guest image or dyld cache is unnecessary and is
  outside the project's explicit file-handling boundary.

Build validation:

- `ninja -C build qemu-system-aarch64` succeeds.
- The standalone probe proves both the pre-fix `UNDEFINED` result and the
  post-compatibility `SVC` result.

Runtime validation:

- A dedicated headless `t8030` HVF run used the existing writable `bench-a/`
  namespace. No disk image, firmware, kernel, or dyld-cache file was modified
  by the compatibility code.
- The QEMU log reported exactly:
  `HVF: replaced 2 unsupported SPRR writes in executable guest page
  0x825434000`.
- The guest completed boot, reached the lock screen, unlocked to SpringBoard,
  and launched App Store.
- App Store remained alive beyond the prior JavaScriptCore crash window and
  rendered its expected offline result: `You must connect to a Wi-Fi or mobile
  data network to access the App Store`. The isolated validation VM
  intentionally had no companion USB network.
- `/private/tmp/sprr-hvf-appstore-try2.png` is the framebuffer evidence for the
  surviving App Store process. Network/Today provisioning remains separate
  from the resolved HVF SPRR compatibility failure.

The compatibility pass deliberately trades startup work for correctness: each
guest host-page that executes for the first time causes one HVF exit before it
is made executable. The boot and App Store run validate correctness, but there
is not yet a controlled before/after timing for this new cost. A future
optimization should narrow trapping only when it can identify all relevant
userspace shared-cache mappings without fixed virtual addresses or a
build-specific “two writes found” assumption. Until then, keeping the
page-granular pass is safer than silently reintroducing SIGILL on another iOS
build.

### Measured micro-optimizations

The uncommitted `snapshot_state` optimization avoids immediately writing an
unchanged full vCPU snapshot back to HVF during fallback MMIO emulation.
Four fixed-progress CPU-time runs reached approximately 800 serial lines:

| run | baseline | optimized |
|---|---:|---:|
| 1 | 2:08.78 | 2:08.68 |
| 2 | 2:02.52 | 2:00.92 |
| mean | 125.65 s | 124.80 s |

The measured gain is about **0.85 seconds / 0.7%**. It is small but the code
change is also small. Keep it only after auditing the invariant that no path
writes meaningful `CPUARMState` between the read-only snapshot and exit
completion; otherwise clearing `vcpu_dirty` can silently discard state.

The page-granular SEP DART DMA mirror is not justified:

- it adds roughly 330 lines plus a machine property and memory-slot lifecycle;
- only 17 of 2048 SEP-window pages are mapped and they are noncontiguous; and
- measured fallback count changed only 617,943 to 614,038, about **0.6%**.

Recommendation: do not land it as a default-on optimization. The complexity
and memory-mapping risk outweigh the measured gain.

### Where a material HVF win could exist

A measured boot had about 2.62 million HVF data-abort exits. The largest
regions were:

| region | exits |
|---|---:|
| SEP A7IOP mailbox/registers | 930,592 |
| SEP AES engine | 439,634 |
| Apple UART | 151,640 |
| SEP I2C | 50,962 |

The useful performance work is therefore:

1. Profile and reduce SEP mailbox round trips.
2. Add a correctness-preserving bulk path for repeated SEP AES register
   traffic.
3. Disable or reduce `serial=3` logging for non-diagnostic runs.
4. Consider batching a proven side-effect-free tight MMIO loop, but only after
   tracing exact addresses and device semantics.

Do not expose side-effecting register pages as RAM merely to avoid exits. The
SEP DMA mirror experiment already demonstrated that broad mapping shortcuts
are low-yield and high-risk.

## Next actions

1. Re-run the App Store Today retry while tracing AuthKit/AMS anisette and
   token-service status, ensuring the UI actually triggered a fresh request.
2. Determine whether the prior Apple ID sign-in attempt provisioned enough
   local anisette state for the App Store's fetch-only caller.
3. If it still returns `-45061`/401, continue the non-forging provisioning
   investigation; do not misclassify it as a network or acceleration failure.
4. Capture the populated Today framebuffer plus the successful token/content
   responses before marking the objective complete.

## Completion status

The HVF SPRR fix is complete and runtime-validated. The broader Today objective
is not complete: SpringBoard, input, App Store launch, and guest Internet access
are proven, but successful token provisioning and rendered Today content are
not yet proven.
