#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace a3vr {

struct WorldShadingProjection {
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
};

enum class WorldTileRate : std::uint8_t { Full, TwoByOne, OneByTwo, TwoByTwo, FourByFour };

inline constexpr unsigned kWorldShadingTile = 16;
inline constexpr int kWorldShadingMinRadius = 20;
inline constexpr int kWorldShadingMaxRadius = 55;

inline constexpr int WorldShadingTransitionEnd(int innerDegrees)
{
    return innerDegrees + 5;
}
inline constexpr float kFullRateTangent = 0.7002F;  // tan(35 degrees)
inline constexpr float kOuterRateTangent = 1.1918F; // tan(50 degrees)

struct WorldShadingRadii {
    float inner = kFullRateTangent;
    float middle = kOuterRateTangent;
};

struct WorldShadingGuide {
    WorldShadingProjection eyes[2]{};
    WorldShadingRadii radii;
    float farTangent = 0.0F;
};

inline WorldShadingRadii WorldShadingRadiiFromDegrees(int inner, int middle)
{
    constexpr float radiansPerDegree = 0.01745329252F;
    return {inner == 35 ? kFullRateTangent : std::tan(inner * radiansPerDegree),
            middle == 50 ? kOuterRateTangent : std::tan(middle * radiansPerDegree)};
}

inline bool ValidWorldShadingProjection(const WorldShadingProjection& projection)
{
    return std::isfinite(projection.scaleX) && projection.scaleX > 0.0F &&
           std::isfinite(projection.scaleY) && projection.scaleY > 0.0F &&
           std::isfinite(projection.offsetX) && std::isfinite(projection.offsetY);
}

inline WorldTileRate WorldShadingTileRate(unsigned tileX, unsigned tileY, unsigned width,
                                         unsigned height, const WorldShadingProjection (&eyes)[2],
                                         const WorldShadingRadii& radii = {},
                                         float farTangent = 0.0F)
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
    if (radiusSquared <= radii.inner * radii.inner) {
        return WorldTileRate::Full;
    }
    if (radiusSquared > radii.middle * radii.middle) {
        return farTangent > radii.middle && radiusSquared > farTangent * farTangent
            ? WorldTileRate::FourByFour : WorldTileRate::TwoByTwo;
    }
    return x > y ? WorldTileRate::TwoByOne : WorldTileRate::OneByTwo;
}

struct WorldVisibilityPoint {
    float x, y;  // Per-eye NDC, positive Y up.
};

inline bool WorldTileOutsideVisibility(std::span<const WorldVisibilityPoint> contour,
                                       float left, float right, float bottom, float top)
{
    if (contour.size() < 3) return false;
    // Keep a margin around the visible rim for compositor pose correction.
    constexpr float margin = 0.16F;
    left -= margin;
    right += margin;
    bottom -= margin;
    top += margin;
    const float x = 0.5F * (left + right), y = 0.5F * (bottom + top);
    const float halfWidth = 0.5F * (right - left), halfHeight = 0.5F * (top - bottom);
    bool inside = false;
    auto a = contour.back();
    for (const auto b : contour) {
        const float dx = b.x - a.x, dy = b.y - a.y;
        // Test both box axes and the segment's normal.
        if (std::max(a.x, b.x) >= left && std::min(a.x, b.x) <= right &&
            std::max(a.y, b.y) >= bottom && std::min(a.y, b.y) <= top &&
            std::fabs((x - a.x) * dy - (y - a.y) * dx) <=
                halfWidth * std::fabs(dy) + halfHeight * std::fabs(dx)) {
            return false;
        }
        if ((a.y > y) != (b.y > y) &&
            x < a.x + dx * ((y - a.y) / dy)) {
            inside = !inside;
        }
        a = b;
    }
    return !inside;
}

}  // namespace a3vr
