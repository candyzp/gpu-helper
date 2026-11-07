#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "gpu/MetalCompute.hpp"
#include "gpu/AtlasCulling.hpp"
#include "Diagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {

constexpr int kDebugOverlayTag = 0x475055;
constexpr int kCullingDriverTag = 0x475043;
constexpr int kInteropProbeTag = 0x475049;
constexpr std::size_t kMinimumDecorationBatch = 96;
constexpr std::size_t kMaximumDecorationBatch = 524288;
constexpr float kCameraSafetyMargin = 640.0f;


std::uint64_t g_diagnosticSequence = 0;
auto const g_diagnosticSessionStart = std::chrono::steady_clock::now();
void diagnosticEvent(std::string const& message) {
    ++g_diagnosticSequence;
    double const elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - g_diagnosticSessionStart
    ).count();
    std::ostringstream line;
    line << std::fixed << std::setprecision(3)
         << "+" << elapsed << "s #" << g_diagnosticSequence << " " << message;
    log::info("[GPU Helper][DIAG] {}", line.str());
    gpuhelper::diagnostics::append(line.str());
}
bool g_scanComplete = false;
std::size_t g_levelObjectCount = 0;
std::size_t g_decorationTypeCount = 0;
std::size_t g_staticFlagCount = 0;
std::size_t g_ungroupedStaticCount = 0;
std::size_t g_directLayerBoundsCount = 0;
std::size_t g_transformedBoundsCount = 0;
std::size_t g_detachedFallbackBoundsCount = 0;
std::size_t g_rejectedBoundsCount = 0;
std::uint64_t g_submittedJobs = 0;
std::uint64_t g_lastLoggedFailures = 0;
std::uint64_t g_driverTicks = 0;
bool g_metalAvailable = false;
std::string g_metalDeviceName = "Unknown";
std::string g_lastGraphicsMode;

std::uint64_t g_cullGeneration = 0;
std::uint64_t g_cullPendingFrame = 0;
gpuhelper::metal::ViewRect g_cullPendingView{};
gpuhelper::metal::ViewRect g_cullResultView{};
bool g_haveCullResultView = false;
std::string g_configuredGraphicsMode;
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

std::string currentGraphicsMode() {
    auto mode = Mod::get()->getSettingValue<std::string>("graphics-mode");
    if (mode != "gpu-driven") {
        mode = "legacy";
    }
    if (mode != g_lastGraphicsMode) {
        if (!g_lastGraphicsMode.empty()) {
            diagnosticEvent("GRAPHICS_MODE_CHANGED from=" + g_lastGraphicsMode + " to=" + mode);
        }
        g_lastGraphicsMode = mode;
    }
    return mode;
}

bool gpuDrivenModeEnabled() {
    return currentGraphicsMode() == "gpu-driven";
}

bool hybridModeEnabled() {
    return false;
}

void clearCullingState(std::string const& status, PlayLayer* rememberedLayer = nullptr) {
    ++g_cullGeneration;
    gpuhelper::metal::clearDecorationCulling(g_cullGeneration);
    gpuhelper::atlas::clear(g_cullGeneration);
    g_cullPendingFrame = 0;
    g_haveCullResultView = false;
    g_configuredGraphicsMode.clear();

    g_cullConfigured = false;
    g_cullCandidateCount = 0;
    g_cullCulledCount = 0;
    g_cullLayer = rememberedLayer;
    g_cullStatus = status;
    g_havePreviousView = false;
}

enum class BoundsBuildPath {
    DirectObjectLayer,
    TransformedParent,
    DetachedFallback,
    Invalid
};

