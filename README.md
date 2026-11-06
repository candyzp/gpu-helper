# GPU Helper

An iOS-only Geometry Dash 2.2081 / Geode 5.11.2 prototype exploring whether Metal compute can assist with independent mathematical work.

## Current milestone: compute proof

- Keeps Geometry Dash's renderer, gameplay, player physics, collision, triggers, and timing untouched.
- Adds a small Objective-C++ bridge to Metal.
- Implements one independent element-wise multiply-add kernel as a concrete GPU-compute proof.
- Adds a macOS Metal regression test that compiles and executes the same kernel used by the iOS mod.
- Logs whether a Metal device is available when the mod loads.

This is **not yet connected to Geometry Dash's gameplay or background systems**. The first milestone is only to prove that the Metal compute path builds and returns correct results. It does not claim an FPS improvement.

## Why this starts small

GPU work has submission, synchronization, and data-transfer costs. A kernel that returns a result to the CPU may cost more than doing a small calculation on the CPU. Before attempting to offload any real Geometry Dash workload, we need a known-correct compute path and regression coverage.

## Build

The root project is built for iOS arm64 using the Geode build action:

- Geometry Dash: 2.2081
- Geode: 5.11.2
- Target: iOS arm64

The GitHub Actions workflow runs two independent jobs:

1. **iOS Build** — builds and uploads the Geode package.
2. **Metal Compute Regression** — compiles the Metal bridge on an Apple Silicon macOS runner, runs it against boundary-sized input batches, and checks every result against a CPU reference.

## Regression test

Run locally on a Mac with Xcode command-line tools and a Metal-capable device:

```sh
mkdir -p build
xcrun --sdk macosx clang++ -std=c++23 -fobjc-arc -Isrc \
  src/gpu/MetalCompute.mm tests/metal_compute_regression.mm \
  -framework Foundation -framework Metal \
  -o build/metal-compute-regression
./build/metal-compute-regression
```

The test covers empty input, invalid pointers, and sizes around the 64-thread dispatch boundary (1, 63, 64, 65, 257, and 1024 elements).

## Next step

Only after the build and regression test pass should we profile candidate Geometry Dash calculations and decide whether any independent workload is large enough to benefit from GPU offload. No game logic is moved to the GPU in this milestone.
