# Inferno Metal bridge, protocol version 5

This experimental device executes bounded guest requests on the host Metal GPU.
It is available in Darwin builds with the Metal framework and is disabled by
default. It is an execution transport: stock iOS does not contain a matching
driver, and enabling it does not make `MTLCreateSystemDefaultDevice()` succeed
inside iOS. A guest driver and Metal provider remain required.

For the verified N104 t8030 topology, enable `-M t8030,metal-bridge=true`.
The Apple DeviceTree node `arm-io/inferno-metal` has compatible
`inferno,metal-v1`, child address `0xfff00000`, size `0x10000`, AIC parent
`0x20`, and interrupt `0x200`. Its physical interval is
`[0x2fff00000, 0x2fff10000)`. This does not replace the native AGX nodes.
The `inferno,metal-v1` binding name remains unchanged; the MMIO register reports protocol version 5 and peers require exact equality.

For freestanding ARM tests, `virt` also accepts
`-device inferno-metal-bridge,addr=0x0b000000`. Its generated FDT supplies the
address and platform-bus interrupt. Software must discover these values from
the appropriate tree. Only one outstanding request is supported. Migration and
hot unplug are unsupported.

## Registers

Registers are aligned, 32-bit, little-endian MMIO accesses. Unsupported offsets,
access sizes, and read/write directions fail the bus transaction.

| Offset | Access | Meaning |
| --- | --- | --- |
| `0x00` | R | Magic `0x4c544d49` |
| `0x04` | R | Protocol version, 5 |
| `0x08` | R | State: IDLE 0, BUSY 1, DONE 2, FAILED 3 |
| `0x0c` | R | Error: success 0, bad descriptor 1, bad memory 2, backend 3 |
| `0x10`, `0x14` | RW | Descriptor physical address, low/high halves |
| `0x18` | W | Write 1 to submit while IDLE |
| `0x20`, `0x24` | R | Completed sequence, low/high halves |
| `0x28` | R | Completion pending, bit 0 |
| `0x2c` | RW | Interrupt enable, bit 0; reset value 0 |
| `0x30` | W | Write bit 0 to acknowledge completion and return to IDLE |
| `0x34` | W | Write 1 to reset; other values ignored |
| `0x38` | R | Batch progress: bit 0 records actual host scheduling |

A doorbell while BUSY or while a completion remains unacknowledged is ignored.
An invalid doorbell value while IDLE produces a bad-descriptor completion with
sequence 0. A descriptor that cannot be read also completes with sequence 0.
The level interrupt is asserted while pending and enabled. Acknowledgement
clears the pending bit; the guest must also acknowledge its interrupt controller.

Scheduling progress does not complete a request, raise an interrupt, or permit
buffer reuse. It is cleared on accepted submission, acknowledgement and reset.
A rejected busy submission preserves the active request's progress. Generation
checks prevent a delayed scheduled callback from publishing after reset.

Before submission, make descriptor, shader and input writes visible with the
guest architecture's DMA/MMIO barriers. Read completion, use an appropriate
read barrier, and verify the full sequence and error before consuming output.
Successful output writeback precedes the completion and interrupt. The guest
owns buffer lifetime and must not reuse those buffers until completion or reset
retirement.

## Descriptor

The descriptor is exactly 256 bytes; this is a wire layout, not a host C struct.
All integers are little endian. Addresses are guest physical addresses.

| Byte offset | Type | Meaning |
| --- | --- | --- |
| 0 | u32 | Version, 5 |
| 4 | u32 | Operation: compute 1, triangle render 2, clear 3, library query 4, pipeline query 5, imageblock query 6, compute batch 7, resource batch 8, typed library/compute/render/imageblock queries 9/10/11/12 |
| 8 | u64 | Guest sequence identifier |
| 16 | u64 | Shader source address |
| 24 | u32 | Shader source byte length |
| 28 | u32 | Input byte length |
| 32 | u64 | Input address |
| 40 | u64 | Output address |
| 48 | u32 | Output byte length |
| 52, 56 | u32 each | Width, height |
| 60 | u32 | Compute depth; render vertex count; 1 for clear |
| 64 | char[64] | NUL-terminated UTF-8 compute or vertex function name |
| 128 | char[64] | NUL-terminated UTF-8 fragment function name |
| 192 | u32 | Options: zero for defaults, for every operation |
| 196 | byte[60] | Reserved, zero |

Source is UTF-8 Metal Shading Language, at most 64 KiB; its supplied byte length
does not require a trailing NUL. Compute and render require nonempty source
and function names. Their source compilation, function lookup/type checking,
pipeline creation and command execution errors produce error 3 without output
writeback. Compiler queries use the typed result envelope below.

Compute binds input at buffer index 0 and a zero-initialized output at index 1.
Each dimension is nonzero, and total dispatched threads cannot exceed 16 Mi.
Shaders must respect the supplied buffer lengths. Dispatch uses an x-only
threadgroup sized up to the pipeline execution width and threadgroup limit.

Render draws a triangle list with 3..65535 vertices in multiples of three. Input
is buffer 0 for both vertex and fragment stages. The target is cleared to
transparent black. Render and clear return tightly packed BGRA8Unorm bytes,
four bytes per pixel and `width * 4` bytes per row. Dimensions cannot exceed
4096, and output size must equal `width * height * 4` within the 16 MiB limit.
These basic render/clear operations do not expose depth, blending or texture
bindings. Resource batches below add configurable rendering and texture/sampler
bindings. Native presentation remains unfinished.

Clear consumes exactly 16 input bytes: four little-endian IEEE float32 RGBA
values, each finite and in [0,1]. It requires zero source length and depth 1.

Input and output are each limited to 16 MiB. Every nonempty range, including
the descriptor, must resolve entirely within one ordinary contiguous RAM
mapping. MMIO, inaccessible memory, overflowing ranges, and unsupported
scatter/gather mappings are rejected. Descriptor, source and input are copied
under QEMU's global lock before the worker starts; the GPU worker never holds
a guest-memory pointer. Output uses QEMU's DMA write path, including TCG code
translation invalidation.

## Compiler queries

