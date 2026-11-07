#pragma once

#include "MetalCompute.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cocos2d {
class CCTextureAtlas;
}

class PlayLayer;

namespace gpuhelper::atlas {

struct DrawRun {
    std::uint32_t start = 0;
    std::uint32_t count = 0;
};

// Builds one flattened, parent-layer-space bounds array from the actual
// CCTextureAtlas quads owned by PlayLayer::m_batchNodes.
bool buildLevelBounds(
    PlayLayer* layer,
    std::vector<metal::ObjectBounds>& bounds,
    std::string& error
);

// The atlas registry and mask are tied to the same level generation as Metal.
void clear(std::uint64_t nextGeneration);
void setCurrentFrame(std::uint64_t frame);
void acceptVisibilityMask(
    std::vector<std::uint32_t> const& mask,
    std::uint64_t generation,
    std::uint64_t completedFrame
);
void invalidateResults();

// Sprite updates make that quad permanently fail-open for this level.
// Atlas structural changes invalidate the affected atlas's entire cached range.
void markQuadUpdated(cocos2d::CCTextureAtlas* atlas, unsigned int index);
void markStructureChanged(cocos2d::CCTextureAtlas* atlas);

// Returns true only when a valid GPU mask was consumed and at least one quad
// can be skipped. An empty run list means every quad in this draw was culled.
// False means the caller must execute the original full draw.
bool buildVisibleRuns(
    cocos2d::CCTextureAtlas* atlas,
    unsigned int count,
    unsigned int start,
    std::vector<DrawRun>& runs
);

struct FrameStats {
    std::uint64_t interceptedDraws = 0;
    std::uint64_t culledQuads = 0;
    std::uint64_t emittedRuns = 0;
    std::uint64_t invalidatedAtlases = 0;
    bool hasAppliedCulling = false;
};

FrameStats getFrameStats();

} // namespace gpuhelper::atlas
