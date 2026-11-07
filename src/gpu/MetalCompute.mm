#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "MetalCompute.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr char kKernelSource[] = R"metal(
#include <metal_stdlib>
using namespace metal;

struct ObjectBounds {
    float minX;
    float minY;
    float maxX;
    float maxY;
};

struct ViewRect {
    float minX;
    float minY;
    float maxX;
    float maxY;
};

kernel void gpuhelper_multiply_add(
    device const float* lhs       [[buffer(0)]],
    device const float* rhs       [[buffer(1)]],
    device const float* addend    [[buffer(2)]],
    device float* output          [[buffer(3)]],
    constant uint& elementCount   [[buffer(4)]],
    uint index                    [[thread_position_in_grid]]
) {
    if (index < elementCount) {
        output[index] = lhs[index] * rhs[index] + addend[index];
    }
}

kernel void gpuhelper_cull_decoration_bounds(
    device const ObjectBounds* bounds [[buffer(0)]],
    device uint* visible              [[buffer(1)]],
    constant ViewRect& view           [[buffer(2)]],
    constant uint& objectCount        [[buffer(3)]],
    uint index                        [[thread_position_in_grid]]
) {
    if (index >= objectCount) {
        return;
    }

    ObjectBounds const object = bounds[index];
    bool const intersects =
        object.maxX >= view.minX &&
        object.minX <= view.maxX &&
        object.maxY >= view.minY &&
        object.minY <= view.maxY;

    visible[index] = intersects ? 1u : 0u;
}
)metal";

id<MTLDevice> g_device = nil;
id<MTLCommandQueue> g_commandQueue = nil;
id<MTLComputePipelineState> g_pipeline = nil;
id<MTLComputePipelineState> g_decorationCullPipeline = nil;
std::string g_initializationError;
std::string g_decorationCullInitializationError;
std::once_flag g_initializationOnce;

std::mutex g_statsMutex;
gpuhelper::metal::ComputeStats g_computeStats;

std::mutex g_cullMutex;
id<MTLBuffer> g_cullBoundsBuffer = nil;
id<MTLBuffer> g_cullOutputBuffer = nil;
std::uint32_t g_cullObjectCount = 0;
std::uint64_t g_cullGeneration = 0;
std::uint64_t g_cullPendingGeneration = 0;
bool g_cullInFlight = false;
bool g_cullResultsReady = false;
std::vector<std::uint32_t> g_cullCompletedResults;

std::string nsStringToStdString(NSString* value) {
    if (value == nil) {
        return "Unknown Metal error";
    }

    char const* utf8 = value.UTF8String;
    return utf8 ? std::string(utf8) : std::string("Unknown Metal error");
}

void initializeBackend() {
    std::call_once(g_initializationOnce, [] {
        @autoreleasepool {
            g_device = MTLCreateSystemDefaultDevice();
            if (g_device == nil) {
                g_initializationError = "Metal is unavailable on this device.";
                g_decorationCullInitializationError = g_initializationError;
                return;
            }

            g_commandQueue = [g_device newCommandQueue];
            if (g_commandQueue == nil) {
                g_initializationError = "Could not create a Metal command queue.";
                g_decorationCullInitializationError = g_initializationError;
                return;
            }

            NSString* source = [NSString stringWithUTF8String:kKernelSource];
            NSError* metalError = nil;
            id<MTLLibrary> library = [g_device
                newLibraryWithSource:source
                options:nil
                error:&metalError];

            if (library == nil) {
                g_initializationError = nsStringToStdString(metalError.localizedDescription);
                g_decorationCullInitializationError = g_initializationError;
                return;
            }

            id<MTLFunction> function =
                [library newFunctionWithName:@"gpuhelper_multiply_add"];
            if (function == nil) {
                g_initializationError = "Metal library did not contain gpuhelper_multiply_add.";
            }
            else {
                g_pipeline = [g_device
                    newComputePipelineStateWithFunction:function
                    error:&metalError];

                if (g_pipeline == nil) {
                    g_initializationError = nsStringToStdString(metalError.localizedDescription);
                }
            }

            id<MTLFunction> cullFunction =
                [library newFunctionWithName:@"gpuhelper_cull_decoration_bounds"];
            if (cullFunction == nil) {
                g_decorationCullInitializationError =
                    "Metal library did not contain gpuhelper_cull_decoration_bounds.";
            }
            else {
                metalError = nil;
                g_decorationCullPipeline = [g_device
                    newComputePipelineStateWithFunction:cullFunction
                    error:&metalError];

                if (g_decorationCullPipeline == nil) {
                    g_decorationCullInitializationError =
                        nsStringToStdString(metalError.localizedDescription);
                }
            }
        }
    });
}

