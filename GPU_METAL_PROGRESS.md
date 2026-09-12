# GPU and Metal progress

Last updated: 12 September 2026.

## Overall status

**The host GPU bridge works in test guests. Native Metal acceleration inside
the iOS 26 guest is not working yet.**

The driver connection, application-side client, compiler coordinator, and native
library/function/compute-pipeline objects are implemented. They preserve real
host metadata and compiler errors. Verification passed 2,410 client/compiler/
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
object. The initializer's 231 device capability queries have now been identified and
checked against the captured calls. The correct capability mapping for Inferno
and the base methods it can safely inherit still need verification. The current bridge cannot
claim a complete Apple GPU family based on the host GPU.

The v3 compiler-query and object layer is implemented and tested. It preserves
library type and nullable install name, signed patch counts, function constants
and attributes, pipeline allocation size and limits, and imageblock sizing.
All 14 captured replies from the final TCG/HVF binary pass the application
decoder and match direct host Metal metadata. The coordinator and native objects
also passed their final error, recovery, protocol and lifetime checks.

Direct host measurements confirmed that metadata cannot be replaced with empty
values: executable libraries report an install name, ordinary kernels use a
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
