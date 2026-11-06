#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "gpu/MetalCompute.hpp"

#include <iomanip>
#include <sstream>
#include <string>

using namespace geode::prelude;

namespace {

bool g_metalAvailable = false;

} // namespace

$on_mod(Loaded) {
    std::string deviceName;
    std::string error;

    g_metalAvailable = gpuhelper::metal::getDeviceName(deviceName, error);
    if (g_metalAvailable) {
        log::info(
            "[GPU Helper] Metal device detected: {}. Compute is not connected to gameplay yet.",
            deviceName
        );
    }
    else {
        log::warn("[GPU Helper] Metal device unavailable: {}", error);
    }
}

class $modify(GPUHelperPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* debugOverlay = nullptr;
        float refreshElapsed = 0.0f;
    };

    void update(float dt) {
        PlayLayer::update(dt);

        if (!Mod::get()->getSettingValue<bool>("debug-overlay")) {
            if (m_fields->debugOverlay) {
                m_fields->debugOverlay->setVisible(false);
            }
            return;
        }

        if (!m_fields->debugOverlay) {
            auto const winSize = CCDirector::sharedDirector()->getWinSize();
            m_fields->debugOverlay = CCLabelBMFont::create("", "bigFont.fnt");
            if (!m_fields->debugOverlay) {
                return;
            }

            m_fields->debugOverlay->setAnchorPoint({1.0f, 1.0f});
            m_fields->debugOverlay->setPosition({
                winSize.width - 10.0f,
                winSize.height - 64.0f
            });
            m_fields->debugOverlay->setScale(0.25f);
            m_fields->debugOverlay->setOpacity(220);
            this->addChild(m_fields->debugOverlay, 10000);
            m_fields->refreshElapsed = 0.25f;
        }

        m_fields->debugOverlay->setVisible(true);
        m_fields->refreshElapsed += dt;
        if (m_fields->refreshElapsed < 0.25f) {
            return;
        }
        m_fields->refreshElapsed = 0.0f;

        auto const stats = gpuhelper::metal::getStats();
        std::ostringstream display;
        display << "GPU HELPER DEBUG\n";
        display << "Metal: " << (g_metalAvailable ? "READY" : "UNAVAILABLE") << '\n';

        if (stats.completedDispatches == 0) {
            display << "Workload: NONE\n";
            display << "Dispatches: 0\n";
            display << "Elements: 0\n";
            display << "No gameplay job connected";
        }
        else {
            display << "Last job: MULTIPLY-ADD\n";
            display << "Dispatches: " << stats.completedDispatches << '\n';
            display << "Elements: " << stats.totalElementsProcessed << '\n';
            display << "Last batch: " << stats.lastElementCount << '\n';

            if (stats.gpuTimingAvailable) {
                display << std::fixed << std::setprecision(3)
                        << "GPU: " << stats.lastGpuMilliseconds << " ms\n";
            }
            else {
                display << "GPU: timing unavailable\n";
            }

            display << std::fixed << std::setprecision(3)
                    << "Submit + wait: "
                    << stats.lastSubmitAndWaitMilliseconds << " ms";
        }

        m_fields->debugOverlay->setString(display.str().c_str());
    }
};
