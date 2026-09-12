# Native Metal discovery on iOS 26

Status: static investigation of iOS 26.5.2, build 23F84, N104 arm64e.
This identifies the framework's provider contract. No Inferno provider has
passed this path in a running iOS guest.

## Verified selection and construction

`MTLIOAccelServiceGlobalContext` performs the following sequence:

1. Obtain the IOKit main port and enumerate
   `IOServiceMatching("IOAcceleratorES")` with
   `IOServiceGetMatchingServices`.
2. Read the service's `MetalPluginName` property and require a string. Resolve
   its bundle under `/System/Library/Extensions`, with the `.bundle` suffix,
   through `NSBundle`'s `bundleWithPath:`.
3. Use `classNamed:` when `MetalPluginClassName` supplies a valid class name.
   Otherwise fall back to the bundle's `principalClass`.
4. Require the selected class to pass `isSubclassOfClass:` against
   `_MTLDevice`. A declaration of public protocol conformance alone does not
   satisfy this check.
5. Store the accelerator port and selected class in an
   `MTLIOAccelServiceDescriptor`. When processing pending descriptors,
   allocate the selected class and call `initWithAcceleratorPort:`.
6. Pass a non-null result to `MTLAddDevice`. That function asserts that the
   device array exists and that the object conforms to both `MTLDevice` and
   `MTLDeviceSPI`. It then calls `initLimits`, `initFeatureQueries`, and
   `initWorkarounds` before further setup and the synchronized insertion path.

The descriptor initializer `initWithAcceleratorPort:deviceClass:` is an
intermediate step. It is distinct from the plugin's single-argument initializer.
`_MTLDevice` is exported by the target Metal image; its initializer, private
state, and required overrides need to be respected when subclassing.

## Base-class requirements recovered so far

The target kernel registers the hierarchy
`IOService <- IOAcceleratorES <- IOGPU`. `IOAcceleratorES` has instance size
`0x88`, the same as `IOService`. All 168 virtual slots have matching pointer
authentication metadata, and 165 have identical function targets. The three
different slots are the destructor pair and the metaclass getter. Its
meta-taking constructors forward to the `IOService` constructor and install
the accelerator class's virtual table. Its metaclass allocator returns null;
direct allocation of this abstract base is not a way to publish a service.
A genuine concrete subclass and verified link bindings remain necessary.

Primary IOKit/XNU source confirms that `IOServiceMatching` uses the runtime
metaclass hierarchy. Registry names, DeviceTree `compatible` strings, and
`IOMatchCategory` do not create that relationship. The source revision is
XNU `12377.121.6`, compared with target `12377.122.8`; the hierarchy above was
independently recovered from the target kernel image.

In userspace, `_MTLDevice`'s default `initWithAcceleratorPort:` calls
`doesNotRecognizeSelector:`. A provider must override it. The ordinary base
`init` performs substantive allocation and queue setup, and the base destructor
cleans up that state; a subclass must not free those base allocations again.
The inherited `initWorkarounds` is a no-op. Limits and feature initialization
depend on `featureProfile`. The actual `initLimits` entry calls that selector
and immediately compares its 64-bit return against small integer constants;
it is a scalar profile selector, not an Objective-C object. Its recovered type is a 64-bit unsigned scalar; valid values and a truthful
mapping for the bridge remain unresolved. The family initializer passes it to a
helper that builds dynamically allocated family storage before replacing a
24-byte region of base state.

Installed SDK headers provide no declaration of this private profile. Public
`MTLGPUFamily` and `MTLFeatureSet` values describe different capability
contracts. The narrow bridge cannot advertise a complete family based on the
host GPU's capabilities. Some individual queries also need more implementation:
`MTLArgumentBuffersTier`, for example, has no unsupported value. Zero-filled
answers cannot stand in for a verified capability contract.

