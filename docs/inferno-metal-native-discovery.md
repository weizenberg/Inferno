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
it is a scalar profile selector, not an Objective-C object. Its canonical type,
valid values and truthful mapping for the bridge remain unresolved. The family
initializer passes it to a helper before replacing a 24-byte region of base
state; that helper still needs verification.

Installed SDK headers provide no declaration of this private profile. Public
`MTLGPUFamily` and `MTLFeatureSet` values describe different capability
contracts. The narrow bridge cannot advertise a complete family based on the
host GPU's capabilities. Some individual queries also need more implementation:
`MTLArgumentBuffersTier`, for example, has no unsupported value. Zero-filled
answers cannot stand in for a verified capability contract.

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
The final registration block also needs instruction-level verification; its
current decompilation is insufficient to settle optional wrapping behavior.

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
These target-specific observations are not a stable public plugin API or
proof of compatibility with another iOS release.
