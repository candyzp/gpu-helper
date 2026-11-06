# Metal compute regression

This test compiles the exact Objective-C++ Metal bridge linked into the iOS mod and executes its element-wise multiply-add kernel on the macOS runner.

It checks:

- A zero-element no-op.
- Clean rejection of invalid pointers for non-empty batches.
- GPU output correctness for 1, 63, 64, 65, 257, and 1024 elements.
- Correct handling of dispatch sizes below, at, and above the 64-thread group boundary.

The test intentionally fails if the runner has no Metal device or if the GPU command buffer fails. It does not silently switch to a CPU implementation.