void clearCullFlightForGeneration(std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(g_cullMutex);
    if (g_cullPendingGeneration == generation) {
        g_cullInFlight = false;
    }
}

} // namespace

namespace gpuhelper::metal {

ComputeStats getStats() {
    std::lock_guard<std::mutex> lock(g_statsMutex);
    return g_computeStats;
}

bool getDeviceName(std::string& name, std::string& error) {
    name.clear();
    error.clear();

    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            error = "Metal is unavailable on this device.";
            return false;
        }

        name = nsStringToStdString(device.name);
        if (name.empty()) {
            name = "Unknown Metal device";
        }
        return true;
    }
}

bool multiplyAdd(
    float const* lhs,
    float const* rhs,
    float const* addend,
    float* output,
    std::size_t count,
    std::string& error
) {
    error.clear();

    if (count == 0) {
        return true;
    }

    if (lhs == nullptr || rhs == nullptr || addend == nullptr || output == nullptr) {
        error = "Non-empty Metal compute calls require all input and output pointers.";
        return false;
    }

    if (count > std::numeric_limits<std::uint32_t>::max() ||
        count > std::numeric_limits<std::size_t>::max() / sizeof(float) ||
        count > std::numeric_limits<NSUInteger>::max() / sizeof(float)) {
        error = "Metal compute batch is too large.";
        return false;
    }

    initializeBackend();
    if (g_pipeline == nil || g_commandQueue == nil || g_device == nil) {
        error = g_initializationError.empty()
            ? "Metal compute backend initialization failed."
            : g_initializationError;
        return false;
    }

    @autoreleasepool {
        NSUInteger const byteLength = static_cast<NSUInteger>(count * sizeof(float));

        id<MTLBuffer> lhsBuffer = [g_device
            newBufferWithBytes:lhs
            length:byteLength
            options:MTLResourceStorageModeShared];
        id<MTLBuffer> rhsBuffer = [g_device
            newBufferWithBytes:rhs
            length:byteLength
            options:MTLResourceStorageModeShared];
        id<MTLBuffer> addendBuffer = [g_device
            newBufferWithBytes:addend
            length:byteLength
            options:MTLResourceStorageModeShared];
        id<MTLBuffer> outputBuffer = [g_device
            newBufferWithLength:byteLength
            options:MTLResourceStorageModeShared];

        if (lhsBuffer == nil || rhsBuffer == nil ||
            addendBuffer == nil || outputBuffer == nil) {
            error = "Metal could not allocate one or more compute buffers.";
            return false;
        }

        id<MTLCommandBuffer> commandBuffer = [g_commandQueue commandBuffer];
        if (commandBuffer == nil) {
            error = "Metal could not create a command buffer.";
            return false;
        }

        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        if (encoder == nil) {
            error = "Metal could not create a compute command encoder.";
            return false;
        }

        [encoder setComputePipelineState:g_pipeline];
        [encoder setBuffer:lhsBuffer offset:0 atIndex:0];
        [encoder setBuffer:rhsBuffer offset:0 atIndex:1];
        [encoder setBuffer:addendBuffer offset:0 atIndex:2];
        [encoder setBuffer:outputBuffer offset:0 atIndex:3];

        std::uint32_t const elementCount = static_cast<std::uint32_t>(count);
        [encoder setBytes:&elementCount length:sizeof(elementCount) atIndex:4];

        NSUInteger const maxThreads = g_pipeline.maxTotalThreadsPerThreadgroup;
        NSUInteger const threadgroupWidth = std::min<NSUInteger>(64, maxThreads);
        if (threadgroupWidth == 0) {
            [encoder endEncoding];
            error = "Metal reported an invalid threadgroup size.";
            return false;
        }

        MTLSize const threadsPerThreadgroup = MTLSizeMake(threadgroupWidth, 1, 1);
        MTLSize const threadgroups = MTLSizeMake(
            (static_cast<NSUInteger>(count) + threadgroupWidth - 1) / threadgroupWidth,
            1,
            1
        );

        [encoder dispatchThreadgroups:threadgroups
                threadsPerThreadgroup:threadsPerThreadgroup];
        [encoder endEncoding];

        auto const submitAndWaitStart = std::chrono::steady_clock::now();
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
        auto const completedAt = std::chrono::steady_clock::now();

        if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
            error = commandBuffer.error != nil
                ? nsStringToStdString(commandBuffer.error.localizedDescription)
                : "Metal command buffer did not complete successfully.";
            return false;
        }

        void const* gpuOutput = outputBuffer.contents;
        if (gpuOutput == nullptr) {
            error = "Metal output buffer has no CPU-visible contents.";
            return false;
        }

        std::memcpy(output, gpuOutput, byteLength);

        double const submitAndWaitMilliseconds =
            std::chrono::duration<double, std::milli>(
                completedAt - submitAndWaitStart
            ).count();

        double gpuMilliseconds = 0.0;
        bool gpuTimingAvailable = false;
        CFTimeInterval const gpuStart = commandBuffer.GPUStartTime;
        CFTimeInterval const gpuEnd = commandBuffer.GPUEndTime;
        if (gpuStart > 0.0 && gpuEnd >= gpuStart) {
            gpuMilliseconds = static_cast<double>((gpuEnd - gpuStart) * 1000.0);
            gpuTimingAvailable = true;
        }

        {
            std::lock_guard<std::mutex> lock(g_statsMutex);
            ++g_computeStats.completedDispatches;
            g_computeStats.totalElementsProcessed += static_cast<std::uint64_t>(count);
            g_computeStats.lastElementCount = static_cast<std::uint64_t>(count);
            g_computeStats.lastGpuMilliseconds = gpuMilliseconds;
            g_computeStats.lastSubmitAndWaitMilliseconds = submitAndWaitMilliseconds;
            g_computeStats.gpuTimingAvailable = gpuTimingAvailable;
        }

        return true;
    }
}