Library query (4), compute-pipeline query (5), and imageblock-size query (6)
compile exact UTF-8 source with Metal's default options. They use the existing
asynchronous transport slot without creating or committing a GPU command buffer.
All require 1..65536 source bytes, zero input bytes, empty fragment name, and
zero options. Library query requires an empty function name; pipeline and
imageblock queries require a kernel name of at most 63 UTF-8 bytes. Library and
pipeline dimensions are 1/1/1. Imageblock dimensions are independently 1..65536.

Library output capacity is 17168..1368848 bytes. Pipeline and imageblock outputs
are exactly 17168 bytes. Capacity is a byte count, including fixed metadata and
all variable records; it is not a function count.

A compiler rejection is a successfully delivered result: device state DONE,
error zero, with a non-success outcome in the output envelope. A transport or
backend fault still fails without output writeback. Consumers must check both
transport completion and the typed outcome before creating a native object.
All integers are little endian; the complete output allocation is zero-filled.

| Offset | Type | Meaning |
| --- | --- | --- |
| 0, 4 | u32 each | Protocol version 5, query opcode echo |
| 8 | u64 | Sequence echo |
| 16, 20, 24 | u32 each | Outcome, phase, flags |
| 28, 32 | u32 each | Function count, selected function type |
| 36, 40, 44 | u32 each | Maximum threads, execution width, static threadgroup bytes |
| 48, 52 | u32 each | Required output size on capacity failure, metadata sub-record count |
| 56, 60 | u32 each | Function-failure stage for query 11; reserved zero |
| 64 | i64 | Signed NSError code |
| 72, 76 | u32 each | Error domain and description lengths in bytes |
| 80 | byte[128] | UTF-8 error domain, terminated and zero-padded |
| 208 | byte[16384] | UTF-8 description, terminated and zero-padded |
| 16592 | byte[64] | Pipeline metadata on successful pipeline/imageblock query |
| 16656 | byte[512] | Library metadata on successful library query; render-pipeline metadata for query 11 |
| 17168 | variable records | Complete function inventory on successful library query |

Pipeline metadata carries u64 allocation size and three u64 required threadgroup
dimensions, i64 shader-validation value, u64 imageblock byte length, and a u32
flag for indirect-command-buffer support. The last 12 bytes are reserved zero.
Imageblock length is populated only for opcode 6 or 12. These are actual host values;
no persistent GPU resource identifier is transported.

Library metadata carries u32 library type, u32 presence flags, u32 install-name
length, four reserved zero bytes, and a 496-byte terminated UTF-8 install-name
field. Bit 0 distinguishes a present install name from nil; an empty present
string is valid. Type and install name come from the host library.

Each function record has a 296-byte header followed by 128-byte metadata
sub-records. The header carries function type/name, patch type, signed 64-bit
patch control-point count, 64-bit options, attribute-presence flags, constant and
attribute counts, and the complete record size. Its 248-byte name field holds at
most 247 UTF-8 bytes. Nil attribute arrays remain distinct from empty arrays.
Sub-records carry kind, data type, 64-bit index, flags and a 104-byte name field
(maximum 103 UTF-8 bytes). Constants precede vertex attributes, then stage-input
attributes. Constant flags identify required values; attribute flags identify
active, patch-data and patch-control-point-data values. The portable header
defines every field offset. Limits are 1024 functions and 8192 sub-records.

Outcomes are OK 0, COMPILE_FAILED 1, FUNCTION_NOT_FOUND 2,
FUNCTION_TYPE_MISMATCH 3, INVENTORY_UNSUPPORTED 4, OUTPUT_TOO_SMALL 5 and
SPECIALIZATION_REQUIRED 6. Phases are NONE 0, LIBRARY 1, INVENTORY 2, FUNCTION 3
and PIPELINE 4. Flags mark truncated description (bit 0), truncated domain
(bit 1), absent NSError (bit 2), and a warning accompanying success (bit 3).
Diagnostic truncation preserves UTF-8 boundaries. Bridge explanations do not
invent Apple error codes.

An inventory is complete or absent. Unrepresentable metadata produces an
explicit failure rather than a partial successful inventory. A successful
library can have zero functions. Capacity failures report the required complete
size, allowing a bounded retry after acknowledgement with a fresh sequence.
Pipeline maximum threads and execution width must be positive; static
threadgroup memory and required dimensions may be zero. These metadata queries
do not add arbitrary threadgroup control to compute execution.

Unspecialized function constants are rejected before compute pipeline creation,
because the native API can abort for that input instead of returning NSError.
Queries report SPECIALIZATION_REQUIRED; ordinary compute retains its backend
failure without output. Specialization requires a future options/identity
contract.

Pipeline queries share the exact-source/function key with compute execution.
The library and pipeline caches retain warning diagnostics. Reset preserves
immutable cached objects and discards stale query output and interrupts through
the existing generation check. No persistent host resource handle is exposed.
Both hardware and application protocols require version 5. Older transports
fail at open, old descriptors fail validation, and application/kernel mismatches
fail capability negotiation. There is no version fallback.

## Compute batches

Opcode 7 carries a complete batch in the descriptor's input range. Descriptor
source and function names are empty, dimensions are 1/1/1, and options are zero.
The batch carries its own source and entrypoint records. Its output capacity is
exactly 16,640 bytes plus the combined buffer-image length.

`include/standard-headers/inferno/metal.h` defines every wire offset. The input
is a 64-byte header followed by pipeline, buffer, dispatch and binding records,
then source bytes, inline bytes and complete initial buffer images. All integers
are little endian; regions must be contiguous and reserved bytes zero.

| Record or region | Size and limit |
| --- | --- |
| Pipeline | 80 bytes, at most 8; source range and terminated function name |
| Buffer | 16 bytes, at most 64; byte length and default flags |
| Dispatch | 64 bytes, at most 64; pipeline, binding range, grid and group dimensions |
| Binding | 32 bytes, at most 1,024; kind, slot, resource/range and offset |
| Source | At most 64 KiB per source and 8 × 64 KiB combined |
| Inline bytes | At most 4,096 per binding and 64 KiB combined |
| Buffer images | Combined length at most 16 MiB minus 16,640 bytes |

