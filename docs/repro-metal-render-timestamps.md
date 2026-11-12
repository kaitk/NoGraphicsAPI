# Metal 4 render timestamp repro

On Apple M3 Max, macOS 26.6.2 (25G83), Xcode 27.0 (27A266a), native render-encoder timestamps leave
some entries unwritten after GPU completion. The failure reproduces with and without API validation.
This points to a Metal driver/runtime issue; the root cause is not vendor-confirmed.
The same native failure reproduces on Apple A18 Pro, macOS 27.0.1 (26A434), Xcode 27.1 on 17 October 2026:
interleaved draws write 5 of 10 entries; grouped before/after markers write 4 of 10. Both API-validation modes fail,
including a blended 1080p workload. Relaxed granularity and spaced counter indices also leave entries unwritten.
Command-buffer markers remain valid. No replacement or skipped correctness check is applied.

The [standalone source](../tests/repro_metal_render_timestamps.mm) uses native Metal and inline MSL,
without NoGraphicsAPI calls or Slang. Its target is excluded from normal builds and CTest.

## Workload

One command buffer contains one render pass and a 32-entry counter heap. Ten
`writeTimestampWithGranularity:afterStage:intoHeap:atIndex:` calls use precise granularity and the
fragment stage, interleaved with fullscreen draws. Two command-buffer markers sit outside rendering.
There is no pass suspension or resource reuse.

After submission, the CPU waits for an `MTLSharedEvent`, then calls `resolveCounterRange:` and checks
its returned size before reading entries. This follows Apple's
[CPU counter-resolution synchronization](https://developer.apple.com/documentation/metal/mtl4counterheap/resolvecounterrange%28_%3A%29).
All ten render entries should be nonzero; repeated nonzero values alone are not a failure.

## Run

From a [configured test build](building.md):

```sh
cmake --build build --target repro_metal_render_timestamps
export MTL_SHADER_VALIDATION=0
MTL_DEBUG_LAYER=0 build/tests/repro/repro_metal_render_timestamps
MTL_DEBUG_LAYER=0 build/tests/repro/repro_metal_render_timestamps --heavy
MTL_DEBUG_LAYER=0 build/tests/repro/repro_metal_render_timestamps --before
MTL_DEBUG_LAYER=0 build/tests/repro/repro_metal_render_timestamps --after
MTL_DEBUG_LAYER=1 build/tests/repro/repro_metal_render_timestamps --heavy
```

Default uses an 8 × 8 attachment; `--heavy` uses 1920 × 1080 with blending.
`--before` and `--after` place all markers around a single draw. Exit codes: 0 for valid entries,
1 for missing entries, 77 when no Metal 4 device is available.

## Results, 25 September 2026

| Configuration | Render entries |
| --- | --- |
| Default or `--heavy`, API validation off | 0–4 nonzero; 5–9 zero |
| `--before` or `--after`, API validation off | 0–3 nonzero; 4–9 zero |
| `--heavy`, API validation on | 0–4 nonzero; 5–9 zero; no validation error |

Outside-render markers work in these runs. CPU and GPU counter-resolution diagnostics show the same
missing entries. No reliable workaround for in-render markers is established. Other Apple GPU families
and iOS have not been tested for this failure; an undocumented restriction or API usage issue has not
been conclusively excluded.

The failure affects profiling and causes `test_render_continuation` to fail. Timestamp-free continuation
passes. This repro does not establish a rendering defect; the backend preserves native results.
