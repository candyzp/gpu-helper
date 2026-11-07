#include "AtlasCulling.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCTextureAtlas.hpp>
#include <Geode/modify/CCSpriteBatchNode.hpp>
#include <OpenGLES/ES2/gl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {

constexpr std::size_t kMaximumAtlasQuadCount = 524288;
constexpr std::uint64_t kMaximumResultAgeFrames = 2;
constexpr float kTransformCompareEpsilon = 0.001f;

struct BatchTransformSnapshot {
    std::array<float, 6> values {};
};

struct AtlasBatchState {
    cocos2d::CCSpriteBatchNode* batch = nullptr;
    CCNode* objectLayer = nullptr;
    std::size_t offset = 0;
    std::size_t count = 0;
    std::vector<std::uint8_t> dynamicQuads;
    BatchTransformSnapshot transform;
    bool structurallyInvalid = false;
};

std::unordered_map<cocos2d::CCTextureAtlas*, AtlasBatchState> g_atlases;
std::vector<std::uint32_t> g_latestVisibilityMask;
std::uint64_t g_generation = 0;
std::uint64_t g_currentFrame = 0;
std::uint64_t g_resultFrame = 0;
bool g_resultReady = false;
gpuhelper::atlas::FrameStats g_frameStats;
bool g_loggedGLVersion = false;

bool isFinitePoint(CCPoint const& point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

CCPoint toObjectLayerSpace(
    cocos2d::CCSpriteBatchNode* batch,
    CCNode* objectLayer,
    CCPoint const& point
) {
    if (!batch || !objectLayer) {
        return { std::numeric_limits<float>::quiet_NaN(),
                 std::numeric_limits<float>::quiet_NaN() };
    }
    return objectLayer->convertToNodeSpace(batch->convertToWorldSpace(point));
}

BatchTransformSnapshot captureBatchTransform(
    cocos2d::CCSpriteBatchNode* batch,
    CCNode* objectLayer
) {
    auto const origin = toObjectLayerSpace(batch, objectLayer, {0.0f, 0.0f});
    auto const xAxis = toObjectLayerSpace(batch, objectLayer, {1.0f, 0.0f});
    auto const yAxis = toObjectLayerSpace(batch, objectLayer, {0.0f, 1.0f});
    return {{
        origin.x, origin.y,
        xAxis.x, xAxis.y,
        yAxis.x, yAxis.y
    }};
}

bool batchTransformStillMatches(AtlasBatchState const& state) {
    auto const current = captureBatchTransform(state.batch, state.objectLayer);
    for (std::size_t i = 0; i < current.values.size(); ++i) {
        if (!std::isfinite(current.values[i]) ||
            std::abs(current.values[i] - state.transform.values[i]) >
                kTransformCompareEpsilon) {
            return false;
        }
    }
    return true;
}

void makeAlwaysVisible(gpuhelper::metal::ObjectBounds& bounds) {
    constexpr float kLarge = std::numeric_limits<float>::max() * 0.25f;
    bounds = { -kLarge, -kLarge, kLarge, kLarge };
}

bool appendQuadBounds(
    cocos2d::ccV3F_C4B_T2F_Quad const& quad,
    cocos2d::CCSpriteBatchNode* batch,
    CCNode* objectLayer,
    gpuhelper::metal::ObjectBounds& output
) {
    cocos2d::ccV3F_C4B_T2F const* vertices[] = {
        &quad.bl, &quad.br, &quad.tl, &quad.tr
    };

    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();

    for (auto const* vertex : vertices) {
        CCPoint const local = toObjectLayerSpace(
            batch,
            objectLayer,
            { vertex->vertices.x, vertex->vertices.y }
        );
        if (!isFinitePoint(local)) {
            makeAlwaysVisible(output);
            return false;
        }

        minX = std::min(minX, local.x);
        minY = std::min(minY, local.y);
        maxX = std::max(maxX, local.x);
        maxY = std::max(maxY, local.y);
    }

    if (!std::isfinite(minX) || !std::isfinite(minY) ||
        !std::isfinite(maxX) || !std::isfinite(maxY) ||
        minX > maxX || minY > maxY) {
        makeAlwaysVisible(output);
        return false;
    }

    output = { minX, minY, maxX, maxY };
    return true;
}

void logOpenGLESVersionOnce() {
    if (g_loggedGLVersion) {
        return;
    }
    g_loggedGLVersion = true;

    auto const* version = glGetString(GL_VERSION);
    auto const* renderer = glGetString(GL_RENDERER);
    if (version) {
        log::info(
            "[GPU Helper] Atlas culling draw hook active; OpenGL version='{}', renderer='{}'.",
            reinterpret_cast<char const*>(version),
            renderer ? reinterpret_cast<char const*>(renderer) : "unknown"
        );
    }
    else {
        log::warn("[GPU Helper] Atlas culling draw hook ran without a readable GL_VERSION.");
    }
}

} // namespace