Buffer and inline bindings share slots 0 through 30; threadgroup-memory bindings
use a separate namespace. Full images preserve aliases and bytes outside shader
writes. Pipelines sharing source may be grouped canonically, with dispatch IDs
remapped without changing dispatch order. A nonempty batch requires pipelines
and dispatches but may have no buffers. An empty batch has a
64-byte header with version 5 and every other field zero; it still commits a
real empty host command buffer.

Validation checks all dimensions and products, actual pipeline/device limits,
and static plus dynamic threadgroup memory before encoding. Host Metal compiles
the real entrypoints, executes dispatches in order and returns final images.
The scheduled flag comes from its native scheduled handler; committing a request
does not itself establish scheduling.

The fixed 16,640-byte result header includes version/opcode/sequence, typed
outcome and phase, flags, failed-record kind/index, buffer/image counts, host
command-buffer status, and bounded NSError domain/description with signed code.
Complete buffer images follow. Successful results require observed scheduling
and host status Completed. Typed failures retain diagnostics and return zeroed
image storage. The entire response is validated before any guest shadow copy.
This ABI is separate from the 17,168-byte compiler-query header.

## Resource batches and typed libraries

Version 5 changes all outer versions together; there is no mixed-version mode.
Operations 1 through 7 retain their layouts and validators with the version
field changed to 5. The new operations carry their manifests only in the input
range, with zero source length and empty descriptor function names.

| Opcode | Input and output | Descriptor dimensions |
| --- | --- | --- |
| 8, resource batch | Input at least 128 bytes; output exactly 16640 plus all resource images | 1/1/1 |
| 9, typed library query | Input at least 64 bytes; output 17168 through compiler maximum | 1/1/1 |
| 10, typed compute-pipeline query | Input at least 64 bytes; output exactly 17168 | 1/1/1 |
| 11, render-pipeline query | Input at least 64 bytes; output exactly 17168 | 1/1/1 |
| 12, typed imageblock query | Input at least 64 bytes; output exactly 17168 | Each 1..65536 |

A resource manifest starts with a 128-byte header, then contiguous tables in
this order: libraries, compute pipelines, render pipelines, buffers, textures,
samplers, commands, draws and bindings. Payload bytes, inline bytes and complete
initial resource images follow without padding. Integer and floating-point wire
values are little endian; unaligned byte regions are read without native struct
casts. Reserved fields and unused per-kind fields are zero.

| Table | Record bytes | Maximum records |
| --- | --- | --- |
| Library | 32 | 16 |
| Compute pipeline | 80 | 8 |
| Render pipeline | 192 | 8 |
| Buffer | 16 | 64 |
| Texture | 32 | 32 |
| Sampler | 80 | 32 |
| Ordered command | 96 | 64 |
| Draw | 160 | 256 |
| Binding | 32 | 2048 |

Library kind 1 carries nonempty UTF-8 source, at most 64 KiB. Kind 2 carries an
opaque compiled Metal library, at most 4 MiB. Total payload is at most 8 MiB.
Inline bindings retain the 4096-byte individual and 64-KiB aggregate limits.
Each complete input and output must fit the 16-MiB transport bound. Nonempty
manifests require a command and every declared resource must be referenced.
The all-zero-count header is a valid real empty GPU submission.

Textures are shared, tracked, non-sparse 2D resources, one level/slice/sample,
with dimensions 1..8192. Supported four-byte formats are RGBA8Unorm,
RGBA8Unorm_sRGB, RGBA8Snorm, BGRA8Unorm and BGRA8Unorm_sRGB. Buffer/inline and
texture slots each run from 0 through 30; sampler slots run from 0 through 15.
Transport table capacities are separate from these binding-slot limits. The
host requires a supported Apple GPU family; this does not imply that the guest
implements that family's complete feature set.

Final pipeline creation reflection determines texture read/write usage
requirements for each stage before encoding. Unsupported binding shapes fail
explicitly. Render-target usage is checked separately, and every draw pipeline's
color format must match its pass attachment. Non-normalized samplers use clamp
to edge, no mip filtering, equal min/mag filtering and anisotropy 1.

Compute and render commands execute in manifest order in one command buffer.
Render passes preserve load/clear and store behavior even without draws.
Each draw captures its pipeline, bindings and raster state; omitted optional
raster fields select full-attachment defaults rather than a previous draw's
state. Color write masks apply independently of blending being enabled.

Opcode 8 extends the batch result with texture count at byte 48; bytes 52..63
remain reserved zero. All buffer images precede all tight texture images in
table order. Success requires host Completed and observed scheduling. Failures
return zero image storage. New outcomes are unsupported state 9 and resource
creation failure 10, with resource phase 7. The compiler and batch outcome enums
remain distinct; callers must use the envelope selected by opcode.

Typed query manifests use a 64-byte header, one library record (or two for a
render pipeline), a compute/render pipeline record when applicable, and payload
bytes. Queries 9, 10 and 12 reuse the existing inventory/compute result layouts.
Query 11 leaves the compute block and unused compute header limits zero, and
places actual render metadata in the 512-byte block at byte 16656. It carries
allocation and imageblock sample sizes, thread/threadgroup limits, execution
widths, shader validation, required tile/object/mesh dimensions and flags for
indirect command buffers and tile-size matching. Its remaining 364 bytes are
zero. Header byte 56 is stage 1 (vertex) or 2 (fragment) only on a query-11
function failure; otherwise it is zero. Byte 60 remains zero for all queries.

The exact schema constants and bounds live in
`include/standard-headers/inferno/metal.h`. These transport and host execution
features do not establish native iOS Metal device discovery or presentation.

## Reset and lifetime

Reset clears descriptor, completed sequence, error, pending interrupt and mask.
It invalidates an outstanding request's generation without waiting for Metal.
The device stays BUSY until that worker retires, then becomes IDLE. The stale
request performs no guest DMA and raises no interrupt. Poll IDLE before reusing
the slot, then configure the descriptor and interrupt mask again.

