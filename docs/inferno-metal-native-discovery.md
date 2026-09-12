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
state, and required overrides still need to be established before subclassing.

## What this means for Inferno

The published `InfernoMetalService` and type-0 user client provide a private
command transport. They do not implement this userspace provider. Adding
plugin properties to an ordinary service has not been shown to satisfy the
`IOAcceleratorES` match. `IOMatchCategory` alone is not evidence of that match.

The remaining dependencies are the actual accelerator service matching
contract, final service packaging, `_MTLDevice` initialization and capability
methods, and a provider that owns real Metal resource and command objects.
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
These target-specific observations are not a stable public plugin API or
proof of compatibility with another iOS release.