namespace gpuhelper::atlas {

bool buildLevelBounds(
    PlayLayer* layer,
    std::vector<metal::ObjectBounds>& bounds,
    std::string& error
) {
    bounds.clear();
    error.clear();
    g_atlases.clear();
    g_latestVisibilityMask.clear();
    g_resultReady = false;

    if (!layer || !layer->m_batchNodes || !layer->m_objectLayer) {
        error = "PlayLayer batch nodes or object layer are unavailable.";
        return false;
    }

    auto* batchNodes = layer->m_batchNodes;
    auto const batchNodeCount = batchNodes->count();
    if (batchNodeCount == 0) {
        error = "PlayLayer contains no sprite batch nodes.";
        return false;
    }

    std::size_t totalQuads = 0;
    for (unsigned int batchIndex = 0; batchIndex < batchNodeCount; ++batchIndex) {
        auto* batch = static_cast<cocos2d::CCSpriteBatchNode*>(
            batchNodes->objectAtIndex(batchIndex)
        );
        if (!batch) {
            continue;
        }

        auto* atlas = batch->m_pobTextureAtlas;
        if (!atlas || !atlas->getQuads()) {
            continue;
        }

        auto const count = static_cast<std::size_t>(atlas->getTotalQuads());
        if (count == 0) {
            continue;
        }

        if (g_atlases.contains(atlas)) {
            // A shared atlas cannot be associated with one unambiguous node transform.
            g_atlases[atlas].structurallyInvalid = true;
            continue;
        }

        if (count > kMaximumAtlasQuadCount ||
            totalQuads > kMaximumAtlasQuadCount - count) {
            error = "Level atlas quad count exceeds the safe GPU culling limit.";
            bounds.clear();
            g_atlases.clear();
            return false;
        }

        AtlasBatchState state;
        state.batch = batch;
        state.objectLayer = layer->m_objectLayer;
        state.offset = totalQuads;
        state.count = count;
        state.dynamicQuads.assign(count, 0u);
        state.transform = captureBatchTransform(batch, layer->m_objectLayer);

        if (!std::all_of(
                state.transform.values.begin(),
                state.transform.values.end(),
                [](float value) { return std::isfinite(value); })) {
            state.structurallyInvalid = true;
        }

        auto* quads = atlas->getQuads();
        for (std::size_t quadIndex = 0; quadIndex < count; ++quadIndex) {
            metal::ObjectBounds quadBounds;
            appendQuadBounds(
                quads[quadIndex],
                batch,
                layer->m_objectLayer,
                quadBounds
            );
            bounds.push_back(quadBounds);
        }

        totalQuads += count;
        g_atlases.emplace(atlas, std::move(state));
    }

    if (bounds.empty()) {
        error = "No usable atlas quads were found in PlayLayer::m_batchNodes.";
        return false;
    }

    log::info(
        "[GPU Helper] Atlas scan: {} batch nodes, {} atlas groups, {} quads.",
        batchNodeCount,
        g_atlases.size(),
        bounds.size()
    );
    return true;
}

void clear(std::uint64_t nextGeneration) {
    g_generation = nextGeneration;
    g_atlases.clear();
    g_latestVisibilityMask.clear();
    g_resultFrame = 0;
    g_resultReady = false;
    g_frameStats = {};
}

void setCurrentFrame(std::uint64_t frame) {
    if (frame != g_currentFrame) {
        g_currentFrame = frame;
        g_frameStats = {};
    }
}

void acceptVisibilityMask(
    std::vector<std::uint32_t> const& mask,
    std::uint64_t generation,
    std::uint64_t completedFrame
) {
    std::size_t expectedCount = 0;
    for (auto const& [atlas, state] : g_atlases) {
        (void)atlas;
        expectedCount = std::max(expectedCount, state.offset + state.count);
    }

    if (generation != g_generation || mask.size() != expectedCount) {
        g_resultReady = false;
        g_latestVisibilityMask.clear();
        return;
    }

    g_latestVisibilityMask = mask;
    g_resultFrame = completedFrame;
    g_resultReady = true;
}

void invalidateResults() {
    g_resultReady = false;
    g_latestVisibilityMask.clear();
}

void markQuadUpdated(cocos2d::CCTextureAtlas* atlas, unsigned int index) {
    auto it = g_atlases.find(atlas);
    if (it == g_atlases.end()) {
        return;
    }

    auto& state = it->second;
    if (index < state.dynamicQuads.size()) {
        state.dynamicQuads[index] = 1u;
    }
}

void markStructureChanged(cocos2d::CCTextureAtlas* atlas) {
    auto it = g_atlases.find(atlas);
    if (it == g_atlases.end()) {
        return;
    }

    if (!it->second.structurallyInvalid) {
        it->second.structurallyInvalid = true;
        ++g_frameStats.invalidatedAtlases;
        log::warn(
            "[GPU Helper] Atlas structure changed after its visibility map was built; this atlas now fails open."
        );
    }
}

bool buildVisibleRuns(
    cocos2d::CCTextureAtlas* atlas,
    unsigned int count,
    unsigned int start,
    std::vector<DrawRun>& runs
) {
    runs.clear();

    auto it = g_atlases.find(atlas);
    if (it == g_atlases.end() || !g_resultReady ||
        g_currentFrame < g_resultFrame ||
        g_currentFrame - g_resultFrame > kMaximumResultAgeFrames ||
        g_generation == 0) {
        return false;
    }

    auto& state = it->second;
    if (state.structurallyInvalid ||
        !batchTransformStillMatches(state) ||
        count == 0 ||
        static_cast<std::uint64_t>(start) + count > state.count ||
        state.offset + state.count > g_latestVisibilityMask.size()) {
        if (!state.structurallyInvalid && !batchTransformStillMatches(state)) {
            state.structurallyInvalid = true;
            ++g_frameStats.invalidatedAtlases;
            log::warn(
                "[GPU Helper] A batch-node transform changed after atlas scan; its draws now fail open."
            );
        }
        return false;
    }

    bool insideRun = false;
    std::uint32_t runStart = 0;
    std::uint32_t hiddenCount = 0;

    for (std::uint32_t localIndex = start;
         localIndex < static_cast<std::uint64_t>(start) + count;
         ++localIndex) {
        std::size_t const maskIndex = state.offset + localIndex;
        bool const dynamic = state.dynamicQuads[localIndex] != 0;
        bool const visible = dynamic || g_latestVisibilityMask[maskIndex] != 0;

        if (visible) {
            if (!insideRun) {
                runStart = localIndex;
                insideRun = true;
            }
        }
        else {
            ++hiddenCount;
            if (insideRun) {
                runs.push_back({
                    runStart,
                    localIndex - runStart
                });
                insideRun = false;
            }
        }
    }

    if (insideRun) {
        runs.push_back({
            runStart,
            start + count - runStart
        });
    }

    if (hiddenCount == 0) {
        runs.clear();
        return false;
    }

    ++g_frameStats.interceptedDraws;
    g_frameStats.culledQuads += hiddenCount;
    g_frameStats.emittedRuns += runs.size();
    g_frameStats.hasAppliedCulling = true;
    return true;
}

FrameStats getFrameStats() {
    return g_frameStats;
}

} // namespace gpuhelper::atlas