The host API does not support cancelling a submitted command buffer. Device
teardown joins the worker before releasing its buffers and queue; a host GPU
driver that never completes can therefore delay shutdown. Buffers and textures
are per-request. Each device retains up to 32 libraries and 32 compute/render pipelines,
evicting the least recently used entry on insertion. Legacy keys retain exact
source and entrypoint identity. Typed keys additionally distinguish source from
compiled payloads and include canonical render state. Typed query and execution
share the same constructors and keys. Cached pipelines retain their warning and
creation reflection together through promotion, eviction and teardown. Failed compilation, function validation,
or pipeline creation is never cached. A later encoding or execution failure
still reports an error even when the valid pipeline is retained.

Reset preserves these immutable host pipelines while invalidating all request
DMA and completion state as described above. The single outstanding worker
owns cache access; teardown joins it before releasing the cache. No guest
pointers, input/output buffers, command buffers, or render textures are cached.
The v4 provider retains guest buffer shadows across batches; host buffers remain
per-request. Textures and native presentation remain future work.

The `inferno_metal_submit`, `inferno_metal_complete`, and `inferno_metal_error`
trace events expose accepted work and errors. A reset-invalidated request has
a submit event and deliberately has no completion event.

The `inferno_metal_pipeline_cache` event records lookup hits and the number of
entries at lookup (before insertion on a miss), without shader contents.
`inferno_metal_backend_phase` records host wall-clock nanoseconds for successful
processing phases. On cache hits, the `library` phase includes key construction
and lookup; the pipeline phase measures reuse instead of pipeline creation.
These timings exclude guest submission and completion overhead and do not
measure native iOS frame rate.

## Guest transport implementation

`include/standard-headers/inferno/metal.h` defines the portable protocol constants
and native command fields. The host and guest use that same definition, while
both serialize/parse the 256-byte little-endian wire descriptor explicitly.
`contrib/inferno-metal/transport.[ch]` implements a nonblocking, allocation-free
little-endian AArch64 transport. It does not depend on QEMU, libc, or IOKit.
The caller provides a device-memory register mapping and prepared physical RAM,
serializes every call (including callbacks), and schedules polling or interrupts.

Initialize an `ImtlTransport` to zero, call `imtl_open`, fill and encode a command,
and call `imtl_submit` with the descriptor GPA and its full sequence. `imtl_poll`
returns `IMTL_AGAIN` while work is pending. On `IMTL_OK` with phase COMPLETED,
inspect `ImtlCompletion.error` before using output, then call `imtl_ack`.
Errors in device execution are distinct from local API/protocol errors. Calls
apply ARM `dsb sy` barriers around device access and output consumption.

A sequence mismatch keeps request ownership and requires a reset drain. For an
unreadable descriptor the host cannot recover its sequence and returns zero;
this also requires reset if the caller expected a different sequence. Do not
interpret a transport failure as permission to free outstanding DMA memory.
`imtl_reset` can return `IMTL_AGAIN`; keep every backing allocation and mapping
alive until polling returns `IMTL_OK` with phase IDLE. `imtl_close` refuses any
unacknowledged completion or undrained request. A timeout does not cancel DMA.

Open intentionally refuses a busy device or pending completion left by an
unknown owner. It does not adopt that owner's buffers or reset its work. Initial
device recovery requires an external owner to establish safe reset/lifetime
conditions before opening. This transport supplies no IOMMU translation, memory
allocation, OS workloop, user-client interface, Metal provider, or presentation.
It is a reusable submission component for those remaining driver layers.

## IOKit memory owner

`contrib/inferno-metal/iokit-transport.[h,cpp]` owns the register map and DMA
buffers for a kernel caller. It maps provider memory index zero without caching,
validates the portable transport's magic/version/idle checks, and keeps a 16 KiB
physical descriptor allocation. Source, input and output get separate buffers
sized to each submitted request, preserving the protocol's full 64 KiB source
and 16 MiB input/output limits. It imposes no smaller render-size cap. Large
physically contiguous allocations can fail with `kIOReturnNoMemory`.

Both `kIOMemoryPhysicallyContiguous` and `kIOMemoryMapperNone` are required.
Every nonempty buffer must report a nonzero physical address, a segment covering
its entire length, and an exclusive end that does not overflow. These are wired,
nonpageable `IOBufferMemoryDescriptor` allocations; the factory/destructor own
automatic preparation and completion. No unmatched explicit `complete()` is
added. If this changes to pageable or explicitly prepared memory, the lifetime
implementation must change with it.

Submission copies the command, source and input into private kernel-owned RAM.
Caller-supplied command GPA fields are replaced with the owner's physical ranges.
Those caller buffers need remain alive only until submission returns. In-flight
private buffers and the descriptor are immutable until acknowledgement or reset
retirement. After successful completion, `imtl_iokit_read_output` supports bounded
copies into a kernel destination; device or transport failures never copy output.

The caller must keep its provider connection valid and serialize every method on
its workloop. Even polling/reset can free memory when retirement is observed;
none of these calls belongs in a hardware interrupt handler or under a simple
lock. An interrupt handler should schedule the owning workloop. This component
supplies no user-pointer handling, user client, interrupt source or timer itself.

`imtl_iokit_destroy` stops new submissions and resets unacknowledged work. If it
returns `IMTL_AGAIN`, keep the handle, provider connection, and callback machinery
alive and retry after retirement. Only `IMTL_OK` clears the handle and releases
the map and memory. A timeout or other error is not permission to unload the
owner while its work is still outstanding.

This source compiles against the available macOS 26.5 Kernel SDK for an iOS 26.5
arm64e target. That is a compilation check, not proof that the target iOS kernel
exports all required APIs or matches the SDK's virtual-call ABI. Exact-target
execution remains required before native iOS Metal can be claimed.

## Kernel service and completion workloop

`contrib/inferno-metal/InfernoMetalService.[h,cpp]` wraps the memory owner in
an `IOService`. Startup opens the provider, creates a private workloop, command
gate and completion timer, and publishes the protocol and buffer limits. All
fallible setup precedes successful transport creation. Failed startup detaches
event sources and closes any opened provider; factory references remain until
the service is freed.