The base `limits` getter returns the structure at `self + 8`. Its initializer
requires positive color-attachment and indirect-buffer, texture, sampler, and
per-device sampler limits. Its ordinary return precedes the separate
`argumentBuffersSupport` method; a merged decompilation had incorrectly combined
them. The inherited indirect-capability mask defaults to zero, which selects
Tier 1. That is a concrete baseline answer, not an abstract method or a claim
that Inferno already implements argument buffers.

The feature-query path uses a class cluster. Allocation of
`MTLDeviceFeatureQueries` selects the concrete `_MTLDeviceFeatureQueries`, then
calls `initWithDevice:` and stores the result. The concrete `validate` method is
a no-op on this build. The concrete initializer's 231 device queries are now
identified. Their inherited methods forward through the feature-query object;
valid capability answers still need review. The base initializer returns nil; normal allocation selects
the concrete subclass before initialization.

An empty family array makes the inherited family-membership query return false;
this alone does not establish successful limits initialization. The recovered
family builder owns dynamically allocated storage through a vector-shaped
container. A conditional family flag uses the main executable's linked SDK via
`dyld_program_sdk_at_least`, so it must not be interpreted as a host-GPU check.

## What this means for Inferno

The published `InfernoMetalService` and type-0 user client provide a private
command transport. The application-side `user-client.[ch]` library now supplies
capability negotiation, packet encoding and connection ownership for a future
provider, but does not implement the provider itself. Adding
plugin properties to an ordinary service has not been shown to satisfy the
`IOAcceleratorES` match. `IOMatchCategory` alone is not evidence of that match.

The remaining dependencies are the concrete accelerator subclass and link
bindings, final service packaging, `_MTLDevice` capability initialization, and
a provider that owns real Metal resource and command objects.
Library and pipeline creation must compile and return errors synchronously.
The implemented v3 compiler-query path supplies complete bounded function
metadata, library type and nullable install name, pipeline limits/allocation,
imageblock sizing and structured compiler errors before execution. A serialized
C coordinator and ARC library/function/compute-pipeline objects now consume those
validated replies. SDK builds, sanitized fixtures and freestanding TCG/HVF tests
pass; native device integration is still missing.

A direct macOS 26.5 public-API probe on the host observed executable libraries
with `installName == "default.metallib"`, ordinary kernels with
`patchControlPointCount == -1`, nullable attribute arrays, and attributed vertex
functions with concrete vertex and stage-input records. Function constants
reported their real names, types, indices and required flags. These are measured
examples, not defaults for arbitrary shaders. The v3 metadata extension
serializes the host values. All 14 captured guest replies passed the production
decoder and the native host metadata comparison.

Sol implemented the native-object component from the reviewed Fable plan.
Unsupported resource-dependent selectors remain explicit. Exporting a host GPU
resource identifier requires an ownership policy that survives cache eviction;
the current source-key cache supplies no persistent guest handle. SDK and
macOS object tests do not establish that these objects initialize on the iOS
runtime; native driver startup and device initialization remain prerequisites.

The final registration block is now verified at instruction level. If its
runtime wrapper callback is absent, it inserts the original device. Otherwise
it asks for `_deviceWrapper`; an existing different wrapper is used directly,
while a self result causes the callback to create a wrapper. The selected
object is inserted with `addObject:`. Only the callback-created path then
releases that object through a tail call to `objc_release`, balancing the
wrapper after retaining-array insertion. The earlier decompiler interpretation
missed this release. No device-return contract follows from its inferred return
types, and no nil-result check exists between wrapper selection and insertion.

The inherited `_MTLDevice._deviceWrapper` is now verified to return `nil`
(`MOV X0,#0; RET` at `0x1861e29c8`). Inheriting that default is insufficient
when the optional callback is present: registration takes the different-object
branch and attempts to insert nil. The future provider must deliberately supply
its wrapper behavior; no corresponding implementation or runtime is proven.

