#include "gpu/MetalCompute.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool fail(std::string const& message) {
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

bool runBatch(std::size_t count) {
    std::vector<float> lhs(count);
    std::vector<float> rhs(count);
    std::vector<float> addend(count);
    std::vector<float> output(count, 0.0f);

    for (std::size_t i = 0; i < count; ++i) {
        int const a = static_cast<int>((i * 13) % 101) - 50;
        int const b = static_cast<int>((i * 7) % 67) - 33;
        int const c = static_cast<int>((i * 5) % 43) - 21;

        lhs[i] = static_cast<float>(a) * 0.03125f;
        rhs[i] = static_cast<float>(b) * 0.0625f;
        addend[i] = static_cast<float>(c) * 0.015625f;
    }

    std::string error;
    if (!gpuhelper::metal::multiplyAdd(
            lhs.data(), rhs.data(), addend.data(), output.data(), count, error)) {
        return fail("GPU batch of " + std::to_string(count) +
                    " elements failed: " + error);
    }

    for (std::size_t i = 0; i < count; ++i) {
        float const expected = lhs[i] * rhs[i] + addend[i];
        float const tolerance = 1.0e-5f * (1.0f + std::fabs(expected));

        if (!std::isfinite(output[i]) ||
            std::fabs(output[i] - expected) > tolerance) {
            return fail(
                "GPU result mismatch at index " + std::to_string(i) +
                " for batch " + std::to_string(count) +
                ": expected " + std::to_string(expected) +
                ", got " + std::to_string(output[i])
            );
        }
    }

    std::cout << "PASS: " << count << " elements\n";
    return true;
}

} // namespace

int main() {
    std::string deviceName;
    std::string error;

    if (!gpuhelper::metal::getDeviceName(deviceName, error)) {
        fail("Metal device unavailable: " + error);
        return 1;
    }

    std::cout << "Metal device: " << deviceName << '\n';

    // Empty batches are defined no-ops and may use null pointers.
    if (!gpuhelper::metal::multiplyAdd(
            nullptr, nullptr, nullptr, nullptr, 0, error)) {
        fail("empty batch was not treated as a no-op: " + error);
        return 1;
    }
    std::cout << "PASS: empty batch\n";

    // Invalid pointers must fail cleanly instead of being sent to Metal.
    if (gpuhelper::metal::multiplyAdd(
            nullptr, nullptr, nullptr, nullptr, 1, error)) {
        fail("non-empty batch accepted null pointers");
        return 1;
    }
    if (error.empty()) {
        fail("invalid-pointer failure did not provide an error");
        return 1;
    }
    std::cout << "PASS: invalid pointer guard\n";

    // These sizes cover both sides of the 64-thread dispatch boundary.
    constexpr std::size_t kBatchSizes[] = {1, 63, 64, 65, 257, 1024};
    for (std::size_t count : kBatchSizes) {
        if (!runBatch(count)) {
            return 1;
        }
    }

    std::cout << "PASS: Metal compute regression suite\n";
    return 0;
}
