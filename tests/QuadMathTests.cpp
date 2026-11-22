#include "QuadMath.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

bool Near(float a, float b, float tolerance = 1e-3F)
{
    return std::fabs(a - b) <= tolerance;
}

}  // namespace

int main()
{
    using namespace a3vr;

    Expect(Near(QuadHeightMeters(2.4F, 2560, 1440), 1.35F), "16:9 height");
    Expect(Near(QuadHeightMeters(1.6F, 0, 0), 0.9F), "fallback height");
    const float quadDistance = 20.0F;
    for (float eye : {-1.0F, 1.0F}) {
        const float left = -1.09F + eye * 0.19F;
        const float right = 1.09F + eye * 0.19F;
        const auto quad = FrustumQuadAtDistance(std::atan(left), std::atan(right),
                                                 std::atan(1.111F), std::atan(-1.192F), quadDistance);
        Expect(Near((quad.centerX - 0.5F * quad.width) / quadDistance, left) &&
               Near((quad.centerX + 0.5F * quad.width) / quadDistance, right),
               "quad preserves the asymmetric horizontal rays for each eye");
        Expect(Near((quad.centerY - 0.5F * quad.height) / quadDistance, -1.192F) &&
               Near((quad.centerY + 0.5F * quad.height) / quadDistance, 1.111F),
               "quad preserves the asymmetric vertical rays");
    }
    Expect(Near(HorizontalDegrees(2.0F, 1.0F), 90.0F, 0.01F), "90 degrees");
    for (float pitch : {-1.2F, 0.0F, 1.2F}) {
        for (float roll : {-0.4F, 0.0F, 0.4F}) {
            const float tilt = std::cos(pitch);
            Expect(Near(CameraRollFromView(tilt * std::sin(roll), tilt * std::cos(roll)), roll),
                   "camera roll is independent of pitch");
        }
    }
    Expect(CameraRollFromView(0.001F, 0.002F) == 0.0F, "vertical view has no reliable roll");
    Expect(Near(CameraRollFromView(0.12F * std::sin(0.4F), 0.12F * std::cos(0.4F)), 0.2F),
           "roll correction tapers as the view approaches vertical");
    Expect(CameraRollFromView(NAN, 1.0F) == 0.0F, "invalid camera basis is ignored");
    Expect(Near(ClampQuadWidth(100.0F), 20.0F), "width upper clamp");
    Expect(Near(ClampQuadWidth(NAN), 2.4F), "width NaN fallback");
    Expect(Near(ClampQuadDistance(0.0F), 0.3F), "distance lower clamp");
    Expect(Near(StepQuadWidth(2.0F, true, false), 2.1F), "width step up");
    Expect(Near(StepQuadWidth(2.1F, false, false), 2.0F), "width step down");

    float width = 2.4F;
    float distance = 2.0F;
    const float before = HorizontalDegrees(width, distance);
    StepQuadDistance(width, distance, true, false);
    Expect(distance < 2.0F, "closer reduces distance");
    Expect(Near(HorizontalDegrees(width, distance), before, 0.01F),
           "distance step keeps angular size");

    Expect(StereoFrameSize(2636, 2804, 100, 8192, 8192) == std::pair{5280U, 2970U},
           "wide frame fits the full left and right eye");
    Expect(StereoFrameSize(2636, 2804, 100, 4096, 2160) == std::pair{3840U, 2160U},
           "constrained frame keeps 16:9");
    Expect(StereoFrameSize(2636, 2804, 75, 8192, 8192) == std::pair{3968U, 2232U},
           "manual fraction scales the 16:9 frame");
    Expect(StereoFrameSize(0, 0, 100, 8192, 8192) == std::pair{0U, 0U},
           "missing XR recommendation does not request a resize");

    for (float eyeOffset : {-0.08F, 0.0F, 0.15F}) {
        constexpr float scale = 1.00002F, nativeBias = -0.01F;
        constexpr float clearance = 0.08F;
        const float adjustedBias = FirstPersonDepthBias(scale, nativeBias, eyeOffset, clearance);
        const auto clipZ = [&](float eyeDepth) { return (eyeDepth - eyeOffset) * scale + adjustedBias; };
        Expect(clipZ(0.04F) < 0.0F, "near surface clips after eye translation");
        Expect(Near(clipZ(clearance), 0.0F), "first-person clip plane at requested distance");
        Expect(clipZ(0.25F) > 0.0F, "arm beyond the first-person clip plane stays visible");
        Expect(Near(FirstPersonDepthBias(scale, nativeBias, eyeOffset, 0.0F),
                    nativeBias + scale * eyeOffset), "zero clearance preserves native depth");
    }

    for (int extent : {1200, 2100, 2636, 2804}) {
        for (int click : {0, extent / 2, extent - 1}) {
            constexpr int hotspot = 3, bitmapExtent = 32;
            const float center = CursorBitmapCenter(click, hotspot, bitmapExtent, extent);
            const float displayedHotspot = center * extent - 0.5F * bitmapExtent + hotspot;
            Expect(Near(displayedHotspot, click + 0.5F), "cursor hotspot stays on clicked pixel");
        }
    }

    const std::vector<std::int64_t> srgbAndUnorm = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                                     DXGI_FORMAT_R8G8B8A8_UNORM};
    Expect(SelectQuadFormat(DXGI_FORMAT_R8G8B8A8_UNORM, srgbAndUnorm, true) ==
               DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
           "prefers sRGB");
    Expect(SelectQuadFormat(DXGI_FORMAT_R8G8B8A8_UNORM, srgbAndUnorm, false) ==
               DXGI_FORMAT_R8G8B8A8_UNORM,
           "UNORM when sRGB off");
    const std::vector<std::int64_t> bgraOnly = {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    Expect(SelectQuadFormat(DXGI_FORMAT_R8G8B8A8_UNORM, bgraOnly, true) == DXGI_FORMAT_UNKNOWN,
           "never crosses RGBA/BGRA groups (copy would fail)");
    Expect(SelectQuadFormat(DXGI_FORMAT_B8G8R8A8_UNORM, bgraOnly, true) ==
               DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
           "BGRA sRGB");
    Expect(SelectQuadFormat(DXGI_FORMAT_R10G10B10A2_UNORM, srgbAndUnorm, true) ==
               DXGI_FORMAT_UNKNOWN,
           "unsupported HDR format is rejected");

    if (g_failures == 0) {
        std::printf("All QuadMath tests passed\n");
        return 0;
    }
    return 1;
}
