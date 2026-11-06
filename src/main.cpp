#include <Geode/Geode.hpp>

#include "gpu/MetalCompute.hpp"

#include <string>

using namespace geode::prelude;

$on_mod(Loaded) {
    std::string deviceName;
    std::string error;

    if (gpuhelper::metal::getDeviceName(deviceName, error)) {
        log::info(
            "[GPU Helper] Metal device detected: {}. Compute is not connected to gameplay yet.",
            deviceName
        );
    }
    else {
        log::warn("[GPU Helper] Metal device unavailable: {}", error);
    }
}