class $modify(GPUHelperTextureAtlas, cocos2d::CCTextureAtlas) {
    void drawNumberOfQuads(unsigned int count, unsigned int start) {
        std::vector<gpuhelper::atlas::DrawRun> runs;
        if (!gpuhelper::atlas::buildVisibleRuns(this, count, start, runs)) {
            cocos2d::CCTextureAtlas::drawNumberOfQuads(count, start);
            return;
        }

        // Only inspect the GL context after a tracked gameplay atlas is actually
        // being culled. Loading-screen bitmap-font atlases always take the
        // untouched original draw path above.

        // Replaying contiguous atlas ranges preserves the original atlas order,
        // texture, blend state, and Cocos shader state established by the batch node.
        for (auto const& run : runs) {
            if (run.count != 0) {
                cocos2d::CCTextureAtlas::drawNumberOfQuads(run.count, run.start);
            }
        }
    }

    void updateQuad(cocos2d::ccV3F_C4B_T2F_Quad* quad, unsigned int index) {
        cocos2d::CCTextureAtlas::updateQuad(quad, index);
        gpuhelper::atlas::markQuadUpdated(this, index);
    }

    void insertQuad(cocos2d::ccV3F_C4B_T2F_Quad* quad, unsigned int index) {
        gpuhelper::atlas::markStructureChanged(this);
        cocos2d::CCTextureAtlas::insertQuad(quad, index);
    }

    void removeQuadAtIndex(unsigned int index) {
        gpuhelper::atlas::markStructureChanged(this);
        cocos2d::CCTextureAtlas::removeQuadAtIndex(index);
    }

    void removeAllQuads() {
        gpuhelper::atlas::markStructureChanged(this);
        cocos2d::CCTextureAtlas::removeAllQuads();
    }

    bool resizeCapacity(unsigned int capacity) {
        gpuhelper::atlas::markStructureChanged(this);
        return cocos2d::CCTextureAtlas::resizeCapacity(capacity);
    }

    void setQuads(cocos2d::ccV3F_C4B_T2F_Quad* quads) {
        gpuhelper::atlas::markStructureChanged(this);
        cocos2d::CCTextureAtlas::setQuads(quads);
    }
};


// No CCSpriteBatchNode or texture-initialization hooks are installed.
// Geode creates CCLabelBMFont instances during startup through these Cocos
// initialization paths; atlas culling only needs to intercept an atlas draw
// after a PlayLayer has registered that atlas.
