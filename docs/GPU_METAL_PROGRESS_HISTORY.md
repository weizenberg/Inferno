# GPU/Metal detailed history — archived 13 September 2026

This is a historical evidence log. For current status, use
[GPU_METAL_PROGRESS.md](../GPU_METAL_PROGRESS.md).

---

# GPU and Metal progress

Last updated: 13 September 2026, 13:12 Israel time (10:12 UTC).

## Overall status

**The host GPU bridge works in test guests. Native Metal acceleration inside
the iOS 26 guest is not working yet.**

V5 is published with compiled-library loading, shared buffers and textures,
samplers, and ordered compute/render commands. Host GPU tests and freestanding
ARM tests under TCG/HVF pass within the scopes recorded below. The application
provider tests substitute the coordinator/IOKit connection; they do not prove
that ordinary iOS applications can use the GPU yet.

The portable v6 argument-buffer interface is implemented and accepted for
integration. All 14 iPhoneOS/macOS compile/link stages pass, and the decoder
preserves nine descriptors from two recorded native layouts under ASan/UBSan.
Host/driver and provider v6 changes are implemented. The coordinated QEMU
build and signature checks pass, as do all 46 provider SDK compile/link stages.
The first provider argument-buffer GPU checks pass: 32 completed commands and
3,328 independently checked input/output/guard bytes across four native layouts.
These tests substitute the coordinator/IOKit connection. Native iOS runtime
integration and broader v6 acceptance remain unfinished.
Legacy opcodes 1–7 now pass under TCG/HVF, with all 28 captures matching the
accepted references. V5/v6 client negotiation passes 142 sanitized checks.
Focused provider tests pass resource lifetime, bulk-update rejection, sampler
recovery, writable aliases, error-gated writeback and destination selection.
The v6 source is ready for final review. After Fable reached its session limit,
the user switched planning to Sol. Sol is now reviewing the v6 source and, in
parallel, preparing the render/null-buffer implementation plan. Codex reviews
the results before Sol implements changes. Publication awaits the final review;
full native iOS integration remains unfinished.
The preceding r2 revision returned Opus metadata and was rejected for technical
errors; r3 returned actual Fable 5.1 metadata.

New host rendering references now pass eight buffer/constant draws and four
texture/sampler draws, with 5,664 pixel, input and guard bytes checked against
independent calculations. These establish expected behavior for the next
render-stage argument-buffer implementation. They run directly on the Mac GPU;
the Inferno render-stage extension and native iOS execution remain unfinished.

Target: iOS 26 applications use GPU/Metal rendering, and Settings identifies
the emulated panel as a **virtual display**.

