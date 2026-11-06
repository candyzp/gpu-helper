# GPU Helper

An iOS-only Geometry Dash 2.2081 / Geode 5.11.2 prototype exploring whether Metal compute can assist with independent mathematical work.

## Current milestone: visible in-game diagnostics

- Keeps Geometry Dash's renderer, gameplay, player physics, collision, triggers, and timing untouched.
- Adds a small Objective-C++ bridge to Metal.
- Implements one independent element-wise multiply-add kernel as a concrete GPU-compute proof.
- Tracks only successfully completed Metal dispatches, processed elements, last batch size, GPU execution time (when Metal exposes it), and CPU submit-plus-wait time.
- Adds a visible in-game diagnostics panel to the gameplay scene after PlayLayer setup completes.
- The panel is enabled by default and can be disabled using **Show in-game GPU diagnostics overlay** in GPU Helper settings.
- The overlay refreshes independently on its own Cocos node schedule and logs when it attaches.

**Important current limitation:** the multiply-add API is not connected to Geometry Dash gameplay or background systems yet. In a normal level, the overlay will honestly show 'GPU JOB: NONE', zero dispatches, and zero elements. 'METAL: READY' means that the device exposes Metal; it does not mean a compute workload has run.

## Why this starts small

GPU work has submission, synchronization, and data-transfer costs. A kernel that returns a result to the CPU may cost more than doing a small calculation on the CPU. The overlay reports both GPU execution time (when available) and CPU submit-plus-wait time so later experiments can measure the full cost. It does not claim an FPS improvement.

## Build

The root project is built for iOS arm64 using the Geode build action:

- Geometry Dash: 2.2081
- Geode: 5.11.2
- Target: iOS arm64

The GitHub Actions workflow runs two independent jobs:

1. **iOS Build** — builds and uploads the Geode package.
2. **Metal Compute Regression** — compiles the Metal bridge on a macOS runner, runs it against boundary-sized input batches, and checks every result against a CPU reference.

## Regression test

Run locally on a Mac with Xcode command-line tools and a Metal-capable device:

    mkdir -p build
    xcrun --sdk macosx clang++ -std=c++23 -fobjc-arc -Isrc \
      src/gpu/MetalCompute.mm tests/metal_compute_regression.mm \
      -framework Foundation -framework Metal \
      -o build/metal-compute-regression
    ./build/metal-compute-regression

The test covers empty input, invalid pointers, and sizes around the 64-thread dispatch boundary (1, 63, 64, 65, 257, and 1024 elements). It also verifies that diagnostic counters only increase after successful GPU dispatches.

## Next step

Identify one independent Geometry Dash calculation suitable for GPU offload and instrument its real invocation path. Until then, the overlay intentionally reports that no gameplay workload is connected.
