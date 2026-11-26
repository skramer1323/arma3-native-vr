#include "QuadMath.h"
#include "XrPoseMath.h"
#include "WorldShadingMask.h"
#include "VrsPixelShader.h"
#include "OpticsZoom.h"
#include "InventoryLayout.h"

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

    Expect(IsInventoryCloseButton(0.630303F, 0.555556F, 0.665151F, 0.507071F),
           "the captured inventory close button matches the stock inventory layout");
    Expect(IsInventoryCloseButton(0.504F, 0.444F, 0.532F, 0.4054F),
           "the inventory layout scales with interface size");
    Expect(!IsInventoryCloseButton(0.630303F, 0.579798F, 0.672121F, 0.526465F) &&
           !IsInventoryCloseButton(0.95F, 1.0F, 1.0F, 0.93F),
           "the group screen and map close buttons are not classified as inventory");

    constexpr float normalProjection = 0.5673759F;
    constexpr float binocularLow = 1.7021277F, binocularHigh = 34.042553F;
    Expect(ValidZoomProjection(binocularHigh, 47.058823F, 1.0F) &&
           !ValidZoomProjection(binocularHigh, 0.0F, 1.0F) &&
           !ValidZoomProjection(binocularHigh, 47.058823F, 0.0F),
           "valid high-power optics are accepted without accepting a bad projection");
    Expect(Near(NativeOpticsZoom(binocularLow, normalProjection, true, false), 3.0F) &&
           Near(NativeOpticsZoom(binocularHigh, normalProjection, true, false), 60.0F),
           "keyboard binoculars follow native magnification through the 20x step");
    Expect(!NativeZoomMayUpdateReference(binocularLow, normalProjection, false) &&
           NativeZoomMayUpdateReference(normalProjection * 1.2F, normalProjection, false) &&
           Near(NativeOpticsZoom(normalProjection * 1.2F, normalProjection, true, false), 1.0F),
           "normal camera changes may update the reference, optics cannot consume it");
    constexpr float vehicleProjection = 0.4728F;
    constexpr float hunterSight = 0.9726F;
    const bool hunterEntered = KeepNativeOptics(hunterSight, vehicleProjection, true, false);
    const bool hunterReleased = KeepNativeOptics(hunterSight, vehicleProjection, false, hunterEntered);
    Expect(hunterEntered && hunterReleased &&
           Near(NativeOpticsZoom(hunterSight, vehicleProjection, true, hunterReleased),
                hunterSight / vehicleProjection) &&
           !KeepNativeOptics(vehicleProjection, vehicleProjection, false, hunterReleased) &&
           !KeepNativeOptics(hunterSight, vehicleProjection, false, false),
           "the Hunter sight keeps its first zoom after release until the normal view returns");
    for (const float eye : {-1.0F, 1.0F}) {
        const float left = 0.9505F - eye * 0.2305F;
        const float right = 0.9505F + eye * 0.2305F;
        const auto fov = CenteredOpticsFov({-std::atan(left), std::atan(right),
                                           std::atan(0.829F), -std::atan(1.226F)});
        Expect(fov.angleLeft == -fov.angleRight && fov.angleDown == -fov.angleUp,
               "the optic axis is at the image centre in both eyes");
        Expect(Near(2.0F * std::tan(fov.angleRight), left + right) &&
               Near(2.0F * std::tan(fov.angleUp), 0.829F + 1.226F),
               "the centred optic preserves the horizontal and vertical projection scale");
    }

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
    Expect(OnFootCameraRollFromView(0.0F, -0.56F) == 0.0F,
           "pitching past vertical does not introduce a half-turn on foot");
    Expect(Near(OnFootCameraRollFromView(0.04F, -0.56F),
                OnFootCameraRollFromView(0.04F, 0.56F)),
           "an inverted up vector does not change the on-foot roll correction");
    Expect(Near(OnFootCameraRollFromView(0.0F, 0.02F), 0.0F) &&
               Near(OnFootCameraRollFromView(0.0F, -0.02F), 0.0F),
           "both sides of the vertical crossing remain continuous");

    constexpr std::array<float, 16> forwardView{
        1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    const auto centered = WorldYawOrbitCorrection(forwardView, 0.0F, 0.07F, 0.04F);
    Expect(centered == std::array<float, 3>{}, "yaw correction leaves the centered anchor alone");
    for (float side : {-1.0F, 1.0F}) {
        const std::array<float, 16> turnedView{
            0.0F, 0.0F, -side, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
            side, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
        const auto correction =
            WorldYawOrbitCorrection(turnedView, side * std::atan2(1.0F, 0.0F), 0.07F, 0.04F);
        const float cameraX = -side * (0.07F + correction[2]);
        const float cameraZ = side * (0.04F + correction[0]);
        Expect(Near(cameraX, 0.04F) && Near(cameraZ, 0.07F) && Near(correction[1], 0.0F),
               "both yaw directions return the native orbit to its neutral anchor");
    }
    const std::array<float, 16> verticalView{};
    const auto vertical = WorldYawOrbitCorrection(verticalView, 0.8F, 0.07F, 0.04F);
    Expect(vertical == std::array<float, 3>{}, "vertical view has no reliable yaw correction");

    const XrPosef newLocalInOld{{0.0F, std::sin(0.3F), 0.0F, std::cos(0.3F)},
                               {1.5F, 0.2F, -0.5F}};
    const auto rebasedIdentity = RebaseLocalOffset(kIdentityXrPose, newLocalInOld);
    Expect(Near(rebasedIdentity.orientation.y, -newLocalInOld.orientation.y) &&
           Near(rebasedIdentity.orientation.w, newLocalInOld.orientation.w),
           "the anchored origin rotates opposite a LOCAL origin change");
    const XrPosef anchorInOld{{std::sin(0.2F), 0.0F, 0.0F, std::cos(0.2F)},
                             {-0.3F, 1.0F, -2.0F}};
    const auto rebasedAnchor = RebaseLocalOffset(anchorInOld, newLocalInOld);
    const auto translatedBack = Rotate(newLocalInOld.orientation, rebasedAnchor.position);
    const auto rotatedBack = Multiply(newLocalInOld.orientation, rebasedAnchor.orientation);
    Expect(Near(translatedBack.x + newLocalInOld.position.x, anchorInOld.position.x) &&
           Near(translatedBack.y + newLocalInOld.position.y, anchorInOld.position.y) &&
           Near(translatedBack.z + newLocalInOld.position.z, anchorInOld.position.z) &&
           Near(rotatedBack.x, anchorInOld.orientation.x) &&
           Near(rotatedBack.y, anchorInOld.orientation.y) &&
           Near(rotatedBack.z, anchorInOld.orientation.z) &&
           Near(rotatedBack.w, anchorInOld.orientation.w),
           "rebasing a translated and rotated origin preserves the anchor pose");

    const WorldShadingProjection normalEyes[2]{{1.0F, 1.0F, 0.0F, 0.0F},
                                                {1.0F, 1.0F, 0.0F, 0.0F}};
    Expect(WorldShadingTileRate(8, 8, 512, 256, normalEyes) == WorldTileRate::Full &&
           WorldShadingTileRate(24, 8, 512, 256, normalEyes) == WorldTileRate::Full,
           "both eye centers keep one shader evaluation per pixel");
    Expect(WorldShadingTileRate(13, 8, 512, 256, normalEyes) == WorldTileRate::Full &&
           WorldShadingTileRate(14, 8, 512, 256, normalEyes) == WorldTileRate::TwoByOne,
           "the central cone keeps full rate and peripheral tiles reduce shading");
    Expect(WorldShadingTileRate(0, 0, 512, 256, normalEyes) == WorldTileRate::TwoByTwo,
           "the outer corners can use quarter-rate shading");
    Expect(WorldShadingTileRate(16, 0, 528, 256, normalEyes) == WorldTileRate::Full,
           "a hardware tile shared by the two eye images uses full rate");
    Expect(WorldShadingTileRate(0, 0, 512, 256, normalEyes, {}, 1.21F) == WorldTileRate::FourByFour &&
           WorldShadingTileRate(8, 8, 512, 256, normalEyes, {}, 1.21F) == WorldTileRate::Full &&
           WorldShadingTileRate(16, 0, 528, 256, normalEyes, {}, 1.21F) == WorldTileRate::Full,
           "4x4 only changes the far edge and protects the stereo seam");
    const WorldVisibilityPoint diamond[]{{0.0F, -1.0F}, {1.0F, 0.0F}, {0.0F, 1.0F}, {-1.0F, 0.0F}};
    Expect(WorldTileOutsideVisibility(diamond, 0.85F, 0.95F, 0.85F, 0.95F) &&
           !WorldTileOutsideVisibility(diamond, 0.32F, 0.38F, 0.67F, 0.73F) &&
           !WorldTileOutsideVisibility(diamond, -0.04F, 0.04F, -0.04F, 0.04F) &&
           !WorldTileOutsideVisibility({}, 0.85F, 0.95F, 0.85F, 0.95F),
           "only tiles beyond the visible contour and margin can use the stronger edge rate");
    Expect(IsOpaqueVrsMaterial("ps_5_0\ndcl_output o0.xyzw\nmov o0, v0\nret\n"),
           "VRS accepts one ordinary color output");
    Expect(!IsOpaqueVrsMaterial("ps_5_0\ndcl_output o0.xyzw\ndiscard_nz r0.x\nret\n"),
           "alpha-tested materials keep per-pixel coverage");
    Expect(!IsOpaqueVrsMaterial("ps_5_0\ndcl_output o0.xyzw\ndcl_output oDepth\nret\n") &&
           !IsOpaqueVrsMaterial("ps_5_0\ndcl_output o0.xyzw\ndcl_output oMask\nret\n") &&
           !IsOpaqueVrsMaterial("ps_5_0\ndcl_output o0.xyzw\ndcl_output o1.xyz\nret\n"),
           "depth, coverage and multi-target shaders stay at full rate");
    Expect(!IsOpaqueVrsMaterial("ps_5_0\ndcl_output o0.xyzw\n"
                                "dcl_input_ps_sgv constant v0.x, sample_index\nret\n"),
           "sample-frequency shaders stay at full rate");
    const auto defaultRadii = WorldShadingRadiiFromDegrees(35, 50);
    const auto smallerRadii = WorldShadingRadiiFromDegrees(20, WorldShadingTransitionEnd(20));
    Expect(Near(defaultRadii.inner, kFullRateTangent) && Near(defaultRadii.middle, kOuterRateTangent),
           "the default angular conversion preserves the release mask");
    Expect(WorldShadingTileRate(12, 8, 512, 256, normalEyes, defaultRadii) == WorldTileRate::Full &&
           WorldShadingTileRate(12, 8, 512, 256, normalEyes, smallerRadii) == WorldTileRate::TwoByTwo,
           "reducing the sharp center radius moves both shading boundaries");
    Expect(WorldShadingTileRate(24, 8, 528, 256, normalEyes, smallerRadii) == WorldTileRate::Full &&
           WorldShadingTileRate(16, 8, 528, 256, normalEyes, smallerRadii) == WorldTileRate::Full,
           "smaller radii preserve eye centers and the stereo seam");
    const WorldShadingProjection wideEyes[2]{{0.5F, 1.0F, 0.0F, 0.0F},
                                              {1.0F, 1.0F, 0.0F, 0.0F}};
    Expect(WorldShadingTileRate(13, 8, 512, 256, wideEyes) == WorldTileRate::TwoByTwo &&
           WorldShadingTileRate(29, 8, 512, 256, wideEyes) == WorldTileRate::Full,
           "angular masks use each eye's projection scale");
    const WorldShadingProjection invalidEyes[2]{{0.0F, 1.0F, 0.0F, 0.0F},
                                                 normalEyes[1]};
    Expect(WorldShadingTileRate(0, 0, 512, 256, invalidEyes) == WorldTileRate::Full,
           "invalid projection uses full shading");

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