Development branch: [gpu-metal-bridge](https://github.com/weizenberg/Inferno/tree/gpu-metal-bridge).

## Current checkpoint

| Item | Status |
| --- | --- |
| V5 GPU/Metal implementation | Published as `f97c852fbb`; latest published documentation is `701152265f`. |
| Host callback lifetime | Accepted: 13 observed batches clear callback/work pointers before retirement. |
| Argument-buffer native references | Accepted: compiled function encoders, backing-copy and destination-selection observations, plus a three-texture/two-constant layout with eight successful executions and 416 checked output/guard bytes. These are host Tier2 observations. |
| Vertex/fragment render references | Eight buffer/constant draws pass with 3,840 checked bytes. The corrected texture/sampler run passes four draws and 1,824 checked bytes, with generated shader-library hashes recorded before and after execution. Direct host Tier2 tests only. |
| Function-only layout metadata | Four recorded function probes return argument-buffer reflection without creating a pipeline or submitting GPU work. This is a scoped host observation for the rendering-extension plan. |
| Architecture plan | R3 returned from Fable. Portable checkpoint A is accepted with explicit C builder/ownership corrections; host/provider review amendments are recorded. |
| Argument-encoder SDK inventory | Verified: 96 direct/inherited method signatures across the two SDKs match compiler ASTs, including 28 direct required selectors per SDK. This is an inventory, not implemented API coverage. |
| Portable v6 interface | Implemented and frozen after source review, 14 SDK build stages and two native-layout decoder checks. |
| Host and driver changes (B/C) | Implemented; QEMU build, signature and Hypervisor entitlement verified. Eight actual host layout-query replies match four native references. Native kernel runtime remains pending. |
| Application provider changes (D) | Implemented and frozen; 46/46 SDK compile/link stages pass. Initial argument-buffer GPU check passes 32 commands and 3,328 CPU-verified bytes with a substituted coordinator/IOKit connection. |
| V6 ARM transport | Passed 14 scenarios under each of TCG/HVF: 28 matching replies, 16 serial progress lines per guest and clean exits. This uses freestanding ARM guests. |
| Dispatch capture correction | Fixed retroactive declaration changes and sorted expanded member IDs. A two-dispatch regression fails on the earlier source and passes after correction, with 156 checked bytes. |
| Resource lifetime and local recovery | Focused fixture passed: three local rejections submit no GPU work, followed by one successful command and 104 checked bytes. Declaration release and recovery from the 96-live-sampler limit pass. |
| Legacy opcodes 1–6 on v6 | Accepted: TCG/HVF pass with 29 serial lines each; 14 captures match prior references and pass the current production decoder under sanitizers. |
| Legacy opcode 7 on v6 | Accepted after fixing the fixture header: 14 captures match, including unchanged reset sentinels; both TCG/HVF exercise live reset/recovery and produce 11 serial lines. |
| V5/v6 client negotiation | Accepted: 142 ASan/UBSan checks; v5 rejects v6, while v6 validates capabilities and recovers. Uses fake IOKit. |
| Aliases and error recovery | Accepted: nine valid GPU commands; 468 provider and 540 backend bytes checked. Injected remote/timer/cleanup/protocol errors prevent writeback; following commands recover. |
| Destination selection | Accepted: selection/detach and rejected overlapping mutation preserve the existing region, which then executes correctly. |
| Cache and combined limits | Independent source review found no defect; broad runtime cache/limit coverage is not claimed. |
| V6 publication | Uncommitted; final source review assigned to Sol following the user’s routing change. |
| Native nil-buffer reference | Accepted: 36 host commands and 5,184 checked bytes across scalar/bulk clear/rebind, three encoder factories and URL/data libraries. One layout only; current provider still rejects nil at dispatch. |
| Parallel review and planning | User reports Fable quota reached and selects Sol planning. Two Sol workers now own the final v6 review and next render/null-buffer plan; Codex coordinates verification and publication. |
| Full iOS outcome | Still incomplete: native device initialization, truthful capabilities, driver/provider integration, presentation and Settings virtual-display identity. |

Current planning evidence: `metal-argument-buffer-plan-r3-wffmuekn`, including
the coordinator review and checkpoint A, B/C and D contracts. R3 returned Fable
5.1 metadata with no permission denials. Earlier planning passes are retained
as history; r2 was superseded after technical review.

## Next steps

1. Review Sol's render/null-buffer plan against current source and verified
   native references, then assign implementation to Sol. Stage-aware residency,
   per-stage limits and canonical null transport remain required; compute-only
   v6 does not complete Tier 1 support.
2. Resolve the parallel Sol final v6 review findings with focused verification.
   The external Fable assignments are superseded by the user's routing change.
3. Commit and push the coordinated v6 change on the dedicated fork branch.
4. Complete native iOS device/driver integration and run an iOS application
   through Metal with truthful capabilities.
5. Complete rendering/presentation and verify Settings' virtual-display identity.

V6 changes are currently uncommitted. Scoped host, provider and freestanding ARM
acceptance is recorded; final code review and publication are the next steps.
The last published implementation is v5. None of these checks establishes native
iOS acceleration.

## Latest evidence update — 13 September, 12:52 Israel

- **Buffer/constant rendering:** eight successful commands cover library loading
  from data and URL, separate/shared indirect buffers, and full/partial target
  coverage. Both shader stages use the same binding slot with distinct argument
  regions. All 3,840 captured pixel/input bytes match the CPU reference, and each
  command delivers its scheduled and completed callback once.
- **Texture/sampler rendering:** four successful commands cover a fragment
  texture array, sampler and constant together with vertex buffer arguments.
  All 1,824 pixel/input/texture bytes match. Sol's independent review found that
  the first run had not recorded the generated library hash. The corrected run
  records AIR and library hashes and verifies the library before and after
  execution; those saved hashes have now been checked. The earlier run is retained
  with its provenance limitation.
- **Layout metadata:** the function API returned reflection for all four tested
  vertex/fragment functions without a render pipeline or GPU submission. The
  observed buffer layouts are 16 bytes with 8-byte alignment; the texture/sampler
  fragment layout is 32 bytes with 8-byte alignment. This supports planning a
  function-based query; it does not establish universal reflection availability.
- **Render implementation requirements:** Sol completed a source packet covering
  stage queries, per-draw capture, wire validation, shared-resource residency and
  per-stage limits. Root verified 28 hashes and corrected parser ownership
  terminology. The concrete Fable request is prepared but has not been submitted
  during the quota wait; implementation still requires its reviewed plan.
- **Native initialization:** the shared constructor dependency is now identified
  as an `NSString` cache-directory path. Root verified 247 contiguous helper
  instruction items and 15 import identities, plus the matching retain/release
  ownership calls. Sol diagnosed an export-parser offset error; the corrected
  targeted lookup confirms the class. Preserve normal superclass initialization
  and cleanup. Actual native device startup remains unverified.
- **Publication:** v5 remains the last published implementation. V6 source is
  still uncommitted and unpushed, pending the requested final Fable review. Its
  latest attempt returned a session-limit error and no review, with a reported
  reset at 14:10 Israel. That failure was a provider quota limit.
- **Native iOS:** the default Metal device, real driver/provider connection,
  application execution, presentation and Settings virtual-display identity
  remain unverified or unfinished.

All new rendering observations above are from public Metal APIs on the Apple
M5 Pro Tier2 host using iOS 26-compiled libraries. They do not establish Inferno
render-stage argument-buffer support or GPU execution inside iOS.

Evidence paths below are relative to
`/Users/weizenberg/InfernoData/ios26/gpu-display-20260911/`:

- `metal-argument-render-native-06edzezo/run-n31ol3iv/root-review.json`
- `metal-argument-render-textures-zf7tala0/run-rvd9awfm/root-review.json`
- `metal-argument-render-textures-zf7tala0/run-rvd9awfm/results.json`
- `metal-argument-render-textures-zf7tala0/run-rvd9awfm/runtime/verification.json`
- `metal-render-native-fixture-review-pawusyk6/report.md`
- `metal-render-function-layout-g47bqmw1/root-review.json`
- `metal-render-function-layout-g47bqmw1/observations.json`
- `metal-render-function-layout-g47bqmw1/results.json`
- `metal-render-argument-plan-input-4YDqU2/coordinator-review.md`
- `metal-native-directory-helper-gbzm4ir_/coordinator-review.md`
- `foundation-export-parser-diagnostic-it9WGBxD/report.md`
- `metal-v6-fable-final-review-r3mpcbbw/coordinator-status.json`

## Parallel session handoff and nullable-buffer reference

The shared UltraCode task board is
`/Users/weizenberg/InfernoData/ios26/gpu-display-20260911/SESSION_COORDINATION.json`.
The startup handoff is `FABLE_COORDINATION.md` in that same evidence directory.
The user subsequently reported Fable's session limit and switched planning to
Sol. The external Fable assignments are paused. Sol reviews the pinned v6 source
and prepares the next render/null-buffer plan in parallel. Codex reviews both,
assigns production edits, runs integration checks and publishes. Actual handles:
`/root/argument_host_review` (review) and `/root/m2b_selector_review` (plan).
Reports are assigned to `sol-v6-final-review-6ugsg2hl/report.md` and
`sol-render-null-plan-frukmz6j/plan.md`; assignment is not acceptance.
The original Fable quota evidence is retained; no duplicate Fable retry is running.

The new native buffer-clearing reference passes 36 commands and independently
checks all 5,184 output, input and backing-guard bytes. It covers live binding,
explicit nil and rebinding through scalar and bulk setters, three encoder
factories, and URL/data library loading. Every command completes before its
backing is changed. This verifies one pointer-plus-constant layout on the M5 Pro
Tier2 host; it does not establish native iOS execution, in-flight mutation
semantics, or nil texture/sampler support.

An initial fixture build used a deprecated enum; a second stopped before GPU work
because library-level reflection was nil. Both are retained. The corrected run
uses compute-pipeline reflection and pins generated AIR, library and executable
hashes; binary/library hashes agree before and after execution. Independent Sol
review found no defect in the stated scope. The source audit confirms current
v6 setters preserve explicit nil, but dispatch capture rejects it and the wire
has no null representation. Sol must return the coherent extension plan for Codex review before
production implementation begins.

Evidence: `metal-nullable-buffer-reference-6ose7tta/run-0pg1avcw/root-review.json`,
`metal-nullable-buffer-review-hNFutZPV/report.md`, and
`metal-v6-null-buffer-source-review-gfTjdJ/coordinator-review.md` under the evidence
root.

## V5 branch milestone

The dedicated branch now includes v5 compiled-library payloads, reusable shared
textures and samplers, and ordered compute/render provider objects. The accepted
host and freestanding ARM checks, SDK builds, native-reference comparisons and
observer-lifetime check are recorded below. Provider GPU tests substitute the
coordinator/IOKit seam; full native iOS integration remains unfinished.

## What is done

| Area | What works | Evidence |
| --- | --- | --- |
| Host GPU execution | Runs compute, clear and basic rendering through the Mac's Metal GPU. | Passed the transport test under both TCG and HVF on the dedicated branch. |
| Submission and recovery | Tracks completions, reports shader failures and drains old work before reusing memory after reset. | Compute output, rendered output, error handling, reset and recovery checks passed. |
| Pipeline reuse | Reuses compiled GPU pipelines through a bounded cache. | Earlier cache/repeated-render tests passed; implementation included on this branch. |
| Guest transport | Encodes and submits commands with explicit size limits. | Real ARM guest transport passed under TCG and HVF. |
| Driver memory ownership | Copies application data into driver-owned buffers and keeps active GPU memory alive until work retires. | Earlier owner and service lifecycle tests passed. |
| Per-connection sessions | Keeps independent connections' results separate; disconnect starts cleanup without freeing active GPU memory early. | Earlier 443-check session fixture passed. |
| Driver connection | Implements capability, submit, status, read, acknowledge and reset calls, including large buffers. | Kernel SDK compile/partial link and the combined 994-check fixture passed against the dedicated branch. |
| Application-side C client | Opens an existing service, negotiates limits, encodes requests, validates replies and closes its connection exactly once. | Actual iPhoneOS 26.5 and macOS 26.5 userspace compilation and linking passed; 2,410 independent ASan/UBSan client/compiler/coordinator checks passed. |
| Compiler queries | Returns complete function inventories, signed compiler errors and actual pipeline limits; compiled pipelines are reused by compute execution. | TCG/HVF tests passed for compilation, failure reporting, limits, cache reuse and query reset; captured results match direct host Metal. |
| Native compilation objects | Constructs ARC library, function and compute-pipeline objects with real metadata, error propagation and asynchronous lifetime handling. | Sanitized macOS object tests and recursive protocol audit passed; iPhoneOS/macOS SDK compile and link passed. |
| Compiler coordinator | Serializes queries, handles uncertain submissions, bounded growth, timeouts, ACK and drain without losing diagnostics. | Included in the 2,410-check sanitized suite; sequence exhaustion additionally source-reviewed. |
| Isolated publication build | GPU/Metal code builds on its own dedicated branch. | QEMU build passed; that exact binary passed TCG/HVF transport tests. |

These results prove the components described above. They do not prove native
iOS app acceleration or a working Metal display.

## Latest completed milestone

- [x] Implement the kernel application-to-driver connection.
- [x] Implement its application-side C client using the approved Opus plan and Sol.
- [x] Compile and link the client with the actual iPhoneOS and macOS SDKs.
- [x] Pass 2,410 independent client/compiler/coordinator checks with address and undefined-behavior sanitizers.
- [x] Pass 994 combined service, connection, buffer-copy and cleanup checks.
- [x] Implement library and compute-pipeline queries using a reviewed Fable plan and Sol.
- [x] Verify compiler queries and their reset/recovery behavior under TCG and HVF.
- [x] Verify old v1 transport peers are rejected explicitly.
- [x] Build the isolated branch and pass the TCG/HVF transport tests.
- [x] Trace the real iOS Metal service selection and plugin construction path.
- [x] Verify the accelerator class hierarchy and key Metal base-class lifecycle requirements.
- [x] Implement v3 metadata, the compiler coordinator and native compilation objects using a reviewed Fable plan and Sol.
- [x] Pass native object lifetime/error tests and verify all required protocol selectors.
- [x] Validate 14 TCG/HVF result captures against the production decoder and host Metal metadata.
- [ ] Implement the native device and verify provider initialization inside iOS.

The latest investigation established how iOS finds a GPU service, selects its
Metal plugin, and creates its device object. It requires a subclass of Apple's
`_MTLDevice`, both Metal protocols, and several initialization methods. The
existing driver connection does not yet supply that userspace implementation.
See [native discovery findings](https://github.com/weizenberg/Inferno/blob/gpu-metal-bridge/docs/inferno-metal-native-discovery.md) for the
verified contract and its remaining unknowns.

The kernel investigation now confirms that a real accelerator subclass is
required; changing registry properties alone will not make iOS discover the
service. The application-side IOKit client is now implemented. It accepts an
existing service and supplies the future provider's application-to-driver calls.
It does not yet create or register a native Metal device.

The capability investigation confirms the private profile's type, the limits
that must be positive, and the factory that creates the concrete feature-query
object. The initializer's 231 device capability queries have now been identified
and checked against the captured calls. Their inherited implementations forward
through the feature-query object. During construction, an initially nil object
would yield false answers; this does not establish a valid capability profile
or successful device initialization. The correct capability mapping for Inferno
still needs verification. The bridge cannot claim a complete Apple GPU family
based on the host GPU.

The v3 compiler-query and object layer is implemented and tested. It preserves
library type and nullable install name, signed patch counts, function constants
and attributes, pipeline allocation size and limits, and imageblock sizing.
All 14 captured replies from the final TCG/HVF binary pass the application
decoder and match direct host Metal metadata. The coordinator and native objects
also passed their final error, recovery, protocol and lifetime checks.

Direct host measurements confirmed that metadata cannot be replaced with empty
values: some source-created executable libraries report a non-nil install name,
while the new compiled-library reference returns nil; ordinary kernels use a
signed patch count of `-1`, and attributed functions expose real attribute lists.
The implementation preserves these distinctions. At the v3 checkpoint GPU resource IDs and argument encoders were unsupported;
the later v6 compute argument-encoder implementation supersedes that status,
while raw host GPU IDs remain unavailable. Native object construction has been tested on macOS, with iOS
SDK compilation; target iOS runtime construction still needs verification.

The existing kernel connection tests cover large requests, short and failed
copies, independent connections, disconnect during outstanding work, reset failures and cleanup after
service shutdown. They use substitute OS objects, so native iOS behavior still
needs runtime verification. The compile uses nearby macOS kernel headers for an
iOS target; matching the actual iOS kernel interface is still outstanding.

## Current work

The sections below retain milestone history. The current checkpoint and next
steps above distinguish the uncommitted v6 work from published v5 behavior.

The v5 host executor now builds and passes actual GPU checks for compiled
compute, texture sampling, clear/render passes, and restoring default raster
state between two draws. Across the two accepted runs, all 2,520 checked image
bytes matched independent CPU calculations. All 12 GPU command buffers completed
with scheduling observed. Typed library, compute, imageblock and render queries
also returned consistent replies when repeated on the same backend.

The new C client/coordinator checkpoint is implemented. All 12 iPhoneOS/macOS
compile and link stages passed, and the production decoders accepted 14 unchanged
real backend replies under address/undefined-behavior sanitizers. Root source
review corrected unused render-envelope fields before this acceptance.

The full QEMU target builds and its HVF signature verifies. Existing compute,
alias/offset, live-reset and recovery checks passed under both TCG and HVF, with
all 14 captures matching their accepted reference and 11 serial lines per guest.

The coordinated v5 host, driver and client transition is implemented and builds.
Ten new-opcode ARM scenarios passed under both TCG and HVF; all 20 complete
replies matched the accepted host references. The v5 binary also passed the
legacy compute, alias/offset, live scheduled-reset and recovery checks, with all
14 captures matching the prior reference after changing only the protocol
version word. Each accelerator produced 12 serial progress lines for the new
scenarios and 11 for the legacy regression.

Legacy opcodes 1–6 now also pass under v5 with only the test's version
expectations changed. Both TCG and HVF produce 29 serial progress lines; all
14 compiler captures match between accelerators, pass the production decoder
under sanitizers, and match the accepted native host metadata. Together with
the earlier opcode-7 result, this closes the ordinary legacy regression gate.
Frozen v4 transport sources from the published commit reject the v5 host under
both accelerators. Application-client negotiation also passes 142 focused
sanitized checks: the frozen v4 client rejects v5, and the current client accepts
valid v5 capabilities, rejects unknown bits/reserved flags, and recovers.
The client checks use fake IOKit and do not prove the native kernel connection.
Evidence: `metal-v5-legacy-six-wccoh_l6/compiler-v4-regression-e56itfxb/root-runtime-verification.json`,
`metal-v4-peer-v5-host-hbjndnsw/legacy-v4-ejmv0r2u/root-runtime-verification.json`,
and `metal-v4-v5-client-negotiation-xoh57odq/root-review.json`.

The Objective-C provider now implements source, compiled data, URL, file and
bundle/default library factories, typed compute queries and buffer execution
through resource batch8. All 34 iPhoneOS/macOS compile and link stages passed
after correcting a concrete pipeline-type error. The texture, sampler and render
objects now also compile and link: all 42 stages passed across both SDKs. Sol
corrected misplaced descriptor properties, ARC control flow, unsupported barrier
behavior, and rejection of Apple's default empty vertex descriptor. The provider
now passes real-host compute-to-texture/render ordering, pass and binding capture,
and clear-only execution. The texture/render acceptance results are recorded below; full native integration remains in progress.
The frozen M2a provider also passed real-host compute through all six source/
compiled-data/URL/file/bundle/default factories: all 1,200 guarded image bytes
matched the independent CPU calculation, and all seven commands scheduled and
completed. This test substitutes the coordinator/IOKit seam and does not prove
that connection at runtime. A production native MTLDevice adapter is still missing, so
ordinary iOS application calls cannot yet reach these context methods.

Both the frozen M2a and the new M2b provider passed the same 48 lifecycle
checks with address and undefined-behavior sanitizers: queue ordering,
exactly-once handlers, recovery, and no writeback after remote, timer or cleanup
errors. These use a scripted coordinator and do not validate production IOKit.

The M2b provider now matches the independent native Mac Metal reference for
compute writing a texture, rendering from it, and sampling that result in a
second command. Both commands were committed before waiting; all 636 padded
texture bytes matched the CPU expectation. Additional state-capture checks
verified 1,060 bytes, including a clear-only pass and an unchanged neighboring
texture. Six nondefault vertex-layout cases remain explicitly rejected. These
actual-GPU tests substitute the coordinator/IOKit connection.

The original texture reference also passes through the provider: padded uploads,
partial updates, region reads, overlapping render passes and shader sampling
matched all 1,224 CPU/native-reference bytes. The same checks pass using the
saved iOS-compiled render library through the data factory and sampling library
through the URL factory. Each variant completed all four commands and their
handlers. This verifies those fixed artifacts, not arbitrary iOS libraries.

The four new object classes have all required available SDK selectors, with
438 concrete signature matches across the two SDKs. Unsupported methods remain
explicitly unsupported; selector coverage does not imply full Metal support.

Evidence: `metal-provider-texture-oracle-omix3erl/run-t7jeug4_/root-runtime-verification.json`,
`metal-provider-compiled-render-zt9pfelg/run-xniwbfl0/root-runtime-verification.json`,
and `metal-provider-selector-audit-m2b-r2-A7c9p2/root-review.json`.

Native iOS device initialization, argument-buffer support, truthful capabilities,
presentation and virtual-display identity remain unfinished.

Compute sampling now passes with a buffer and texture both using slot 3:
all 384 guarded output and unchanged source bytes match the native reference.
Both asynchronous render-pipeline factories preserve descriptors captured at
call time, invoke their callbacks once, and execute the ordering workload.
All 14 transported pipeline getters match a directly created host pipeline.

A real texture-usage failure exposed a client decoder mismatch: it rejected
the host's explanatory description when there was no native NSError. Sol
corrected both batch decoders within the reviewed plan. All 42 SDK compile/link
stages pass. The same captured reply now produces the specific unsupported-state
error, without scheduling or changing 280 texture bytes and 56 buffer bytes.
Two subsequent GPU commands complete and reproduce all 636 expected output
bytes. These tests still substitute the coordinator/IOKit connection.

Evidence: `metal-provider-compute-sampling-prf9ce0w/run-n8slvb5t/root-runtime-verification.json`,
`metal-provider-render-async-a9022ybc/run-v6d7apfh/root-runtime-verification.json`,
`metal-provider-sdk-check-0l03k2sk/results.json`, and
`metal-provider-texture-access-error-60ir5k8s/run-nhyr4ju2/root-runtime-verification.json`.

The provider also matches native Metal across a ten-draw raster-state comparison:
all 2,720 output and padding bytes agree. It covers viewport, scissor, both
windings, front/back culling, fill/line mode, observable RGB/alpha blend constants,
restoring an earlier state, and mutations after recorded draws. The CPU checker
verifies nine exact image regions; the line region uses native equality plus
independent coverage and interior checks. Each run schedules and completes once.
Evidence: `metal-provider-raster-native-l2sm_ps2/run-xqnbsost/coordinator-review.json`.

All five supported texture formats now pass the same native/provider transfer
and clear reference: RGBA8Unorm, RGBA8Unorm_sRGB, RGBA8Snorm, BGRA8Unorm and
BGRA8Unorm_sRGB. All 3,520 bytes match, including padded uploads, partial updates,
unchanged inputs, readbacks and red clears. This verifies exact zero/one clear
endpoints, not intermediate sRGB conversion. Each of the five commands schedules
and completes once. Evidence:
`metal-provider-texture-formats-qrkwuox8/run-hhvuqyds/coordinator-review.json`.

Four local failure cases also pass: unsupported sampler reduction and pipeline
depth format, an oversized texture, and same-pass attachment feedback. Each
preserves 476 existing resource bytes and causes no query or submission. The
following valid GPU recovery still matches all 636 expected output bytes.
Evidence: `metal-provider-local-texture-errors-khx66fnv/run-_z2wlca0/root-runtime-verification.json`.

The host observer lifetime check now passes with the unchanged accepted QEMU
binary. Debugger observations record 13 batches: seven legacy compute batches
and six texture/render batches. Each clears the scheduled callback and its work
pointer before worker retirement; both guests finish successfully. This supports
the source review's lifetime ordering, while debugger timing and finite workloads
limit the conclusion. Evidence:
`metal-host-observer-runtime-9ifhwfyy/root-runtime-verification.json`.

Argument-buffer preparation now has a native host measurement. Both public
encoder APIs agree on a sparse texture/sampler/constant layout, and four valid
dispatches reproduce all 208 guarded output bytes. This was measured on the
M5 Pro's Tier 2 implementation. At that checkpoint, Inferno argument buffers
were not implemented; the v6 implementation and scoped acceptance now supersede
that status.
Evidence: `metal-argument-layout-host-nowv2byg/root-review.json`.

The compiled argument-buffer experiment found a material limitation for the
next design. The same iOS-compiled shader has no library function reflection.
Pipeline reflection and the function encoder's reflection report a zero array
byte stride, while the argument-index stride is 1. Creating an encoder from the
pipeline binding gives incorrect output; zero byte stride alone is not a proven
cause of that failure.
Function-derived encoders nevertheless execute all eight data/URL tests
correctly, matching 416 guarded output bytes. The implementation must preserve
the function-derived encoding context; public reflection fields alone have not
proved sufficient to reconstruct it. This is direct host reference evidence;
the later v6 implementation uses function-derived encoders. Evidence:
`metal-argument-function-reflection-85dm68sf/root-review.json` and the retained
failed alternative `metal-argument-pipeline-debug-q9r6hpre`.

A further native argument-buffer test verifies backing-byte copies. A buffer
that was never selected as an encoder destination executes after receiving a
CPU copy of the encoded bytes. Re-encoding the original buffer leaves the copy
unchanged; editing only the copy's constant changes its next result. All three
commands complete and all 268 output/input/texture bytes match independent CPU
checks. This is one compiled shader on the M5 Pro Tier2 host, with all CPU writes
before the affected command's bindings. It does not prove a universal Tier1
contract. Public-source review supports treating this as a Tier2 compatibility
boundary; it does not by itself require Tier2 copying in the next Tier1 milestone.
Evidence: `metal-argument-backing-copy-8_99rbs_/root-review.json`.
A focused follow-up also confirms that selecting an overlapping destination and
then detaching, without member writes, preserves the original encoded bytes and
its execution. The plan must distinguish selecting a destination from changing
its contents. Evidence: `metal-argument-destination-selection-gbhe0sje/root-review.json`.

The expanded iOS-compiled shader now verifies three texture-array elements and
two separated constants. All eight native host executions pass, with 416 output
and guard bytes checked against independent CPU calculations. This layout needs
64 bytes and 16-byte alignment, with constants at offsets 0 and 48; the earlier
shader needs 40 bytes and 8-byte alignment. Layout values must therefore remain
specific to each function. The array reports member IDs 8, 9 and 10 and an
argument-index stride of 1 despite its zero byte stride. This contradicts the
revision's reason for dropping array support. These are M5 Pro Tier2 host
results, not Inferno argument-buffer or native iOS acceptance. Evidence:
`metal-argument-multiple-constants-qkvkdbd0/root-review.json`.

The SDK inventory is now coordinator-verified against saved compiler ASTs and
header hashes: 96 method signatures across iPhoneOS and macOS. The subsequent
r3 review and Sol implementation completed the layout, wire-format and resource-
lifetime work described below. Published v5 remained unchanged during those
planning and reference checks. SDK inventory:
`metal-argument-encoder-sdk-inventory-20260913-093405/root-review.json`.

A native reflection comparison now distinguishes ordinary constant structs from
argument buffers. Both compiled kernels report a struct and `isArgument=true`;
those fields alone cannot classify the binding. The reflected pointer's
`elementIsArgumentBuffer` is false for the ordinary constant struct and true for
the resource-containing argument struct. Both pipelines were created successfully.
This is a classification check with full buffer-type reflection, not a GPU
execution test. It provides a concrete guard against rejecting ordinary shaders.
Evidence: `metal-argument-classification-xd_6habr/root-review.json`.

The same distinction now passes for vertex and fragment stages: two valid
render pipelines expose four checked bindings, with `elementIsArgumentBuffer`
false for ordinary constants and true for resource-containing argument structs.
No draw was submitted. This extends the classification evidence needed to
preserve ordinary rendering while validating argument-buffer use. Evidence:
`metal-argument-render-classification-1mucd9ew/root-review.json`.

A further native GPU test verifies three-component constants. `float3` and
`packed_float3` report the same data type, but use different function layouts:
48 bytes/alignment 16 versus 24 bytes/alignment 8. Copying 12 value bytes at the
function-derived constant pointer works for both, with two completed commands
and all 104 output/guard bytes checked independently. Assuming a 16-byte payload
would overlap the following constant in the packed layout. Evidence:
`metal-argument-three-component-0t5fkwam/root-review.json`.

The corrected r3 pass completed in `metal-argument-buffer-plan-r3-wffmuekn`
using Fable 5.1. Portable checkpoint A is now implemented: argument/member and
resource-declaration records, copied constant bytes, fixed-size layout replies,
and coordinated client gates. Root verified the ten source-file hashes against
the reviewed diff, all 14 SDK compile/link stages, and two independently encoded
native-observed layouts through the production decoder under ASan/UBSan. The
latter is a decoder check, not execution of the new host query. All nine member
descriptors match the native observations.

The accepted interface is frozen in
`metal-v6-portable-frozen-bzr9gptk/root-review.json`. Build evidence:
`metal-v6-portable-sdk-1kgfgtwo/results.json`; decoder evidence:
`metal-v6-layout-codec-zgwndrto/results.json`. B/C now implements host query and
argument execution plus the driver capability bit; D implements provider objects,
resource declarations and capture/writeback. Their accepted contracts are
`checkpoint-bc-contract.md` and `checkpoint-d-contract.md` in the r3 directory.
These source changes remain uncommitted. Both Sol handoffs are now returned.
Root verified the source hashes and the frozen portable interface, corrected
two new host compiler warnings through Sol, and rebuilt the coordinated QEMU
target. Its strict signature check and Hypervisor entitlement verify. Evidence:
`metal-v6-host-root-build-rmwcsalo/run-2/host-provenance.json`.

Eight actual host opcode-13 replies match the accepted native layouts: two
complete texture-array descriptor sets and the recorded natural/packed Float3
constant fields, each queried twice. This verifies pipeline/argument-encoder
construction and reply decoding, without a GPU dispatch. Evidence:
`metal-v6-host-layout-query-lcf4uhiy/root-review.json`.

The corrected provider is frozen in `metal-v6-provider-frozen-r2-m_1dcyld`. All 46 actual
SDK compile/link stages pass with stable source hashes. The initial real-host
integration fixture also passes 32 GPU commands, 32 scheduling callbacks and
all 3,328 independently checked input/output/guard bytes. It covers both
function-encoder factories, compiled data/URL loading, bulk setters, nonzero
backing offsets, texture arrays and natural/packed three-component constants.
Evidence: `metal-v6-provider-sdk-r2-chal50km/results.json` and
`metal-v6-provider-arguments-lhmqiy1y/run-_ikrgpl2/root-review.json`.
The coordinator/IOKit connection is substituted; native iOS execution,
full native compatibility remains open. The later focused lifecycle and
error checks below extend this initial acceptance.

The same signed v6 QEMU binary now passes 14 valid ARM scenarios under each
of TCG and HVF. All 28 replies match the accepted host references and each other;
each guest produces 16 serial progress lines and exits cleanly. The scenarios
cover the existing typed queries and compute/render batches plus four new
argument-layout queries. The repository's metrics comparison also passed.
These are freestanding ARM transport tests, not native iOS application tests.
Evidence: `metal-v6-arm-valid-udmpfcf5/run-jfk190qi/root-runtime-verification.json`.

Independent source review found a declaration-capture bug: upgrading a resource
from Read to Read|Write for a later dispatch also changed the earlier dispatch's
usage. Sol fixed this by copying declaration values per dispatch, and added the
required sort of expanded argument-member IDs. A valid two-dispatch regression
reproduced the old bug and passes on the corrected source. The first declaration
now remains Read; the second is Read|Write. Both dispatches retain their distinct
constant values (17 and 23), and all 156 input/output/guard bytes match the
provider's capture contract. This is a provider policy check, not a claim of
native shared-memory equivalence. The original 32 GPU cases and all 46 SDK
stages pass again after correction. Evidence:
`metal-v6-provider-dispatch-state-w_3uf354/run-sm4cgg1g/root-review.json`;
source review: `metal-v6-provider-integration-review-e0E73hTf/report.md`;
corrections: `metal-v6-provider-d-corrections-v9PcWr/report.md`.

The latest focused provider lifetime/recovery fixture reports success on the
corrected frozen source. Released and nil argument assignments are rejected;
a bulk texture update containing a foreign resource leaves the prior assignment
intact. All three local rejections cause zero GPU submissions. The following
valid command completes with 104 independently checked input/output/guard bytes.
Declaration-only resources are released after ending an empty encoder, and
captured resources survive through dispatch completion before release. The
96-live-sampler limit rejects another allocation and recovers after resources
are released. All 24 build/runtime stages pass, and the recorded source/artifact
hashes still match. This uses the real host backend with a substituted
coordinator/IOKit connection. Root verified the recorded hashes and exact
input/output bytes. Evidence:
`metal-v6-provider-lifetime-bg6uqfhg/run-wm_yrluq/root-review.json`.

Legacy opcodes 1–6 now pass on v6 under TCG/HVF with 29 serial lines per guest,
clean exits and 14 captures matching accepted v5 references after changing only
the envelope version word. All 14 also pass the current production decoder under
ASan/UBSan. Evidence:
`metal-v6-legacy-six-rqdrheqa/compiler-v4-regression-nipohxbx/root-runtime-verification.json`.

The opcode-7 fixture's stale v5 request header is corrected. The rerun passes
under both accelerators with 11 serial lines, actual live-reset coverage,
recovery and 14 matching captures. Reset sentinels remain unchanged; other
captures differ from accepted v5 only in the version word. The initial failed
TCG run is retained with a fixture-failure note. Accepted evidence:
`metal-v6-legacy-seven-h1kg59_u/metal-v4-arm-oracle-amrldssq/root-runtime-verification.json`.
V5/v6 client negotiation now passes all 142 sanitized checks, including a v5
control rejected after changing only its capability version to 6. Evidence:
`metal-v5-v6-client-negotiation-z85_ehdm/root-review.json`.

A focused alias/error matrix runs nine valid host GPU commands with one buffer
used as both indirect read input and ordinary writable output. All 540 backend
image bytes match the independent result. Five successful provider commands
write the expected aliased result; four injected coordinator conditions—remote
execution outcome, timer error, cleanup error and protocol failure—preserve the
original application buffer. The next command after each error succeeds. All
468 provider bytes and expected error metadata are verified. These are injected
coordinator conditions, not actual native GPU fault or production IOKit recovery
claims. Evidence:
`metal-v6-provider-alias-errors-_org7a1h/run-dk_kgl96/root-review.json`.

Destination selection and detach preserve existing backing bytes and assignments.
A mutation at a distinct overlapping base raises the specific invalid-use error
before changing bytes or submitting work. The original region then executes
correctly, with 52 provider and 60 backend bytes verified. An initial fixture
asserted the wrong exception category; that failure is preserved, and production
source was unchanged. Evidence:
`metal-v6-provider-selection-75si2c1u/run-rtg6h8me/root-review.json`.

Sol's bounded independent source review found no remaining concrete defect in
alias union, error-gated writeback, cache ownership or combined binding bounds.
Its smallest missing runtime check was the alias/error matrix now accepted above.
Cache and combined-limit conclusions remain source-review evidence. Report:
`metal-v6-provider-alias-review-xaBLhqTh/report.md`.
The final Claude Fable review request against the frozen v6 source snapshot
exited with a session-limit error, empty model usage and no review. The client
reports a reset at 14:10 Israel time; no retry has been launched. Evidence:
`metal-v6-fable-final-review-r3mpcbbw/coordinator-status.json`.
DeepSeek Flash completed the separate synthesis of saved native base
constructor/destructor evidence. Root verified 37 instruction items against
the original iOS cache. The initial storage fields and paired destruction
sequence are now established; a guarded helper and missing constructor tail
were followed through two bounded passes. The slow branch calls a thunk whose
target passes the addresses of a shared guard and static payload to another
helper; it replaces the first two argument registers rather than forwarding
the unfinished device there. Root verified those bytes too. The final helper
is now identified as `dispatch_once`, and the callback's byte-to-string
initializer is instruction-verified. The byte source is now identified as a
cache-directory path, the receiver as `NSString`, and the storage ownership as
paired retain/release. Successful native initialization remains unresolved. Evidence:
`metal-native-storage-evidence-review-htc1hfyh/coordinator-review.md`,
`metal-native-storage-guard-912l56f2/coordinator-review.md`, and
`metal-native-storage-once-e27qsf5m/coordinator-review.md`.

The refreshed public-device prerequisite review confirms that the old statement
"argument buffers are entirely absent" is obsolete. V6 provides a real compute
subset. Vertex/fragment argument-buffer draw support, stage-aware resource
residency and per-stage limits are the next missing public path. Required
device factories and valid nullable-resource clearing remain additional gaps;
full Tier 1 support cannot yet be advertised. Evidence:
`metal-tier1-device-prereq-refresh-Bi4H7e/coordinator-review.md`.

DeepSeek Flash also located the native base initializer's allocation and cleanup
of the internal storage used by the optional initialization method. Root verified
the captured instructions against the original cache. The provider must preserve
this base ownership; constructor internals and actual iOS initialization remain
open. Evidence: `metal-native-base-storage-infrkawg/coordinator-review.md`.
The follow-up constructor/destructor analysis reached DeepSeek's response limit
and produced no final findings. Its five saved IDA queries remain unaccepted;
no automatic retry was made. Evidence:
`metal-native-storage-constructor-mqc4cslh/coordinator-status.json`.

Evidence: `metal-provider-sdk-check-8grmk6pn/results.json`,
`metal-provider-m2b-order-check-cqzymk8h/run-psu4myzd/root-runtime-verification.json`,
`metal-provider-m2b-state-check-4ylry0vr/run-3doxdtuv/root-runtime-verification.json`,
and `metal-provider-m2b-lifecycle-a69vg2fz/run-1/root-review.json`.

Evidence: `metal-v5-host-root-build-8cl5viw9`,
`metal-v5-host-executor-smoke-z8s152jv/run-1/root-verification.json`,
`metal-v5-valid-batches-gngK7O/root-run-1/root-verification.json`, and
`metal-v5-client-root-sdk-9pe167x5/root-review.json`,
`metal-v5-arm-valid-votsw9bi/run-3fabilss/root-runtime-verification.json`,
`metal-v5-legacy-arm-ovss8ny1/metal-v4-arm-oracle-saarhp21/root-runtime-verification.json`,
`metal-provider-m2a-root-sdk-r2-ew_uqltj/root-review.json`, and
`metal-provider-m2a-real-host-fw8ghfp0/run-_npuqlms/root-runtime-verification.json`.

Compiled shader libraries now have a direct host reference. After installing
Apple's missing Metal Toolchain, the same kernel was compiled for iOS 26 and
macOS 26. Both artifacts loaded through the public data and URL APIs on the
Mac and executed correctly: all four 100-byte outputs, including guards,
matched an independent calculation. This proves one artifact's compatibility;
it does not establish arbitrary iOS library compatibility. The earlier v4 bridge accepted only UTF-8 shader source. The v5 implementation adds opaque library payloads and shared pipeline caches,
and its new ARM transport checks now pass. Guest context bundle/URL factories
now also pass the scoped real-host provider test described above.

The iOS-compiled render and sampling shaders also loaded through the public
host library API. Four commands reproduced all 1,224 bytes of the unchanged CPU
texture oracle, including padding and guards. This extends compiled-library
evidence to the rendering reference; it remains a direct host test.

The inherited device wrapper method returns nil, so the native provider must
explicitly handle Metal's optional wrapper path. This is now confirmed from
actual method metadata and instructions. The optional initialization method
clears a base-object byte and forwards to the compiler getter's result. The
inherited getter returns nil; concrete overrides and native initialization
remain unverified. The corrected instruction-level evidence is recorded in
`metal-native-optional-method-1pp5se_v/coordinator-review.md`.

The native device-registration investigation now confirms wrapper selection
and ownership after insertion into Metal's device list. Instruction-level
review corrected a missed release in the model's decompilation report. The
runtime wrapper configuration, truthful device capabilities and actual iOS
initialization remain unresolved; this is framework-contract evidence only.

The retained client/compiler/coordinator and service regression suites now
passed against the earlier v4 source: **2,390 and 994 checks**, respectively,
with address and undefined-behavior sanitizers. Three obsolete v3 assertions
were omitted because their reserved values are now valid v4 fields. The service
fixture supplies zero scheduled progress through a mock. These runs preserve
coverage of the existing behavior; they do not cover the new batch coordinator,
production kernel transport, or native iOS execution. Exact compatibility diffs
and results are saved in `metal-v4-existing-regressions-8p5vic4w/root-review.json`
under the local evidence directory.

Preparation for reusable textures and rendering now has a verified host
reference. A real shared texture accepted padded-row uploads and partial
updates; four render commands exercised preserved contents, clears,
overlapping draws and shader sampling into a second texture. All 1,224 captured
bytes matched a separate CPU calculation,
including padding and guard bytes. Both dependent render commands were
committed before waiting, and vertex/fragment bindings used separate buffers at
the same slot. Sampling used a real nearest/clamp sampler and preserved the
patterned source image exactly. This established the reference for provider
implementation; the current M2b code and its scoped acceptance are described
above.
The SDK method and descriptor inventory is now checked, including optional
methods and platform differences. A source-and-evidence packet now includes compiled-library requirements. Its
Fable planning pass returned. Root review found an incompatible capability-mask
assumption, an inconsistent sampler record size, and unresolved API contracts.
The focused Fable revision has returned. Its portable and host v5 design
checkpoints are accepted with corrections for sampler combinations, independent
color write masks and texture access checked against the final pipeline. Sol has completed the portable records, parser, builder and result decoder.
The host parser, host/iPhoneOS builders and C++ headers compile successfully.
Seven valid manifests—empty, clear-only, ordinary rendering and all four typed
queries—match 20,296 independently constructed bytes and pass the production
parser under AddressSanitizer and UBSan. These tests do not execute GPU work. Source review
also corrected color-channel mask bits and required each pipeline's format to
match its render target. The coordinated interface is now v5, replacing the previously published v4 baseline.
Public Apple documentation supplies the family-specific texture, sampler and
dimension limits; this does not yet select a complete guest GPU profile.

Both public default-library APIs now have a verified host reference: the main
application bundle and a supplied bundle each loaded a known compiled library
and produced the correct 100-byte guarded compute image. An empty bundle returned
nil with a real NSError. The first Python checker incorrectly treated numeric
JSON `1` as failure; an independent review of exits, objects, hashes and every
output byte accepted the saved runtime without rerunning it. These are host
observations, not proof of iOS bundle loading.

All 28 isolated render-pipeline getter probes returned successfully on the M5
Pro host: 14 getters on each of two valid ordinary/sampling pipelines. Values
must be queried per pipeline; they are not constants for the emulated GPU.
Actual passes also reported a 32×32 tile and 4096-byte imageblock requirement.
Host pipeline reflection is now verified on six real pipelines. Source and
compiled sampling shaders return identical stage/slot metadata; compute shaders
report distinct read-only, write-only and read/write texture access. Retained
reflection remains usable after its autorelease pool drains. The corresponding
host operations and provider texture, sampler and render-encoder objects are now
implemented; current validation is recorded above.

The registration investigation also corrected an ownership claim in DeepSeek's
report. Target-cache bytes identify the Blocks calls and an unretained byref
layout. Apple's pinned public runtime makes the observed `0x83` object helper
assignment unretained and its disposal a no-op. The report's retain/release
inference is rejected; the closed target runtime's exact implementation and
actual native-device startup remain unverified.

Reusable shared buffers and ordered compute command buffers are implemented
under the reviewed Fable plan, with Sol owning the source changes. The bounded
batch protocol preserves bindings, aliases and dependent dispatches. The host
and transport code pass independent ARM execution under both TCG and HVF.

The final native correction makes commit serial allocation, command-state
publication and queue readiness one scheduler-locked transition. All 40
kernel/iPhoneOS/macOS compile and link stages pass against this source. The
recursive macOS protocol audit finds no missing required methods across the
four new object classes. The native API fixture now passes 60 assertions with
address and undefined-behavior sanitizers, covering admission-time buffer
capture, ordering, callback waits, capacity recovery and typed error handling.
Remote execution errors and cleanup diagnostics preserve error metadata while
withholding every shadow-buffer writeback byte. A subsequent valid command
still completes. These tests use a scripted coordinator; production driver
integration and target iOS behavior remain unverified.

Fable’s read-only review completed after the quota reset. Its source review
identified retained scheduled-handler captures, incorrect first-commit behavior
after a precommit failure, and unsafe host callback lifetime. Sol corrected the
two affected files under the reviewed plan. The regression fixture reproduced
six failures before the provider fix and now passes all 60 checks, including
late completion handlers and waits that include every handler body. All 40 SDK
compile/link stages also pass on the corrected provider. Host callback resource
cleanup is corrected. The rebuilt, signed QEMU passes empty and inline-only
batches, dependent compute, scheduled reset and recovery under TCG and HVF;
all captured regions match the independently verified references. An additional
Fable review with the actual accepted plan included exhausted its session quota
before a final report. That final review remains incomplete; the earlier source
review and root-verified corrections are recorded.
Local source inspection found and a deterministic regression confirmed a queue
stall: abandoning an enqueued, uncommitted head left later committed work asleep.
Sol corrected head removal to schedule a rescan after releasing the queue lock.
The same sanitized test now passes, including proof that the abandoned command
deallocated and its successor completes without another submission. All 40 SDK
stages were rerun successfully after that correction. Full v4 production
coordinator/kernel acceptance remains incomplete.

An earlier isolated wire test exposed validation gaps; source review confirms
several have been corrected. A follow-up fixture revision was rejected by the
platform and remains stopped. That rejection was a model-call failure, not a
QEMU or GPU-test failure.

A direct host Metal reference now passes dependent dispatches, simultaneous
buffer aliases, nonzero offsets, inline-byte copies and two command buffers
committed before waiting. All 576 buffer bytes, including guards, match an
independent CPU calculation at both checkpoints. This supplies the expected
results for upcoming bridge tests; it is not native iOS acceptance.
The independent ARM batch fixture now passes actual host execution and reset
recovery under both TCG and HVF. At four compute checkpoints per accelerator,
all 576 bytes match independent CPU calculations. All seven captured result
regions are identical between accelerators. Reset after an observed scheduled
event preserves all 17,664 bytes of retired output; a fresh two-dispatch compute
submission then succeeds. These are freestanding ARM tests, not native iOS.

The existing compute/render/compiler guest assertions also pass under both
accelerators. All 14 compiler reply captures pass the production decoder and
match direct host metadata. A legacy v3 client is rejected explicitly by the
v4 bridge under both TCG and HVF. The refreshed host also accepts a valid inline-only compute dispatch with no
buffers under both accelerators. Native-object behavior now has the scoped macOS evidence above; target iOS
runtime checks remain pending.

The separate capability-getter investigation hit Kimi's weekly quota limit.
After availability was restored, one bounded retry completed. Three concrete
getters are now verified to query the device live, correcting the earlier
cached-answer interpretation. Full capability/profile mapping and native device
initialization remain unresolved.
DeepSeek’s restored pass also confirmed that private profile 0 advertises
Apple1 and Common1; it is not a neutral unsupported profile. Parts of its broader
mapping were incorrect and remain unaccepted. No profile has been selected.
The resource/command implementation proceeds independently.

## What remains for the full goal

| Remaining work | What will count as done |
| --- | --- |
| Verify the real iOS driver interface and startup | The new driver starts on the target iOS build and an application opens its connection successfully. |
| Native Metal device discovery | Stock `MTLCreateSystemDefaultDevice()` returns the Inferno device instead of `nil`. |
| Native compilation integration | Connect the implemented objects to the native device and verify creation, metadata and errors in an iOS application. |
| Native application execution | An iOS application creates a Metal queue, submits work and receives correct results through the real driver connection. |
| Complete resource and command support | Complete argument-buffer compatibility and truthful device capabilities beyond the scoped v6 compute implementation, then verify resource and command behavior through an iOS application. |
| Native presentation | Metal drawables reach the iOS compositor and display, with correct synchronization. |
| Virtual-display identification | Settings visibly identifies the panel as a virtual display; no native display-parts authentication claim. |
| End-to-end verification | Repeatable iOS boots, responsive UI, real app rendering and reset/reconnect recovery are demonstrated under TCG and HVF. |

The existing iOS runtime samples still report no default Metal device.
The main selection and factory path, class relationship and base cleanup are
now known. Concrete subclass bindings, capability initialization, final
registration details and runtime integration still
need verification. A previous
driver-loading continuation was rejected by the platform; that operation remains
stopped while its cause is unresolved. Independent implementation and testing
have continued.

## How to read future updates

“Implemented” means code exists. “Verified” names the test or runtime observation
that passed. A standalone test passing will not mark an iOS runtime milestone
complete. This file will keep the unfinished items visible as work progresses.

Technical implementation: [Metal bridge documentation](https://github.com/weizenberg/Inferno/blob/gpu-metal-bridge/docs/inferno-metal-bridge.md).
Local test evidence: `~/InfernoData/ios26/gpu-display-20260911/publish-gpu-metal-YOcuW48k/`.
Combined fixture: `~/InfernoData/ios26/gpu-display-20260911/metal-userclient-fixture-2yPADzP7/`.

Application client SDK evidence: `~/InfernoData/ios26/gpu-display-20260911/metal-native-client-build-el7fjj7_/`.
Application client fixture: `~/InfernoData/ios26/gpu-display-20260911/metal-native-client-fixture-WFRaYum4/`.

Compiler-query SDK, QEMU and runtime evidence:
`~/InfernoData/ios26/gpu-display-20260911/metal-compiler-root-checks-8dib4nyj/`.
Current v3 SDK, QEMU, native object and runtime acceptance evidence:
`~/InfernoData/ios26/gpu-display-20260911/metal-native-objects-root-checks-4e2zun6d/acceptance.json`.
Current application/kernel fixtures:
`~/InfernoData/ios26/gpu-display-20260911/metal-v3-fixtures-4esdlng_/`.
