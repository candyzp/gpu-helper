#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gpuhelper::metal {

struct ComputeStats {
    std::uint64_t completedDispatches = 0;
    std::uint64_t totalElementsProcessed = 0;
    std::uint64_t lastElementCount = 0;
    double lastGpuMilliseconds = 0.0;
    double lastSubmitAndWaitMilliseconds = 0.0;
    bool gpuTimingAvailable = false;

    std::uint64_t decorationCullDispatches = 0;
    std::uint64_t totalDecorationObjectsProcessed = 0;
    std::uint64_t lastDecorationObjectCount = 0;
    std::uint64_t lastDecorationVisibleCount = 0;
    double lastDecorationCullGpuMilliseconds = 0.0;
    double lastDecorationCullSubmitMilliseconds = 0.0;
    bool decorationCullTimingAvailable = false;
    std::uint64_t decorationCullFailures = 0;
    std::string lastDecorationCullError;
};

struct ObjectBounds {
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
};

struct ViewRect {
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
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

// Configures a persistent, asynchronous Metal visibility batch. Each input
// bounds record is in the same coordinate space as the submitted view rect.
// A new generation invalidates any completed result from an older level.
bool configureDecorationCulling(
    ObjectBounds const* bounds,
    std::size_t count,
    std::uint64_t generation,
    std::string& error
);

// Enqueues one non-blocking visibility dispatch. Returns false with an empty
// error when an earlier dispatch is still in flight. There is no CPU fallback.
bool submitDecorationCulling(
    ViewRect const& view,
    std::uint64_t generation,
    std::string& error
);

// Moves the most recently completed visibility mask to the caller.
// Each result is 1 when the corresponding object intersects the view rect.
bool takeDecorationCullingResults(
    std::vector<std::uint32_t>& visible,
    std::uint64_t& generation
);

// Invalidates the configured level without waiting for an in-flight command.
// Completion from the previous generation is discarded.
void clearDecorationCulling(std::uint64_t nextGeneration);

} // namespace gpuhelper::metal


namespace gpuhelper::interop {

struct ProbeStatus {
    bool attempted = false;
    bool ready = false;
    std::uint32_t glTextureName = 0;
    std::string error;
};

// Call only while Geometry Dash's OpenGL ES context is current. This creates
// Metal and OpenGL ES texture views over the same Core Video pixel buffer.
bool probeCurrentOpenGLContext(std::string& error);
ProbeStatus getProbeStatus();

} // namespace gpuhelper::interop