bool configureDecorationCulling(
    ObjectBounds const* bounds,
    std::size_t count,
    std::uint64_t generation,
    std::string& error
) {
    error.clear();

    if (count == 0) {
        clearDecorationCulling(generation);
        return true;
    }

    if (bounds == nullptr) {
        error = "Decoration culling requires a non-null bounds array.";
        return false;
    }

    if (count > std::numeric_limits<std::uint32_t>::max() ||
        count > std::numeric_limits<std::size_t>::max() / sizeof(ObjectBounds) ||
        count > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t) ||
        count > std::numeric_limits<NSUInteger>::max() / sizeof(ObjectBounds) ||
        count > std::numeric_limits<NSUInteger>::max() / sizeof(std::uint32_t)) {
        error = "Decoration culling batch is too large.";
        return false;
    }

    initializeBackend();
    if (g_device == nil || g_commandQueue == nil || g_decorationCullPipeline == nil) {
        error = g_decorationCullInitializationError.empty()
            ? "Metal decoration culling pipeline is unavailable."
            : g_decorationCullInitializationError;
        return false;
    }

    @autoreleasepool {
        NSUInteger const boundsLength =
            static_cast<NSUInteger>(count * sizeof(ObjectBounds));
        NSUInteger const outputLength =
            static_cast<NSUInteger>(count * sizeof(std::uint32_t));

        id<MTLBuffer> boundsBuffer = [g_device
            newBufferWithBytes:bounds
            length:boundsLength
            options:MTLResourceStorageModeShared];
        id<MTLBuffer> outputBuffer = [g_device
            newBufferWithLength:outputLength
            options:MTLResourceStorageModeShared];

        if (boundsBuffer == nil || outputBuffer == nil) {
            error = "Metal could not allocate decoration culling buffers.";
            return false;
        }

        std::lock_guard<std::mutex> lock(g_cullMutex);
        g_cullBoundsBuffer = boundsBuffer;
        g_cullOutputBuffer = outputBuffer;
        g_cullObjectCount = static_cast<std::uint32_t>(count);
        g_cullGeneration = generation;
        g_cullResultsReady = false;
        g_cullCompletedResults.clear();
    }

    return true;
}

