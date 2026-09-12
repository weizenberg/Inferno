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

Initial App Store crash reports captured over the USB link showed
JavaScriptCore dying with `SIGILL` while executing:

```text
msr S3_6_c15_c1_5
```

After the write-only compatibility pass allowed App Store to progress further,
a networked HVF run captured the matching unsupported read:

```text
Exception Type:  EXC_BAD_INSTRUCTION (SIGILL)
Exception Codes: 0x0000000000000001, 0x00000000d53ef1a9
Thread 4 Crashed:
0   libsystem_pthread.dylib
1   JavaScriptCore
```

`0xd53ef1a9` is `mrs x9, S3_6_c15_c1_5`. This is an Apple private SPRR
write-protection register used by JSC's JIT write-protection path. TCG models
the relevant Apple behavior; under HVF the host does not implement the register
and Hypervisor.framework provides no API to force-trap either encoding into
QEMU. The instructions reach the guest as undefined instructions before QEMU
can emulate them.

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
  `SPRR_EL0BR0_EL1` read/write encodings (with any general-purpose register).
  Writes become `NOP`. For reads, QEMU recognizes libsystem_pthread's preceding
  commpage load and moves that expected mode into the read destination
  register. This preserves the helper's readback validation while SPRR itself
  is unenforced. QEMU then flushes the instruction cache and grants that page
  its original execute permission.
- EL1 pages are granted execute permission without scanning or modification.
- A per-slot bitmap ensures only compatibility-generated first-execute faults
  are consumed; later genuine instruction aborts are still injected into the
  guest.
- Dirty-log permission changes retain the per-page execute state.

This does not alter or persist changes to any disk image, firmware, kernel, or
dyld-cache file. Treating writes as no-ops and reads as the expected commpage
mode matches the effective HVF memory model: HVF does not enforce the guest's
SPRR permissions in its hardware page-table walk, so changing or observing the
shadow permission value would not change access rights. Returning zero was
explicitly rejected after runtime validation: libsystem_pthread deliberately
executes `BRK #1` when its readback does not contain the expected mode.

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

## 2026-07-30 run (wlan-netdev merge + USB NCM tether)

Run tag `goal` (`run-goal.sh` in InfernoData): same t8030 HVF command as the
`s2a` run, `sprr-x/` namespace, USB remote symlinked to the companion's
`/tmp/InfernoUSBRemote`. Findings:

- The merged WLAN path associates and gets a slirp DHCP lease (10.0.7.15),
  but its RX path is broken in practice: the guest serial floods with
  `rxCompRingDrain@965:failed, try again, rx buffer request fail 0xe00002be`
  and wifid LQA reports Rssi 0 / RxFrms 0 / BcnPer 90%. The Wi-Fi icon drops
  from the status bar after a few minutes.
- The USB NCM reverse tether (companion `enxdeadbeef22ed`,
  systemd-networkd `50-inferno-iphone.network`, DHCPServer + IPMasquerade)
  is up; the guest leases 192.168.178.144 and answers ping. App Store TLS
  traffic to Apple completes (HTTP responses observed), so the guest has
  working Internet despite the broken WLAN.
- Re-paired lockdownd (`idevicepair` + Trust dialog); `idevicesyslog` and
  pymobiledevice3 work over the USB link.
- App Store launches (SPR fix holds) and the Today tab shows
  `Cannot Connect to App Store / Retry`. The Retry tap triggers a fresh
  fetch; syslog shows the exact same chain as 2026-07-28:
  `AMSAnisette: Failed to generate Anisette (type: 2) headers. error =
  -45061`, then `sf-api-token-service.itunes.apple.com/apiToken` returns
  HTTP 401 (AMSErrorDomain 301), JetEngine shows the generic error page for
  `viewToday?cc=il`. `AMSAbsinthe: Skipping absinthe`; no akd involvement at
  all on the AMS path (fetch-only caller, provision=NO).
- Research (see session): `-45061` is ADI `notProvisioned`; provisioning is
  a CoreADI SPIM/CPIM exchange with gsa.apple.com that Apple accepts with
  synthetic identifiers (libprovision precedent), but nothing on this guest
  triggers `akd` provisioning. The one caller that would (Apple ID sign-in)
  dies earlier at absinthe NAC init (`AAAbsintheError=-42052`, after an
  HTTP 200 from Apple, i.e. a *local* FairPlay-layer failure, plausibly
  missing activation-record FairPlayKeyData). apsd confirms no activation
  identity in the keychain (`-25300`), and akd's sign-in stalls at
  `No APS public token`.
- `mobileactivationd` reports `Activated` (bypass state from the restored
  image, not an albert-issued record). A `CreateActivationInfoRequest` over
  lockdownd fails with `Failed to establish session ... Invalid input` in
  `handleSessionResponse` — the tunnel1 activation session with albert does
  not even get created.
- akd's own anisette fetch during the 2026-07-28 sign-in attempt DID
  succeed (`Remote Anisette service returned Anisette data`) yet AMS still
  saw -45061 afterwards, so akd's remote-service anisette does not imply
  ADI state provisioned for the AMS type-2 caller.
- UI note: QMP home/app-switcher gestures do not commit in this emulation
  (flicks rubber-band; drag-to-rest works for the lock screen only).
  Navigation between apps currently requires a VM restart or working from
  the home screen right after unlock.

## 2026-07-30, hactivation=false experiment

