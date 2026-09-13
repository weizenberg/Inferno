# GPU / Metal progress

**50% complete** — 5 of 10 milestones finished, not a time estimate.
**iOS 26 GPU acceleration: not working yet.**

**Timeline:** ✅ Foundation + compute → 🔄 Rendering → ⏳ iOS apps → ⏳ Display

**Finished:** GPU communication, shader compilation, buffers/textures, and basic GPU commands, and compute argument buffers (v6). These work in tests.

**Still to finish:**

1. Finish rendering argument support — **plan ready; checking reusable code**.
2. Make iOS recognize a working Metal device.
3. Run real iOS apps through the GPU driver.
4. Show GPU-rendered frames on screen.
5. Show “Virtual display” in Settings.

**Next delivery:** render-stage and null-buffer argument support.
**Published:** v6, `d152c4e248`, on `gpu-metal-bridge`.