BoundsBuildPath buildObjectBounds(
    GameObject* object,
    CCNode* objectLayer,
    gpuhelper::metal::ObjectBounds& output
) {
    if (!object || !objectLayer) {
        return BoundsBuildPath::Invalid;
    }

    // boundingBox() is already axis-aligned in the node's parent coordinate
    // space. Avoid a world-space round trip when that parent is objectLayer.
    auto const rect = object->boundingBox();
    float const rectMinX = rect.getMinX();
    float const rectMinY = rect.getMinY();
    float const rectMaxX = rect.getMaxX();
    float const rectMaxY = rect.getMaxY();

    if (!std::isfinite(rectMinX) || !std::isfinite(rectMinY) ||
        !std::isfinite(rectMaxX) || !std::isfinite(rectMaxY) ||
        rectMinX > rectMaxX || rectMinY > rectMaxY) {
        return BoundsBuildPath::Invalid;
    }

    auto* parent = object->getParent();
    if (parent == objectLayer) {
        output = { rectMinX, rectMinY, rectMaxX, rectMaxY };
        return BoundsBuildPath::DirectObjectLayer;
    }

    // Some entries in PlayLayer::m_objects can lack a Cocos parent. Their
    // parent-relative bounds are treated as object-layer local coordinates
    // for diagnostics only. This fallback never changes game rendering.
    if (!parent) {
        output = { rectMinX, rectMinY, rectMaxX, rectMaxY };
        return BoundsBuildPath::DetachedFallback;
    }

    CCPoint const corners[] = {
        { rectMinX, rectMinY },
        { rectMinX, rectMaxY },
        { rectMaxX, rectMinY },
        { rectMaxX, rectMaxY }
    };

    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();

    for (auto const& corner : corners) {
        auto const world = parent->convertToWorldSpace(corner);
        auto const local = objectLayer->convertToNodeSpace(world);
        if (!finitePoint(local)) {
            return BoundsBuildPath::Invalid;
        }

        minX = std::min(minX, local.x);
        minY = std::min(minY, local.y);
        maxX = std::max(maxX, local.x);
        maxY = std::max(maxY, local.y);
    }

    if (!std::isfinite(minX) || !std::isfinite(minY) ||
        !std::isfinite(maxX) || !std::isfinite(maxY)) {
        return BoundsBuildPath::Invalid;
    }

    output = { minX, minY, maxX, maxY };
    return BoundsBuildPath::TransformedParent;
}

