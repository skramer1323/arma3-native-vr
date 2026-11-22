#pragma once

// Pure helpers without OpenXR or D3D objects, so tests can run them.

#include <dxgiformat.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace a3vr {

inline float ClampQuadWidth(float meters)
{
    return std::isfinite(meters) ? std::clamp(meters, 0.2F, 20.0F) : 2.4F;
}

inline float ClampQuadDistance(float meters)
{
    return std::isfinite(meters) ? std::clamp(meters, 0.3F, 20.0F) : 2.0F;
}

// Quad height keeps the back buffer's aspect ratio.
inline float QuadHeightMeters(float widthMeters, std::uint32_t pixelWidth,
                              std::uint32_t pixelHeight)
{
    if (pixelWidth == 0 || pixelHeight == 0) {
        return widthMeters * 9.0F / 16.0F;
    }
    return widthMeters * static_cast<float>(pixelHeight) / static_cast<float>(pixelWidth);
}

struct FrustumQuad {
    float centerX;
    float centerY;
    float width;
    float height;
};

// Intersect the four frustum edge rays with a plane at the chosen distance.
inline FrustumQuad FrustumQuadAtDistance(float left, float right, float up, float down,
                                          float distance)
{
    const float x0 = std::tan(left) * distance;
    const float x1 = std::tan(right) * distance;
    const float y0 = std::tan(down) * distance;
    const float y1 = std::tan(up) * distance;
    return {(x0 + x1) * 0.5F, (y0 + y1) * 0.5F, x1 - x0, y1 - y0};
}

// The world-up row of the view matrix contains the vertical components of
// camera right and up. Fade the correction near vertical, where those
// components no longer define a reliable horizon.
inline float CameraRollFromView(float rightY, float upY)
{
    if (!std::isfinite(rightY) || !std::isfinite(upY)) {
        return 0.0F;
    }
    const float horizontal = std::hypot(rightY, upY);
    if (horizontal <= 0.04F) {
        return 0.0F;
    }
    const float t = std::clamp((horizontal - 0.04F) / 0.16F, 0.0F, 1.0F);
    return std::atan2(rightY, upY) * t * t * (3.0F - 2.0F * t);
}

// Choose a 16:9 game frame with room for two full eye images at the requested density.
inline std::pair<unsigned, unsigned> StereoFrameSize(unsigned eyeWidth, unsigned eyeHeight,
                                                      unsigned percent, unsigned maxWidth,
                                                      unsigned maxHeight)
{
    if (eyeWidth == 0 || eyeHeight == 0) {
        return {0, 0};
    }
    const unsigned units = std::max((eyeWidth + 7) / 8, (eyeHeight + 8) / 9);
    const unsigned scaled = (units * percent + 50) / 100;
    const unsigned fitted = std::min({scaled, maxWidth / 16, maxHeight / 9});
    return {16 * fitted, 9 * fitted};
}

// Position the center of a cursor bitmap so its hotspot coincides with the clicked pixel.
inline float CursorBitmapCenter(int pixel, int hotspot, int bitmapExtent, int inputExtent)
{
    return (static_cast<float>(pixel - hotspot) + 0.5F + 0.5F * bitmapExtent) / inputExtent;
}

// clip.z = scale * clip.w + bias, where clip.w includes the per-eye depth offset.
inline float FirstPersonDepthBias(float scale, float bias, float eyeOffset, float clearance)
{
    return std::min(bias, -scale * clearance) + scale * eyeOffset;
}

inline float HorizontalDegrees(float widthMeters, float distanceMeters)
{
    return 2.0F * std::atan(widthMeters * 0.5F / distanceMeters) * 57.2957795F;
}

// One tuning step: 5 % (Shift: 25 %), clamped.
inline float StepQuadWidth(float widthMeters, bool bigger, bool coarse)
{
    const float factor = coarse ? 1.25F : 1.05F;
    return ClampQuadWidth(bigger ? widthMeters * factor : widthMeters / factor);
}

// Moving the quad keeps its angular size (width scales with distance).
inline void StepQuadDistance(float& widthMeters, float& distanceMeters, bool closer,
                             bool coarse)
{
    const float factor = coarse ? 1.25F : 1.05F;
    const float next = ClampQuadDistance(closer ? distanceMeters / factor
                                                : distanceMeters * factor);
    widthMeters = ClampQuadWidth(widthMeters * next / distanceMeters);
    distanceMeters = next;
}

inline DXGI_FORMAT SrgbVariant(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    default:
        return format;
    }
}

// The swapchain must be in the back buffer's typeless group, because the
// back buffer is copied with CopySubresourceRegion (no shader). sRGB first:
// an UNORM swapchain makes the image look washed out.
inline DXGI_FORMAT SelectQuadFormat(DXGI_FORMAT backBufferFormat,
                                    const std::vector<std::int64_t>& supported, bool useSrgb)
{
    const auto has = [&supported](DXGI_FORMAT format) {
        return std::find(supported.begin(), supported.end(),
                         static_cast<std::int64_t>(format)) != supported.end();
    };
    const DXGI_FORMAT srgb = SrgbVariant(backBufferFormat);
    if (useSrgb && srgb != backBufferFormat && has(srgb)) {
        return srgb;
    }
    if (has(backBufferFormat)) {
        return backBufferFormat;
    }
    if (!useSrgb && srgb != backBufferFormat && has(srgb)) {
        return srgb;
    }
    return DXGI_FORMAT_UNKNOWN;
}

}  // namespace a3vr