Kernel callers retain the service across each call and use `submitKernel`,
`getStatus`, `readOutput`, `acknowledge`, `resetCommand` and `requestShutdown`.
These methods serialize through the command gate and accept kernel pointers
only. One submission remains owned until acknowledgement or reset retirement.
The one-shot timer polls completion every millisecond while work is pending;
this service does not enable the hardware completion interrupt. `getStatus`
also polls, allowing progress when timer arming fails. Submission success means
the doorbell was written even if the timer failed; inspect `Status.timer_error`
separately. A transport fault retains the request for reset or shutdown.
The timer error records the most recent arm attempt; a later terminal poll can
leave it visible until acknowledgement or a new submission clears status.

Shutdown rejects further commands and retries memory-owner destruction until
the host has retired DMA. Until then it keeps the provider open and preserves
the workloop, mapping, and all allocations. A timeout or timer error does not
permit their release. After retirement it closes the provider. During provider
removal this allows IOKit's deferred stop/detach to continue. `stop()` enters the
service's own workloop gate before checking retirement and removing event
sources; the framework may call it on the provider's workloop. `free()` releases
their factory references and the workloop. An early stop/free with live resources
is an invariant failure.

Provider recursion enters through the public `requestTerminate` callback. A
scoped context under the command gate permits its synchronous nested
`terminate` call to delegate immediately to the superclass; later
`willTerminate` begins draining. No private IOKit option value is copied into
the driver. Direct callers must first finish `requestShutdown`, then invoke
`terminate` outside the workloop and its gate. Direct termination before draining
returns false. The superclass termination call runs after leaving the gate so
synchronous termination cannot prevent its own workloop callbacks.

This layer provides a kernel submission service. Exact iOS ABI and lifecycle
verification, a native Metal provider and display presentation remain required.
SDK compilation and tests with stand-in IOKit objects do not establish that it
loads or accelerates iOS applications.

## Connection sessions and request packets

The service's `newSession` creates a kernel session object. Multiple sessions
may coexist. `submitSession` claims the device for one command and takes a
service-owned reference to that session. Other sessions may query their own
status, but their submission returns Busy until the current command is
acknowledged or reset retires. Only the active session can read, acknowledge or
reset its command. Legacy non-session read/ACK/reset methods also reject a
session-owned command; the global kernel status and shutdown APIs remain
administrative operations.

Each session keeps its own status snapshot. Reset/ACK can return it to Idle
while another session starts work, without exposing the new owner's sequence
or result. Session callers retain both the service and the session for every
call. The session's service pointer is only a weak identity check; callbacks
operate through the service's active reference and never use a user-client
pointer.

`closeSession` first revokes new operations. If it owns a command, it starts
reset and retains the session and DMA resources until retirement. The caller
may drop its own session reference immediately after close; the service owns
the asynchronous lifetime. A transport or timer failure keeps the device Busy.
A later session operation can retry an abandoned reset and observe retirement
even if the timer could not be armed. Closing an idle session does not affect
another session or destroy the device transport. Whole-service shutdown drains
and releases any active session before stop.

Closing a valid session revokes it even when the return value is NotReady
(reset still draining) or a reset error. Those return values preserve the drain
diagnostic; they do not mean that the caller should reuse the session or retain
its reference indefinitely. Repeated close is allowed. A failed ACK preserves
the original completion/polling fault in status. Reads before completion return
NotReady, reads from a faulted command return IOError, and invalid completed
output ranges remain BadArgument.

`include/standard-headers/inferno/metal-user.h` defines a versioned wire ABI for
capabilities, submit, status, bounded output read, ACK and reset. Submission is
an explicitly little-endian 192-byte header followed by shader and input bytes.
It carries no pointers or physical addresses. The 64 KiB shader and 16 MiB
input/output caps are preserved, with exact packet length, supported opcode,
version, reserved-zero fields and terminated function names checked by
`contrib/inferno-metal/user-request.c`. A failed decode leaves its output
unchanged; a successful decode has zero GPA fields and slices only the supplied
kernel packet. GPU-specific shader and execution validation remains in the
existing transport/backend path.

This decoder requires an immutable, fully copied kernel packet.
`InfernoMetalUserClient` supplies that boundary for connection type 0. Each
successful open creates a fresh session and exposes six synchronous selectors:
capabilities, submit, status, read, acknowledge and reset. GPU execution remains
asynchronous and service-owned. A transferred Mach connection remains the same
capability and session; a separate open creates a separate session.

The adapter accepts exactly version 2 `IOExternalMethodArguments` and validates
the complete selector-specific shape itself. Capabilities and status return
exactly 32 bytes. Submit accepts one packed 192-byte-through-maximum request.
Read takes one 64-bit offset and returns 1 byte through 16 MiB. A structure input
or output larger than the normal 4096-byte inline limit must use an out-of-line
descriptor. Pointer values associated with zero sizes or counts are ignored.
Async arguments and variable object output are unsupported.

Out-of-line submission is copied completely into exact-sized kernel staging,
then completed, before request decoding or service submission. Out-of-line read
prepares and completes the descriptor around one bounded copy. Every successful
prepare has one complete, including short-copy failures; a copy failure remains
the reported error if completion also fails. Descriptor capacities are explicit
and may be smaller than their backing descriptor, but never larger. The adapter
retains no user pointer, descriptor or staging allocation after the synchronous
call, and the service makes its own DMA copy before submit returns.

Capabilities advertise the explicit opcode bits 1 through 12. Unknown bits
remain errors. Resource batches require input/output capacities of at least
128/16640 bytes; typed queries require at least 64/17168 bytes. These transport
minimums do not describe a guest GPU feature family. Status state and
transport results use explicit stable wire mappings;
the timer error is the 32-bit `IOReturn` pattern, and completion sequence/error
come only from the connection's session snapshot. Unknown internal enum values
fail the call rather than encoding success.

The status word at byte 28 contains batch progress bit 0, exposed only for the
owning active session. Unknown bits fail validation. SUBMITTED status retains
sequence zero; COMPLETED must echo the submitted sequence before its progress
can be attributed to that command. Capability, submission and status packets
all use version 5; older peers are rejected explicitly.