void configureDecorationCulling(PlayLayer* layer) {
    diagnosticEvent("LEVEL_SCAN_BEGIN");
    clearCullingState("BUILDING");
    g_cullLayer = layer;
    g_scanComplete = false;
    g_levelObjectCount = 0;
    g_decorationTypeCount = 0;
    g_staticFlagCount = 0;
    g_ungroupedStaticCount = 0;
    g_directLayerBoundsCount = 0;
    g_transformedBoundsCount = 0;
    g_detachedFallbackBoundsCount = 0;
    g_rejectedBoundsCount = 0;
    if (!layer || !layer->m_objects || !layer->m_objectLayer) {
        g_cullStatus = "LEVEL DATA UNAVAILABLE";
        g_scanComplete = true;
        diagnosticEvent("LEVEL_SCAN_FAILED missing_playlayer_objects_or_object_layer");
        return;
    }

    std::vector<gpuhelper::metal::ObjectBounds> bounds;
    auto* objects = layer->m_objects;
    auto const objectCount = objects->count();
    g_levelObjectCount = objectCount;
    g_configuredGraphicsMode = currentGraphicsMode();

    if (gpuDrivenModeEnabled()) {
        std::string atlasError;
        if (!gpuhelper::atlas::buildLevelBounds(layer, bounds, atlasError)) {
            g_scanComplete = true;
            g_cullStatus = "ATLAS SCAN FAILED";
            diagnosticEvent("ATLAS_SCAN_FAILED reason=" + atlasError);
            return;
        }

        g_cullCandidateCount = bounds.size();
        g_scanComplete = true;
        diagnosticEvent(
            "ATLAS_SCAN complete=1 level_objects=" +
            std::to_string(g_levelObjectCount) +
            " atlas_quads=" + std::to_string(g_cullCandidateCount) +
            " source=CCTextureAtlas::getQuads"
        );
    }
    else {
        bounds.reserve(std::min<std::size_t>(
            static_cast<std::size_t>(objectCount), kMaximumDecorationBatch));

        for (unsigned int i = 0; i < objectCount; ++i) {
            auto* object = static_cast<GameObject*>(objects->objectAtIndex(i));
            if (!object || object->m_objectType != GameObjectType::Decoration) continue;
            ++g_decorationTypeCount;
            if (!object->m_isDecoration2) continue;
            ++g_staticFlagCount;
            if (object->m_groupCount != 0) continue;
            ++g_ungroupedStaticCount;
            gpuhelper::metal::ObjectBounds objectBounds;
            auto const boundsPath = buildObjectBounds(object, layer->m_objectLayer, objectBounds);
            if (boundsPath == BoundsBuildPath::Invalid) {
                ++g_rejectedBoundsCount;
                continue;
            }
            if (boundsPath == BoundsBuildPath::DirectObjectLayer) ++g_directLayerBoundsCount;
            else if (boundsPath == BoundsBuildPath::TransformedParent) ++g_transformedBoundsCount;
            else if (boundsPath == BoundsBuildPath::DetachedFallback) ++g_detachedFallbackBoundsCount;
            bounds.push_back(objectBounds);
        }

        g_cullCandidateCount = bounds.size();
        g_scanComplete = true;
        std::ostringstream scan;
        scan << "LEVEL_SCAN complete=1 objects=" << g_levelObjectCount
             << " decoration_type=" << g_decorationTypeCount
             << " static_flag=" << g_staticFlagCount
             << " ungrouped_static=" << g_ungroupedStaticCount
             << " direct_layer_bounds=" << g_directLayerBoundsCount
             << " transformed_bounds=" << g_transformedBoundsCount
             << " detached_fallback_bounds=" << g_detachedFallbackBoundsCount
             << " valid_bounds=" << g_cullCandidateCount
             << " rejected_bounds=" << g_rejectedBoundsCount;
        diagnosticEvent(scan.str());
    }

    if (g_cullCandidateCount < kMinimumDecorationBatch) {
        g_cullStatus = "NOT ENOUGH STATIC DECOR";
        diagnosticEvent("GPU_SUBMIT_SKIPPED reason=batch_below_96");
        return;
    }
    std::string error;
    if (!gpuhelper::metal::configureDecorationCulling(
            bounds.data(), bounds.size(), g_cullGeneration, error)) {
        log::warn("[GPU Helper] Static decoration culling setup failed open: {}", error);
        clearCullingState("METAL SETUP FAILED", layer);
        g_cullCandidateCount = bounds.size();
        diagnosticEvent("METAL_SETUP_FAILED reason=" + error);
        return;
    }
    g_cullConfigured = true;
    g_cullStatus = "WAITING FOR GPU RESULT";
    diagnosticEvent(
        std::string("METAL_BATCH_CONFIGURED source=") +
        (gpuDrivenModeEnabled() ? "atlas_quads" : "legacy_decoration_bounds") +
        " candidates=" + std::to_string(g_cullCandidateCount)
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
    gpuhelper::atlas::setCurrentFrame(g_driverTicks);
    if (!layer || !g_cullConfigured || layer != g_cullLayer) {
        return;
    }

    std::vector<std::uint32_t> completedMask;
    std::uint64_t completedGeneration = 0;
    if (gpuhelper::metal::takeDecorationCullingResults(
            completedMask,
            completedGeneration)) {
        if (completedGeneration == g_cullGeneration &&
            completedMask.size() == g_cullCandidateCount) {
            g_cullCulledCount = 0;
            for (auto const visible : completedMask) {
                if (visible == 0) {
                    ++g_cullCulledCount;
                }
            }

            if (gpuDrivenModeEnabled()) {
                gpuhelper::atlas::acceptVisibilityMask(
                    completedMask, completedGeneration, g_cullPendingFrame
                );
                g_cullStatus = "GPU MASK READY";
                g_cullResultView = g_cullPendingView;
                g_haveCullResultView = true;
            }
            else {
                g_cullStatus = "MEASURED - RENDERING UNCHANGED";
            }
            diagnosticEvent(
                "METAL_RESULT generation=" + std::to_string(completedGeneration) +
                " visible=" + std::to_string(g_cullCandidateCount - g_cullCulledCount) +
                " outside_view=" + std::to_string(g_cullCulledCount) +
                " consumer=" + (gpuDrivenModeEnabled() ? "atlas_draw_runs" : "diagnostics_only")
            );
        }
        else {
            diagnosticEvent("METAL_RESULT_REJECTED generation=" +
                std::to_string(completedGeneration) +
                " expected=" + std::to_string(g_cullGeneration) +
                " result_count=" + std::to_string(completedMask.size()));
        }
    }

    gpuhelper::metal::ViewRect view;
    if (!currentViewRect(layer->m_objectLayer, view)) {
        g_cullCulledCount = 0;
        g_cullStatus = "VIEW UNAVAILABLE";
        gpuhelper::atlas::invalidateResults();
        g_haveCullResultView = false;
        return;
    }

    if (gpuDrivenModeEnabled() && g_haveCullResultView) {
        // The GPU tested the previous frame's viewport expanded by the safety
        // margin. Reuse the mask only while the current real viewport remains
        // fully inside the exact region that was tested.
        gpuhelper::metal::ViewRect const currentViewport {
            view.minX + kCameraSafetyMargin,
            view.minY + kCameraSafetyMargin,
            view.maxX - kCameraSafetyMargin,
            view.maxY - kCameraSafetyMargin
        };
        bool const contained =
            currentViewport.minX >= g_cullResultView.minX &&
            currentViewport.minY >= g_cullResultView.minY &&
            currentViewport.maxX <= g_cullResultView.maxX &&
            currentViewport.maxY <= g_cullResultView.maxY;
        if (!contained) {
            gpuhelper::atlas::invalidateResults();
            g_haveCullResultView = false;
        }
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
            g_cullCulledCount = 0;
            gpuhelper::atlas::invalidateResults();
            g_haveCullResultView = false;
        }
    }

    g_previousView = view;
    g_havePreviousView = true;

    std::string error;
    if (gpuhelper::metal::submitDecorationCulling(view, g_cullGeneration, error)) {
        ++g_submittedJobs;
        g_cullPendingFrame = g_driverTicks;
        g_cullPendingView = view;
    }
    else if (!error.empty()) {
        log::warn("[GPU Helper] Metal decoration culling failed open: {}", error);
        diagnosticEvent("METAL_DISPATCH_FAILED reason=" + error);
        clearCullingState("GPU DISPATCH FAILED", layer);
    }
    auto const stats = gpuhelper::metal::getStats();
    if (stats.decorationCullFailures > g_lastLoggedFailures) {
        g_lastLoggedFailures = stats.decorationCullFailures;
        diagnosticEvent("METAL_ASYNC_FAILURE count=" +
            std::to_string(stats.decorationCullFailures) +
            " last_error=" + stats.lastDecorationCullError);
    }
}


