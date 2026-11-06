#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "gpu/MetalCompute.hpp"

#include <iomanip>
#include <sstream>
#include <string>

using namespace geode::prelude;

namespace {

constexpr int kDebugOverlayTag = 0x475055;

bool g_metalAvailable = false;
std::string g_metalDeviceName = "Unknown";

class GPUHelperDebugOverlay final : public CCNode {
    CCLabelBMFont* m_label = nullptr;
    float m_refreshElapsed = 0.0f;

    void refreshText() {
        if (!m_label) {
            return;
        }

        auto const stats = gpuhelper::metal::getStats();
        std::ostringstream display;
        display << "GPU HELPER | LIVE\n";
        display << "METAL: " << (g_metalAvailable ? "READY" : "UNAVAILABLE") << '\n';
        display << "DEVICE: " << g_metalDeviceName << '\n';

        if (stats.completedDispatches == 0) {
            display << "GPU JOB: NONE\n";
            display << "DISPATCHES: 0\n";
            display << "ELEMENTS: 0\n";
            display << "NO GAME JOB CONNECTED";
        }
        else {
            display << "GPU JOB: MULTIPLY-ADD\n";
            display << "DISPATCHES: " << stats.completedDispatches << '\n';
            display << "ELEMENTS: " << stats.totalElementsProcessed << '\n';
            display << "LAST BATCH: " << stats.lastElementCount << '\n';

            if (stats.gpuTimingAvailable) {
                display << std::fixed << std::setprecision(3)
                        << "GPU: " << stats.lastGpuMilliseconds << " ms\n";
            }
            else {
                display << "GPU: timing unavailable\n";
            }

            display << std::fixed << std::setprecision(3)
                    << "SUBMIT+WAIT: "
                    << stats.lastSubmitAndWaitMilliseconds << " ms";
        }

        m_label->setString(display.str().c_str());
    }

public:
    static GPUHelperDebugOverlay* create() {
        auto* ret = new GPUHelperDebugOverlay();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }

        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool init() {
        if (!CCNode::init()) {
            return false;
        }

        auto const winSize = CCDirector::sharedDirector()->getWinSize();

        auto* background = CCLayerColor::create(ccc4(0, 0, 0, 190));
        if (!background) {
            return false;
        }

        background->setContentSize({292.0f, 142.0f});
        background->setAnchorPoint({1.0f, 1.0f});
        background->setPosition({winSize.width - 8.0f, winSize.height - 48.0f});
        this->addChild(background, 0);

        m_label = CCLabelBMFont::create("", "bigFont.fnt");
        if (!m_label) {
            return false;
        }

        m_label->setAnchorPoint({0.0f, 1.0f});
        m_label->setPosition({9.0f, 132.0f});
        m_label->setScale(0.27f);
        m_label->setOpacity(245);
        background->addChild(m_label, 1);

        this->scheduleUpdate();
        refreshText();
        return true;
    }

    void update(float dt) override {
        CCNode::update(dt);

        bool const enabled = Mod::get()->getSettingValue<bool>("debug-overlay");
        this->setVisible(enabled);
        if (!enabled) {
            return;
        }

        m_refreshElapsed += dt;
        if (m_refreshElapsed < 0.20f) {
            return;
        }

        m_refreshElapsed = 0.0f;
        refreshText();
    }
};

} // namespace

$on_mod(Loaded) {
    std::string error;

    g_metalAvailable = gpuhelper::metal::getDeviceName(g_metalDeviceName, error);
    if (g_metalAvailable) {
        log::info(
            "[GPU Helper] Metal device detected: {}. Compute is not connected to gameplay yet.",
            g_metalDeviceName
        );
    }
    else {
        g_metalDeviceName = "Unavailable";
        log::warn("[GPU Helper] Metal device unavailable: {}", error);
    }

    log::info(
        "[GPU Helper] In-game diagnostics overlay setting: {}",
        Mod::get()->getSettingValue<bool>("debug-overlay") ? "enabled" : "disabled"
    );
}

class $modify(GPUHelperPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        auto* scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene) {
            log::warn("[GPU Helper] Could not attach diagnostics overlay: no running scene.");
            return;
        }

        if (auto* existing = scene->getChildByTag(kDebugOverlayTag)) {
            existing->removeFromParentAndCleanup(true);
        }

        auto* overlay = GPUHelperDebugOverlay::create();
        if (!overlay) {
            log::warn("[GPU Helper] Failed to create diagnostics overlay.");
            return;
        }

        overlay->setTag(kDebugOverlayTag);
        scene->addChild(overlay, 1000000);
        log::info("[GPU Helper] Diagnostics overlay attached to gameplay scene.");
    }
};
