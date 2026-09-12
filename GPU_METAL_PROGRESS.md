# GPU and Metal progress

Last updated: 12 September 2026.

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

Compiled shader libraries now have a direct host reference. After installing
Apple's missing Metal Toolchain, the same kernel was compiled for iOS 26 and
macOS 26. Both artifacts loaded through the public data and URL APIs on the
Mac and executed correctly: all four 100-byte outputs, including guards,
matched an independent calculation. This proves one artifact's compatibility;
it does not establish arbitrary iOS library compatibility. The Inferno bridge
still accepts only UTF-8 shader source. Opaque library payloads, cache identity,
pipeline reuse and guest bundle/URL loading remain implementation work.

The iOS-compiled render and sampling shaders also loaded through the public
host library API. Four commands reproduced all 1,224 bytes of the unchanged CPU
texture oracle, including padding and guards. This extends compiled-library
evidence to the rendering reference; it remains a direct host test.

The inherited device wrapper method returns nil, so the native provider must
explicitly handle Metal's optional wrapper path. This is now confirmed from
actual method metadata and instructions. The remaining optional initialization
call has been identified, but its behavior and compiled-library compatibility
are still unverified.

The native device-registration investigation now confirms wrapper selection
and ownership after insertion into Metal's device list. Instruction-level
review corrected a missed release in the model's decompilation report. The
runtime wrapper configuration, truthful device capabilities and actual iOS
initialization remain unresolved; this is framework-contract evidence only.

The retained client/compiler/coordinator and service regression suites now
pass against the current v4 source: **2,390 and 994 checks**, respectively,
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
patterned source image exactly. This establishes expected results for future guest code;
textures and rendering are not yet implemented in the native provider.
The SDK method and descriptor inventory is now checked, including optional
methods and platform differences. A source-and-evidence packet now includes compiled-library requirements. Its
Fable planning pass returned. Root review found an incompatible capability-mask
assumption, an inconsistent sampler record size, and unresolved API contracts.
A focused revision is running before texture/render implementation. Public Apple
documentation now resolves normal default.metallib bundle loading and provides
the family-specific texture, sampler and dimension limits needed by that plan.

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
| General resource and command support | Reusable buffers/textures and ordered commands work beyond the current single-operation bridge; required compiled-shader support is established. |
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
