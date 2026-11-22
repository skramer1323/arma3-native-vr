#pragma once

#include <cmath>
#include <cstdint>

namespace a3vr {

// Stage 1 of the DLSS-for-VR plan (docs/DLSS_FOR_VR_PLAN.md): a sub-pixel jitter of the projection,
// the input a temporal upscaler needs. Pure math, no game or D3D types, covered by the unit tests.

// Halton low-discrepancy sequence value in [0, 1), index >= 1.
inline float Halton(std::uint32_t index, std::uint32_t base)
{
    float result = 0.0F;
    float fraction = 1.0F;
    while (index > 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

// Jitter of one frame in render pixels, each axis in [-0.5, 0.5): Halton(2,3) with `phases` steps
// (the usual choice for temporal upscalers is 8 to 16). x right, y down (pixel convention).
struct JitterPixels {
    float x = 0.0F;
    float y = 0.0F;
};

inline JitterPixels JitterForFrame(std::uint32_t frameIndex, std::uint32_t phases)
{
    if (phases == 0) {
        return {};
    }
    const std::uint32_t phase = frameIndex % phases + 1;
    return {Halton(phase, 2) - 0.5F, Halton(phase, 3) - 0.5F};
}

// The engine projection constants seen by the vertex shader, row vector, per draw:
//   clip.x = x*P00 + z*M20 + M30      clip.y = y*P11 + z*M21 + M31      w = z + M33
// A shift of the picture by (dx, dy) in NDC (x right, y up) needs clip += (dx, dy) * w, that is
//   M20 += dx, M30 += dx*M33   and   M21 += dy, M31 += dy*M33.
// (M33 is the forward head offset: without the second term the shift would depend on depth.)
inline void ShiftProjectionNdc(float& m20, float& m21, float& m30, float& m31, float m33,
                               float dxNdc, float dyNdc)
{
    m20 += dxNdc;
    m21 += dyNdc;
    m30 += dxNdc * m33;
    m31 += dyNdc * m33;
}

// Pixel jitter (x right, y down) to NDC (x right, y up) for a render target of the given size.
inline void JitterToNdc(JitterPixels jitter, float scale, float targetWidth, float targetHeight,
                        float& dxNdc, float& dyNdc)
{
    dxNdc = 2.0F * jitter.x * scale / targetWidth;
    dyNdc = -2.0F * jitter.y * scale / targetHeight;
}

}  // namespace a3vr
