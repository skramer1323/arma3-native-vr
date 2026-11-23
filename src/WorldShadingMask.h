#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace a3vr {

struct WorldShadingProjection {
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
};

enum class WorldTileRate : std::uint8_t { Full, TwoByOne, OneByTwo, TwoByTwo };

inline constexpr unsigned kWorldShadingTile = 16;
inline constexpr float kFullRateTangent = 0.7002F;  // tan(35 degrees)
inline constexpr float kOuterRateTangent = 1.1918F; // tan(50 degrees)

inline bool ValidWorldShadingProjection(const WorldShadingProjection& projection)
{
    return std::isfinite(projection.scaleX) && projection.scaleX > 0.0F &&
           std::isfinite(projection.scaleY) && projection.scaleY > 0.0F &&
           std::isfinite(projection.offsetX) && std::isfinite(projection.offsetY);
}

inline WorldTileRate WorldShadingTileRate(unsigned tileX, unsigned tileY, unsigned width,
                                         unsigned height, const WorldShadingProjection (&eyes)[2])
{
    const float eyeWidth = width * 0.5F;
    const float left = static_cast<float>(tileX * kWorldShadingTile);
    const float right = std::min(left + kWorldShadingTile, static_cast<float>(width));
    const float top = static_cast<float>(tileY * kWorldShadingTile);
    const float bottom = std::min(top + kWorldShadingTile, static_cast<float>(height));
    const unsigned eye = left >= eyeWidth ? 1U : 0U;
    if (left >= right || top >= bottom || (eye == 0 && right > eyeWidth)) {
        return WorldTileRate::Full;
    }

    const auto& projection = eyes[eye];
    if (!ValidWorldShadingProjection(projection)) {
        return WorldTileRate::Full;
    }
    const float centerX = (eye + 0.5F * (projection.offsetX + 1.0F)) * eyeWidth;
    const float centerY = 0.5F * (1.0F - projection.offsetY) * height;
    // Evaluate the tile's nearest point to the optical axis. A tile that
    // intersects the protected cone keeps the finer rate throughout.
    const float x = std::fabs(centerX - std::clamp(centerX, left, right)) *
                    (2.0F / (eyeWidth * projection.scaleX));
    const float y = std::fabs(centerY - std::clamp(centerY, top, bottom)) *
                    (2.0F / (height * projection.scaleY));
    const float radiusSquared = x * x + y * y;
    if (radiusSquared <= kFullRateTangent * kFullRateTangent) {
        return WorldTileRate::Full;
    }
    if (radiusSquared > kOuterRateTangent * kOuterRateTangent) {
        return WorldTileRate::TwoByTwo;
    }
    return x > y ? WorldTileRate::TwoByOne : WorldTileRate::OneByTwo;
}

}  // namespace a3vr