class GPUHelperDebugOverlay final : public CCNode {
    CCLabelBMFont* m_label = nullptr;
    CCLabelBMFont* m_shadowLabel = nullptr;
    float m_refreshElapsed = 0.0f;
    std::uint64_t m_refreshCount = 0;
    void refreshText() {
        if (!m_label || !m_shadowLabel) return;
        auto const stats = gpuhelper::metal::getStats();
        bool const enabled = Mod::get()->getSettingValue<bool>("gpu-decoration-culling");
        std::ostringstream display;
        display << "GPU HELPER v0.5.2 | ATLAS CULLING\n";
        display << "HUD UPDATE: " << m_refreshCount << " | DRIVER TICKS: " << g_driverTicks << '\n';
        display << "DEVICE: " << g_metalDeviceName << '\n';
        display << "METAL: " << (g_metalAvailable ? "AVAILABLE" : "UNAVAILABLE")
                << " | SETTING: " << (enabled ? "ON" : "OFF") << '\n';
        display << "LEVEL SCAN: " << (g_scanComplete ? "COMPLETE" : "WAITING")
                << " | OBJECTS: " << g_levelObjectCount << '\n';
        display << "DECOR TYPE: " << g_decorationTypeCount
                << " | STATIC FLAG: " << g_staticFlagCount << '\n';
        display << "UNGROUPED: " << g_ungroupedStaticCount << '\n';
        display << "BOUNDS: DIRECT " << g_directLayerBoundsCount
                << " | TRANSFORMED " << g_transformedBoundsCount << '\n';
        display << "DETACHED FALLBACK: " << g_detachedFallbackBoundsCount
                << " | INVALID: " << g_rejectedBoundsCount << '\n';
        display << "ELIGIBLE: " << g_cullCandidateCount
                << " / " << kMinimumDecorationBatch << " MIN\n";
        display << "GPU STATE: " << (enabled ? g_cullStatus : "DISABLED IN SETTINGS") << '\n';
        display << "GPU JOBS: " << stats.decorationCullDispatches
                << " COMPLETE / " << g_submittedJobs << " SUBMITTED\n";
        display << "GPU FAILURES: " << stats.decorationCullFailures << '\n';
        if (!stats.lastDecorationCullError.empty()) {
            display << "LAST ERROR: "
                    << stats.lastDecorationCullError.substr(0, 64) << '\n';
        }
        display << "PROCESSED: " << stats.totalDecorationObjectsProcessed << " TOTAL OBJECTS\n";
        display << "VISIBILITY: " << (g_cullCandidateCount >= g_cullCulledCount
                    ? g_cullCandidateCount - g_cullCulledCount : 0)
                << " VISIBLE | " << g_cullCulledCount << " WOULD CULL\n";
        if (stats.decorationCullTimingAvailable) {
            display << std::fixed << std::setprecision(3)
                    << "TIMING: " << stats.lastDecorationCullGpuMilliseconds
                    << "ms GPU | " << stats.lastDecorationCullSubmitMilliseconds << "ms SUBMIT\n";
        } else display << "TIMING: WAITING FOR COMPLETED GPU JOB\n";
        auto const graphicsMode = currentGraphicsMode();
        auto const atlasStats = gpuhelper::atlas::getFrameStats();
        display << "GRAPHICS PATH: "
                << (graphicsMode == "gpu-driven"
                    ? "GPU-DRIVEN ATLAS CULLING"
                    : "LEGACY METAL MEASUREMENT") << '\n';
        if (graphicsMode == "gpu-driven") {
            display << "ATLAS DRAW HOOK: "
                    << (atlasStats.hasAppliedCulling ? "ACTIVE" : "WAITING FOR VALID GPU MASK") << '\n';
            display << "ACTUALLY SKIPPED: " << atlasStats.culledQuads
                    << " QUADS | DRAW RUNS: " << atlasStats.emittedRuns << '\n';
            display << "ATLAS SAFETY: DYNAMIC VISIBLE | STRUCTURE CHANGES FAIL OPEN\n";
            display << "METAL MASK -> CPU RUN LIST -> OPENGL ES DRAWS\n";
        }
        else {
            display << "RENDERING: UNCHANGED (LEGACY MEASUREMENT ONLY)\n";
        }
        display << "RENDER HOOK: "
                << (graphicsMode == "gpu-driven"
                    ? "CCTextureAtlas::drawNumberOfQuads"
                    : "DISABLED") << '\n';
        display << "CRASH TRACE: PERSISTENT LOG ENABLED\n";
        display << "LOG EVENTS: " << g_diagnosticSequence;
        auto const text = display.str();
        m_shadowLabel->setString(text.c_str());
        m_label->setString(text.c_str());
    }
public:
    static GPUHelperDebugOverlay* create() {
        auto* ret = new GPUHelperDebugOverlay();
        if (ret && ret->init()) { ret->autorelease(); return ret; }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
    bool init() {
        if (!CCNode::init()) return false;
        auto const winSize = CCDirector::sharedDirector()->getWinSize();
        m_shadowLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_label = CCLabelBMFont::create("", "bigFont.fnt");
        if (!m_shadowLabel || !m_label) return false;
        constexpr float kOverlayScale = 0.20f;
        m_shadowLabel->setAnchorPoint({0.0f, 1.0f});
        m_shadowLabel->setPosition({26.0f, winSize.height - 26.0f});
        m_shadowLabel->setScale(kOverlayScale);
        m_shadowLabel->setColor(ccc3(0, 0, 0));
        m_shadowLabel->setOpacity(230);
        this->addChild(m_shadowLabel, 0);
        m_label->setAnchorPoint({0.0f, 1.0f});
        m_label->setPosition({24.0f, winSize.height - 24.0f});
        m_label->setScale(kOverlayScale);
        m_label->setColor(ccc3(255, 255, 255));
        m_label->setOpacity(255);
        this->addChild(m_label, 1);
        this->scheduleUpdate();
        refreshText();
        diagnosticEvent("HUD_CREATED scale=0.20 refresh_interval=0.25s");
        return true;
    }
    void update(float dt) override {
        CCNode::update(dt);
        bool const enabled = Mod::get()->getSettingValue<bool>("debug-overlay");
        this->setVisible(enabled);
        if (!enabled) return;
        m_refreshElapsed += dt;
        if (m_refreshElapsed < 0.25f) return;
        m_refreshElapsed = 0.0f;
        ++m_refreshCount;
        refreshText();
        if ((m_refreshCount % 8u) == 0u) {
            diagnosticEvent("HUD_HEARTBEAT updates=" + std::to_string(m_refreshCount) +
                " driver_ticks=" + std::to_string(g_driverTicks));
        }
    }
};

class GPUHelperInteropProbe final : public CCNode {
    bool m_loggedResult = false;
public:
    static GPUHelperInteropProbe* create() {
        auto* ret = new GPUHelperInteropProbe();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    void draw() override {
        CCNode::draw();
        if (!hybridModeEnabled() || m_loggedResult) {
            return;
        }

        std::string error;
        bool const ready = gpuhelper::interop::probeCurrentOpenGLContext(error);
        auto const status = gpuhelper::interop::getProbeStatus();
        if (ready) {
            diagnosticEvent(
                "METAL_OPENGL_INTEROP_READY gl_texture=" +
                std::to_string(status.glTextureName) +
                " backing=shared_CVPixelBuffer metal=BGRA8Unorm"
            );
            m_loggedResult = true;
        }
        else if (status.attempted) {
            diagnosticEvent("METAL_OPENGL_INTEROP_FAILED reason=" + status.error);
            m_loggedResult = true;
        }
        // If no EAGLContext is current, retry on the next Cocos draw.
    }
};

class GPUHelperCullingDriver final : public CCNode {
    float m_heartbeatElapsed = 0.0f;
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
        ++g_driverTicks;
        gpuhelper::atlas::setCurrentFrame(g_driverTicks);

