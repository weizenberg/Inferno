# Inferno Metal bridge, protocol version 2

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
The `inferno,metal-v1` binding name remains unchanged; the MMIO register negotiates protocol version 2.

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
| `0x04` | R | Protocol version, 2 |
| `0x08` | R | State: IDLE 0, BUSY 1, DONE 2, FAILED 3 |
| `0x0c` | R | Error: success 0, bad descriptor 1, bad memory 2, backend 3 |
| `0x10`, `0x14` | RW | Descriptor physical address, low/high halves |
| `0x18` | W | Write 1 to submit while IDLE |
| `0x20`, `0x24` | R | Completed sequence, low/high halves |
| `0x28` | R | Completion pending, bit 0 |
| `0x2c` | RW | Interrupt enable, bit 0; reset value 0 |
| `0x30` | W | Write bit 0 to acknowledge completion and return to IDLE |
| `0x34` | W | Write 1 to reset; other values ignored |

A doorbell while BUSY or while a completion remains unacknowledged is ignored.
An invalid doorbell value while IDLE produces a bad-descriptor completion with
sequence 0. A descriptor that cannot be read also completes with sequence 0.
The level interrupt is asserted while pending and enabled. Acknowledgement
clears the pending bit; the guest must also acknowledge its interrupt controller.

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
| 0 | u32 | Version, 2 |
| 4 | u32 | Operation: compute 1, triangle render 2, clear 3, library query 4, pipeline query 5 |
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
There are no depth, blending, texture binding, or presentation operations yet.

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

Library query (4) and compute-pipeline query (5) compile exact UTF-8 source with
Metal's default options. They use the existing asynchronous transport slot but
never create or commit a GPU command buffer. Both require 1..65536 source bytes,
zero input bytes, dimensions 1/1/1, empty fragment name, and zero options. Library
query requires an empty function name; pipeline query requires a kernel name of
at most 63 UTF-8 bytes. Output capacity is 16592..278736 bytes.

A compiler rejection is a successfully delivered result: device state DONE,
error zero, with a non-success outcome in the output envelope. A transport or
backend fault still fails without output writeback. Consumers must check both
transport completion and the typed outcome before creating a native object.

All envelope integers are little endian; the complete output allocation is
zero-filled before fields are written.

| Offset | Type | Meaning |
| --- | --- | --- |
| 0, 4 | u32 each | Protocol version 2, query opcode echo |
| 8 | u64 | Sequence echo |
| 16, 20, 24 | u32 each | Outcome, phase, flags |
| 28, 32 | u32 each | Function count, selected function type |
| 36, 40, 44 | u32 each | Maximum threads, execution width, static threadgroup bytes |
| 48 | byte[16] | Reserved, zero |
| 64 | i64 | Signed NSError code |
| 72, 76 | u32 each | Error domain and description lengths in bytes |
| 80 | byte[128] | UTF-8 error domain, terminated and zero-padded |
| 208 | byte[16384] | UTF-8 description, terminated and zero-padded |
| 16592 | records | Complete function inventory, if successful library query |

Each 256-byte function record contains a u32 type, u32 name length, and 248-byte
UTF-8 name field. Names are nonempty, at most 247 bytes, terminated and padded
with zeroes. At most 1024 records are supported. Function type values are vertex
1, fragment 2, kernel 3, visible 5, intersection 6, mesh 7 and object 8.

Outcomes are OK 0, COMPILE_FAILED 1, FUNCTION_NOT_FOUND 2,
FUNCTION_TYPE_MISMATCH 3, INVENTORY_UNSUPPORTED 4 and OUTPUT_TOO_SMALL 5.
Phases are NONE 0, LIBRARY 1, INVENTORY 2, FUNCTION 3 and PIPELINE 4.
Flags mark truncated description (bit 0), truncated domain (bit 1), absent
NSError (bit 2), and a nonfatal warning accompanying success (bit 3).
Diagnostic truncation preserves UTF-8 boundaries. Bridge explanations do not
invent Apple error codes.

An inventory is complete or absent. Unsupported count, name, duplicate or type
cannot produce a partial successful inventory. The two inventory failures carry
the actual count and zero records, including when the count exceeds capacity or
1024. A successful library can have zero functions. A pipeline result carries
actual host pipeline limits: maximum threads and execution width are positive;
static threadgroup memory may be zero. These values do not add arbitrary
threadgroup control to the existing compute execution operation.

Unspecialized function constants are unsupported. They are rejected before
compute pipeline creation, because the native API can abort for that input
instead of returning an error object. Specialization requires a future options
and identity contract.

Pipeline queries use the same exact-source/function cache key as compute
execution. Reset preserves immutable cached pipelines and discards stale query
output and interrupts through the existing generation check. No persistent host
resource handle is exposed.

The hardware and user protocols both require version 2. Old/new transport pairs
fail at open; old descriptors fail validation. Old/new application/kernel pairs
fail capability negotiation and close the new connection. No v1 fallback is
provided. A v2 service advertises queries only after opening a v2 host.

## Reset and lifetime

Reset clears descriptor, completed sequence, error, pending interrupt and mask.
It invalidates an outstanding request's generation without waiting for Metal.
The device stays BUSY until that worker retires, then becomes IDLE. The stale
request performs no guest DMA and raises no interrupt. Poll IDLE before reusing
the slot, then configure the descriptor and interrupt mask again.

The host API does not support cancelling a submitted command buffer. Device
teardown joins the worker before releasing its buffers and queue; a host GPU
driver that never completes can therefore delay shutdown. Buffers and textures
are per-request. Each device retains up to eight compute/render pipelines,
evicting the least recently used entry on insertion. Keys contain the opcode,
an owned copy of the exact shader bytes, and both applicable entrypoint names.
Protocol v2 fixes the remaining pipeline state, including BGRA8Unorm; future
configurable state must extend the key. Failed compilation, function validation,
or pipeline creation is never cached. A later encoding or execution failure
still reports an error even when the valid pipeline is retained.

Reset preserves these immutable host pipelines while invalidating all request
DMA and completion state as described above. The single outstanding worker
owns cache access; teardown joins it before releasing the cache. No guest
pointers, input/output buffers, command buffers, or render textures are cached.
Persistent guest resources and presentation remain future work.

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

Capabilities advertise compute, render, clear, library query and pipeline query
as opcode bits 1 through 5. Status state and transport results use explicit stable wire mappings;
the timer error is the 32-bit `IOReturn` pattern, and completion sequence/error
come only from the connection's session snapshot. Unknown internal enum values
fail the call rather than encoding success.

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

The v2 independent host fixture passes 1,020 ASan/UBSan checks. It substitutes public
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

`contrib/inferno-metal/compiler-client.[ch]` builds typed library and pipeline
requests and validates full result envelopes. Compile it with `user-client.c`
and link IOKit and CoreFoundation. The caller supplies exact source bytes and
sequence; the library query also takes an inventory capacity. Zero capacity
permits an initial count probe. Pipeline names longer than 63 UTF-8 bytes and
unsupported compile options cannot be silently shortened or ignored.

The result decoder checks the complete supplied output, including unused
padding, before publishing any result. Error codes retain their signed 64-bit
value. Successful decoded views borrow the immutable caller buffer; copy any
needed data before freeing or modifying that buffer. Failure leaves the output
result unchanged.

The helpers do not wait, acknowledge, reset or close automatically. The caller
serializes client operations, polls existing status until matching completion,
checks transport errors, reads the complete output allocation, then decodes and
copies the result before acknowledging. A timeout alone does not retire a
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
