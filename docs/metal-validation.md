# Metal validation

The 18 October 2026 Release run on Apple A18 Pro, macOS 27.0.1, Xcode 27.1 and Slang 2026.20 passes
**36 of 37 tests** with API and shader validation, subject to the instrumentation exceptions below. No tests are skipped.
The remaining failure is the native render-timestamp issue below. The library and utilities also compile for arm64 iOS 26;
this is compile coverage, not a device test. Earlier hardware coverage includes M3 Max.
M1/M2 hardware execution remains unverified. The backend requires Metal 4, OS 26+ and Apple GPU family 7 or newer.

## Running the checks

Follow the [macOS build instructions](../README.md#macos-installation-and-quick-start), then run:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --test-dir build --output-on-failure
```

`test_render_continuation` exposes the [native render-timestamp failure](repro-metal-render-timestamps.md).
Add `-E '^test_render_continuation$'` to run the remaining checks separately.

Coverage includes shared root layouts, GPU pointers, descriptor indexing/copies, direct and indirect
commands, texture transfers, viewport orientation, presentation and command-pool reuse. Triangle,
cube and deferred-renderer examples run with Metal API validation.
Presentation tests cover default and Steam-compatible modes with two and three drawables, two frames in flight,
resize after idle, shutdown and the layer's framebuffer-only setting. Native execution of the Steam-compatible cases remains pending.
The tests do not load Steam; overlay composition must also be checked in the installed game with `steam_overlay_compatibility` enabled.

## Concurrency and lifetime

Tests exercise independent recording, cross-queue waits, heap creation/destruction, concurrent
updates to disjoint descriptor slots, timestamp allocation and CPU retrieval. Address-index tests
reach the 64-heap limit and repeatedly reuse entries. The shared CPU address-map test also exercises concurrent
lookups during registration and removal. Timeline retirement checks cover future cross-queue waits,
transitive completion, polling, nonzero initial values and immediate resource destruction.
See [the implementation](metal-support.md) for synchronization responsibilities.

## Stage dependencies and overlap

Barrier tests check compute-to-fragment visibility, geometry side effects, GPU-generated index and
indirect arguments, and attachment readback. Bounded probes observe later vertex/mesh work overlapping
earlier fragment work, both within a submission and across submissions with timestamps enabled.
Broad-barrier controls wait as expected; output checks pass.

A three-drawable presentation probe also observes overlap between frames. This establishes a scheduling
opportunity on M3 Max, not a guaranteed speedup or iPhone result. Drawable availability can still stall
presentation. CPU timestamp retrieval adds no GPU wait.

## MetalTools limitations

- Concurrent sampler creation/destruction can crash API validation in `MTLSamplerDescriptorHashMap`.
  A native Metal-only stress test reproduces it; uninstrumented execution passes.
- Shader validation cannot reliably enumerate placed resources through heap residency. In that mode,
  the backend registers individual resources and serializes MetalTools residency enumeration.
- Shader instrumentation of native indirect mesh commands can crash in `resolvedSharedPacketData`
  or leave readbacks unwritten. Direct commands pass with instrumentation; indirect commands pass
  with API validation. CTest exercises these separately.

To reproduce the indirect-mesh tool failure:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 build/tests/test_metal_mesh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 build/tests/test_task_shader
```

The corresponding `--direct-only` runs pass. These diagnostic accommodations do not change production commands.