        auto* layer = PlayLayer::get();
        bool const enabled =
            Mod::get()->getSettingValue<bool>("gpu-decoration-culling");

        if (layer && !enabled) {
            if (g_cullLayer || g_cullConfigured || g_cullStatus != "OFF") {
                clearCullingState("OFF");
                diagnosticEvent("CULLING_SETTING disabled");
            }
        }
        else if (layer && enabled) {
            std::string const graphicsMode = currentGraphicsMode();
            bool const needsConfiguration =
                g_cullLayer != layer ||
                g_configuredGraphicsMode != graphicsMode ||
                (!g_cullConfigured && g_cullStatus == "OFF");
            if (needsConfiguration) {
                configureDecorationCulling(layer);
            }
            tickDecorationCulling(layer);
        }

        m_heartbeatElapsed += dt;
        if (m_heartbeatElapsed >= 2.0f) {
            m_heartbeatElapsed = 0.0f;
            auto const stats = gpuhelper::metal::getStats();
            std::ostringstream event;
            event << "DRIVER_HEARTBEAT ticks=" << g_driverTicks
                  << " playlayer=" << (layer ? "present" : "absent")
                  << " setting=" << (enabled ? "on" : "off")
                  << " configured=" << (g_cullConfigured ? "yes" : "no")
                  << " status=" << g_cullStatus
                  << " eligible=" << g_cullCandidateCount
                  << " submitted=" << g_submittedJobs
                  << " completed=" << stats.decorationCullDispatches
                  << " failures=" << stats.decorationCullFailures
                  << " last_batch=" << stats.lastDecorationObjectCount
                  << " last_gpu_ms=" << stats.lastDecorationCullGpuMilliseconds
                  << " last_submit_ms=" << stats.lastDecorationCullSubmitMilliseconds;
            if (!stats.lastDecorationCullError.empty()) {
                event << " last_error=" << stats.lastDecorationCullError;
            }
            diagnosticEvent(event.str());
        }
    }
};

} // namespace