The separate optional call before dispatch is now identified: a nonzero return
from CoreFoundation's `__CFMZEnabled` triggers
`allowLibrariesFromOtherPlatforms` on the original device. Only the call
identity is established; predicate and method semantics are unresolved and this
does not establish cross-platform compiled-library support. The runtime wrapper
callback identity/value and complete byref ownership also remain unverified.
See `metal-registration-defaults-w04w61p9/coordinator-review.md` under the local
evidence root for verified partial-query evidence and the model's length-limit
failure. Static registration does not prove
native initialization. Exact instructions, selector-chain verification, export
resolution and coordinator corrections are saved in
`~/InfernoData/ios26/gpu-display-20260911/metal-registration-lifetime-gt7pyqa5/coordinator-review.md`.

A successful first runtime milestone must show that stock
`MTLCreateSystemDefaultDevice()` returns the Inferno provider and that an iOS
application executes an offscreen command through its real user connection.
Persistent resources, general command submission, compiled shaders, and native
drawable presentation remain separate unfinished requirements.

## Evidence and limits

All addresses below are unslid shared-cache virtual addresses for 23F84:

| Evidence | Address |
| --- | --- |
| Global context initializer | `0x186100c94` |
| Bundle and class selection | `0x186100de0` |
| Pending descriptor processing | `0x18610111c` |
| `MTLAddDevice` | `0x186111a90` |
| Exported `_MTLDevice` class | `0x1ec69be80` |
| Device array / serial queue | `0x1ec69de70` / `0x1ec69de78` |

Cache UUID: `6ef96c18-8234-33e0-a1fa-e8aac6342568`.
Metal analysis image SHA-256:
`b34516f0499c0d8d89de4f970af0cfaa31bf8dfd0d242fa4bae9566a6779be1f`.

Sol checked primary dyld source and public SDK contracts; DeepSeek Flash
examined the target instructions. The coordinator checked decisive selector
strings, class metadata, branch destinations, and authentic export tables.
Compact method names were resolved using the cache-wide selector base from
the ObjC optimization header. The installed metadata tool's corrupt method
names were discarded. External imports resolve to `IOMainPort`,
`IOServiceMatching`, and `IOServiceGetMatchingServices`; the collection helper
is `dispatch_sync`.

Detailed local evidence is retained under
`~/InfernoData/ios26/gpu-display-20260911/metal-provider-discovery-huk90j0y/`.
Base-class and kernel interface evidence is under
`~/InfernoData/ios26/gpu-display-20260911/metal-native-base-271dpkdu/`;
the kernel extraction's code and data sections were checked byte-for-byte
against the original image before analysis.
The scalar profile verification and source capability limits are recorded in
`metal-feature-profile-hyhddo34/coordinator-review.md` and
`metal-feature-profile-source-bFkjagLr/findings.md` under the same evidence root.
Additional method types, allocation and limits evidence is in
`metal-profile-contract-7rd7xfxx/coordinator-review.md`; source API requirements
are in `metal-argument-buffer-source-CF5M1n7A/findings.md` and
`metal-compiler-api-source-Zmm1FCHK/findings.md` under that evidence root.
These target-specific observations are not a stable public plugin API or
proof of compatibility with another iOS release.


## Concrete feature-query initializer

The concrete `_MTLDeviceFeatureQueries` initializer calls `featureProfile` and
231 distinct capability selectors on the supplied device. Full-cache selector
resolution closed the gap left by the extracted Metal image, which does not
include the shared selector strings. The coordinator checked all 231 call targets
against the saved BL instruction bytes and sampled the resolved strings from
the full cache. Only 12 of those selectors appear in the public iPhoneOS 26.5
Metal headers. Their declarations do not establish private base behavior.

