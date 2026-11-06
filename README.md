# GPU Helper

An iOS-only Geometry Dash 2.2081 / Geode 5.11.2 experiment using Metal compute for a real, narrowly scoped gameplay workload.

## Current milestone: GPU-assisted static decoration visibility (v0.3.2)

GPU Helper now uses Metal to evaluate camera intersection tests for ungrouped static decoration objects in the loaded level.

- At level setup, the mod gathers bounds of eligible static decorations once and uploads them to persistent shared Metal buffers.
- During play, a Metal compute kernel tests those bounds against an expanded camera rectangle.
- The dispatch is asynchronous. The gameplay thread does not wait for the GPU or read back a result synchronously.
- Completed masks are applied to a GameObject visit hook. Only objects proven to be outside the expanded view are skipped during Cocos render traversal.
- The camera rectangle has a 640-unit safety margin. Large camera jumps or zoom changes immediately clear the cull mask so drawing fails open.
- Only objects marked as decorations, with the engine's static-decoration flag set and no group membership, are candidates.
- If Metal initialization, buffer setup, dispatch, result validation, or the driver fails, the mod clears its cull mask and normal drawing continues.

What this does not do: it does not replace Geometry Dash's renderer, draw objects with Metal, change object visibility flags, remove objects, or alter player physics, collision, triggers, object updates, or level timing. Cocos2d remains responsible for drawing. On death, the PlayLayer and its static-object cache remain in place; the bounds are not rebuilt unless the level is exited and loaded again.

## Performance expectations

This is an experiment, not a guaranteed FPS boost. It is intended to reduce Cocos render traversal for levels with many eligible static decorations. Sparse levels or levels dominated by dynamic/grouped objects may see little or no benefit. The GPU dispatch itself also consumes GPU time, so on-device comparisons are required before calling it a net optimization.

The diagnostics overlay attaches directly to Geometry Dash's screen-space PlayLayer HUD after setup completes and reports the real number of candidate objects, objects currently culled, completed Metal dispatches, GPU execution time (when available), and CPU submission time. It does not fabricate activity.

## Settings

- GPU-assisted static decoration culling — enabled by default; disable it to return to normal Cocos traversal.
- Show in-game GPU diagnostics overlay — shows the live workload and timing information.

## Regression test

The macOS Metal regression test covers the original multiply-add proof kernel and input validation, asynchronous decoration-bound visibility dispatch, exact inside/outside/intersection results, generation tracking, and completed-work counters.

Run locally on a Mac with Xcode command-line tools and a Metal-capable device:

    mkdir -p build
    xcrun --sdk macosx clang++ -std=c++23 -fobjc-arc -Isrc \
      src/gpu/MetalCompute.mm tests/metal_compute_regression.mm \
      -framework Foundation -framework Metal \
      -o build/metal-compute-regression
    ./build/metal-compute-regression

## Build target

- Geometry Dash: 2.2081
- Geode: 5.11.2
- Target: iOS arm64