$on_mod(Loaded) {
    diagnosticEvent("SESSION_START version=v0.5.2 mode=legacy_or_gpu_driven atlas_hook=drawNumberOfQuads startup_texture_hooks=none gl_probe=active_culling_only");
    std::string error;

    g_metalAvailable = gpuhelper::metal::getDeviceName(g_metalDeviceName, error);
    if (g_metalAvailable) {
        log::info("[GPU Helper] Metal device detected: {}.", g_metalDeviceName);
        diagnosticEvent("METAL_DEVICE_AVAILABLE name=" + g_metalDeviceName);
    }
    else {
        g_metalDeviceName = "Unavailable";
        log::warn("[GPU Helper] Metal device unavailable: {}", error);
        diagnosticEvent("METAL_DEVICE_UNAVAILABLE reason=" + error);
    }

    log::info(
        "[GPU Helper] In-game diagnostics overlay setting: {}",
        Mod::get()->getSettingValue<bool>("debug-overlay") ? "enabled" : "disabled"
    );
    log::info(
        "[GPU Helper] GPU decoration culling setting: {}",
        Mod::get()->getSettingValue<bool>("gpu-decoration-culling") ? "enabled" : "disabled"
    );
    diagnosticEvent(
        std::string("SETTINGS overlay=") +
        (Mod::get()->getSettingValue<bool>("debug-overlay") ? "on" : "off") +
        " visibility_measurement=" +
        (Mod::get()->getSettingValue<bool>("gpu-decoration-culling") ? "on" : "off") +
        " graphics_path=" + currentGraphicsMode()
    );
}

