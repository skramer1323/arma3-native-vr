#pragma once

#include <array>
#include <cmath>

namespace a3vr {

using SinglePassDepthConstants = std::array<float, 8>;

inline bool BuildSinglePassDepthConstants(const float* left, const float* right,
                                          SinglePassDepthConstants& result)
{
    for (int i = 0; i < 8; ++i) {
        if (!std::isfinite(left[i]) || !std::isfinite(right[i])) return false;
    }
    const bool leftDepthPatched = left[7] != 0.0F;
    const bool rightDepthPatched = right[7] != 0.0F;
    if (leftDepthPatched != rightDepthPatched || left[0] < 0.01F || left[1] < 0.01F ||
        right[0] < 0.01F || right[1] < 0.01F) {
        return false;
    }
    const float scaleX = right[0] / left[0];
    const float scaleY = right[1] / left[1];
    const float deltaX = right[2] - scaleX * left[2];
    const float deltaY = right[3] - scaleY * left[3];
    // Recover camera Z as Wleft - M33left. Shared projection slopes keep clip Z affine.
    result = {scaleX, scaleY, deltaX, deltaY,
              right[4] - scaleX * left[4] - deltaX * left[7],
              right[5] - scaleY * left[5] - deltaY * left[7],
              leftDepthPatched ? right[6] - left[6] : 0.0F, right[7] - left[7]};
    return true;
}

} // namespace a3vr