The initializer stores its device argument using a runtime-loaded ivar offset
whose cache value is 5792. A plain store does not establish retained ownership.
It also creates capability descriptions with constant names/tags and dynamic
query results. Descriptor-to-query pairing currently relies on decompiler
dataflow; tag meanings and a valid Inferno capability profile remain unresolved.
All 231 queries resolve to distinct `_MTLDevice` implementations with BOOL
return types. Each begins with `LDR X0, [X0, #0x270]` before forwarding through a
`familySupports*` selector trampoline. The receiver is the feature-query object,
not the device itself. The coordinator checked every first instruction and tail
branch, and sampled the corresponding concrete feature-query methods.
`initFeatureQueries` stores the initializer result at that offset afterwards
(`STR X0, [X19, #0x270]` at `0x186112c58`).

For an initially nil field, the inherited calls made during construction return
false under normal Objective-C message semantics. That conditional inference
does not prove complete device initialization, establish all post-initialization
answers, or justify advertising any GPU family. The concrete feature-query
methods can depend on the profile. The base instance method table does not
provide `featureProfile`; its concrete implementation and a truthful capability
mapping remain required investigation. Numeric switch ranges alone are not a
valid Inferno profile.

Kimi's preliminary analysis omitted the receiver load and inferred missing
methods on the device. Its revised mapping and the coordinator's independent
byte checks correct that conclusion. The controlling interpretation is retained
in `root-base-defaults-review.md` and `root-base-receiver-verification.json`.

This investigation ran through the authorized DeepSeek Flash retry, followed by
the user's selected Kimi K3 fallback for the missing full-cache selector data.
No native driver-loading operation was retried. Evidence is retained under
`kimi-feature-query-selectors-gq3rxr09/`, including `root-verification.json`.

### Concrete getter dataflow correction

A focused follow-up verified three concrete `familySupports*` getters. Each
loads the stored device from query+5792, then asks that device for the result:

| Concrete query method | Device call | Verified consequence |
| --- | --- | --- |
| `familySupportsBufferlessClientStorageTexture` | `supportsFamily:0` | Result comes from live family membership. |
| `familySupports2DLinearTexArraySPI` | `supportsFamily:1005` | Result comes from live membership; the public SDK names tag 1005 `MTLGPUFamilyApple5`. |
| `familySupports32BitMSAA` | `isMsaa32bSupported` | The base device implementation returns NO; concrete devices may override it. |

The verified `supportsFamily:` helper checks the device's family vector. These
getters do not read the constructor's Boolean records, so the earlier model
claim that later YES answers necessarily require construction-time `supportsX`
overrides is withdrawn for these methods. Three checked tails do not classify
all 231 methods or establish a valid Inferno family/profile. Advertising Apple5
would still require its actual capability contract.

Kimi's quota-limited follow-up produced no findings; the user later reported
restored availability and one bounded retry completed. Root checked the decisive
selrefs, immediates, branches and base constant-NO predicate. Evidence is in
`kimi-feature-query-selectors-gq3rxr09/findings-getter-dataflow.md` and
`root-getter-dataflow-verification.json` under the existing evidence root.

### Verified baseline profile families

The profile-to-family builder's signed jump table at cache VA `0x18612b3f8`
and its target immediates establish two baseline cases:

| Private profile | Family vector | Public SDK names |
| --- | --- | --- |
| `0` | `[1001, 3001]` | Apple1, Common1 |
| `1` | `[1002, 1001, 3001]` | Apple2, Apple1, Common1 |

Profile zero therefore advertises real GPU families; it is not a neutral
unsupported value. The empty vectors for cases 2 and 3 do not establish a
usable device either: independent initialization limits still require positive
values. No profile has been selected for Inferno.

DeepSeek Flash's larger returned table contained incorrect prefixes and omitted
fallthrough tails, so it is not accepted as a complete mapping. The verified
baseline cases and remaining contradictions are recorded in
`metal-profile-family-map-de62tgtz/coordinator-review.md` and
`root-verification.json`. Buffer and command implementation can proceed while
the native device capability contract remains unresolved.
