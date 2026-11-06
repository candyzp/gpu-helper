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

namespace {

constexpr char kKernelSource[] = R"metal(
#include <metal_stdlib>
using namespace metal;

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
)metal";

id<MTLDevice> g_device = nil;
id<MTLCommandQueue> g_commandQueue = nil;
id<MTLComputePipelineState> g_pipeline = nil;
std::string g_initializationError;
std::once_flag g_initializationOnce;

std::mutex g_statsMutex;
gpuhelper::metal::ComputeStats g_computeStats;

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
                return;
            }

            g_commandQueue = [g_device newCommandQueue];
            if (g_commandQueue == nil) {
                g_initializationError = "Could not create a Metal command queue.";
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
                return;
            }

            id<MTLFunction> function =
                [library newFunctionWithName:@"gpuhelper_multiply_add"];
            if (function == nil) {
                g_initializationError = "Metal library did not contain gpuhelper_multiply_add.";
                return;
            }

            g_pipeline = [g_device
                newComputePipelineStateWithFunction:function
                error:&metalError];

            if (g_pipeline == nil) {
                g_initializationError = nsStringToStdString(metalError.localizedDescription);
            }
        }
    });
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

    // An empty batch is a valid no-op and does not need a GPU dispatch.
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

} // namespace gpuhelper::metal