bool submitDecorationCulling(
    ViewRect const& view,
    std::uint64_t generation,
    std::string& error
) {
    error.clear();

    id<MTLBuffer> boundsBuffer = nil;
    id<MTLBuffer> outputBuffer = nil;
    std::uint32_t objectCount = 0;

    {
        std::lock_guard<std::mutex> lock(g_cullMutex);
        if (g_cullInFlight) {
            return false;
        }

        if (generation != g_cullGeneration ||
            g_cullObjectCount == 0 ||
            g_cullBoundsBuffer == nil ||
            g_cullOutputBuffer == nil) {
            error = "Decoration culling context is not configured for this level.";
            return false;
        }

        g_cullInFlight = true;
        g_cullPendingGeneration = generation;
        objectCount = g_cullObjectCount;
        boundsBuffer = g_cullBoundsBuffer;
        outputBuffer = g_cullOutputBuffer;
    }

    auto const submitStart = std::chrono::steady_clock::now();

    @autoreleasepool {
        id<MTLCommandBuffer> commandBuffer = [g_commandQueue commandBuffer];
        if (commandBuffer == nil) {
            clearCullFlightForGeneration(generation);
            error = "Metal could not create a decoration culling command buffer.";
            return false;
        }

        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        if (encoder == nil) {
            clearCullFlightForGeneration(generation);
            error = "Metal could not create a decoration culling encoder.";
            return false;
        }

        [encoder setComputePipelineState:g_decorationCullPipeline];
        [encoder setBuffer:boundsBuffer offset:0 atIndex:0];
        [encoder setBuffer:outputBuffer offset:0 atIndex:1];
        [encoder setBytes:&view length:sizeof(view) atIndex:2];
        [encoder setBytes:&objectCount length:sizeof(objectCount) atIndex:3];

        NSUInteger const maxThreads =
            g_decorationCullPipeline.maxTotalThreadsPerThreadgroup;
        NSUInteger const threadgroupWidth = std::min<NSUInteger>(64, maxThreads);
        if (threadgroupWidth == 0) {
            [encoder endEncoding];
            clearCullFlightForGeneration(generation);
            error = "Metal reported an invalid decoration culling threadgroup size.";
            return false;
        }

        MTLSize const threadsPerThreadgroup =
            MTLSizeMake(threadgroupWidth, 1, 1);
        MTLSize const threadgroups = MTLSizeMake(
            (static_cast<NSUInteger>(objectCount) + threadgroupWidth - 1) /
                threadgroupWidth,
            1,
            1
        );

        [encoder dispatchThreadgroups:threadgroups
                threadsPerThreadgroup:threadsPerThreadgroup];
        [encoder endEncoding];

        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
            bool const completed =
                completedBuffer.status == MTLCommandBufferStatusCompleted;
            std::vector<std::uint32_t> results;
            std::uint64_t visibleCount = 0;

            if (completed && outputBuffer.contents != nullptr) {
                results.resize(objectCount);
                std::memcpy(
                    results.data(),
                    outputBuffer.contents,
                    static_cast<std::size_t>(objectCount) * sizeof(std::uint32_t)
                );
                for (auto const value : results) {
                    if (value != 0) {
                        ++visibleCount;
                    }
                }
            }

            double gpuMilliseconds = 0.0;
            bool gpuTimingAvailable = false;
            CFTimeInterval const gpuStart = completedBuffer.GPUStartTime;
            CFTimeInterval const gpuEnd = completedBuffer.GPUEndTime;
            if (gpuStart > 0.0 && gpuEnd >= gpuStart) {
                gpuMilliseconds =
                    static_cast<double>((gpuEnd - gpuStart) * 1000.0);
                gpuTimingAvailable = true;
            }

            std::string completionError;
            if (!completed) {
                completionError = completedBuffer.error != nil
                    ? nsStringToStdString(completedBuffer.error.localizedDescription)
                    : "Metal command buffer did not complete successfully.";
            }
            else if (outputBuffer.contents == nullptr || results.size() != objectCount) {
                completionError = "Metal completed without a readable visibility result buffer.";
            }

            bool acceptedResult = false;
            bool failedCurrentGeneration = false;
            {
                std::lock_guard<std::mutex> lock(g_cullMutex);
                if (g_cullInFlight &&
                    g_cullPendingGeneration == generation) {
                    g_cullInFlight = false;

                    if (g_cullGeneration == generation &&
                        completionError.empty() &&
                        results.size() == objectCount) {
                        g_cullCompletedResults = std::move(results);
                        g_cullResultsReady = true;
                        acceptedResult = true;
                    }
                    else if (g_cullGeneration == generation) {
                        g_cullObjectCount = 0;
                        g_cullBoundsBuffer = nil;
                        g_cullOutputBuffer = nil;
                        g_cullResultsReady = false;
                        g_cullCompletedResults.clear();
                        failedCurrentGeneration = true;
                    }
                }
            }

            if (acceptedResult) {
                std::lock_guard<std::mutex> lock(g_statsMutex);
                ++g_computeStats.decorationCullDispatches;
                g_computeStats.totalDecorationObjectsProcessed += objectCount;
                g_computeStats.lastDecorationObjectCount = objectCount;
                g_computeStats.lastDecorationVisibleCount = visibleCount;
                g_computeStats.lastDecorationCullGpuMilliseconds = gpuMilliseconds;
                g_computeStats.decorationCullTimingAvailable = gpuTimingAvailable;
            }
            else if (failedCurrentGeneration) {
                std::lock_guard<std::mutex> lock(g_statsMutex);
                ++g_computeStats.decorationCullFailures;
                g_computeStats.lastDecorationCullError = completionError;
            }
        }];

        [commandBuffer commit];
    }

    double const submitMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - submitStart
        ).count();

    {
        std::lock_guard<std::mutex> lock(g_statsMutex);
        g_computeStats.lastDecorationCullSubmitMilliseconds = submitMilliseconds;
    }

    return true;
}

bool takeDecorationCullingResults(
    std::vector<std::uint32_t>& visible,
    std::uint64_t& generation
) {
    std::lock_guard<std::mutex> lock(g_cullMutex);
    if (!g_cullResultsReady) {
        return false;
    }

    visible = std::move(g_cullCompletedResults);
    generation = g_cullGeneration;
    g_cullResultsReady = false;
    return true;
}

void clearDecorationCulling(std::uint64_t nextGeneration) {
    std::lock_guard<std::mutex> lock(g_cullMutex);
    g_cullGeneration = nextGeneration;
    g_cullObjectCount = 0;
    g_cullBoundsBuffer = nil;
    g_cullOutputBuffer = nil;
    g_cullResultsReady = false;
    g_cullCompletedResults.clear();
}

} // namespace gpuhelper::metal
