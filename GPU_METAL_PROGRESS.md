# GPU and Metal progress

Last updated: 13 September 2026.

## Overall status

**The host GPU bridge works in test guests. Native Metal acceleration inside
the iOS 26 guest is not working yet.**

The driver connection, application-side client, compiler coordinator, and native
library/function/compute-pipeline objects are implemented. They preserve real
host metadata and compiler errors. The published v3 layer passed 2,410 client/compiler/
coordinator checks, 994 kernel-service checks, native object tests, SDK builds,
and ARM guest tests under both TCG and HVF. All 14 captured compiler replies
passed the production decoder and matched direct host Metal metadata. Native
iOS device discovery, general commands and presentation remain unfinished.

Target: iOS 26 applications use GPU/Metal rendering, and Settings identifies
the emulated panel as a **virtual display**.

Development branch: [gpu-metal-bridge](https://github.com/weizenberg/Inferno/tree/gpu-metal-bridge).

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
The implementation preserves these distinctions. GPU resource IDs and argument
encoders remain explicitly unsupported until resource ownership and layout
support exists. Native object construction has been tested on macOS, with iOS
SDK compilation; target iOS runtime construction still needs verification.

The existing kernel connection tests cover large requests, short and failed
copies, independent connections, disconnect during outstanding work, reset failures and cleanup after
service shutdown. They use substitute OS objects, so native iOS behavior still
needs runtime verification. The compile uses nearby macOS kernel headers for an
iOS target; matching the actual iOS kernel interface is still outstanding.

## Current work

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
M5 Pro's Tier 2 implementation; Inferno argument buffers are not implemented.
The architecture still needs a Fable plan, review and Sol implementation.
Evidence: `metal-argument-layout-host-nowv2byg/root-review.json`.

The compiled argument-buffer experiment found a material limitation for the
next design. The same iOS-compiled shader has no library function reflection.
Pipeline reflection and the function encoder's reflection report a zero array
stride; creating an encoder from the pipeline binding gives incorrect output.
Function-derived encoders nevertheless execute all eight data/URL tests
correctly, matching 416 guarded output bytes. The implementation must preserve
the function-derived encoding context; public reflection fields alone have not
proved sufficient to reconstruct it. This is direct host evidence, with no
Inferno argument-buffer implementation yet. Evidence:
`metal-argument-function-reflection-85dm68sf/root-review.json` and the retained
failed alternative `metal-argument-pipeline-debug-q9r6hpre`.

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
| Complete resource and command support | Finish M2b acceptance and implement the argument-buffer and capability contracts needed by the native device; verify them through an iOS application. |
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