The user client explicitly enables all three documented default-locking
properties. It retains the service, shared workloop and session until `free`.
Close and task death revoke the session under that workloop, initiate any needed
reset, then terminate the user client without waiting for GPU retirement. The
service retains active session and DMA ownership until retirement, including
timer or reset failures. A later open may create an independent idle session,
although its submit remains Busy while another session owns the device.

The explicit service factory keeps normal `IOServiceOpen` platform checks and
accepts only public connection type 0. It adds no private entitlement,
authorization shortcut or shared-instance property. Native iOS admission,
port-transfer behavior, exact target ABI and runtime still require on-device
verification before publishing an application-accessible service.

## Application-side IOKit client

`contrib/inferno-metal/user-client.[ch]` provides the userspace half of the
connection for the eventual native Metal provider. Compile the C source with
`include/` on the header search path and link IOKit and CoreFoundation. It is
not part of the host QEMU binary. The library does not discover a service or
implement an `MTLDevice`; the caller supplies an existing, borrowed
`io_service_t` to `imtl_user_client_open`.

A successful open owns one type-0 connection and exposes immutable negotiated
capabilities through `imtl_user_client_caps`. The caller must serialize every
operation, capability access and close. `imtl_user_client_close(&client)` clears
the handle and closes its connection exactly once, even when close returns an
error. Do not retry the old connection. A failed open leaves the output handle
null; failures after a successful `IOServiceOpen` close that new connection and
preserve the original error. The supplied service reference remains caller-owned.

`ImtlUserSubmit` is a native request, not the wire layout. Its source and input
are borrowed for the synchronous submission call, and both 64-byte function
name arrays must contain a NUL. The client validates the negotiated independent
size limits and aggregate request bound, creates a canonical little-endian
packet with zero options/flags/reserved/name tails, and frees it after the call. The
full 64-bit sequence is preserved. Public IOKit calls handle inline and
out-of-line marshalling; GPU execution continues asynchronously in the service.

Capability and status replies must have the exact fixed size and valid version,
reserved fields and enumerated values before the client publishes them. A read
must return exactly its requested length. Raw IPC failures remain unchanged;
malformed successful replies return `kIOReturnBadMessageID`, local invalid
arguments return `kIOReturnBadArgument`, and allocation failure returns
`kIOReturnNoMemory`. Read sets the returned byte count to zero on failure, but
IOKit may already have modified the destination: consume no bytes unless the
call succeeds. The client also checks offset-plus-length overflow.

Submit, status, read, acknowledge and reset do not retain application buffers
or duplicate the service's GPU state. This layer adds no polling thread or
locking. The kernel service continues to own outstanding work after disconnect.
The source compiles and links with the actual iPhoneOS 26.5 userspace SDK and
macOS 26.5 SDK. These checks do not establish native iOS driver startup, a
successful application connection, or Metal device discovery.

The historical v2 independent host fixture passed 1,020 ASan/UBSan checks. It substitutes public
IOKit calls and uses the production kernel decoder as an independent packet
oracle. Coverage includes maximum requests and reads, lower negotiated limits,
malformed replies, partial failed writes, raw errors and balanced allocation
and connection cleanup. It does not execute native IPC or GPU work. Fixture
and build commands, source hashes and results are preserved under
`~/InfernoData/ios26/gpu-display-20260911/metal-native-client-fixture-WFRaYum4/`
and `metal-native-client-build-el7fjj7_/` (v1 baseline). The v2 fixture and SDK
results are in `metal-v2-fixtures-rjBLw1CQ/` and
`metal-compiler-root-checks-8dib4nyj/`.

## Application-side compiler helpers

`contrib/inferno-metal/compiler-client.[ch]` builds typed library, pipeline and
imageblock requests and validates full result envelopes. Compile it with `user-client.c`
and link IOKit and CoreFoundation. The caller supplies exact source bytes and
sequence; the library query also takes an output byte capacity. The minimum
capacity permits an initial size probe. The output-size helper accepts function
and metadata capacities; zero/zero computes that minimum. Pipeline names longer than 63 UTF-8 bytes and
unsupported compile options cannot be silently shortened or ignored.

The result decoder checks the complete supplied output, including unused
padding, before publishing any result. Error codes retain their signed 64-bit
value. Successful decoded views borrow the immutable caller buffer; copy any
needed data before freeing or modifying that buffer. Failure leaves the output
result unchanged.

The helpers do not wait, acknowledge, reset or close automatically. The caller
serializes client operations, polls existing status until matching completion,
checks transport errors, reads the complete output allocation, then decodes and
validates the private reply before acknowledging. Decoded views remain valid
while that private buffer remains alive and unchanged; ACK does not free it.
A timeout alone does not retire a
request; the existing reset/drain and close ownership rules still apply.

This layer supplies the future provider's compilation contract. It does not
create native `MTLLibrary` or pipeline proxy objects by itself.

The v2 implementation passed the real freestanding ARM transport test under
both TCG and HVF, including compute/render/clear regressions, compiler failures
at both creation phases, complete inventories, unsupported metadata, query to
execution cache reuse, function-constant rejection, and reset during compilation.
The stale query wrote no output or completion; fresh queries recovered. An old
v1 transport failed open with `IMTL_BAD_DEVICE` under both accelerators.

Eight captured success and library/pipeline failure envelopes pass the
production application decoder under ASan/UBSan. Actual error domains and signed
codes match native Metal, as does the pipeline failure diagnostic. Pipeline values match a direct native Metal compile of the same
source on the test host: 1024 maximum threads, execution width 32, zero static
threadgroup memory. These measurements are not protocol constants. The combined
kernel-service fixture passes 985 checks. Userspace sources compile and link
with iPhoneOS 26.5 and macOS 26.5 SDKs; kernel sources compile and partially link
for arm64e iOS using the nearby macOS kernel SDK, which still does not establish
the target iOS kernel ABI or native driver loading.


## Native compilation objects and coordinator

