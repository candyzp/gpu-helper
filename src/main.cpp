#include <Geode/Geode.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "gpu/MetalCompute.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {

constexpr int kDebugOverlayTag = 0x475055;
constexpr int kCullingDriverTag = 0x475043;
constexpr std::size_t kMinimumDecorationBatch = 96;
constexpr std::size_t kMaximumDecorationBatch = 131072;
constexpr float kCameraSafetyMargin = 640.0f;

bool g_metalAvailable = false;
std::string g_metalDeviceName = "Unknown";

std::vector<GameObject*> g_cullObjects;
std::unordered_set<GameObject*> g_culledObjects;
std::uint64_t g_cullGeneration = 0;
PlayLayer* g_cullLayer = nullptr;
bool g_cullConfigured = false;
std::size_t g_cullCandidateCount = 0;
std::size_t g_cullCulledCount = 0;
std::string g_cullStatus = "OFF";
gpuhelper::metal::ViewRect g_previousView{};
bool g_havePreviousView = false;

bool finitePoint(CCPoint const& point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

void clearCullingState(std::string const& status, PlayLayer* rememberedLayer = nullptr) {
    ++g_cullGeneration;
    gpuhelper::metal::clearDecorationCulling(g_cullGeneration);

    g_cullObjects.clear();
    g_culledObjects.clear();
    g_cullConfigured = false;
    g_cullCandidateCount = 0;
    g_cullCulledCount = 0;
    g_cullLayer = rememberedLayer;
    g_cullStatus = status;
    g_havePreviousView = false;
}

bool buildObjectBounds(
    GameObject* object,
    CCNode* objectLayer,
    gpuhelper::metal::ObjectBounds& output
) {
    if (!object || !objectLayer) {
        return false;
    }

    auto* parent = object->getParent();
    if (!parent) {
        return false;
    }

    auto const rect = object->boundingBox();
    CCPoint const corners[] = {
        { rect.getMinX(), rect.getMinY() },
        { rect.getMinX(), rect.getMaxY() },
        { rect.getMaxX(), rect.getMinY() },
        { rect.getMaxX(), rect.getMaxY() }
    };

    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();

    for (auto const& corner : corners) {
        auto const world = parent->convertToWorldSpace(corner);
        auto const local = objectLayer->convertToNodeSpace(world);
        if (!finitePoint(local)) {
            return false;
        }

        minX = std::min(minX, local.x);
        minY = std::min(minY, local.y);
        maxX = std::max(maxX, local.x);
        maxY = std::max(maxY, local.y);
    }

    if (!std::isfinite(minX) || !std::isfinite(minY) ||
        !std::isfinite(maxX) || !std::isfinite(maxY)) {
        return false;
    }

    output = { minX, minY, maxX, maxY };
    return true;
}

void configureDecorationCulling(PlayLayer* layer) {
    clearCullingState("BUILDING");
    g_cullLayer = layer;

    if (!layer || !layer->m_objects || !layer->m_objectLayer) {
        g_cullStatus = "LEVEL DATA UNAVAILABLE";
        return;
    }

    std::vector<gpuhelper::metal::ObjectBounds> bounds;
    auto* objects = layer->m_objects;
    auto const objectCount = objects->count();
    bounds.reserve(std::min<std::size_t>(
        static_cast<std::size_t>(objectCount),
        kMaximumDecorationBatch
    ));
    g_cullObjects.reserve(bounds.capacity());

    for (unsigned int i = 0; i < objectCount; ++i) {
        auto* object = static_cast<GameObject*>(objects->objectAtIndex(i));
        if (!object ||
            object->m_objectType != GameObjectType::Decoration ||
            !object->m_isDecoration2 ||
            object->m_groupCount != 0) {
            continue;
        }

        if (g_cullObjects.size() >= kMaximumDecorationBatch) {
            break;
        }

        gpuhelper::metal::ObjectBounds objectBounds;
        if (!buildObjectBounds(object, layer->m_objectLayer, objectBounds)) {
            continue;
        }

        g_cullObjects.push_back(object);
        bounds.push_back(objectBounds);
    }

    g_cullCandidateCount = g_cullObjects.size();

    if (g_cullCandidateCount < kMinimumDecorationBatch) {
        g_cullStatus = "NOT ENOUGH STATIC DECOR";
        return;
    }

    std::string error;
    if (!gpuhelper::metal::configureDecorationCulling(
            bounds.data(),
            bounds.size(),
            g_cullGeneration,
            error)) {
        log::warn("[GPU Helper] Static decoration culling setup failed open: {}", error);
        clearCullingState("METAL SETUP FAILED", layer);
        g_cullCandidateCount = bounds.size();
        return;
    }

    g_cullConfigured = true;
    g_cullStatus = "WARMING UP";
    log::info(
        "[GPU Helper] Metal decoration culling configured for {} static objects.",
        g_cullCandidateCount
    );
}

bool currentViewRect(
    CCNode* objectLayer,
    gpuhelper::metal::ViewRect& output
) {
    if (!objectLayer) {
        return false;
    }

    auto const winSize = CCDirector::sharedDirector()->getWinSize();
    CCPoint const screenCorners[] = {
        { 0.0f, 0.0f },
        { winSize.width, 0.0f },
        { 0.0f, winSize.height },
        { winSize.width, winSize.height }
    };

    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();

    for (auto const& corner : screenCorners) {
        auto const local = objectLayer->convertToNodeSpace(corner);
        if (!finitePoint(local)) {
            return false;
        }

        minX = std::min(minX, local.x);
        minY = std::min(minY, local.y);
        maxX = std::max(maxX, local.x);
        maxY = std::max(maxY, local.y);
    }

    output = {
        minX - kCameraSafetyMargin,
        minY - kCameraSafetyMargin,
        maxX + kCameraSafetyMargin,
        maxY + kCameraSafetyMargin
    };
    return true;
}

void tickDecorationCulling(PlayLayer* layer) {
    if (!layer || !g_cullConfigured || layer != g_cullLayer) {
        return;
    }

    std::vector<std::uint32_t> completedMask;
    std::uint64_t completedGeneration = 0;
    if (gpuhelper::metal::takeDecorationCullingResults(
            completedMask,
            completedGeneration)) {
        if (completedGeneration == g_cullGeneration &&
            completedMask.size() == g_cullObjects.size()) {
            g_culledObjects.clear();
            g_cullCulledCount = 0;

            for (std::size_t i = 0; i < completedMask.size(); ++i) {
                if (completedMask[i] == 0) {
                    g_culledObjects.insert(g_cullObjects[i]);
                    ++g_cullCulledCount;
                }
            }

            g_cullStatus = "LIVE";
        }
    }

    gpuhelper::metal::ViewRect view;
    if (!currentViewRect(layer->m_objectLayer, view)) {
        g_culledObjects.clear();
        g_cullCulledCount = 0;
        g_cullStatus = "VIEW UNAVAILABLE";
        return;
    }

    if (g_havePreviousView) {
        float const oldCenterX = (g_previousView.minX + g_previousView.maxX) * 0.5f;
        float const oldCenterY = (g_previousView.minY + g_previousView.maxY) * 0.5f;
        float const newCenterX = (view.minX + view.maxX) * 0.5f;
        float const newCenterY = (view.minY + view.maxY) * 0.5f;

        // Large camera jumps or zoom changes fail open immediately. The 640-unit
        // view margin also covers ordinary one-frame asynchronous result delay.
        if (std::abs(newCenterX - oldCenterX) > 512.0f ||
            std::abs(newCenterY - oldCenterY) > 512.0f ||
            std::abs((view.maxX - view.minX) -
                     (g_previousView.maxX - g_previousView.minX)) > 1024.0f ||
            std::abs((view.maxY - view.minY) -
                     (g_previousView.maxY - g_previousView.minY)) > 1024.0f) {
            g_culledObjects.clear();
            g_cullCulledCount = 0;
        }
    }

    g_previousView = view;
    g_havePreviousView = true;

    std::string error;
    if (!gpuhelper::metal::submitDecorationCulling(
            view,
            g_cullGeneration,
            error) && !error.empty()) {
        log::warn("[GPU Helper] Metal decoration culling failed open: {}", error);
        clearCullingState("GPU DISPATCH FAILED", layer);
    }
}

bool isDecorationCulled(GameObject* object) {
    return object && g_culledObjects.find(object) != g_culledObjects.end();
}

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

        if (g_cullConfigured) {
            display << "GPU JOB: DECORATION VISIBILITY\n";
            display << "STATUS: " << g_cullStatus << '\n';
            display << "CANDIDATES: " << g_cullCandidateCount << '\n';
            display << "CULLED: " << g_cullCulledCount << '\n';
            display << "DISPATCHES: " << stats.decorationCullDispatches << '\n';

            if (stats.decorationCullTimingAvailable) {
                display << std::fixed << std::setprecision(3)
                        << "GPU: " << stats.lastDecorationCullGpuMilliseconds << " ms\n";
            }
            else {
                display << "GPU: timing pending\n";
            }

            display << std::fixed << std::setprecision(3)
                    << "CPU SUBMIT: "
                    << stats.lastDecorationCullSubmitMilliseconds << " ms";
        }
        else {
            display << "GPU JOB: NONE\n";
            display << "CULLING: " << g_cullStatus << '\n';
            display << "CANDIDATES: " << g_cullCandidateCount << '\n';
            display << "NO ACTIVE GPU WORKLOAD";
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

        background->setContentSize({310.0f, 164.0f});
        background->setAnchorPoint({1.0f, 1.0f});
        background->setPosition({winSize.width - 8.0f, winSize.height - 48.0f});
        this->addChild(background, 0);

        m_label = CCLabelBMFont::create("", "bigFont.fnt");
        if (!m_label) {
            return false;
        }

        m_label->setAnchorPoint({0.0f, 1.0f});
        m_label->setPosition({9.0f, 154.0f});
        m_label->setScale(0.245f);
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

class GPUHelperCullingDriver final : public CCNode {
public:
    static GPUHelperCullingDriver* create() {
        auto* ret = new GPUHelperCullingDriver();
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

        this->scheduleUpdate();
        return true;
    }

    void update(float dt) override {
        CCNode::update(dt);

        auto* layer = PlayLayer::get();
        if (!layer) {
            return;
        }

        bool const enabled =
            Mod::get()->getSettingValue<bool>("gpu-decoration-culling");

        if (!enabled) {
            if (g_cullLayer || g_cullConfigured ||
                g_cullStatus != "OFF") {
                clearCullingState("OFF");
            }
            return;
        }

        if (g_cullLayer != layer) {
            configureDecorationCulling(layer);
        }

        tickDecorationCulling(layer);
    }

    void onExit() override {
        clearCullingState("OFF");
        CCNode::onExit();
    }
};

} // namespace

class $modify(GPUHelperGameObject, GameObject) {
    void visit() {
        // Skip only the render traversal for GPU-culled, static decorations.
        // Object updates, triggers, collision state, and player physics continue.
        if (g_cullConfigured && isDecorationCulled(this)) {
            return;
        }

        GameObject::visit();
    }
};

$on_mod(Loaded) {
    std::string error;

    g_metalAvailable = gpuhelper::metal::getDeviceName(g_metalDeviceName, error);
    if (g_metalAvailable) {
        log::info("[GPU Helper] Metal device detected: {}.", g_metalDeviceName);
    }
    else {
        g_metalDeviceName = "Unavailable";
        log::warn("[GPU Helper] Metal device unavailable: {}", error);
    }

    log::info(
        "[GPU Helper] In-game diagnostics overlay setting: {}",
        Mod::get()->getSettingValue<bool>("debug-overlay") ? "enabled" : "disabled"
    );
    log::info(
        "[GPU Helper] GPU decoration culling setting: {}",
        Mod::get()->getSettingValue<bool>("gpu-decoration-culling") ? "enabled" : "disabled"
    );
}

class $modify(GPUHelperPlayLayer, PlayLayer) {
    void update(float dt) {
        PlayLayer::update(dt);

        // Attach after the PlayLayer is actively updating, rather than relying
        // on setupHasCompleted being invoked on every supported iOS binding.
        if (PlayLayer::get() != this) {
            return;
        }

        auto* scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene) {
            return;
        }

        if (auto* existing = scene->getChildByTag(kDebugOverlayTag)) {
            (void)existing;
        }
        else {
            auto* overlay = GPUHelperDebugOverlay::create();
            if (overlay) {
                overlay->setTag(kDebugOverlayTag);
                scene->addChild(overlay, 1000000);
                log::info("[GPU Helper] Diagnostics overlay attached from active PlayLayer::update.");
            }
            else {
                log::warn("[GPU Helper] Failed to create diagnostics overlay.");
            }
        }

        if (auto* existing = scene->getChildByTag(kCullingDriverTag)) {
            (void)existing;
        }
        else {
            auto* driver = GPUHelperCullingDriver::create();
            if (driver) {
                driver->setTag(kCullingDriverTag);
                scene->addChild(driver, 1000001);
                log::info("[GPU Helper] GPU decoration culling driver attached.");
            }
            else {
                clearCullingState("DRIVER UNAVAILABLE", this);
                log::warn("[GPU Helper] Failed to create GPU culling driver; normal drawing remains enabled.");
            }
        }

        if (g_cullLayer != this) {
            if (Mod::get()->getSettingValue<bool>("gpu-decoration-culling")) {
                configureDecorationCulling(this);
            }
            else {
                clearCullingState("OFF", this);
            }
        }
    }
};