class $modify(GPUHelperPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        diagnosticEvent("PLAYLAYER_SETUP objects=" +
            std::to_string(this->m_objects ? this->m_objects->count() : 0));

        // The HUD is Geometry Dash's screen-space layer. Parenting the panel
        // here keeps it above the camera/world layers and on screen.
        auto* uiLayer = this->m_uiLayer;
        if (!uiLayer) {
            log::warn("[GPU Helper] Gameplay HUD layer unavailable; overlay not attached.");
        }
        else if (!uiLayer->getChildByTag(kDebugOverlayTag)) {
            auto* overlay = GPUHelperDebugOverlay::create();
            if (overlay) {
                overlay->setTag(kDebugOverlayTag);
                uiLayer->addChild(overlay, 1000000);
                log::info("[GPU Helper] Diagnostics overlay attached to PlayLayer HUD.");
            }
            else {
                log::warn("[GPU Helper] Failed to create diagnostics overlay.");
            }
        }


        if (uiLayer && !uiLayer->getChildByTag(kInteropProbeTag)) {
            auto* probe = GPUHelperInteropProbe::create();
            if (probe) {
                probe->setTag(kInteropProbeTag);
                uiLayer->addChild(probe, 1000002);
                diagnosticEvent("INTEROP_PROBE_NODE_ATTACHED parent=PlayLayer.m_uiLayer");
            }
        }

        if (uiLayer && !uiLayer->getChildByTag(kCullingDriverTag)) {
            auto* driver = GPUHelperCullingDriver::create();
            if (driver) {
                driver->setTag(kCullingDriverTag);
                uiLayer->addChild(driver, 1000001);
                diagnosticEvent("DRIVER_ATTACHED parent=PlayLayer.m_uiLayer");
            }
            else {
                clearCullingState("DRIVER UNAVAILABLE", this);
                diagnosticEvent("DRIVER_CREATE_FAILED");
            }
        }
        if (Mod::get()->getSettingValue<bool>("gpu-decoration-culling")) {
            configureDecorationCulling(this);
        }
        else {
            clearCullingState("OFF", this);
            diagnosticEvent("CULLING_SETTING disabled_at_level_setup");
        }

    }
    void onExit() {
        diagnosticEvent("PLAYLAYER_EXIT begin");
        clearCullingState("LEVEL EXIT");
        PlayLayer::onExit();
        diagnosticEvent("PLAYLAYER_EXIT complete");
    }

};