The compilation-object component introduced in v3 is in
`contrib/inferno-metal/provider/`, with the
synchronous C policy layer in `coordinator.[ch]`. It constructs ARC objects for
`MTLLibrary`, `MTLFunction` and `MTLComputePipelineState` from validated host
metadata. SDK builds, sanitized object tests and the recursive protocol audit
pass for this component. It does not create or register the native iOS Metal
device.

The compiler context accepts an existing service and a real device owner. It
holds the owner weakly to avoid a device/context cycle; operations take strong
owner snapshots and returned objects retain the device. Libraries retain exact
immutable source, functions retain their library, and pipelines retain their
function. Labels use the public API's ownership semantics. Derived queries
recreate host objects from immutable source identity after cache eviction.

Only source compilation with nil options and basic compute-pipeline creation
are supported. Specialization, library data/default libraries, descriptors,
reflection-dependent creation and additional linking are explicit unsupported
operations. Resource-dependent argument encoders and GPU resource IDs raise the
provider's unsupported exception. A protocol object answering its selectors is
not evidence of complete Metal functionality, native iOS admission or rendering.

The coordinator exclusively owns its client connection and serializes calls.
It polls status, accepts only matching successful completions, reads and
validates the full allocation, then acknowledges. A submitted status has no
sequence echo; the final completion does. Uncertain IPC submission is resolved
through the exclusively owned session rather than blindly resubmitted. Timer
and cleanup diagnostics are retained separately from the primary result.

Polling, capacity growth and recovery use a configurable monotonic timeout.
Synchronous IOKit calls cannot be interrupted by this layer, so it does not
promise a hard wall-clock bound for blocked IPC or lock contention. Timeout
starts reset/drain or connection cleanup; it never proves DMA retirement.
Invalidation closes the connection once. Final destruction requires callers to
stop admitting work and let all existing calls finish before freeing the handle.
The provider keeps its context alive through asynchronous operations and invokes
completion handlers outside coordinator locks.

Host SDK construction of the three public metadata subclasses has passed; their
initialization on the target iOS 23F84 runtime is still unverified. The native
`_MTLDevice` capability initialization and accelerator class binding requirements
remain recorded in [native discovery](inferno-metal-native-discovery.md).

The final v3 source passed 26 SDK compilation/link stages, including arm64e
userspace iPhoneOS 26.5 and arm64 macOS 26.5 builds. Compile the provider's six
Objective-C implementation files with ARC and blocks, together with
`user-client.c`, `compiler-client.c` and `coordinator.c`; link Foundation, Metal,
IOKit and CoreFoundation. Kernel partial linking still uses nearby macOS headers
for an iOS target and does not verify the target kernel ABI.

The independent ASan/UBSan fixtures passed 2,410 application/compiler/coordinator
checks and 994 kernel-service checks. They substitute IOKit, clocks and kernel
objects. The macOS provider suite uses the actual ARC implementation with a
scripted coordinator; it passed metadata, error, asynchronous callback, lifetime
and local rejection checks. Its recursive audit reports no missing required
selectors, including inherited NSObject and MTLAllocation requirements. Sequence
exhaustion is source-reviewed because the opaque coordinator exposes no test
sequence setter.

The final signed QEMU binary passed all freestanding ARM guest assertions under
TCG and HVF, with 29 serial lines each. Coverage includes compute/render/clear,
compiler errors, attributes/constants, imageblock sizing, strict v1/v2 descriptor
rejection, query cache reuse, and reset/recovery with no stale output or
completion. All 14 captured envelopes passed the actual application decoder
under ASan/UBSan and the native host metadata comparison. These tests use
isolated RAM guests; they are not iOS boot or driver-loading evidence.

Exact source hashes, commands, results and remaining limitations are retained in
`~/InfernoData/ios26/gpu-display-20260911/metal-native-objects-root-checks-4e2zun6d/acceptance.json`.
The independent C/kernel fixtures are in `metal-v3-fixtures-4esdlng_/` under the
same evidence root. Older v1/v2 fixture results above remain historical evidence.

## Native buffers and command objects

The v4 context also creates `InfernoMetalBuffer`, `InfernoMetalCommandQueue`,
`InfernoMetalCommandBuffer`, and `InfernoMetalComputeCommandEncoder`. Shared
buffers have stable, page-aligned contents and default CPU cache behavior.
Default or tracked hazard mode is supported. No-copy, private/managed storage,
unretained command references, textures, events and native presentation remain
explicitly unsupported; GPU addresses, resource IDs and execution times are not
invented.

Encoding snapshots pipeline/binding state and copies inline bytes. Commit
freezes commands. Initial shared-buffer images are captured later, at execution
admission after preceding writeback. A serial executor chooses the earliest
commit serial among ready queue heads. An explicitly enqueued, uncommitted head
blocks its own queue only. Serial allocation, Committed publication and
reservation readiness share one scheduler-locked transition. This does not
promise a global FIFO across blocked queues.

Shared resources use whole-image admission snapshots and successful writeback.
Applications must synchronize CPU access with pending GPU work; concurrent CPU
writes can be overwritten by the later full-image result. The provider does not
track dirty ranges or merge unsynchronized CPU and GPU writes.

Queue capacity waits are cancelable on invalidation. Stable reservation tokens
are refunded exactly once; weak uncommitted reservations do not retain abandoned
commands. Removing an abandoned uncommitted head schedules another scan after
releasing the queue lock, so later committed work can proceed without another
submission. Committed commands retain their resources until possible writeback.
Per-command serial delivery queues run scheduled handlers before completed
handlers on a concurrent executor. Waits include handler completion, and one
command's callback can wait for another command without blocking its execution.
Handlers run outside coordinator, scheduler and object locks. An unscheduled
terminal failure releases undeliverable scheduled captures. If invalidation
fails a command before commit, its first commit preserves that typed failure
and delivers completion handlers added before commit; only a subsequent commit
is a duplicate. Pending delivery batches keep waits from returning before late
handler bodies finish.

