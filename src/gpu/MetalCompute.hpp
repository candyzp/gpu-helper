#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace gpuhelper::metal {

struct ComputeStats {
    std::uint64_t completedDispatches = 0;
    std::uint64_t totalElementsProcessed = 0;
    std::uint64_t lastElementCount = 0;
    double lastGpuMilliseconds = 0.0;
    double lastSubmitAndWaitMilliseconds = 0.0;
    bool gpuTimingAvailable = false;
};

// Returns a thread-safe snapshot of successfully completed Metal workloads.
ComputeStats getStats();

// Returns the default Metal device name without creating a compute pipeline.
bool getDeviceName(std::string& name, std::string& error);

// Executes output[i] = lhs[i] * rhs[i] + addend[i] on Metal.
// There is deliberately no CPU fallback: a successful non-empty call must
// have run the Metal compute kernel.
bool multiplyAdd(
    float const* lhs,
    float const* rhs,
    float const* addend,
    float* output,
    std::size_t count,
    std::string& error
);

} // namespace gpuhelper::metal