Per maintainer direction, the goal is the direct path: a normally-activated
device opening App Store, no workarounds. `hactivation` defaults to true and
is what makes the guest report `Activated` without an albert-issued record
(`boot.c` sets the `allow-hactivation` DT property and
`kernel_patches.c:659` flips launchd into internal/profile-build mode). The
property description says "Set to false to require real device activation."
Working hypothesis: the hactivation state is exactly what blocks the
activation record (and therefore FairPlayKeyData, apsd identity, absinthe
NAC, and akd's ADI provisioning). Rebooted the same `sprr-x/` namespace with
`-M t8030,hactivation=false,...` to observe the activation flow.

Results:

- The first hactivation=false boot died in a flaky kernel
  `Spinlock timeout` panic (~115 s, after WLAN init); the retry booted
  cleanly. Watch boot time for recurrence.
- With hactivation=false the guest boots into **Setup Assistant** (Hello /
  Software Update Complete screens) instead of the lock screen — the real
  activation path.
- Activation fails locally, before any server contact:
  `createTunnel1SessionInfoWithCompletionBlock: Failed to copy ingest data:
  Failed to collect SIK. Failed to perform SIK collection: 0xe00002bc`.
  Setup shows "Unable to Activate ... activation server cannot be reached",
  which is misleading; the failure is on-device.
- The guest kernel log at the same moment shows an AppleCredentialManager
  session for mobileactivationd's uid (SEP cmd 36/19/32 succeed), then
  `AppleSEPKeyStore: operation failed (sel: 43 ret: e007c00a)` and
  `(sel: 43 ret: e00002bc)`. `com.apple.keystore.sik.access` is the
  entitlement mobileactivationd holds for this, so SIK collection is an
  AppleSEPKeyStore operation (selector 43) that the emulated SEP cannot
  currently satisfy. Rootfs is mounted read-only on the host for RE of the
  exact SIK call chain.

### SIK call chain (IDA-verified, 2026-07-30)

- `-[MobileActivationDaemon createTunnel1SessionInfoWithCompletionBlock:]`
  → `-[MACollectionInterface copyIngestData:]` → `collectSIK:` in
  mobileactivationd (the framework is only an XPC client).
- `collectSIK:` (HasPKA path) calls
  `IOConnectCallMethod(AppleKeyStore user client, selector 43,
  scalar = -1, structIn = 31 09 30 07 0c 01 6f 0c 03 6f 73 63)`.
  The struct is DER `SET{SEQ{"o"="osc"}}` — SEP command "osc", no
  parameters, output is a ≤32 KiB DER blob.
- The blob is opaque to userspace: base64'd into the ingest body posted to
  `https://tbsc.apple.com/ingest/register`. Only Apple's server validates
  it. A self-consistent blob would pass the local failure; the server is
  the remaining gate.
- The SEPFW image (extracted to `/tmp/ida-sik/sepfw.bin`) has a command
  table at ~0x54bb00 with `osc` (0x54bb81) and `sikp` (0x54bbd1) among
  other ops. Error 0xe007c00a is the SEP/ACM rejection family, then
  kIOReturnUnsupported (0xe00002bc) on the retry. Why SEPFW rejects "osc"
  on a fresh emulated SEP is under analysis (likely a missing provisioning
  step or a PKA operation the emulation does not implement — PKA commands
  in `sep.c` are interrupt-ACK stubs, and `pka_handle_cmd` knows
  `MPKA_ECPUB_ATTEST` (0x80) but computes nothing).
- Mailbox tracing (`apple_a7iop_mailbox_send/recv`) works at runtime via
  HMP `trace-event`; the SEP debug-trace buffer
  (`SEP_ENABLE_TRACE_BUFFER`) and `sep.c` `DPRINTF` need a rebuild; two
  pre-existing compile errors (`func` vs `__func__` at sep.c:4743/4754)
  were fixed to make that build possible.

### The exact SEPFW failure sequence (DPRINTF capture, 2026-07-30)

Each activation attempt (two per Try Again, matching the two sel-43
errors) runs this SEP peripheral sequence:

1. AESS: SEPFW writes a 32-byte block at AESS regs 0x40-0x5c whose last
   word is ASCII ` sks`, then issues command word 0x13 =
   flag 0x10 | `SEP_AESS_COMMAND_CREATE_KEY_FROM_SEED` (0x3).
   QEMU's `aess_handle_cmd` has no case for it ("Unknown command 0x13"),
   so the derived-key output words (0x60-0x7c) read back as zeros.
2. PKA: SEPFW issues command 0x80 (`MPKA_ECPUB_ATTEST`) and does only a
   status handshake (no data-buffer MMIO). QEMU's `pka_handle_cmd` merely
   ACKs interrupts; no EC math is performed and no public key is produced.
3. SEPFW then fails the "osc" command back to the guest
   (0xe007c00a, then 0xe00002bc on the retry).

So the emulation gaps are precisely: AESS CREATE_KEY_FROM_SEED (0x3) and
PKA ECPUB_ATTEST (0x80). The SIK is a SEP-derived identity: derived seed
key (" sks" label) → EC public key/attestation via the PKA. Both need the
exact SEPFW-side layouts (from the SEPFW binary, command table at
~0x54bb00, `osc` handler) to implement correctly; nettle (already a build
dependency) provides the EC primitives.

### SEPFW "osc" handler analysis (IDA, 2026-07-30)

- `sepfw.bin` is a multi-image payload (L4 kernel + 16 task Mach-Os).
  Pointers in `__const`/`__data` are stored zeroed and populated by
  load-time fixups (DRGKCATS region), so static name→handler xrefs do not
  exist in the file.
- The command server is the `AppleKeyStore_SEP` image; the SIK handler
  (identified behaviorally) has sub-ops: generate key + build X.509
  "SEP Device Certification" cert, export stored blob, attest/sign with
  nonce. Signing uses the **"sep sub ca key"** fetched from the xART
  object store via IPC (tag "acss"), unless config word 17295 (0x438F)
  bit0 is set, in which case a built-in 20-byte dev key
  (`xmmword_54BDA1`) is used.
- `0xe007c00a`/`0xe00002bc` are AP-side mappings of the SEP's negative
  reply; the transport is fine (other ACM commands succeed).
- Root cause: the emulated SEP boots with an empty xART/effaceable store
  and no factory provisioning (`sikp` never ran), so the
  "sep sub ca key" fetch fails and "osc" errors out.
- Candidate fixes, in fidelity order: seed the xART store with a
  synthetic Sub-CA/SIK pair; intercept the xART read in QEMU; or emulate
  the 0x438F-bit0 config override (smallest change if the config table
  source is host-controllable — under analysis).

### The SIK fix (2026-07-30)

Deeper SEPFW analysis (legion2 module table) located the config records:
`sub_4EC200(id, val)` scans 48-byte records at "pass"/"scrd" image data
(payload offsets 0x6ec020 and 0x6e8018), layout
`{u32 id; u64 v1..v4; u8 flag}`; the XPRT query that would populate them
at boot is not handled for 0x438F on this emulator, so injected values
persist. `t8030.c` now patches the two zeroed 0x438F records in the loaded
SEPFW image (v1=1, flag=1) **in memory only** — the firmware file on disk
stays pristine. With the override, the SIK handler signs with the
built-in dev key instead of failing the xART fetch. The xART store itself
is NAND-backed via SEP_Storage/dxio (not the AP filesystem), so direct
blob planting was not an option; factory xART seeding remains the
high-fidelity future work.

Follow-up: the img4 payload is hash-locked (the ROM verifies it during
INIT/BOOT) — any file-level patch panics SEPOS (`SEP Panic: :INIT/BOOT:
0x000137aa`). The override therefore moved to a runtime RAM patch
(`sep_sik_devkey_override` in `sep.c`). First attempts scanned the wrong
regions (the sep-dma-downstream staging area, and the 0x815000000
carveout). Ground truth via QMP `pmemsave` + pattern search: **SEPOS
loads the payload at GPA 0x340000000**, so the "pass" config record sits
at GPA 0x3406ec020 and the "scrd" copy at 0x3406e8018 (payload offsets
identity-mapped). The patch now targets those GPAs directly at command
time (AESS CREATE_KEY_FROM_SEED / PKA ECPUB_ATTEST hooks), which dodges
the load-time manifest verification. The disabled `t8030.c` image patch
remains in the tree, disabled, with the panic evidence in its comment.

### Full "osc" crypto path (SEPD contract, from SEPFW RE)

- AESS cmd 0x13 = flag 0x10 | CREATE_KEY_FROM_SEED: input 32 B at regs
  0x40-0x5c = {28-byte context, 4-byte label (" sks")}, output 32 B at
  0x60-0x7c = a wrap key for the key object. Emulated as
  AES-256-ECB(AESS_UID0, input) — deterministic per device; SEPFW bails
  early on the previous all-zero result.
- PKA cmd 0x80 = ECPUB_ATTEST: input is a RAM descriptor at GPA
  0x34055D870 ({u32 len, data}) with the 32-byte P-256 private scalar;
  output at GPA 0x34055D878 is {u32 status, u8 flag, pad, u32 blob_len,
  blob}. Emulated with nettle secp256r1: pubkey = scalar·G, returned as
  {0, 1, 65, 0x04‖X‖Y}; the attestation blob is opaque to the caller.
- Ordering in the "osc" handler: AESS cmd3 → PKA 0x80 → config-flag
  check (cert builder), so a config override applied at AESS-cmd3 time
  is early enough.

### The 0x438F override is a dead end (proven, 2026-07-31)

- With the runtime config patch applied (verified bytes), the handler
  takes the dev-key path but STILL fails identically. SEPFW RE shows the
  sign-time compute (`sub_5112C4`) only accepts storage-backed key
  objects (`obj+352 == 0`); the dev-key object has the seed embedded at
  obj+352 and is rejected with -10 before any crypto runs. No
  AESS/PKA emulation can fix that branch.
- The AESS cmd-0x13 result is never read by software (only the SEPD
  reply's status dword == 1 is checked); my AES-ECB(UID) KDF is fine.
  The PKA ECPUB_ATTEST result is benign (zeros are success on the
  simple form, and the dev-key path skips the attestation branch).
- The only viable "osc" path is the xART/skg "sep sub ca key" fetch
  (0x438F == 0), which builds a storage-backed object. That store is
  SEP-internal (pass → L4 IPC → skg "acss" → Lynx FTL → dxio → NAND),
  not mailbox-interceptable; a synthetic record must be seeded through
  the store-write path (op 20015, UID-wrapped). "sikp" takes
  caller-provided material (synthetic OK) but is mode-gated and its DER
  format is framework-parsed (not fully recoverable statically).
- Server side caveat: even with SIK, activation must pass
  tbsc.apple.com/albert with the emulated device's synthetic
  serial/MLB/ECID/FairPlay identity — every public data point
  (Corellium, Apple Silicon VMs) says Apple services reject such
  devices. Whether tbsc's structural check accepts a synthetic SIK cert
  is the open question that decides whether the whole chain can ever
  complete; the App Store media-token service (gsa/adjacent) DOES
  accept synthetic ADI provisioning per the libprovision precedent.

## Completion status

The HVF SPRR fix is complete and runtime-validated. The broader Today objective
is not complete: SpringBoard, input, App Store launch, and guest Internet access
are proven, but successful token provisioning and rendered Today content are
not yet proven.

## 2026-07-31, AMS/akd RE conclusion + provisioning-trigger plan

Full AuthKit/AMS/akd RE (via IDA) settled the anisette path:

- AMS type-2 anisette (the apiToken caller) is DSID-free and machine-global;
  one successful ADI provisioning from any source satisfies it.
- The type-2 fetch is read-only (provision=NO); provisioning only
  self-triggers if the apiToken 401 carries an anisette *action*, which it
  does not. Nothing self-heals.
- Every stock UI trigger (iCloud/App Store/Game Center sign-in) gates on
  GSA->absinthe NAC init (-42052) *before* provisioning runs, and needs a
  real Apple ID besides. Game Center is NOT an absinthe-free path.
- The absinthe-free provisioner is AuthKit's
  AKAnisetteProvisioningController -> akd SPIM/CPIM exchange with
  gsa.apple.com/grandslam (device attestation, accepts synthetic client
  data; libprovision precedent).

Plan: inject a one-shot root helper into the rootfs (hdiutil attach +
/System/Library/xpc/launchd.plist LaunchDaemons entry, same flow as
apply-fs-patches.sh) that calls the AuthKit provisioning API at boot with
retries until the NCM tether is up, then verify SPIM/CPIM in the pcap and
AMSAnisette success + apiToken 200 + rendered Today content. The kernel
already patches AMFI ("all binaries in TrustCache") and bypasses the SSV
root hash, so an adhoc/ldid-signed helper can exec; whether amfid honors
ldid entitlements for akd's XPC entitlement check is under RE.

Runtime notes: the touch chip wedged during one boot's firmware-load phase
(`AppleHIDTransportDeviceSPI: Couldn't talk to chip`, serial line ~644) --
touch was dead for the whole boot; only a reboot recovers. The following
boot hit an intermittent `SEP Panic: :sars/sars: 0x0002a310` (possibly
dirty sep_ssc after kill -TERM); the next boot was clean.

## 2026-07-31, akd entitlement gate + helper built (IDA RE)

Second RE pass (IDA, akd + AuthKit) produced the concrete helper spec:

- akd's provisioning endpoint gates on `-[AKClient hasInternalPrivateAccess]`;
  least-privilege key is **com.apple.authkit.client.private=true** (internal
  also works). Root uid is fine; per-method check, not per-connection.
- Mach service is **com.apple.akd.anisette** (NOT com.apple.ak.anisette.xpc);
  server protocol AKAnisetteProvisioningDaemonProtocol:
  `-fetchAnisetteDataAndProvisionIfNecessary:(BOOL) device:(id) completion:
  (void(^)(NSDictionary*,NSError*))` — device may be nil (DSID-free path).
- Confirmed the SPIM->gsa lookup->CPIM->persist flow has NO absinthe/NAC
  involvement (absinthe only exists in the signing path).
- With the agm-* boot args, adhoc ldid-signed binaries with custom
  entitlements run; amfid honors them.

Helper: `InfernoData/provhelper/inferno-prov.m` — raw NSXPCConnection to
com.apple.akd.anisette (initWithMachServiceName:options: is SPI, invoked via
objc_msgSend), retries 60x15s until the tether is up, pumps the runloop for
the async reply, logs to unified log + /var/mobile/Media/inferno-prov.log.
Built arm64 iphoneos14.0, ldid-signed with com.apple.authkit.client.private+
internal. `inject-prov.sh` installs it as /usr/libexec/inferno-prov with a
com.inferno.prov launchd entry (RunAtLoad, root).

Touch regression found + fixed: the three dead-touch boots all had SEP DPRINTF
enabled (#if 1 in sep.c) flooding stderr (57 MB/15 min), stalling the main
loop enough for the touch firmware load to time out (13.5 s, then
`Couldn't talk to chip`). Disabled DPRINTF, rebuilt, goal-run.out is now 3 KB.

## 2026-07-31, touch regression investigation (goal-x dead touch)

Touch stopped committing on every goal-x boot (20+ unlock drags, torch
long-press, horizontal drags all ignored) while it worked on 2026-07-30
(sprr-x). Findings so far:

- The SEP DPRINTF flood hypothesis was tested and rejected: with DPRINTF
  off the boot still logs the boot-time `Couldn't talk to chip` and touch
  stays dead. That boot-time error is universal across ALL full boots
  (goal/s2a/s1c logs) and is followed by `AppleMultitouchHIDService Start
  succeeded`, so it is benign.
- The mt-spi trace points (apple_mt_spi_mouse/path) prove QEMU-side
  delivery is intact: abs events scale to sane sensor coordinates, path
  stages MAKE_TOUCH/TOUCHING/BREAK_TOUCH/OUT_OF_RANGE generate with sane
  velocities, and the queue drains ("1 queued"), i.e. the guest driver
  fetches the reports. The guest just never acts on them.
- One anomaly: a touch starting in the bottom ~16 px of the display wraps
  s->y (uint16 underflow after the hardcoded 16 px calibration subtract,
  e.g. MAKE_TOUCH at y=65458). The calibration is from 2025, so it cannot
  be the regression by itself, but it means unlock drags starting at
  y=1786 always open with one off-surface point.
- AIDReporter errors are identical (15) in good and bad logs.

A/B in progress: boot sprr-x (last known-good-touch namespace) with the
current binary to split guest-state vs binary causes.

## 2026-07-31, touch bisect: sep.c exonerated, SPRR-read patch is the suspect

- A/B: touch is dead on sprr-x (last known-good namespace, 07-30 ~14:07)
  with the current binary -> regression is binary/host, not goal-x state.
- sep.c@HEAD (AESS/PKA/trace-buffer reverted): touch still dead -> sep.c
  exonerated.
- File mtimes isolate the regression window: hvf.c, hvf-all.c, boot.c,
  boot.h, t8030.h, kernel_patches.c, mt-spi.c were ALL modified 07-30
  14:35-14:37, AFTER the last good-touch session (~14:07). mt-spi.c has no
  content diff (spurious touch). boot.c/boot.h/t8030.h/kernel_patches.c are
  inert hactivation gating (with hactivation=true the behavior is identical
  to the good session).
- The only functional change in the window is the SPRR *read* patch
  (uncommitted hvf-all.c + hvf.c): replaces `mrs Xd, SPRR_EL0BR0` with a
  MOV from the preceding commpage LDR's destination, in every executable
  page on first fetch. 2 reads + 2 writes patched in one guest page per
  boot. This runs over all executable pages incl. userspace, so a misfire
  could break any process (e.g. backboardd touch routing).
- Testing now: hvf.c + hvf-all.c reverted to HEAD (write-only SPRR
  handling, as in the good session). Backups in /tmp/*.mine.

## 2026-07-31, touch bisect part 2: binary and namespace both exonerated

- SPRR-read patch reverted (hvf.c/hvf-all.c@HEAD): touch still dead.
  Together with the sep.c@HEAD result this exonerates the whole binary;
  the remaining uncommitted diffs are inert hactivation plumbing
  (verified: with hactivation=true they match the good-era behavior).
- Touch was still working on 07-30 EVENING (s1c/s2a boots, ~15:30): the
  hactivation=false Setup "Try Again" was tapped repeatedly after the
  14:35-14:37 changes, so the regression window is 07-30 ~16:00 ->
  07-31 ~05:30, and contains only my sep.c/t8030.c SIK edits (both
  revert-tested dead).
- bench-a (07-28 known-good-touch namespace): also dead today (40 spaced
  unlock attempts + lag-corrected torch test). Three namespaces dead ->
  not guest state.
- Host did not reboot (uptime 20 days), touch.py unchanged since 07-30,
  and s2a's touch-driver boot sequence is byte-identical to today's logs
  (same 12-13 s firmware load, same fatal chip error, same AIDReporter
  counts). The boot-time error is universal in every full-boot log.
- QEMU-side delivery re-verified by trace: correct coords/stages,
  well-formed reports, and the pending_fw queue drains (guest fetches).
  So the guest reads the reports but nothing acts on them. Syslog access
  secured via the surviving sprr-x<->companion pairing (no Trust tap
  needed) to watch the guest when reports arrive.

Open hypotheses: (a) the boot-time driver fatal error sometimes does not
recover and the fetch-drain is the driver's error-state polling that
discards packets; (b) AppleMultitouchHIDService/backboardd event routing
broke at runtime (syslog will show); (c) HVF timing skew making every
gesture read as an invalid pattern to the recognizer.

## 2026-07-31, touch root cause: DHML hover-lock + mt-spi fidelity fix

Live syslog from the surviving sprr-x<->companion pairing showed touches
DO reach backboardd: `F1: Touching`, `touchstreams: start sending
isFirstDown:true` -- but MultitouchHID's DHML logs
`P1 Hover (stage MakeTouch -- ignoring motion)` and
`Wating for slide, ZInstability=0.000000, TimeInstability=0.135256`, so all
motion is discarded and no gesture commits (taps also die at UI level).
This is why unlock/torch/banner all failed on every 07-31 boot.

IDA RE (MultitouchHID.plugin, MTHandMotion/MTParserPath): slide validation
gates on (a) a timestamp/cadence window, (b) contact shape, (c) Z-signal
instability (frame-to-frame change of the contact ellipse area), (d) time
instability, (e) slide velocity (~40/50 sensor-unit constants). Our reports
send CONSTANT rad0=660/rad1=580 (flat Z -> ZInstability 0) and timestamps
stamped from the host-scheduled virtual clock (jittery cadence), so the
DHML can never confirm a real finger's slide. Why it worked on 07-28/30
with the same report shape is still unexplained (per-boot classifier state
may have been more permissive then), but the fix is the same either way.

mt-spi.c change (this build): report timestamps are emitted on a fixed
50 ms grid anchored at MakeTouch (hardware-style sample cadence), and
rad0/rad1 walk smoothly +/-2% per frame instead of staying constant.
Velocity/position math unchanged. Testing on sprr-x with live DHML syslog.

Filesystem access note: the root image uses 4K sectors, so plain hdiutil
mis-parses the GPT; extracting the APFS container (GPT at byte 4096,
partition 0 at byte 24576) and attaching that mounts /Volumes/System
read-only. The Data volume is APFS-encrypted (offline DHML-state reset is
impossible). RW remount of System needs root -- the inferno-prov injection
will need one sudo command.

## 2026-07-31, DHML fix attempt 1: insufficient

Applied the DHML recipe to mt-spi.c (smooth +/-2% rad0/rad1 walk per frame,
50 ms grid report timestamps anchored at MakeTouch). Result on sprr-x with
live syslog: unchanged -- still `Hover (stage MakeTouch -- ignoring motion)`,
`ZInstability=0.000000`, `TimeInstability=0.167949 (dtstart=0.050000s)`.
The flat ZInstability despite varying radii means the guest's Z signal is
not sourced from the rad0/rad1 report fields as assumed. Upstream
(ChefKissInc/Inferno) has the same report layout (only the velocity formula
differs), so no fix to borrow. Byte-exact unpackContactFrame/instability
analysis is in progress via IDA to determine which report offsets actually
feed zarea and the instability computations.

## 2026-07-31, DHML root cause narrowed: missing pressure field

Byte-level IDA follow-up: ZInstability = |(cur-prev)*100/max| over the
Digitizer PRESSURE signal (contact+0x30), NOT the radii. Our 20-byte path
entry (x,y,x_vel,y_vel,rad0,rad1,angle,multiplier) carries no pressure
field, so the Z signal is constant and the slide never validates. Leading
suspect: the real path entry is longer (pressure after y_vel at offset
12), which would also shift our rad0/rad1/angle/multiplier and make the
guest see a constant "pressure" (our rad0=660) plus a grotesquely
elongated contact ellipse. Kernel-kext (AppleMultitouchSPI) report parsing
is being RE'd via IDA for the exact expected layout.

## 2026-07-31, pressure-slot probe build

Appending a dedicated pressure word (pathLen 20->22) did NOT move
ZInstability (still exactly 0.000000) -- offset 20 is not the slot (or the
kext ignores the extra bytes). TimeInstability is bit-identical across
builds (0.167949), so it is NOT computed from the report timestamp field
(grid change had no effect); it must use driver-side arrival timing.

New probe build populates every plausible slot with a DISTINCT per-frame
amplitude so a single boot identifies the pressure source by the observed
ZInstability value: header word after ts +/-50%, angle +/-10%,
multiplier +/-5%, appended path word +/-30%. Awaiting boot + drag test.

Also established: the plugin receives 96-byte/contact structs from the
KEXT (fixed-offset parser keyed on MT family 0xC3, no HID descriptor is
exchanged), all report words are little-endian, and the kext binary lives
in the kernelcache (not on the System volume). Static analysis could not
pin the pressure offset; runtime identification via this probe, else a
guest watchpoint on contact+0x30.

## 2026-07-31, pressure slot pinned: the "rad multiplier" field

Per-gesture probe (each of four drags varied exactly one candidate field):
only the multiplier walk (offset 18) moved ZInstability off 0.000000
(to 1.000000); angle, the appended word, and the header word did nothing.
So the kext maps report offset 18 (upstream's "rad multiplier", constant
100) to the contact's pressure/Z signal, and the DHML's flat-Z rejection
was caused by that constant.

Final mt-spi.c change (all probes reverted to the upstream 20-byte layout,
real timestamps, constant radii/angle): the multiplier slot now models a
real press -- 90 on MakeTouch, 100+/-8 slow sine while TOUCHING, 40 on
BreakTouch, 0 out-of-range. This is the only functional delta vs upstream
mt-spi.c. Testing whether the slide validates and unlock works.

## 2026-07-31, pressure model live, slide still not validating

With offset-18 pressure modelling, ZInstability became a healthy 0.016
(was flat 0), but the path still hover-locks; first-eval ZI spiked to
0.72 because the 0->90->100 pressure jumps were too abrupt. mt-spi.c now
ramps pressure smoothly (40->55->70->82->92->98 over the first six frames)
then breathes 100+/-8. TimeInstability (~0.135-0.167 at dtstart=0.1s,
settling to 0 by ~1.5s) is unaffected by report timestamps, so it is
computed from driver-side timing. The exact "Wating for slide" exit
predicate (the s0/s2/40.0/50.0 acceptance gate, contact-shape gate
[path+0x180]-[path+0x1bc]==2, and the accepted-state log line) is under
IDA analysis.

## 2026-07-31, TOUCH FIXED (root cause + emulation changes + gesture recipe)

Root cause chain (syslog + IDA): MultitouchHID's DHML hover-locks every
path because (a) the digitizer pressure/Z signal was a constant 100
(read from report offset 18, the "rad multiplier" slot), and (b) batched
report timestamps could repeat, zeroing the guest's frame interval and
killing its velocity estimate ("extractHandMotion Frame interval is
zero"). With a flat Z and broken velocity, slide validation can never
fire, so all motion was ignored and taps died downstream.

mt-spi.c changes (now in the tree):
- offset-18 pressure model: smooth ramp 40..98 over the first frames,
  100+/-8 sine while touching, 70 at break, 0 out-of-range.
- 120 Hz reporting (was 20 Hz) and reports every tick while the finger is
  down (real digitizers never go silent during a dwell).
- report timestamps clamped strictly monotonic (+1 ms min step).

Gesture recipe that the DHML accepts (touch-age debounce = 0.2*(1-dt/0.31s)):
press, hold ~0.45 s, then slide fast, lift. For taps: pure
press-hold-release (~0.3 s) with NO wiggle -- micro-moves while
hover-locked eat the up-delivery and the button never fires.

Verified on sprr-x: swipe-up advanced Setup Hello -> Software Update
Complete, and a pure tap fired Continue -> Apple ID page.

## 2026-07-31, activation state (live lockdownd evidence)

Direct lockdownd queries on goal-x (paired):
- `ActivationState: Activated` -- but this is only the hactivation bypass
  flag (kernel patch + DT property), not a real activation.
- `ActivationRecord` -> **No such value** (lockdownd has none).
- `FairPlayKeyData` -> **No such value**.
- `ActivationStateAcknowledged` -> No such value.
- Identity is synthetic: SerialNumber INFERNO01122, MLB INFERNOMLB0011220,
  ECID 0x1122334455667788, ProductType iPhone12,1.

Conclusion: the device is NOT activated in any meaningful sense -- no
albert-issued activation record, no FairPlayKeyData, no device certs.
Real activation also cannot complete: mobileactivationd's SIK collection
fails locally (SEP "osc" needs the "sep sub ca key" from the SEP xART
store, which is empty on a factory-fresh emulated SEP), and even with SIK,
albert would have to accept a synthetic serial/MLB/ECID/FairPlay identity.

## 2026-07-31, absinthe -42052 root cause (IDA-verified)

- The NAC implementation (NACInit/NACKeyEstablishment/NACSign) is
  statically linked, o-LLVM-obfuscated, into AppleAccount.framework
  (AAAbsintheSigner/AAAbsintheContext — the GSA path akd uses),
  AuthKit.framework (AKAbsintheSigner) and Social.framework. There is no
  Absinthe.framework. -42052 is a runtime-computed AAAbsintheError from
  the `Failed to initialize NAC session` path; no compile-time label
  exists (verified by whole-cache immediate scans).
- NAC session init requires: (a) an Apple-issued device client
  certificate (DeviceIdentity/BAA attestation), (b) its ECDSA P-256
  private key in the keychain, (c) working ADI provisioning. The flow:
  `Fetching absinthe cert` (the HTTP 200 we saw) -> `No certificate to
  validate, bailing!` -> -42052. The failure is local, before the
  create-session-info POST.
- This emulator fails all three prerequisites, one root cause: no
  Apple-issued identity. No activation record -> no device cert; keychain
  -25300 (no device signing key); ADI -45061 (not provisioned).
- Recoverability: ADI provisioning alone is insufficient (BAA attestation
  needs the real device key); a fabricated activation record is useless
  (self-signed cert does not chain to Apple's BAA root, server rejects);
  the only way past NAC init is transplanting a real activated iPhone's
  identity (activation record + FairPlay/BAA private key + ADI blob).
- Consequence for the main goal: sign-in paths are permanently dead on
  this device; the App Store Today fetch must be (and is being) pursued
  via direct ADI provisioning (akd XPC, absinthe-free), since the type-2
  apiToken anisette is machine-global and needs no account/absinthe.

## 2026-07-31, genuine activation: requirement map

Goal changed (user): genuinely activate the emulator (no hactivation bypass).

Public + live evidence:
- Corellium (the commercial iOS emulator) does NOT support iCloud/App
  Store sign-in on its virtual devices -- the industry reference point
  says genuine activation with synthetic identity is not offered.
- albert's macOS flow (theapplewiki): activation requires
  FairPlayCertChain + a FairPlay RSA signature over the activation info,
  and a device cert request. iOS adds the tunnel1/SIK session (SEP).
- Live on goal-x: lockdownd `FairPlayCertChain` and `DeviceCertificate`
  both "No such value"; `pymobiledevice3 activation activate` refuses
  ("Device is already activated!" -- the bypass flag), and the SIK
  collection behind it fails (SEP "osc", empty xART store).
- So genuine activation needs TWO Apple-issued secret sets: the SEP
  sub-CA key (SIK) and the device FairPlay cert+key (factory
  provisioning). Neither exists in any firmware image; both live only in
  factory-provisioned SEP secure storage.

Only conceivable paths: (A) the dev key/cert embedded in the retail
SEPFW (under IDA evaluation: is it an Apple-chained cert albert accepts,
and can the SIK handler be made to use it despite the obj+352 check);
(B) transplant a real activated iPhone's identity (SEP-protected,
unextractable -- no known t8030 SEP exploit).

## 2026-07-31, genuine activation: impossible (IDA-verified)

The SEPFW dev-key evaluation is final:

- The built-in dev key (0x438F override) is a bare 16-byte KDF seed at
  0x54bda1 with NO certificate. The entire SEPFW image contains exactly
  three X.509 certs, all self-signed Apple ROOT trust anchors (Apple Root
  CA, Secure Boot Root CA G2/G3). There is no "sep sub ca" certificate
  and no device leaf anywhere -- nothing that could chain to Apple.
- "osc" builds the SIK cert with fixed DN strings ("SEP Device
  Certification"/Apple Inc.) and the device SIK public key, EC P-256
  ECDSA. No serial/ECID/MLB field is included or checked; the identity
  is just the (emulator-generated) SIK keypair, which is not in Apple's
  per-device registry.
- Two ways to make "osc" emit a syntactically complete cert exist:
  import caller key material as a storage-backed xART "sep sub ca key"
  via the "sikp" command (passes the obj+352==0 gate; its mode gate is
  likely production/demotion-based, unverified), or patch the sub_5112C4
  v5==0 embedded-seed check to use the built-in dev key. Both produce a
  cert signed by a NON-Apple key.
- albert.apple.com validates the SIK cert against Apple's production
  "sep sub ca" chain AND the factory-registered device identity
  (serial/MLB/ECID). Neither can be satisfied: no Apple-issued sub-CA
  key+cert exists outside real factory-provisioned SEPs (non-exportable
  by design), and the emulator's identity is synthetic.

CONCLUSION: genuine activation of the emulator is cryptographically
impossible, matching Corellium's documented position (no iCloud/App
Store sign-in on virtual devices). The only route would be a real
factory-provisioned "sep sub ca" key + Apple-signed cert + registered
device identity, which no firmware image contains and no real device
exports.

## 2026-07-31, ADI PROVISIONED (the apiToken blocker is cleared)

The inferno-prov helper (root launchd daemon, ldid-signed with
com.apple.authkit.client.private+internal, injected into the rootfs)
successfully provisioned ADI via akd's own XPC. Its log (AFC-visible at
/var/mobile/Media/inferno-prov.log):

  SUCCESS on attempt 1, anisette: AKAnisetteData {MID: c59082TEHPwK..., OTP: AAAABQAAABCoiVKC..., RD: 50660608}
  provisioning complete, exiting

Iterations needed to get there:
- com.apple.akd.anisette (RE-reported name) -> 4099: that listener is
  runtime-only; the launchd-registered service that spawns akd and serves
  AKAnisetteProvisioningDaemonProtocol is com.apple.ak.anisette.xpc
  (confirmed via AKAnisetteProvisioningMachService constant).
- NSInvocation proxy call + runtime protocol -> akd fault "undecodable
  message": the completion block needs the statically-declared extended
  type encoding; a runtime-protocol lookup loses it.
- completion declared (NSDictionary*, NSError*) -> akd fault
  "incompatible reply block signature": the reply is an AKAnisetteData
  object; the class name is part of the XPC block signature hash.
- Connection-level entitlement gate (AKDaemonConnectionManager
  shouldAllowClient) exists but PASSES for the adhoc ldid-signed helper
  (the AMFI trustcache patch honors the entitlements) -- the 4097s were
  all downstream of the undecodable messages, not the gate.

ADI state now persists in the guest data volume. Next: unlock -> App
Store -> Today; the apiToken should get its type-2 anisette (no -45061).

## 2026-07-31, App Store attempt 1: provisioning holds, tap flakiness

With ADI provisioned, unlocked to the home screen. The App Store did not
launch on this boot: taps were misread as long-presses (SpringBoard
logged the icon shortcut menu: "Edit Home Screen"/"Remove App") and the
DHML hover-locked the gestures again (this boot was slow under host
load; per-boot touch variance remains high). ADI state persists in the
data volume (provisioning is one-time), so a reboot for a better-touch
boot is free. The final check per boot is now just: unlock -> tap App
Store -> Today renders + apiToken 200 in syslog/pcap.

## 2026-07-31, App Store crash = my SPRR-read revert (fixed)

The user's App Store launch crashed: EXC_BAD_INSTRUCTION with the faulting
instruction 0xd53ef1a9 = `mrs x9, SPRR_EL0BR0`. During the touch bisect I
reverted hvf.c/hvf-all.c to HEAD (dropping the SPRR READ handling added
07-30 14:35) and never restored it; under HVF the SPRR read is an
undefined instruction and libsystem_pthread's readback validation takes
the deliberate BRK path. Restored both files from /tmp/*.mine and rebuilt.

## 2026-07-31, App Store launches; type-2 fetch still -45061 despite provisioning

With the SPRR-read patch restored, the App Store launches cleanly (user
drove it: unlock + tap). Today shows "Cannot Connect / Retry". The
apiToken request goes out (TLS fine) and returns HTTP 401 with
`AMSAnisette: Failed to generate Anisette (type: 2) headers. error =
-45061` -- the SAME ADI-notProvisioned error, even though inferno-prov
provisions successfully on every boot (attempt 1, identical MID each
time, so ADI state DOES persist across boots and akd serves anisette to
the helper).

Key distinction: the helper's fetch is provision=YES (akd provisions on
demand and returns fresh anisette); the App Store's type-2 fetch is
read-only (provision=NO) via StoreServicesCore `_qi864985u0` and still
reports notProvisioned. Leading hypothesis: provisioning start/end
(SPIM->CPIM) is insufficient -- the ADI state also needs the SYNC step
(the storeservices-wrapper's provision/sync: SIM->mid/srm), without which
the read-only path's provisioning-status check still fails. Under IDA
analysis now (which function returns -45061 in the type-2 path, and what
exactly must be driven to clear it).

## 2026-07-31, the -45061 root cause: type-2 uses DSID -1, we provisioned the wrong context

IDA settled the App Store's persistent -45061:
- `+[AMSAnisette _accountIDForType:account:]` returns `~0` (DSID **-1**)
  for type 2 -- the device-level ADI context. All other types use the
  account's ams_DSID.
- inferno-prov's fetchAnisetteDataAndProvisionIfNecessary:YES device:nil
  provisioned the ENVIRONMENT context (positive DSID from
  lastKnownActiveAnisetteDSID / IDMS environment) -- a DIFFERENT per-DSID
  record in adid. The App Store's `-1` record was never touched.
- akd's com.apple.ak.anisette.xpc cannot address `-1` (the device:/linkType
  model selects native/paired/client device, all keyed off the IDMS
  environment; lastKnownActiveAnisetteDSID returns a positive DSID or
  -2/-3/-4/-5 error sentinels, never -1).
- The type-2 READ path is akd->AKADIProxy->adid `getIDMSRoutingInfo:forDSID:`
  gated by adid's per-DSID isMachineProvisioned. Provisioning (start+end)
  alone suffices for the read; no separate sync is required.
- The drive for `-1`: Option A = direct adid (com.apple.adid) calls
  startProvisioningWithDSID:-1/endProvisioningWithSession: with a gsa
  SPIM->CPIM->PTM/TK loop (adid's client entitlement unrecovered -- needs
  a runtime probe). Option B = StoreServicesCore's type-2 machine
  provisioner (AMSAnisetteProvisionTask /
  storeservicescore::AnisetteProtocolAction::_provisionWithContext), which
  performs the whole flow for the `-1` context through akd/adid itself --
  invocation contract under RE.

## 2026-07-31, the DSID -1 drive: AMS machine provisioner

inferno-prov v3 (built): instead of akd's
fetchAnisetteDataAndProvisionIfNecessary (provisions only the
IDMS-environment account context), it drives the App Store's own type-2
machine provisioner:

  dlopen AppleMediaServices
  bag = [AMSAnisette createBagForSubProfile]        // lazy-fetched
  [AMSAnisette _provisionMachineWithActionData:@{} type:2 account:nil bag:bag]

IDA-verified: AMSAnisetteProvisionTask(initWithData:type:account:bag:)
never reads the actionData keys, so @{} is valid; the task runs the full
SPIM->CPIM->adid exchange for DSID -1 (device context). account=nil and
type=2 select the -1 context via _accountIDForType's csinv. Result is
async (internal resultWithError:), so success is confirmed by the App
Store's type-2 fetch going quiet (no more -45061) and the apiToken
returning 200.

Also established: provisioning start+end suffices for the read; no
synchronize step is required. adid direct drive (com.apple.adid) is the
fallback if AMS's task fails (adid's client entitlement is unrecovered).
Server-side caveat: gsa's provisioning endpoint must accept a device-level
(-1) provision with the emulator's synthetic MLB/ROM/Serial headers --
the account-level flow succeeded, but the device flow exercises the
device-attestation path; a server error there means device-trust
rejection, not a harness bug.

## 2026-08-01, DSID -1 provision driven

inferno-prov v3 required two call fixes (host-verified against macOS's
AppleMediaServices): (1) the provision drive crashed on
`-[__NSDictionary0 length]: unrecognized selector` because the
`actionData` argument must be a STRING/DATA (the task reads
[data length]), not an NSDictionary -- fixed to `@""`. (2) bag creation
is no-arg `+[AMSAnisette createBagForSubProfile]` returning an AMSBag
instance.

With those, the helper ran cleanly on the guest: created the AMSBag,
drove `_provisionMachineWithActionData:@"" type:2 account:nil`, and the
provision task ran its async SPIM->CPIM exchange -- the pcap shows the
guest connecting to gsa.apple.com (17.179.252.2:443) during the window.
Next: unlock -> App Store -> check whether the type-2 fetch's -45061 is
gone and the apiToken returns 200.

## 2026-08-01, -1 provision driven but type-2 still -45061

Drove the AMS machine provisioner for DSID -1 (v3 helper, actionData=@"").
The provision task ran (gsa.apple.com connection at 17.179.252.2:443 in
the pcap during the window) and the helper completed cleanly. But the App
Store's type-2 fetch STILL returns -45061 (verified 10:42 with a fresh
Retry). So the -1 device provision either failed server-side (device-trust
rejection of the synthetic MLB/ROM/Serial -- the caveat the RE flagged,
i.e. the device-attestation path that absinthe/activation also dies on) or
did not write adid's -1 record. The account-level (environment-DSID)
provision DOES succeed (akd fetchAnisetteDataAndProvisionIfNecessary), so
gsa accepts our synthetic identity for the ACCOUNT flow; the DEVICE (-1)
flow is the open question. Need the AMSAnisetteProvisionTask result/error
to distinguish server rejection from a flow bug (under RE).

## 2026-08-01, the -1 provision result: gsa returns "No data found"

Reading performProvisioning's AMSPromise (task instantiated directly;
the class-level convenience returns nil on iOS 14):
  PROVISION FAILED: AMSErrorDomain 307 "Anisette Provisioning Failed:
  No data found"

The provision request DOES reach Apple (via Akamai 23.221.28.28) and the
response carries no SPIM. Meanwhile akd's ACCOUNT-level provision
(fetchAnisetteDataAndProvisionIfNecessary, environment DSID) SUCCEEDS
with the same synthetic MLB/ROM/Serial identity. So gsa accepts our
identity for the account flow but returns nothing for the device-level
(-1) flow. Open: is "No data found" a device-trust rejection on the
device provisioning endpoint, or a request-shape difference (missing
header/payload vs akd's request)? Under RE (request diff + whether
setIDMSRoutingInfo:forDSID:-1 is a viable shortcut).

## 2026-08-01, the -1 provision verdict: gsa refuses it

Request-level IDA comparison (akd account flow vs AMS -1 flow):
- Both use device headers (MLB/ROM/Serial/ClientTime/LocalUserUUIDHash);
  NEITHER is absinthe-signed. Same synthetic identity presented.
- akd's account-level provision (GsService2/lookup provisioning-start)
  gets a SPIM; the AMSAnisetteProvisionTask's machine-provisioning request
  (type 0x133/307, accountID -1) gets an EMPTY body (no SPIM).
- "No data found" = gsa refusing to mint a SPIM for the -1 device context,
  not a local parse bug, and NOT caused by the actionData (the request
  fires with empty payload too).
- Coherent explanation (matches activation/absinthe walls): gsa provisions
  the account/environment context from this device but refuses the DEVICE
  (-1) context because the device isn't genuinely recognized/registered on
  Apple's side.
- setIDMSRoutingInfo:forDSID:-1 alone won't fix the type-2 read (the read
  also needs requestOTPForDSID:-1, which needs real -1 provisioning).

Remaining client-side angle under evaluation: whether OTP is per-DSID or
device-wide (the account provision may already make requestOTPForDSID:-1
work, leaving only the -1 routing-info record to write via
setIDMSRoutingInfo:forDSID:-1), plus whether akd's WORKING gsa lookup is
DSID-agnostic enough to drive a -1 provision through adid directly.

## 2026-08-01, FINAL: the App Store Today fetch is gated on Apple-issued device identity

The last client-side angles are closed (IDA-verified):
- OTP/MID is PER-DSID (requestOTPForDSID returns -45061 for an
  unprovisioned DSID), so setIDMSRoutingInfo:forDSID:-1 is insufficient --
  the -1 OTP would still be missing. Not worth a cycle.
- akd's working gsa provisioning-start lookup is NOT reusable for -1: the
  SPIM is bound to the device's IDMS anisette-environment (prod/QA/QA2),
  and gsa is context-selective -- it mints a SPIM for the valid
  environment but refuses to mint one for the -1 device-services context
  for a synthetic device.
- adid (the direct -1 drive) gates clients on an ADI/Apple-internal
  entitlement that authkit.client.* does not satisfy.

CONCLUSION: the App Store Today page's apiToken requires the device-level
(DSID -1) ADI context provisioned, and Apple's gsa refuses to provision it
for a device it does not recognize. This is the SAME Apple-issued
device-identity barrier that blocks genuine activation (albert rejects
synthetic identity) and Apple ID sign-in (absinthe -42052 needs the
activation record's device cert). All three reduce to one fact: this
emulator has no Apple-issued device identity, and that identity is
cryptographically unobtainable (SEP-protected on real hardware, present in
no firmware image).

What WAS achieved: the guest boots HVF to SpringBoard, touch was
root-caused and fixed (mt-spi pressure/timestamp fidelity), the App Store
launches (SPRR compat restored), networking/TLS to Apple works, and ADI
provisions at the ACCOUNT/environment level (akd) -- everything except the
device-level anisette the apiToken needs.

## 2026-08-02, real-device (jailbroken iPhone 8 Plus) extraction via Frida/SSH

Connected to the user's jailbroken iPhone 8 Plus (iPhone10,2, iOS 16,
Dopamine) over Frida (USB) and SSH (mobile/alpine via iproxy).

Captured the DEVICE (DSID -1) anisette via akd XPC
(legacyAnisetteDataForDSID:"18446744073709551615"):
  MID: QYzVAh/9CpoKWSaD20qeuFsapMc8voju98KjBdOfQZAu6P6Y3lrM3nyqCqcvF/yS5HUnziXS9f33VbPW
  OTP: AAAABAAAABBCOLl0qcC5c3r5hb2PArXI (ROTATES each fetch)
  RD:  0

Key facts established:
- The MID and RD are STABLE across fetches; the OTP ROTATES (per-fetch
  fresh token). So a static anisette injection is not durable.
- The xART store (/private/xarts/<uuid>.gl) is data-vault protected --
  unreadable even as root, and even to adid itself via the File API.
- fairplaydeviceidentityd (the ADI daemon) reads NO /var files during
  anisette generation (full open() trace) -- the ADI provisioning state
  is held in the FairPlay IOKit driver / SEP, NOT in a userspace file.
  adid's entitlements confirm it works via AvpFairPlayUserClient (the
  FairPlay driver), not a file.
- So the real device's ADI/anisette state is SEP/driver-bound and NOT
  extractable as a file. The one anisette I CAN read (via akd) is the
  generated output (MID/OTP/RD), not the provisioning material that
  produces it.

Implication: a straight file transplant of the real device's identity is
not possible. The remaining candidate paths are (a) replaying the real
device's ADI provisioning blob (PTM/TK) into the emulator's adid if it
can be exported via the FairPlay IOKit user client, or (b) feeding a
WORKING account-level SPIM to adid for DSID -1 (the RE-flagged disproof
to test), or (c) relaying fresh anisette from the real device to the
emulator (anisette-server pattern -- a workaround in spirit).

## 2026-08-02, KBSync transplant attempt — blocked at emulator FairPlay platform

Path taken (user-directed, Teragen getKbsync mechanism):
- Pulled the real device's dyld cache (iOS 16.7.16, arm64, from
  /System/Cryptexes/OS/... — 44 files, ~2.9 GB) via scp; extracted
  AppleMediaServices with `ipsw dyld extract --objc`. Also extracted iOS 14.0
  AMS + AdID + DeviceIdentity from the iPhone12,1 14.0 IPSW dyld cache.
  NOTE: ipsw's extractor leaves section file offsets as cache VAs ("invalid
  section file offsets" in IDA); fixed by rewriting each section offset to
  segment.fileoff + (addr - vmaddr). Verified byte-identical to live memory.
- KEY SIMPLIFICATION: no raw Teragen offsets needed. AMSKeybag ObjC API works
  from Frida: [AMSKeybag sharedInstance] fairplayContextWithError: /
  keybagSyncDataWithAccountID:transactionType:error: / importKeybagWithData:error:.
  Teragen's offsets (0x16a960/0x19cec0/0x1bac20) do NOT match 16.7.16.
- Exported the device-level KBSync blob from the real iPhone 8 Plus:
  keybagSyncDataWithAccountID:nil transactionType:0 -> 420 bytes, no error.
  Saved: ~/InfernoData/provhelper/inferno-kbsync.bin (and repo InfernoData/relay/kbsync-device.bin).
- iOS 14.0 AMSKeybag has the same interface (verified via ipsw --objc symtab;
  selectors are NOT in `strings` — iOS 14 dedups them into the cache's shared
  objc region, use nm instead).
- Guest helper: ~/InfernoData/provhelper/inferno-kbimport.m (root launchd
  daemon com.inferno.kbimport, inject-kbimport.sh; loop re-triggered by
  /var/mobile/Media/kbimport-go sentinel for AFC-driven iteration; logs to
  /var/mobile/Media/inferno-kbimport.log, pulled via companion pmd3 AFC).

RESULT: import FAILS — AMSErrorDomain 505 "Fairplay Error / Failed to
initialize global context due to hardware info".

Root-cause chain (RE + live probes):
- AMSFairPlayGetHardwareInfo (16.7.16: 0x18fd35514; 14.0: 0x18eef6938, a
  LOCAL symbol — resolve via dyld slide, not dlsym) on the real device
  returns {u32 type=0x14, 20-byte UDID=f98b2e6c...} (24-byte struct). The
  fetch itself is an obfuscated FairPlay-internal function (_zxcm2Qme0x on
  14.0) that gets the UDID from the SEP/FairPlay-driver path.
- On the emulator, AMSFairPlayGetHardwareInfo returns 0 (fail) — so
  AMSKeybag's GLOBAL FairPlay context cannot initialize at all. MGCopyAnswer
  on the emulator DOES return well-formed identity (udid=00008030-1122...,
  sn, mlb, ecid) — the failure is below MobileGestalt, in the
  FairPlay-driver/SEP fetch.
- Emulator boot log has ZERO FairPlay driver messages (no
  AppleFairPlayTexturedDriver/AvpFairPlay lines) — suspect the platform gap
  is in the guest's FairPlay IOKit/SEP path, not in AMS.
- Consequence: the whole AMSKeybag global-context import route is blocked at
  emulator platform level until FairPlay hardware info works there.
  (Teragen-style LOCAL contextInit(flags, macData, path, ctx) would bypass
  this but imports into a helper-private context that does NOT feed adid's
  DSID -1 provisioning, so it does not serve the goal.)

Also noted: adid (/usr/libexec/adid) is a thin IOKit client
(AvpFairPlayUserClient); its logic is obfuscated. Account-level ADI
provisioning DID work on the emulator earlier (SPIM/CPIM via akd), so the
adid provisioning store is writable — only gsa's -1 SPIM issuance refuses
synthetic devices. Alternative transplant route to explore: capture the
donor's -1 CPIM/provisioning and replay it into the emulator's adid via the
same handoff akd uses (RE akd <-> adid CPIM exchange next).

QEMU ops note: starting the VM via `nohup bash run-goal.sh &` from an
ephemeral shell gets QEMU killed minutes later ("Unable to read from socket:
Bad file descriptor"); run it as a persistent background task instead.

## 2026-08-02 (cont.), KBSync import route DEAD END — emulator lacks FairPlayIOKit/AVD

Progress chain:
- Emulator UniqueDeviceIDData was missing; set donor UDID via lockdown set
  (persists). WifiAddress settable but does NOT persist reboot (computed from
  absent WiFi driver). Emulator mobilegestaltd fabricates UniqueDeviceIDData
  as the 25 ASCII bytes of the string UDID — wrong format vs real 20 raw bytes.
- mprotect of dyld-cache pages is EPERM on iOS; dlopen of a decached AMS copy
  needed layout surgery (ipsw/dyldex outputs have overlapping/misordered
  segments; codesign REGENERATES segment commands and re-breaks them; fix
  __AUTH_CONST vmsize/filesize=0x1a3f0 + re-pack file layout in vm order).
  Then sandbox blocks executable mmap from /var/mobile/Media anyway.
- FINAL WORKING HOOK: patch AMSFairPlayGetHardwareInfo directly in the
  guest's dyld_shared_cache_arm64e on the rootfs (unique 48B pattern at cache
  off 0xeef6938 == VA 0x18eef6938). Stub returns {0x14, donor UDID20} + 1.
  Done by inject-kbimport.sh step 4b (idempotent). hwinfo now returns the
  donor identity in EVERY guest process.
- Import then reached the real FairPlay init and failed differently:
  AMSErrorDomain 505 "Failed to initialize global context with status:
  -42023".
- On the REAL device, importing its own blob fails too but at a LATER stage
  (-42001 "Import error") — because its global context is already
  initialized. All transactionTypes 0..3 behave identically. So the emulator
  (empty context) was on a viable path; -42023 is the blocker.
- Root cause of -42023: AMS's FairPlay global context init goes through
  FairPlayIOKit. On the emulator IOServiceOpen(com_apple_driver_FairPlayIOKit)
  = 0xe00002e2 kIOReturnUnsupported (zombie registry node, no working user
  client — no AppleAVD backing on the platform). On the real device the same
  open = 0xe00002c2 kIOReturnNotPermitted (driver functional, entitlement-
  gated). => The AMS FairPlay global context can NEVER init on the emulator
  without AVD/FairPlayIOKit platform support. AMSKeybag KBSync import route
  is DEAD for the emulator (would need an AVD emulator — separate project).
- Note: adid ADI provisioning does NOT need this path (account-level
  provision succeeded on the emulator earlier) — ADI uses a different
  FairPlay channel.

PIVOT: replay the donor's -1 (device) ADI provisioning into the emulator's
adid through the same akd->adid channel a CPIM uses. adid provisioning works
on the emulator; only gsa refuses to ISSUE a -1 SPIM for a synthetic device.
Open questions: what blob akd hands adid (CPIM), whether adid accepts a CPIM
minted for the donor, how to extract the donor's -1 CPIM/provisioning record.

## 2026-08-02 (cont.), donor-identity clone path (replaces dead KBSync route)

Strategy per user prefs: transplant a coherent donor identity (iPhone 8 Plus,
FD1ZF1M7JCM0 / FD193750523J08XEW / ECID 396734956765230 = 0x168d40e2b002e /
UDID f98b2e6c...) onto the guest so gsa will issue the DSID -1 SPIM to the
emulator's own akd (account-level ADI provision already works there).

Findings:
- Same disks + donor ECID => userspace panic "boot task failure:
  data-protection - exited due to exit(2)". Same disks + original ECID boots
  fine (isolation test). The keybag/DP state is ECID-bound => donor identity
  needs a fresh restore under the donor ECID.
- AP ticket is ECID-bound (ECID at DER offset 221); byte-patched copy works
  because SEPROM signature checks are patched out on this branch.
- idevicerestore (companion, patched build) usage: `-e -R -T root_ticket.der
  -i <ecid>` (-T capital, ticket auto-read from path given, not cwd-magic).
- Restore with real SEP deterministically dies after "Sending NORData":
  guest panics "SEP ROM boot panic" (stock IPSW SEPFW reload path). The July
  restores worked because they ran with the SIMULATED SEP (build without
  ENABLE_DATA_ENCRYPTION); current build hard-requires SEP
  (ENABLE_DATA_ENCRYPTION is a #define in
  include/hw/arm/apple-silicon/boot.h:24, checked in t8030.c:2858).
- => Rebuilding qemu-system-aarch64 with ENABLE_DATA_ENCRYPTION commented
  out just for the restore; will revert + rebuild after. Current binary
  backed up at /tmp/qemu-system-aarch64.enc.bak.
- goal-donor/ namespace = goal-x clone + pristine-sep flashes. Run scripts:
  run-goal-donor.sh (HVF normal boot), run-restore-donor2.sh (TCG recovery).

## 2026-08-02 (cont.), donor restore: ASR stall on current tree; July-commit rebuild

- goal-x Data volume is FileVault-encrypted; the keybag is ECID-bound, so a
  donor-ECID boot of any existing namespace fails data-protection (exit 2).
  A fresh restore under the donor ECID is unavoidable.
- Real-SEP restore deterministically dies after NORData ("SEP ROM boot
  panic") on the current build, TCG and HVF alike. Sim-SEP restore
  (ENABLE_DATA_ENCRYPTION commented out of boot.h) streams the filesystem at
  full speed but the device-side ASR wedges at 4% with the guest idle —
  reproduced on TCG and HVF, cloned and fresh-zero disks. July's successful
  restore2.log proves it worked on the July tree, so this is a tree
  regression (wlan merges landed after Jul 26).
- Building the restore binary from the July-proven commit 3ea52d57ea in a
  separate git worktree (/tmp/inferno-restore-build, ENABLE_DATA_ENCRYPTION
  commented out there too — it is defined in that tree as well; July's
  restore must have used an uncommitted toggle). Restore script:
  ~/InfernoData/run-restore-donor8.sh (TCG, -smp 6, fresh goal-donor2 disks,
  donor ECID + donor ticket). idevicerestore line (companion):
  `idevicerestore -e -R -T root_ticket.der -i 0x168d40e2b002e
  iPhone11,8,iPhone12,1_14.0_18A5351d_Restore.ipsw` with the DONOR ticket
  placed as ~/root_ticket.der on the companion (orig backed up as
  root_ticket.orig.der).
- The encryption build for normal boots was restored (boot.h define back,
  ninja rebuilt; sim binary kept at build/qemu-system-aarch64-sim; original
  also at /tmp/qemu-system-aarch64.enc.bak).

## 2026-08-02 (cont.), donor-ECID boot achieved (degraded) WITHOUT restore

The TCG/HVF restore is broken at the post-NORData SEP stage on every
available build (real SEP: "SEP ROM boot panic" on restoreSEP reload; sim
SEP: "SEP returned zero-length ART" from seputil --erase — LOAD_SEP_ART was
never implemented in sep-sim.c, and the restore-proven Jul-26 binary is
lost). So the donor identity was brought up by mutating the rootfs instead:

- iOS boot tasks are an embedded XML table inside /sbin/launchd:
  "data-protection" runs /usr/libexec/init_data_protection (symlink to
  seputil), RequireSuccess=true; "mount-phase-2" runs /sbin/mount -P 2,
  RequireSuccess=true. init_data_protection fails under a changed ECID
  (exit 2/17); mount-phase-2 fails on the ECID-bound FileVault Data volume.
- Working recipe (goal-donor = goal-x clone + run-goal-donor.sh with
  ecid=0x168d40e2b002e, serial FD1ZF1M7JCM0, mlb FD193750523J08XEW):
  1. /usr/libexec/init_data_protection -> /bin/sh stub `exit 0`
     (mind: it is a symlink to seputil; write through it or repoint).
  2. keybagd disabled in xpc/launchd.plist (probably not needed; boot tasks
     are hardcoded in launchd and ignore Disabled).
  3. launchd binary: in the mount-phase-2 entry rename RequireSuccess ->
     XequireSuccess (same length) making the task optional.
  4. Fresh empty APFS Data volume replacing the ECID-bound FileVault one
     (diskutil apfs deleteVolume + addVolume ... -role D). NOTE: an iOS
     mount expects the Data volume in the System volume's APFS volume
     GROUP; a macOS-created Data volume is "missing data volume" to it.
- Boot then passes all boot tasks and spawns services (many exit-78 on
  missing /var content — degraded). aks_migrate ... ret = 0 proves the
  emulator's SEP keywrap is NOT ECID-derived (hardcoded AESS_UID), so the
  DP failures are about ECID-keyed state validation, not raw key unwrap.
- First boot attempt panicked (exit 66 "missing data volume"), second boot
  picked up the launchd patch and progressed into service startup.

Still to do: get a usable /var (fresh Data volume needs first-boot
provisioning), reach SpringBoard, re-pair, then test -1 gsa provision +
App Store Today under the donor identity.

## 2026-08-02 (cont.), donor-ECID boot reaches userspace

Full working recipe to boot goal-donor (goal-x clone) with the donor
identity (ecid=0x168d40e2b002e, serial FD1ZF1M7JCM0, mlb FD193750523J08XEW)
WITHOUT the (broken) restore:
- launchd boot tasks are an embedded XML table in /sbin/launchd. Do NOT edit
  /sbin/launchd itself — launchd validates boot-task programs' code-signing
  identity ("code signing identity mismatch for a boot-task" => launchd
  SIGSEGV/assert at startup). Instead replace the boot-task PROGRAMS with
  compiled stubs signed with the ORIGINAL identities:
  - /usr/libexec/init_data_protection (symlink to seputil; id com.apple.seputil)
    -> stubzero (exit 0): skips ECID-bound DP init (exit 2/17 under new ECID).
  - /sbin/mount (id com.apple.mount) -> stubmount: exit 0 for "-P 2" (skips
    mounting the ECID-bound FileVault Data volume; /private/var on the System
    volume keeps its content), else execs /sbin/mount.real.
  - /usr/libexec/keybagd (id com.apple.keybagd) -> stubkeybagd: exit 0 for
    --init (skips MKB_INIT, which HANGS under the new ECID mid-SEPOS
    key-derivation — see DPRINTF notes below), else execs keybagd.real.
  - Delete the FileVault Data volume (diskutil apfs deleteVolume) so nothing
    else tries to unwrap it.
- codesign -f -s - --identifier <id> is enough (AMFI is patched); ldid -S
  adhoc does NOT match (identity must equal the expected one).
- SEP DPRINTF comparison (goal-x vs donor): donor SEPFW stalls mid-sequence
  after TRNG reads, right before the TRNG CONTROL=0x100F write and the PMGR
  TRNG/SEPD power-state changes that complete keybag init on goal-x.
  Unknown AESH commands (0x02/0x11/0x18/0x1c) appear in BOTH and are benign.
- With the stubs the boot passes every boot task and starts services
  (degraded: keychain/DP absent, some services crash-loop).

## 2026-08-02 (cont.), MKB_INIT hang + sim-SEP also hangs + MG-identity pivot

- mount_apfs -o noowners lets a NON-root user edit the guest System volume
  (after plain umount of the ro mount). This removes every osascript-admin
  bottleneck for rootfs edits. Stub files installed this way work.
- MKB_INIT (keybagd --init) HANGS under the donor ECID with BOTH the real
  SEPFW (stalls mid-flow after TRNG reads, before the MONI_BASE burst and
  TRNG CONTROL=0x100F write that complete it on goal-x) and the simulated
  SEP (no keystore messages reach sep-sim at all). keybagd --init is the
  single remaining blocker for a healthy donor-ECID boot; without it the
  system churns (tccd/coresymbolicationd/containermanagerd crash-loops,
  lockdownd never starts).
- Blank sep_ssc panics the SEPROM even with the ORIGINAL ECID on current
  builds AND on the 3ea52d57ea worktree build — the fresh-SEP init is
  broken independent of ECID (this also explains the restore's post-NORData
  SEP ROM boot panic).
- goal-x's sep_ssc has valid xART starting at 0x100; with it the donor ECID
  boots SEPROM fine but MKB_INIT hangs (xART/keybag ECID mismatch inside).
- Pivot test queued: present the donor identity via MobileGestalt values on
  the HEALTHY goal-x boot (lockdown set SerialNumber/MLBSerialNumber/
  UniqueChipID — UniqueDeviceIDData was already proven settable) and re-run
  the AMS type-2 (-1) provision there; if gsa keys on MG-reported identity
  rather than the SEP attestation's ECID, no ECID change is needed at all.

## 2026-08-02 (cont.), status: goal gated on Inferno SEP-emulation bugs

Full chain now proven end to end:
- App Store Today 401s at sf-api-token-service: the type-2 (DSID -1) anisette
  is missing; gsa/grandslam refuses to issue a -1 SPIM for the synthetic
  device (AMSErrorDomain 307 "No data found"). Account-level ADI provisioning
  works on the same emulator, so the device itself is not attested/activated
  in Apple's eyes. Only a genuine (activated-donor) device identity gets -1.
- Donor transplant chosen: boot the emulator as the donor (ECID
  0x168d40e2b002e, serial FD1ZF1M7JCM0, mlb FD193750523J08XEW, UDID
  f98b2e6c..., WiFi MAC 14:9d:99:c2:be:66).
- Donor-ECID boot reaches userspace via: boot-task PROGRAM stubs signed with
  the expected CS identities (seputil/com.apple.seputil for init_data_protection,
  mount/com.apple.mount for mount -P 2, keybagd/com.apple.keybagd for --init),
  deleting the ECID-bound FileVault Data volume, and mount_apfs -o noowners
  for host-side rootfs edits without sudo.

FATAL BLOCKERS (all Inferno SEP-emulation bugs, each independently fatal):
1. MKB_INIT (keybagd --init) HANGS under ANY changed ECID (tested even a
   1-bit change). SEPOS never serves a command (0 SEP cmd traffic); it stalls
   mid-boot after the TRNG/DRBG phase, before endpoint registration. Happens
   with goal-x's valid effaceable (open path) and blank state (create path),
   on the current build, on the 3ea52d57ea worktree build, and on bd547646c1.
2. idevicerestore (any ECID, fresh disks) fails at the post-NORData SEP
   reload: real SEP -> guest "SEP ROM boot panic" on every build; sim SEP ->
   "SEP returned zero-length ART" (AppleSEPARTService reads the gigalocker
   .gl from /private/xarts, which does not exist on a fresh xART volume;
   serving ART via sep-sim LOAD_SEP_ART/ART_LOAD does NOT fix it because the
   read is from the .gl file, not the mailbox).
3. Blank sep_ssc panics the SEPROM ("SEP ROM boot panic") even with the
   original ECID, on all builds tested (current, 3ea52d57ea, bd547646c1).

These mean: no donor-ECID keybag can be created on this emulator today, and
no fresh restore works. The account-level provisioning that succeeded was on
goal-x's July-created identity (ECID 0x1122...), which gsa won't accept for
DSID -1.

NEXT OPTIONS:
(a) QEMU work: fix SEPOS startup on non-original ECID (MKB hang) and/or the
    restore-time SEP reload path (boot panic + gigalocker creation). This is
    multi-day firmware debugging inside sep.c.
(b) Host-assisted anisette (relay/proxy serving the donor's fresh -1
    anisette to the guest) — previously rejected as a workaround.

## 2026-08-02 (final status), goal gated on Inferno SEP-reset emulation

Root cause of the whole App Store Today failure is settled: the apiToken
needs a type-2 (DSID -1) anisette; gsa refuses to provision the synthetic
device. Only an activated-donor identity gets -1. That requires booting (or
restoring) the emulator under the donor ECID.

Both are blocked by the SAME Inferno SEP-emulation limitation class:
1. keybagd --init (MKB_INIT) hangs under ANY changed ECID — SEPOS never
   comes online (0 SEP cmd traffic). Verified on current / 3ea52d57ea /
   bd547646c1 builds, all effaceable variants, real + sim SEP.
2. SEP RESET/RELOAD always panics with guest "SEP ROM boot panic" (the
   restore's post-NORData reload, and the donor boot's init_data_protection
   reset after Gigalocker init completes). Reloading a pristine patched
   SEPROM into SEPROM_BASE on every apple_sep_reset_hold does NOT help —
   verified the reload fires; panic persists. So the cause is not memory
   scribbling; likely the SEPROM's own re-run/secure-boot validation
   (fuse/boot-state mismatch on re-execution) — needs SEPROM-level RE.
3. Blank sep_ssc panics the SEPROM on ALL builds; only the July-created
   master sep_ssc (InfernoData/sep_ssc, cloned everywhere) boots.

Fixes made along the way (all in the tree / InfernoData):
- sep-sim: serve a generated ART for BOOTSTRAP_OP_LOAD_SEP_ART and
  ART_STORAGE_OP_ART_LOAD (the gigalocker read is from /private/xarts/*.gl,
  though — the .gl name is ECID-derived; renaming the existing .gl to the
  ECID-derived UUID makes Gigalocker init SUCCEED).
- t8030.c: keep the patched SEPROM blob; sep.c: rewrite SEPROM_BASE on
  apple_sep_reset_hold (no effect on the panic, but correct in principle).
- Boot-task stubs (compiled, signed with the exact expected CS identities):
  seputil, mount, keybagd — get the donor boot to userspace.
- mount_apfs -o noowners enables rootfs edits without sudo.

Net state: App Store Today CANNOT succeed on this emulator until the SEP
emulation supports (a) SEPOS startup on a non-original ECID, and/or
(b) SEP reset/reload without panic. Both are multi-day firmware-RE items
inside hw/arm/apple-silicon/sep.c, not configuration issues.

## 2026-08-03, the ECID lock precisely located (option (a) investigation)

The ECID surfaces in TWO independent places in t8030.c:
- SoC fuse register 0x3D2BC300/304 (what the SEPROM reads) — machine prop `ecid`.
- DeviceTree `unique-chip-id` (what XNU/MobileGestalt/data-protection read).

New env-based splits added to t8030.c (INFERNO_ECID_DT for the DT,
INFERNO_ECID_FUSE for the SoC register) let each be set independently.
Findings from the splits:
- data-protection reads the DT ECID: DT=donor + old keybag -> exit(2)
  (gigalocker DP check on DT value).
- The SEPROM PANICS whenever the FUSE ECID differs from the sep_ssc's
  recorded ECID (encrypted inside sep_ssc): "SEP ROM boot panic". fuse=donor
  + old SSC always panics on the SEPROM's boot, regardless of the DT value.
  This is the root of BOTH the MKB_INIT hang (fuse changed) and the
  restore's post-NORData reload panic.
- sep_ssc's ECID is baked in ENCRYPTED (no plaintext ECID anywhere in the
  file — verified LE/BE/decimal/hex searches). It is set at SSC creation
  (July, old ECID) and cannot be host-patched.
- The master sep_ssc (InfernoData/sep_ssc) is cloned into every namespace;
  blank SSC panics the SEPROM too, so every working boot depends on that one
  July-created SSC and its old ECID.

CONCLUSION: fuse ECID MUST equal the sep_ssc ECID. Changing the device ECID
requires a donor-ECID sep_ssc, which only the SEPROM can create during a
successful boot/restore — which panics on the same validation. The circle
breaks only by patching the ECID/SSC check inside the obfuscated 86 KB
AppleSEPROM-Cebu-B1 (the genuine multi-day RE task this option chose).
ck_sep_seprom_patches already neutralizes memcmp_validstrs30/14 and
verify_rsa_signature; the remaining ECID check is elsewhere in the ROM and
needs IDA analysis (the ROM is at SEPROM_BASE 0x240000000; reads the ECID
via a base+offset pointer table, not inline immediates).

## 2026-08-03 (cont.), BREAKTHROUGH: DT=donor + fuse=old boots healthy

The worklog previously assumed the donor ECID had to reach the SoC fuse, which
dead-ends on the sep_ssc ECID lock. The split that actually works:

- INFERNO_ECID_DT=0x168d40e2b002e (donor) -> DeviceTree unique-chip-id; all
  userspace identity (MobileGestalt, data-protection, gigalocker vault name)
  derives from THIS.
- ecid=0x1122334455667788 (old) -> SoC fuse; matches the July sep_ssc, so the
  SEPROM never panics and SEPOS/keybag stay fully alive.
- xarts .gl renamed to the DT-ECID-derived UUID
  (81879F33-... -> 0649E99B-711B-5F44-B93F-C62CEB7568A7.gl) on the xarts
  volume (container slice 3, mount_apfs -o noowners, rw edit as non-root).

Result on goal-dt (APFS clone of goal-x + rename): MKB_INIT DONE, Gigalocker
init COMPLETED ("file exists" on the renamed .gl), SEP serving commands
(AppleCredentialManager cmd(19)/cmd(2) ioErr=0), data volume mounts and
flushes xids, zero crash-loops. No SEPROM RE needed for this configuration.

Namespace edit procedure (repeatable):
  dd if=NS/root of=/tmp/X/container.raw bs=4096 skip=6
  hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount ...
  mount_apfs -o noowners /dev/diskNs3 /tmp/mnt   # xarts = slice 3
  mv 81879F33-B0F2-573A-AE12-6B517C322E33.gl 0649E99B-711B-5F44-B93F-C62CEB7568A7.gl
  umount; hdiutil detach; dd back with seek=6 conv=notrunc,sparse

Gotcha: usb-conn-addr symlinks (/tmp/Inferno-goal-*) must pre-exist pointing
at /tmp/InfernoUSBRemote (the companion's usb-tcp-remote bind target), else
the guest's hcd-tcp attach fails once at boot ("Cannot connect to server")
and USB/pairing never comes up. Created /tmp/Inferno-goal-dt symlink.

Open: whether gsa accepts DSID -1 provisioning for the donor identity with
the SEP-side attestation still carrying the OLD fuse ECID; whether
hactivation=true suffices or a real donor activation record transplant is
needed. Next: healthy-boot confirmation -> inferno-prov type:2 (-1) attempt.

## 2026-08-03 (cont.), display "stall" demystified + swipe-up fix

- The feared "all-boots stall at kernel ~289s" was a MISREAD: serial logs go
  quiet on healthy boots too, and 0% CPU is normal for an idle HVF guest.
  goal-x on the current build boots fully: SpringBoard/backboardd run,
  lockdownd pairs, ideviceinfo works. No code regression since Jul 31.
- Headless (-display none) the console surface stays on the boot splash:
  the guest presents only its static boot-logo buffer through the genpipe
  (frames byte-identical, ARGB 828x1792 stride 3328, COMPRESSED bit clear),
  while the real UI is composited into the VRAM framebuffer; the surface
  only advances when a display client drives gfx updates. Under
  `-display cocoa` the UI is fully live (user navigated into apps).
  Diagnostic frame dumps: /tmp/gp0-frame-*.bin (INFERNO-DIAG in
  apple_displaypipe_v4.c, revert before commit).
- Swipe-up-to-home was dead because mt-spi's hardcoded 16 px Y calibration
  subtract underflows for touches starting in the bottom ~16 px band:
  s->y went negative and was consumed as unsigned (~65k = off-surface),
  killing edge gestures at the first contact. Fixed by clamping s->y at 0
  (edge contacts report at the edge, like a real digitizer).
- Touch pressure model works: DHML reports ZInstability=1.000000 and
  validates slides (was 0.000000 flat).

## 2026-08-03 (cont.), donor identity boots but gsa still refuses -1

- goal-dt (DT ECID=donor 0x168d40e2b002e, serial FD1ZF1M7JCM0, MLB donor,
  fuse ECID=old -> SEP healthy) boots to a FULLY healthy system: lockdownd
  pairs (UDID 00008030-000168D40E2B002E), UniqueChipID=396734956765230,
  SpringBoard/UI/touch all work. The DT/fuse ECID split + renamed xarts .gl
  is the complete, sufficient recipe for a healthy donor-identity boot --
  no SEPROM RE needed.
- inferno-prov type-2 (-1) on this full donor identity: STILL
  AMSErrorDomain 307 "Anisette Provisioning Failed / No data found".
  So gsa's -1 acceptance is NOT driven by the MG/DT identity fields
  (ECID/serial/MLB) -- it needs the device's actual provisioned/activated
  material (ADI store / activation identity), not spoofed headers.
- NEXT (user-preferred path): transplant the donor's coherent set --
  /var/db/CoreADI (machine ADI, the -1 context, so akd finds it ALREADY
  provisioned and no gsa call is needed), activation_record.plist,
  data_ark, FairPlay. Channel: inferno-inject (root launchd helper,
  installed in goal-dt rootfs) mirror-copies /var/mobile/Media/inferno-inject/
  into / at boot with correct ownership; files arrive via AFC.
  extract-donor-transplant.sh pulls the set from the jailbroken donor over
  ssh:2223. WAITING ON: donor phone reconnected to USB (iproxy 2223 up,
  device currently absent from usbmuxd).

## 2026-08-03 (cont.), donor transplant material secured

Extracted the coherent donor set from the jailbroken iPhone 8 Plus
(iOS 16.7.16, ssh mobile@ + sudo -S; root@ refused, su hangs non-tty):
- adi.pb (899 B, the machine-level ADI store) at
  /var/containers/Data/System/EBE76839-.../Library/adi.pb -- the classic
  /var/{db,root,mobile}/CoreADI paths do NOT exist on iOS 16; adid runs as
  mobile with com.apple.security.system-container, store = Library/adi.pb
  in its UUID container.
- activation_records/activation_record.plist at
  /var/containers/Data/System/B68F69D5-.../Library/ (mobileactivationd
  container; the old Lockdown path is gone on iOS 16). Verified real:
  AccountToken+Cert+Signature (albert), DeviceCertificate, FairPlayKeyData,
  UniqueDeviceCertificate, unbrick=true, LDActivationVersion 2.
- FairPlay IC-Info.{sidt,sisb,sisv,sidb}, data_ark.plist.

Container UUIDs differ between iOS 16 (donor) and iOS 14 (guest), so
inferno-inject v2 resolves target containers at runtime by
MCMMetadataIdentifier label (adid / mobileactivationd, with a Lockdown
fallback), instead of mirroring donor UUID paths. Transplant tree staged
in ~/InfernoData/donor-transplant/push/inferno-inject/.

## 2026-08-03 (cont.), adid tracing saga + entitlement lesson

Goal: see which store paths iOS 14's adid touches (does it read the
transplanted adi.pb?). Static analysis dead end: iOS 14 adid contains NO
ADI-store strings at all (CoreADI code is obfuscated; only
"com.apple.adid.midchangedV1/V1.5/V3" + "adi-client" appear). The container
is resolved via _container_system_path_for_identifier.

- frida-server on the guest: both 17.x and 16.x crash in static init with
  the same PAC-looking garbage-pointer data abort (0x2f3c4349444e493c,
  "possible pointer authentication failure") -- emulator-specific, abandoned.
- DYLD_INSERT_LIBRARIES is silently DROPPED platform-wide on the guest
  (tested on adid AND on inferno-diag control): root cause = inserted dylib
  was arm64 while daemons run arm64e ("not a compatible arch", visible only
  after forcing a hard dependency: LC_LOAD_DYLIB into adid's Mach-O header
  slack + ldid re-sign).
- LC_LOAD_DYLIB adid crash-looped at dyld stage -> the same arch mismatch;
  after arm64e rebuild, adid hung instead of crashing.
- CRITICAL SIGNING LESSON: re-signing adid with the authkit entitlements
  plist strips adid's own entitlements (application-identifier +
  com.apple.security.system-container); without system-container adid
  cannot resolve its container. Always extract the binary's original
  entitlements (ldid -e) and re-sign with THOSE.
- Behavioral: with the transplanted adi.pb, ALL akd anisette DSID queries
  (-1/0/1) hang (30s timeouts) instead of fast -8004; blanking adi.pb does
  NOT restore -8004; the App Store type-2 fetch starts ("Fetching anisette
  headers for type: 2") and never completes. adid does not crash (no new
  crash reports) and no SEP commands fire during the hang.

## 2026-08-03 (cont.), SSC/I2C trace of the "osc" activation attempt

- Donor ADI-store transplant is a dead end: iOS 14's adid fast-8004s the -1
  query regardless (the iOS 16 adi.pb in its container is ignored; the
  classic /var/{db,root,mobile}/CoreADI paths are all MISSING on the guest
  and were never probed -- to be confirmed by planting).
- gsa DSID -1 provision with the FULL donor set (identity + real activation
  record + FairPlayKeyData): STILL AMSErrorDomain 307. gsa validates the
  SEP/ADI attestation cryptographically, not files on disk.
- The anisette "hang" was an artifact of my LC_LOAD_DYLIB-patched adid;
  reverting to the stock adid restored clean fast -45061/401 behavior.
- Instrumented sep.c SSC handlers (SSC-TRACE, cmds 0x03-0x06) and
  apple_i2c.c transfer starts (I2C-TRACE). Boot-time: SEPFW reads/writes
  SSC slots 1-4 (slot 2 = the SEP-xART locker, rewritten repeatedly);
  525 I2C transfers to 0x71 (SSC), 193 to 0x51 (NVRAM).
- DECISIVE: the activation attempt ("It may take a few minutes..." ->
  "Unable to Activate") produces ZERO new SSC metadata commands and ZERO
  new I2C storage traffic. The skg "acss" store fetch touches NEITHER the
  SSC nor the NVRAM: the SEP-internal store backend (Lynx FTL/dxio/NAND)
  is unemulated -- reads land on mapped-RAM/unimplemented space and return
  empty without bus activity. No unimplemented-MMIO warnings appear.
- Corollary: seeding "sep sub ca key" cannot ride the SSC. Options now:
  (a) plant adi.pb at the classic iOS 14 CoreADI paths (in flight, cheap),
  (b) implement the dxio/FTL store window the skg expects (big RE), or
  (c) RAM-patch the sign-time storage-backed gate in the loaded SEPFW
  (precedent: the July 0x438F runtime patch; the AESS/PKA hooks were since
  reverted and are not in the tree).

## 2026-08-03 (cont.), "osc" dev-key path endgame: unregistered crypto vtable

Implemented and verified, each independently insufficient:
- AESS CREATE_KEY_FROM_SEED (normalized 0x3) emulation:
  AES-256-ECB(AESS_UID0, input) per the SEPD contract (sep.c
  aess_handle_cmd).
- 0x438F dev-key config override at runtime GPAs 0x3406ec020/0x3406e8018
  (the payload is identity-mapped at 0x340000000): pmemsave-verified the
  record reads {id=0x438F, v1=1, flag=1} in the live SEPOS image. NOTE the
  on-disk record has a 4-byte pad after the id (v1 at offset 8, flag at
  offset 36) and SEPFW's own XPRT may set the same values on this build.
- AESS-TRACE/PKA-TRACE (always-on info_report in aess_handle_cmd and
  pka_base_reg_write case 0x0): the "osc" attempt fires exactly AESS 0x13
  once + PKA 0x80 twice per Try Again (matches the July sequence). No
  other SEP hardware commands at attempt time: the sign op is pure SEPFW
  software, not an emulator PKA/AESS gap.

Root failure located one level deeper: the sign compute (sub_5112C4 in the
"pass" image) dispatches through crypto provider tables at payload offsets
0x55cce8/0x55cd00, and pmemsave shows those tables are ALL ZEROS at
runtime (verified 128 bytes around 0x34055ccc0). With no provider
registered, F4F02B0's `ldr x0, [tbl]; cbz` fails -> -10 -> 0xe007c00a.
Since SEPFW does working crypto elsewhere (keybag ops), this table is a
SPECIFIC module's ops (SIK/PKA-driver/provider), and its registration
never ran on the emulator. Two candidate causes, in likelihood order:
  (a) the registering module's init depends on the skg/xART store
      (Lynx FTL/dxio/NAND, unemulated -- the activation attempt produces
      ZERO storage-I2C traffic, proven via SSC-TRACE + I2C-TRACE);
  (b) the SEPOS PKA driver's init handshake fails on the emulator's
      PKA register model (only 0x40/0x80 commands are ACKed; no ID/
      revision handshake exists).
NEXT (multi-day): enable SEP_ENABLE_TRACE_BUFFER for SEPOS-side module
init logs to discriminate (a) vs (b); then either implement the missing
hardware handshake or emulate the skg store (FTL/dxio RE).

## 2026-08-03 (cont.), sign-op failure pinned: unregistered provider slots

- SEP_ENABLE_TRACE_BUFFER compiled in; enable_trace_buffer() IS called
  with a valid shmbuf_base (0x100c000) and writes the TRAC object mapping,
  but SEPOS emits ZERO trace records on this image -- the TRAC object is
  not honored (t8030/iOS14). No SEPOS-side visibility via this route.
- The sign op (F4F02B0) reads its provider slot(s) at payload offsets
  0x55cce8/0x55cd00; pmemsave at runtime shows ALL ZEROS (128B region).
  `cbz` on the empty slot -> error -> -10 -> 0xe007c00a.
- Static scans of sepfw.bin find NO writer for those slots (no adrp+add
  store, no literal-addressed store anywhere in the payload): the slots
  are populated by the L4 loader's DRGKCATS fixup pass, not by code.
  Conclusion: the load-time fixup population for this vtable is missing
  or fails on the emulator, OR the registering module (which would fill
  them via its own init) never completes init on the emulator (candidate
  dependency: the unemulated skg/xART store).
- Net: every on-device blocker for "osc" is now precisely located at
  SEPOS image/fixup level. Options remaining: (a) DRGKCATS/loader
  analysis to find why the vtable stays empty, (b) enable a working
  SEPOS trace for module-init visibility, (c) emulate the skg/Lynx FTL
  store. All multi-day RE.

## 2026-08-03 (cont.), the "osc" wall fully characterized

Final resolution of the dev-key path failure:
- F4F02B0 (the sign op wrapper) does ldr-literal of its provider slot at
  payload offset 0x55cce8; the slot is ZERO both in the file and at
  runtime (pmemsave). cbz -> return -1 -> sub_5112C4 -> -10 -> 0xe007c00a.
  A second slot at 0x55cd00 is zero the same way.
- No code in the SEPFW payload stores to either slot (whole-file scan for
  adrp+add and literal-addressed stores): population is expected from the
  L4 loader's DRGKCATS fixup pass or from the "scrd" task registering its
  ops at runtime. Both config records (0x438F, pass at 0x3406ec020, scrd
  at 0x3406e8018) are populated by SEPFW's own XPRT with v1=1, flag=1
  (padded layout: v1 at offset 8, flag at offset 36), so the dev-key
  branch is natively active on this research SEPFW; it still fails at the
  empty provider slot.
- Therefore: either the "scrd" task never completes bring-up on the
  emulator (likely waiting on a hardware handshake the emulator does not
  implement), or its provider registration is gated on the unemulated
  skg/xART store. Discriminating needs SEPOS-internal visibility: the
  SEP_ENABLE_TRACE_BUFFER path compiles and enable_trace_buffer() runs,
  but SEPOS on t8030/iOS14 ignores the TRAC object (zero trace records).
- Everything else is proven dead: donor ADI store (ignored by iOS 14
  adid), donor activation record + full identity (gsa 307), 0x438F
  override + AESS seed-KDF + PKA ACK (still -10 at the empty slot).
REMAINING ROUTES (all multi-day): (1) make the SEP trace buffer produce
records for this image; (2) reverse scrd task bring-up and emulate the
handshake it waits on; (3) emulate the skg/Lynx FTL store and seed a
storage-backed "sep sub ca key" (user-preferred high-fidelity route).

## 2026-08-03 (cont.), provider image located at payload 0x55c000

- The payload has 17 Mach-O images (L4 kernel + 16 tasks) at 0x4000,
  0x2c4000, 0x2d8000, 0x2e4000, 0x2f0000, 0x30c000, 0x320000, 0x344000,
  0x34c000, 0x35c000, 0x498000, 0x4c8000, 0x4d4000, 0x55c000, 0x568000,
  0x5fc000, 0x630000.
- The SIK sign compute (sub_5112C4) lives in the image at 0x4d4000
  (AppleKeyStore_SEP/"pass"). The empty provider slots (0x55cce8,
  0x55cd00) sit in the image at 0x55c000, i.e. the provider is that
  task's module; its runtime registration (into a shared registry at SEP
  RAM base offsets 0x34000060/0xa0/0x1e0/0x200/0xc0/0x140, heavily
  referenced by that image) never happens on the emulator.
- Also in that image: MMIO-ish refs 0x2a0b014a, 0x2a1303e8, 0x29421263,
  0x3d8003e0, 0x3d800be0, 0x3dc00075 (candidate hardware handshakes it
  waits on -- next RE target), and 0x39402xxx/0x3953xxxx internal ranges.
- Both dumps (pmemsave) confirm payload identity-mapping at 0x340000000
  and SEPFW's own XPRT populating the 0x438F records natively.

## 2026-08-03 (cont.), provider slot: call-through PROVEN, path narrows

- Sentinel experiment (QEMU writes 0x1 into provider slot 0x34055cce8 at
  AESS-cmd3 time): the slot landed (pmemsave reads 0x1) and the "osc"
  error CHANGED from e007c00a to e00002f0/e00002e6. PROVEN: the sign op
  calls through the slot; a correct provider address makes it proceed.
- The slot lives in the provider task image at payload 0x55c000 (its text
  literal pool, image-relative 0xCE8); no code in the payload stores to it,
  so it is populated by the L4 loader's DRGKCATS fixups (region at payload
  0x63c000, magic "DRGKCATS") or by the provider task's runtime
  registration. The payload is nested: the 0x55c000 image's __DATA (file
  0x568000) begins with a byte-swapped Mach-O (0xcefaedfe) — tasks embed
  continuation images in their data segments.
- PC-sampling of the SEP CPU is inconclusive (task bursts are ~ms; 220/220
  samples sit in the kernel idle loop even while sel:43 fires twice).
- The e00002f0 error (with sentinel) vs e007c00a (empty slot) shows the
  op-family works with any nonzero pointer; the gate is precisely the
  missing provider address. Getting it requires: parsing DRGKCATS for the
  fixup value, making scrd finish bring-up, or reversing the nested image
  layout to find the ops registration. All multi-day RE.

## 2026-08-03 (final), complete picture + the one remaining sub-project

The "osc" provider slot (payload 0x55cce8) is not covered by any DRGKCATS
fixup entry (region scanned) nor written by any code in the payload: it is
populated at runtime by the provider TASK registering its ops object. The
provider is the big nested task image at file 0x568000 (LC_UNIXTHREAD,
__TEXT 0x94000 bytes starting at vm 0x8000, __DATA vm 0x9c000) — likely
the CoreCrypto/scrd task. Its __DATA holds a vtable with text-VM code
pointers (e.g. file 0x5fc088 -> {0xafc0, ?, handler@0x2dc14, ...}), a
plausible ops object; the slot should hold that object's runtime VA.
Whether that task completes bring-up on the emulator is UNKNOWN (PC
sampling is too coarse to catch ms bursts; SEP trace buffer is ignored by
this image). Computing the slot's correct runtime value requires the L4
loader's image->VA load map (or a full DRGKCATS parse) -- a multi-day
sub-project by itself, with tbsc/albert/gsa server acceptance still
unproven afterwards.

Sentinel proof (repro): build with the AESS-cmd3 hook writing 0x1 to GPA
0x34055cce8; the slot reads 0x1 and the "osc" error changes from
e007c00a (empty) to e00002f0/e00002e6 (bogus pointer). NOTE: the AESS
cmd-0x13 normalization on T8020 does NOT strip bit 0x10 -- the case must
match (0x10 | CREATE_KEY_FROM_SEED); my first build silently never ran.

## 2026-08-03 (final), shim mechanics proven; remaining = correct provider object

Experiments (all reproducible):
- Slot shim v1 (slot := GPA 0x3405fc088, its +8 := identity-runtime
  0x340825c14 for the 0x2dc14 handler): the sign op VALIDATES the slot
  (no more e007c00a) and CALLS the handler, which fails internally
  (e00002f0 / e00002e6). Mechanism proven end-to-end: QEMU can inject a
  provider pointer and the "osc" sign op uses it.
- Error taxonomy: empty slot -> e007c00a (validation reject); bogus/
  wrong pointer -> e00002f0 / e00002e6 (execution failure).
- The correct slot content is the provider task's registered ops-struct
  (whose +8 is the op dispatcher taking op w6=0xa). The candidate vtable
  at payload file 0x5fc088 holds LINK-VM values (0xafc0, 0x2dc14) --
  unchanged at runtime (pmemsave), i.e. the provider task's registration
  (which computes runtime addresses) has NOT run on the emulator.
- Getting the correct struct requires the L4 loader's image->VA map or
  the provider task's live registration -- a multi-day RE, and the actual
  signing handler may additionally need the (unemulated) skg store for
  key material. tbsc/albert/gsa acceptance remains unvalidated beyond.

Current best hypothesis for full "osc": emulate enough of the provider
task's bring-up that its registration runs naturally (preferred), or
compute+shim the ops-struct from QEMU with the dispatcher's identity-VA
(0x340568000 + (handler_vm - 0x8000)) -- bounded by the same unknowns.

## 2026-08-03 (final), end of bounded experiments

- Corrupting the provider task's entry (payload 0x568000, entry file
  0x56f030) at SEP reset had NO observable effect (SEP healthy, keybag
  fine) -- the provider task's entry either never runs on the emulator or
  its text is relocated off the identity GPA.
- Corrupting sub_5112C4's entry (identity GPA 0x3405112c4) removed the
  e007c00a validation-reject from subsequent attempts (all attempts then
  fail e00002e6/e00002f0 with the provider shim active) -- pass's text
  IS identity-mapped at 0x340000000+file_offset.
- Net: the provider task (nested image at 0x568000) does not complete
  registration on the emulator. The "osc" wall is precisely: provider
  slot 0x55cce8 empty + no runtime registration of the provider's
  ops-struct. Shim mechanics are proven (QEMU can inject and the op
  calls it). Remaining work is the L4 loader map / provider bring-up RE
  (multi-day) + skg-store RE if the handler needs store key material, and
  tbsc/albert/gsa server acceptance validation thereafter.

## 2026-08-03 (final), provider = "sks" task; registration gated on "acss" object

- The payload header (0x0-0x4000) holds the L4 task table: 0x80-byte
  entries {name(16s), hash(16), flags, image_off, size, load_addr,
  data_size, entry_pc, ...}. Key entries:
    "pass" -> image 0x4d4000 (AppleKeyStore_SEP: SIK/osc handler)
    "sks"  -> image 0x55c000 (SEP KeyStore -- THE sign provider), entry
              pc 0x10490, size 0xc000
    "hdcp" -> image 0x568000 (the big 0x94000 image; NOT the provider)
    "sprl_d4x", "sse_r10", ...
- So the empty slot (payload 0x55cce8) is sks's exported-handler slot:
  pass imports it (ldr-literal -> cbz -> -10). sks RUNS on the emulator
  (keybag ops work), so its init completes; the sign-provider registration
  is gated on the store containing the "sep sub ca key" (xART tag "acss",
  referenced ~9x in pass at 0x54b3c9-0x54be3b as a 0x73736361 immediate).
- The store IS the SSC (sep_ssc file, plaintext on disk; AES-CCM is
  wire-only). Master sep_ssc slots 1-6 hold records
  {u32 len; u8 slot; u8 subtype; ...; 32-byte key blob at offset 0x20;
  CRC16-CCITT LE at offset 0x1e (verified for slots 1-2)}; slots 7+ are
  zero. Boot walk (SSC-TRACE) reads every slot 1-72 once, re-reads 1-6.
- The seed: add an "acss"-format record in an empty slot (7+) with a
  synthetic UID-wrapped key blob (wrap = AES-256-CBC with hardcoded
  AESS_UID0/1, iteration XOR -- computable offline). The record subtype/
  tag value and blob layout for "acss" need one focused RE pass over the
  sks init (image 0x55c000, 0xc000 bytes): find where init reads slots,
  which subtype it treats as the SIK signing key, and the exact record
  acceptance checks (CRC/len/tag). Then seed and "osc" should register
  the provider and sign.

## 2026-08-03 (final), next concrete target: sks init RE

- Correction: the 0x34000xxx references (0x34000060 x16, 0x340000a0 x7,
  0x340001e0 x5, ...) are in the SKS image (0x55c000-0x568000), not hdcp.
  The region at GPA 0x34000000+ reads all-zero at runtime (1KB dump), so
  that registry area is not live-populated -- consistent with no provider
  registration.
- The seed path is now fully specified except for the record format:
  (1) sks init reads the "acss" object from the SSC store (plaintext
      sep_ssc); (2) absent -> no sign-provider registration -> slot
      0x55cce8 zero -> "osc" -10. (3) Seeding an "acss" record into a free
      SSC slot (7+) should make init register the provider.
- REMAINING RE (bounded, one image): disassemble sks's init (entry file
  0x564490, init via 0x563f84 -> 0x5640f4/0x563f10, main 0x561654) to
  find: which dataslot the SIK key lives in, the record acceptance checks
  (len/tag/CRC), and the provider-registration call. Then write the
  offline seeder (record + UID-wrap = AES-256-CBC with AESS_UID0/1,
  iteration XOR) and test "osc" end-to-end.

## 2026-08-03 (final), L4 task table decoded; the slot is an unresolved import

- Payload header (0x0-0x4000) = L4 task table, 0x80-byte entries with
  ASCII names: pass(0x4d4000), sks(0x55c000), hdcp(0x568000),
  sprl_d4x(0x5fc000), sse_r10(0x630000-ish), ... Fields include
  image_off, size, load_addr, entry_pc. pass load_addr 0x6ec000 (its data
  region; config record at 0x6ec020 sits there, GPA 0x3406ec020 --
  identity). Task __DATA lives in the payload tail (0x6ec000+); task
  __TEXT runs identity at 0x340000000+file_offset (proven by the
  sub_5112C4 corruption changing "osc" behavior).
- The 0x55c000 image (sks) registers the crypto services (its main at
  0x561654 registers "hdcp"/"AKF "/"KEY "/"AESH" service handles into
  0x56b4d0/0x568388/0x568390). sks is loaded (identity header visible at
  GPA 0x34055c000) and running (its __DATA at 0x3406f0000 is populated
  with live pointers).
- The provider slot at 0x55cce8 is an IMPORT slot in sks's literal region
  that pass reads; the L4 loader should fill it from DRGKCATS (payload
  0x63c000). No DRGKCATS entry references it (scanned both u32/u64), and
  no code stores to it -- so the import is unresolved ON THE EMULATOR.
  The two viable reasons left: (a) the DRGKCATS fixup pass doesn't
  process this import (needs the DRGKCATS format reversed -- entries are
  chained/encoded, not plain addresses), or (b) sks's own init registers
  the handler conditionally on the "acss" store object (still gated on
  the unemulated store, requiring the record-format RE + offline seeder).
- Everything else is done/proven dead. The App Store Today chain is fully
  mapped from the 401 down to this one empty import slot.

## 2026-08-04, frida-server root-caused + patched (was "abandoned" prematurely)

The earlier "PAC garbage pointer" verdict was the 17.x universal binary's
arm64e slice. The arm64-only 16.7.10 build fails differently and fixably:

- Full crash report via launchd job + idevicecrashreport (daemon install
  path: rootfs container cycle, mount_apfs -o noowners, files into
  /usr/libexec + /System/Library/LaunchDaemons + merged xpc/launchd.plist;
  hdiutil attach of the extracted container needs NO -nomount or the APFS
  stack never synthesizes volumes).
- Crash: EXC_BAD_ACCESS (write/translation fault) at dyld_base+0xc000,
  inside sys_icache_invalidate. A mod_init function walks a page list of
  dyld __TEXT, copies each 0x4000 region, applies slide fixups, writes it
  back over the original (COW), then gum_clear_cache()s the original
  (frida thunk = sys_icache_invalidate + sys_dcache_flush). On this
  research build's dyld (iOS 14.0 18A5351d), __TEXT has a 16K PROT_NONE
  hole at +0xc000 (r-x 48K / --- 16K / r-x 384K) and the flush of the
  hole page faults. Release dyld presumably has no hole there.
- Fix: NOP the `bl #0x6107c` (gum_clear_cache thunk) at file 0x56738 in
  frida-server-16 (16.7.10, matches venv client 16.7.10). The write-back
  to the hole already failed silently (mach_vm_write), and the hole holds
  no executable code, so skipping its flush is harmless. Re-signed with
  frida-ents.plist (ldid -S<path> with NO space; the space form asserts).
- Staged binary: /tmp/frida-stage/frida-server (patched+signed); launchd
  plist re.frida.server.plist (listens 0.0.0.0:27042). Access chain:
  host ssh tunnel 27042 -> companion iproxy 27042:27042 -> guest.

## 2026-08-04 (cont.), frida-server WORKS on the guest

- Second crash after the flush NOP: main-loop thread called through a libdyld
  -> dyld function pointer landing in the same hole (dyld+0xFCC0) -- the
  init machinery's write-back had redirected a pointer into the never-written
  hole page. Fix: also NOP the machinery's driver call `bl #0x561c4` at file
  0x62b54 (skips the whole copy/fixup/writeback/flush pass over dyld pages).
  With both NOPs: frida-server 16.7.10 starts, listens on 27042, frida-ps
  enumerates ~176 processes (akd, adid, appstored, itunesstored,
  mobileactivationd all visible).
- Attach needs /usr/lib/frida/frida-agent.dylib on-device: from the
  frida_16.7.10_iphoneos-arm64.deb (var/jb/usr/lib/frida/), ldid-signed with
  frida-ents.plist, installed via the same rootfs cycle.
- Tooling: /tmp/frida-stage/{swap-frida.sh, install-frida.sh} -- attach with
  `hdiutil attach -imagekey diskimage-class=CRawDiskImage` (NO -nomount, the
  APFS stack must synthesize volumes), then mount_apfs -o noowners
  /dev/<sysvol> /tmp/frida-rootfs/mnt. Access chain: ssh tunnel
  27042->companion iproxy->guest; client = ~/InfernoData/venv (16.7.10,
  must match server major.minor).
- NOTE: never background the swap script with a trailing `&` in the same
  shell line as the boot -- a killed shell mid-dd corrupts state; run swap
  in foreground, boot as a tracked background task.

## 2026-08-04 (cont.), frida attach blocked by xnu JOP thread-state check

- frida-ps works (16.7.10 patched; also 15.2.2 arm64 via the same job).
- Attach fails on arm64e targets: 16.x: thread_create_running KERN_PROTECTION_FAILURE;
  15.2.2: thread_set_state KERN_PROTECTION_FAILURE.
- Root cause in xnu-7195 osfmk/arm64/status.c machine_thread_state_convert_from_user():
  a JOP-DISABLED caller (any arm64-slice process, e.g. an arm64 frida-server) may NOT
  set thread state on a JOP-ENABLED target (every arm64e system daemon) ->
  KERN_PROTECTION_FAILURE. On real jailbroken devices frida-server runs its arm64e
  slice (JOP-enabled caller) and passes properly ptrauth-signed state. Under
  hvf-pauth-noop, PACIA is a NOP, so the arm64e slice's signing produces raw pointers;
  the kernel's verify (AUT) is likewise a NOP, so an arm64e frida-server should pass.
- iOS 14 launchd loads jobs ONLY from the merged System/Library/xpc/launchd.plist;
  the entry needs Program (string) -- ProgramArguments arrays in the merged entry
  did NOT spawn (job silently absent, no logs, no crash report). Standalone plists
  in /System/Library/LaunchDaemons are decorative on this build.
- arm64e slices of frida-server 15.2.2 (both old ABI subtype 2 and new ABI
  0x80000002) do not start as the re.frida.server job: no crash report, no listener.
  arm64 slices run fine. Untested whether ANY adhoc-signed arm64e exec runs on the
  guest (all working inferno helpers are arm64). Added /usr/libexec/inferno-exec-test
  (arm64; execve's /usr/libexec/frida-server15, logs errno to /var/mobile/Media/
  exec-test.log) as merged job com.inferno.exectest to split exec-denied vs
  runtime-failure.

## 2026-08-04 (cont.2), frida attach: JOP wall mapped; kernel patch unstable; SEP ops-cell contract decoded

Frida status:
- 16.7.10 arm64 server runs fine on the guest (2 NOPs for the dyld-hole
  page machinery). Attach/spawn into arm64e daemons is blocked by xnu's
  machine_thread_state_convert_from_user(): a JOP-disabled (arm64) caller
  may not set thread state on a JOP-enabled (arm64e) target ->
  KERN_PROTECTION_FAILURE. Confirmed in xnu-7195.50.7.100.1 osfmk/arm64/status.c.
- arm64e frida-server slices: old-ABI (subtype 2) -> execve EBADARCH
  (kernel rejects the slice outright); new-ABI (0x80000002) -> execs but
  SIGSEGVs in libsystem_pthread init with a PAC-signed garbage pointer
  (0x5b0954...). A minimal locally-built arm64e hello-world execs fine,
  so the platform allows adhoc arm64e; frida's own init breaks.
- user_jop=0 / user_ts_jop=0 boot-args are honored by the research
  kernelcache and are boot-stable.
- Kernel patch (kernel_patches.c: ck_kp_thread_state_patch, NOPs the
  caller-side JOP branch in convert_from_user at kernelcache
  0xfffffff007b61874) APPLIES and the guest boots + frida-ps works, but
  launchd deterministically SIGBUSes (initproc panic) minutes later --
  something at runtime depends on the cross-JOP failure semantics.
  Patch left in the tree but DISABLED (if (0)). DO NOT enable blindly.
- New guest infra: inferno-fileserver (launchd, TCP 27044 via hostfwd in
  run-goal-hx.sh): GET/PUT/PUTX/LS for /var/mobile/Media plus
  GETA/LSA <abs path> -- reads CrashReporter WITHOUT lockdownd pairing.
  Source: /tmp/frida-stage/fileserver.c.
- iOS 14 launchd loads jobs only from the merged
  /System/Library/xpc/launchd.plist and needs Program (string);
  ProgramArguments arrays in the merged entry silently don't spawn.

SEP osc line:
- pass's sign primitive F4F02B0 (called from sub_5112C4 with len 0x20)
  reads an OPS STRUCT at sks image +0xce8 (payload 0x55cce8):
  +0x00 ready (cbz-checked), +0x08 handler(x0=&cell, x1=ctx, x2=len,
  x3=data, x4=x5=0, w6=op (0xa=sign)), +0x18 finisher (braaz, called from
  the completion callback at 0x4f039c). All zero on disk AND at runtime.
- Exhaustive scans: no code in the payload writes the cell (VA or PA
  forms), no static vtable in sks/skg images points into their own text,
  so the cell is runtime-registered by an IPC-delivered (memcpy) struct --
  most plausibly from skg after it loads the "acss" key object from the
  xART store (which is empty on the emulator).
- Payload anatomy fully mapped: 17 Mach-O images; table in the header
  lists SEPOS, SEPD, AESSEP, dxio, entitlement, skg, sars, ARTM, xART,
  eispAppl_d4x, scrd, pass, sks, hdcp, sprl_d4x, sse_r1. Images split for
  IDA at /tmp/ida-sik/images/*.macho (TEXT only; __DATA/__LINKEDIT live
  scattered in the payload tail 0x6c0000-0x76c000; pass __DATA at
  0x6ec000, sks __DATA at 0x6f0000).
- sks has an LC_SYMTAB (nsyms 0x5e3) but its tail placement is unresolved;
  the tail contains per-image symtabs (hdcp's at ~0x6f4000 with
  HDCPInterface symbols -- note sks's main registers the hdcp/AKF/KEY/
  AESH services, so sks and hdcp share code).
- SEPD's image (0x2d8000) contains the service routing table of 4CCs
  (both byte orders): pair, sse , hdcp, sprl, scrd, xART, hilo, hibe,
  "sks ", sksm, boop, etc.

## 2026-08-04 (cont.3), gadget attempt + SEP ops-cell RE reversal

Gadget route (LC_LOAD_DYLIB frida-gadget 16.7.10 into akd / inferno-diag
control via insert_dylib, signed with the target's ORIGINAL entitlements):
the gadget's dyld-remap machinery (copy dyld pages -> fixup -> write-back ->
flush) is incompatible with this guest's DSC/dyld layout: first the icache
flush of the dyld +0xC000 PROT_NONE hole (SIGBUS, NOPed at arm64e-slice
+0x3ee04 flush and +0x56400 driver), then an instruction-fetch translation
fault calling libsystem_platform+0x524c (pacibsp) because the machinery's
write-back/deallocate breaks live DSC pages. Killing the whole subsystem
needs more invasive cuts; PARKED. The machinery is the thing to stub if this
is resumed: top mod-init is gadget arm64e-slice function at +0x18a1c (called
with args; needs a void-return-safe stub), or find gum's env/config guard.

SEP "osc" line -- SUBAGENT REVERSAL (verified by spot-check):
- The cell at 0x55cce8 is pass's OWN bss, written UNCONDITIONALLY by pass
  init (0x50bb90 main -> 0x4f1a24 -> 0x4ecaa0 -> builder at 0x53df18):
  {+0=0xa0, +8=paciza(0x53df60 handler), +0x18=paciza(0x53e038 finisher),
  +0x10=paciza(0x53e1bc), +0x20=paciza(0x53e234), +0x28=0x55ccd8}, with
  [0x55ccd8] = 0x559b10 (channel descriptor const, fixed up at load).
  It is the DRBG/TRNG client channel to SEPD's "TRNG" service, NOT the SIK
  provider. The "acss"/"sep sub ca key" xART fetch belongs to a LATER step
  (key-open in sub_50E80C/sub_50EBC4, op 20007).
- RUNTIME TRUTH on the emulator (pmemsave, live guest): descriptor
  0x340559b10 present with fixed-up pointers, but the cell at 0x34055cce8
  is NEVER WRITTEN. pass serves the osc RPC (its F4F02B0 reads the cell ->
  e007c00a), so pass runs but its init never executed the cell-builder.
  => The first blocker is now: WHY does pass's init not reach 0x4ecaa0 on
  the emulator. (Follow-up RE in flight.) The "acss" store gap comes after.

## 2026-08-04 (cont.4), SEP forensics: identity-vs-live VA trap; PC sampling added

- INFERNO-DIAG corruptions + provider-slot shim REMOVED from sep.c (they were
  pollution from the discrimination experiments; the 0x438F config override
  remains). The cell stayed zero after removal -- they were not the cause.
- New QEMU tooling (sep.c, uncommitted): `sep-regs` HMP command (dumps the
  SEP core pc/lr/sp via cpu_synchronize_state) + a 20ms boot PC sampler
  (SEP-PC-SAMPLE in the run log, 180s window). Findings: SEP idles in the
  SEPOS WFI loop (0xffffffe00000af90) once booted; tasks execute at their
  OWN VAs (0x1ec9../0x21e3../0x238a../0x49a6.. ranges), NOT at the identity
  payload GPAs. The identity region (0x340000000+) is the loader's staging;
  pass's LIVE pages are elsewhere (its descriptor's resolved pointers
  reference ~0x468c77000 on this boot).
- CRITICAL CORRECTION to earlier conclusions: the zero cell at identity GPA
  0x34055cce8 proves NOTHING about pass's live bss -- the cell may be
  populated in pass's own pages. The "sentinel changed the osc error"
  experiment is now suspect (it also wrote the shim; needs redoing against
  pass's live VA page).
- The payload's chained fixups ARE applied in-place in the identity region
  (0x559b10 descriptor shows resolved pointers), so identity == live for
  __DATA_CONST at least. The bss cell region is zero both ways.
- Open: does pass serve the osc RPC at all? e007c00a may be synthesized by
  another responder if pass's workloop never starts. Next: sample the SEP
  PC *during* an osc attempt to see which code actually runs.

## 2026-08-04 (cont.5), pass runs; the identity-GPA trap; TRNG suspect

- Discrimination proof (jump-time UDF at pass main entry 0x34050bb90):
  SEP panics at INIT/BOOT => pass's main DOES execute from the staged
  payload. So the DRBG cell builder runs and the cell is populated in
  pass's LIVE pages. Probing the identity GPA measures only the loader's
  staging copy -- all earlier "cell is zero" conclusions were measuring
  the wrong pages. (The jump-time hook is the effective one; reset_hold
  writes get overwritten by the AP's firmware upload.)
- The osc flow per RE: keygen (DRBG derive via the cell) -> key-open
  ("sep sub ca key" from xART store) -> sign. The first plausible failure
  on the emulator is now the DRBG channel to SEPD's TRNG service (or the
  store fetch after it). TRNG MMIO emulation exists in sep.c (status/FIFO);
  whether SEPD's TRNG service answers correctly is the open question.
- Tooling added (uncommitted): sep-regs / sep-trace <sec> HMP commands
  (live SEP PC via cpu_synchronize_state; 20ms sampler). Boot sampler
  shows SEP idles in the SEPOS WFI loop when no work is pending.
- Companion usbmuxd restart fixes USB dropouts:
  sudo sh -c 'USBMUXD_DEFAULT_DEVICE_MODE=3 nohup usbmuxd &'
- NEXT: sample the SEP PC during a real osc attempt (needs a UI tap on
  "Try Again" / pairing Trust) and histogram the executing task VAs.

## 2026-08-04 (cont.6), osc flow pinned + research doc integrated

- New research doc OSC_SIK_ACTIVATION.md (repo root) + decompiles in
  /tmp/ida-sik/certs_out/: osc = AKS selector 43, sub-op 1 = build the SEP
  Device Certification leaf (base64'd as scrt-part1). Flow: key-create
  (AESS 0x13 CREATE_KEY_FROM_SEED + PKA 0x80 ECPUB_ATTEST) -> sub_50276C
  cert builder -> reply. Signing key source (sub_50E80C case 1): xART fetch
  of "sep sub ca key" (sub_50EBC4, store op 20007, tag "acss") or 0x438F
  embedded seed (PROVEN dead end: obj+352 != 0 -> -10).
- RUNTIME CONFIRMED on the clean build: a Try Again tap fires AESS 0x13 +
  PKA 0x80 (keygen runs). The failure is the xART store fetch (empty store).
- PKA 0x80 emulation in sep.c pka_handle_cmd is a STUB (acks interrupts,
  no EC computation) -- will matter for cert validity AFTER the store fetch
  is fixed; not the current blocker.
- New: UI fully drivable via QMP input-send-event (taps/swipes); device
  paired via a QMP-tapped Trust dialog. Mailbox traces
  (apple_a7iop_mailbox_send/recv, role SEP-iop/SEP-ap) captured.
- Tether note: companion NAT masquerade was down; restored with
  iptables -t nat -A POSTROUTING -o enp0s1 -j MASQUERADE + FORWARD rules.
  The tether carries guest internet when iOS picks it (wlan RX is broken
  but association works; iOS prefers whichever has reachability).
- NEXT: xART store backend RE (subagent) -> seed "sep sub ca key" via QEMU,
  then the PKA ECPUB_ATTEST needs a real P-256 point-multiply emulation.

## 2026-08-04 (cont.7), XART_SIKP.md integrated; implementation path chosen

- New research (XART_SIKP.md): the "sep sub ca key" store is the skg task's
  "acss" object store (op 20007 read via sub_51D420), backed by SEP NAND via
  Lynx/dxio, UID-wrapped. Two "xART" paths must not be conflated: the AP
  AppleSEPXART (.gl/SSC locker) is NOT the skg acss store.
- sikp is the factory provisioning op (writes caller key material into the
  store); gated by reentrancy byte, a provisioned bit, config 0x4393, and
  request-flag bit 28. On the empty emulated store the provisioned bit is
  clear so the reject path is skipped, but the full DER schema is unrecovered.
- Ranked options (from the doc): (1) intercept the skg acss read (op 20007)
  and return a canned 64-byte object -- smallest path; (2) implement store
  write + boot-seed a UID-wrapped record (more genuine); (3) drive sikp over
  selector 43 with synthetic DER (needs the schema); (4) RELEASE
  mobileactivationd never calls sikp.
- The fetch contract: sub_51D420(0, 101, 20007, 6, 64, buf) must return a
  64-byte object that opens as a storage-backed key with obj+352 == 0.
- Runtime note: osc keygen confirmed live (AESS 0x13 + PKA 0x80 on tap).
  PKA 0x80 is a stub in sep.c -- needs a real P-256 point-mul (subagent RE
  of the PKA register interface in flight).

## 2026-08-04 (cont.8), skg dispatch table located

- skg's store-op dispatch table is at payload 0x31db00+ (in skg image
  0x30c000): records {u32 opcode, u32 handler_off, ...}. op 20007 (0x4e27)
  at 0x31db2c -> stub at skg+0xd3b4 (payload 0x3193b4: bti; mov w1,#0xb;
  b 0x3192d4) -> common handler 0x3192d4. op 20015 (0x4e1f) at 0x31dcdc.
  This is the read/write entry the subagent is tracing to the backend.
- The store read does NOT hit unmapped space (no unassigned/unimp logs
  during osc) -> the backend is a mapped-but-empty region or an emulated
  device returning zeros. Seeding = write the record to the right GPA.

## 2026-08-04 (cont.9), PKA ECPUB_ATTEST is key-slot-based (not MMIO operands)

- Traced all PKA base + TMM register writes around command 0x80 (ECPUB_ATTEST):
  the SEPFW writes ONLY [0x00]=0x80 then [0x04] interrupt acks -- NO operand
  writes to pka_base or pka_tmm. So the op reads the key from a SEP-internal
  key slot (established by the preceding AESS 0x13 CREATE_KEY_FROM_SEED), not
  from MMIO. Consequence: the current PKA stub (ack-only) does NOT block local
  osc completion; it only affects the cert's public-key content (matters for
  albert acceptance, not for getting a blob back). The store fetch is the
  sole current blocker.
- PKA MMIO: base = armio_base+0x41100000 (GPA 0x241100000), TMM =
  armio_base+0x41504000. No SEPFW MMIO operand traffic to either for cmd 0x80.

## 2026-08-04 (cont.10), store backend fully mapped (subagent RE)

- The "sep sub ca key" store backend = dxio's 128 KiB RAM buffer (L4 shm,
  allocated on first store request; base at dxio global [0x30c7c0]). NO
  NAND on the read path -- objects live in RAM, so a seeded object persists
  for the boot. pass consumes only reply bytes [+0x10..+0x30) as the key;
  the reply must be exactly 64 bytes. Reads are likely RAW (no MAC/unwrap
  on this record class) so a raw seeded record should work.
- Module-name corrections: skg=0x320000 (SEP_Storage/Lynx), dxio=0x2f0000
  (object-store engine, GDST parser), xART@0x35c000 is actually the EISP
  camera stack (misnamed). The acss tag tables are the entitlement image's.
- Fetch wire: pass opens endpoint "skg " (auto-published from the L4 task
  name), calls sub_51D420(handle, sel=0x65, op=0x4E26/20006, flags=6,
  len=0x40, buf) with "acss" at buf+8. skg forwards to dxio via op
  0x4070-0x4073, subcmd 0xF0|(index<<13), 32-byte units.
- sikp (handler sub_506C48) is viable on the empty emulator: mode byte
  [0x55cda9]==0 and config 0x4393 is false, so the gates pass; it writes
  via op 20015 to the same dxio backend. Needs the DER schema (incomplete).
- Seed points ranked (subagent): (1) write dxio's buffer directly (needs
  GDST layout -- in progress), (2) drive sikp (consistent), (3) intercept
  skg's reply (least invasive but SEP-internal, hard for QEMU).
- NEXT: GDST record layout (subagent) -> write the record into dxio's
  buffer at runtime (find buffer GPA via dxio globals; the identity-VA
  trap means I must resolve dxio's live data page first).

## 2026-08-04 (cont.11), PKA 0x80 fully resolved -- NO real PKA work needed

- The PKA driver is the AESSEP module (0x2d8000). Register map (offsets from
  PKA base 0x241100000): 0x00 = op selector, 0x04 = GO/status (write 1=go;
  read bit1=busy, 0x600=error), 0x08 = engine command (0x80), 0x40-0x5c =
  32-byte input window (LE), 0x60-0x7c = 32-byte output window.
- KEY FACT: for cmd 0x80, SEPFW reads back only 32 bytes and treats dword 0
  as a status (0 and 1 both accepted). The osc cert's P-256 pubkey is
  computed by pass's SOFTWARE corecrypto EC (curve params at payload
  0x5593d8/0x559498) from the fetched private key -- the PKA does NOT supply
  it. So the current ack-stub is sufficient for local osc completion AND the
  pubkey is genuine (software-computed). No PKA implementation needed.
- Boot self-test runs cmd 0x80 with zero windows at startup; the stub
  already completes it gracefully.
- So the ONLY real blocker is the store fetch ("sep sub ca key"). The seeded
  key just needs to be a valid P-256 scalar; pass derives the pubkey itself.

## 2026-08-04 (cont.12), record-format endgame + sep-write tool

- Dumped dxio's two fixup-zeroed op tables at runtime (GPA 0x34030baa0 /
  0x34030bad8): they ARE populated with handler pointers (VAs ~0xfc9cxxxx)
  -> the record handlers are now statically decodable (subagent on it).
- Buffer global at 0x34030c7c0 is ZERO even after an osc attempt: dxio's
  128 KiB store buffer is allocated ONLY on the first GDST store WRITE
  (msg 0xC351), not on reads. So seeding needs either a store write first
  (to allocate) or a different trigger. The record handlers will say what a
  read with base==0 returns.
- Added QEMU HMP `sep-write <gpa> <hex>` (writes guest physical memory via
  address_space_rw) -- the seed delivery mechanism. In the build from
  21:11; live after next boot.