`imtl_coordinator_execute_batch` returns a fully validated reply even when it
also carries timer or acknowledgement diagnostics. The provider maps either
diagnostic to terminal Error and publishes no shadow bytes, preserving the
typed result's phase, outcome, failed record, sequence and scheduling metadata.
Transport/protocol failures return no result bytes; actual observed scheduling
and sequence remain available separately. A logical timeout or cancellation
never proves kernel DMA retirement. Existing compiler-query object-plus-cleanup
diagnostic semantics remain unchanged.

Build the eleven provider `.m` files with ARC and blocks, together with
`user-client.c`, `compiler-client.c`, `batch-wire.c`, `batch-client.c` and
`coordinator.c`; link Foundation, Metal, IOKit and CoreFoundation. The final
source passes 40 kernel/iPhoneOS/macOS compilation and link stages. A recursive
macOS runtime audit finds no missing required methods in the four new classes.
Native ordering, abandonment and typed-error fixtures pass 60 assertions with
address and undefined-behavior sanitizers. They use the real ARC objects with
an explicit scripted coordinator boundary, not a native iOS driver connection.

Retained client/compiler/coordinator and kernel-service regressions also pass
against the current v4 source: 2,390 and 994 ASan/UBSan checks. Compatibility-only
fixture changes omit three v3 assertions whose values are valid in v4, add the
batch capability to the expected mask, and supply a zero-progress service mock.
The client link includes the new batch modules; retained cases do not exercise
batch execution. These results do not establish v4 batch coordinator/progress
or production kernel transport coverage. Exact changes and verified results:
`~/InfernoData/ios26/gpu-display-20260911/metal-v4-existing-regressions-8p5vic4w/root-review.json`.

Real host GPU acceptance uses separate freestanding ARM fixtures under TCG and
HVF: complete 576-byte images match independent CPU calculations after dependent
dispatches and successive batches; reset after live scheduling preserves the
retired output and allows fresh compute work. Empty and inline-only dispatches,
legacy peer rejection, compute/render/clear regressions and all 14 compiler
metadata captures also pass. Full-image transfer is deliberately bounded and
does not establish production rendering performance.

The library representation remains UTF-8 source-only, including native library
objects, pipeline queries and batch records. Compiled `.metallib` loading and
guest default/URL library resolution are not implemented. A separate direct
host reference now accepts one iOS26-targeted and one macOS26-targeted kernel
through public data/URL library APIs and executes four correct 100-byte guarded
outputs. Actual compiler targets and artifact hashes are recorded in
`~/InfernoData/ios26/gpu-display-20260911/metal-compiled-library-oracle-ynwcqy3q/root-verification.json`.
This is limited host compatibility evidence; it neither generalizes to arbitrary
iOS libraries nor proves any Inferno compiled-library execution.

Current results and remaining acceptance gaps are recorded in
`~/InfernoData/ios26/gpu-display-20260911/metal-v4-root-checks-vji9prnm/acceptance-registry.json`.
Native iOS device discovery, driver startup, capability selection, full resource
families, presentation and Settings virtual-display identification remain
unverified and incomplete.

## Local v5 integration verification

The coordinated v5 QEMU executable (SHA-256
`4b0fb5bf6f9a1c01d0c0f2bbe45ea8552e16c863c579008b079e320455c015b1`)
passes ten valid new-opcode scenarios under both TCG and HVF. All 20 captured
replies match the independently accepted host references except for the checked
per-request sequence field. Scenarios cover typed library, compute, imageblock
and render queries; empty, clear, load/draw, compiled compute, texture sampling
and per-draw raster-state restoration. Separate legacy opcode7 compute, buffer
alias/offset, live scheduled-reset and recovery tests pass with all 14 captures
matching the prior reference after the version-word migration. The reset image
remains byte-identical. Evidence is in the local GPU/Metal evidence directory
under `metal-v5-arm-valid-votsw9bi/run-3fabilss` and
`metal-v5-legacy-arm-ovss8ny1/metal-v4-arm-oracle-saarhp21`.

These freestanding ARM checks exercise actual MMIO and host GPU execution. They
do not establish native iOS driver loading, provider discovery or presentation.
The provider M2a context compiles and links for iPhoneOS26.5 arm64e and macOS26.5;
its source/data/file/default library factories and resource-batch compute path
also pass a direct real-host provider test. Six factory paths reproduce all
1,200 independently calculated guarded image bytes, with scheduling/completion
for six compute commands and one empty command. That fixture substitutes the
coordinator/IOKit connection while linking actual provider objects, wire builders
and decoders, and the host backend; it does not establish production connection
runtime. Evidence: `metal-provider-m2a-real-host-fw8ghfp0/run-_npuqlms`.
The M2b texture, sampler and render objects now also compile and link on both
SDKs. Their actual-host provider checks match the native reference for padded
transfers, partial updates, clear/load passes, compiled render and sampling
libraries, dependent compute-to-texture rendering, descriptor capture and raster
state. All five supported formats pass transfer and clear checks; the sRGB clear
checks use exact zero/one endpoints. Unsupported local states preserve resources
and submit no work. A real host texture-usage rejection retains its typed error
and explanatory description without shadow writeback; subsequent valid work
completes. These provider checks also substitute the coordinator/IOKit seam.
See GPU_METAL_PROGRESS.md for exact evidence directories and coverage limits.

Legacy opcodes 1–6 additionally pass under TCG/HVF, with all 14 compiler captures
accepted by the production decoder and matching host metadata. Frozen published
v4 transport sources reject the v5 peer under both accelerators; focused client
capability negotiation passes 142 sanitized checks with fake IOKit.

The unchanged QEMU binary also passes a debugger observer-lifetime check using
the accepted opcode-7 and opcode-8 TCG workloads. All 13 observed GPU batches
clear their callback and work pointer before worker retirement; four typed-query
retirements correctly have no GPU observer. Both targets exit zero with complete
workload markers. Debugger timing limits this to the observed runs, supplemented
by source review of the shared helper, reset and worker-join paths. Evidence:
`metal-host-observer-runtime-9ifhwfyy/root-runtime-verification.json`.

A production native MTLDevice adapter, argument buffers, truthful device
capabilities, actual iOS provider/driver integration and presentation remain
unfinished. This v5 layer does not establish native iOS Metal acceleration or
the requested Settings virtual-display identity.
