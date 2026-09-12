# GPU and Metal progress

Last updated: 12 September 2026.

## Overall status

**The host GPU bridge works in test guests. Native Metal acceleration inside
the iOS 26 guest is not working yet.**

The app-to-driver connection is implemented, compiles and passes 940 automated
checks with memory and undefined-behavior sanitizers. The next major gap is
making iOS discover and use a native Metal device.

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
| Application connection | Implements capability, submit, status, read, acknowledge and reset calls, including large buffers. | Kernel SDK compile/partial link and the combined 940-check fixture passed against the dedicated branch. |
| Isolated publication build | GPU/Metal code builds on its own dedicated branch. | QEMU build passed; that exact binary passed TCG/HVF transport tests. |

These results prove the components described above. They do not prove native
iOS app acceleration or a working Metal display.

## Latest completed milestone

- [x] Implement the application-to-driver connection.
- [x] Pass 940 combined service, connection, buffer-copy and cleanup checks.
- [x] Build the isolated branch and pass the TCG/HVF transport tests.
- [ ] Resolve how the real iOS Metal framework discovers and initializes its device.

The combined tests cover large requests, short and failed copies, independent
connections, disconnect during outstanding work, reset failures and cleanup after
service shutdown. They use substitute OS objects, so native iOS behavior still
needs runtime verification. The compile uses nearby macOS kernel headers for an
iOS target; matching the actual iOS kernel interface is still outstanding.

## What remains for the full goal

| Remaining work | What will count as done |
| --- | --- |
| Verify the real iOS driver interface and startup | The new driver starts on the target iOS build and an application opens its connection successfully. |
| Native Metal device discovery | Stock `MTLCreateSystemDefaultDevice()` returns the Inferno device instead of `nil`. |
| Native application execution | An iOS application creates a Metal queue, submits work and receives correct results through the real driver connection. |
| General resource and command support | Reusable buffers/textures and ordered commands work beyond the current single-operation bridge; required compiled-shader support is established. |
| Native presentation | Metal drawables reach the iOS compositor and display, with correct synchronization. |
| Virtual-display identification | Settings visibly identifies the panel as a virtual display; no native display-parts authentication claim. |
| End-to-end verification | Repeatable iOS boots, responsive UI, real app rendering and reset/reconnect recovery are demonstrated under TCG and HVF. |

The existing iOS runtime samples still report no default Metal device.
The precise native provider discovery contract remains unresolved. A previous
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
