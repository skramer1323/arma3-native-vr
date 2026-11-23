#include "Config.h"

#include "Log.h"
#include "QuadMath.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwchar>

namespace a3vr {
namespace {

Config g_config;
constexpr const wchar_t* kSection = L"ArmaVR";

float ReadFloat(const wchar_t* key, float fallback)
{
    std::array<wchar_t, 64> value{};
    GetPrivateProfileStringW(kSection, key, L"", value.data(),
                             static_cast<DWORD>(value.size()), ConfigPath().c_str());
    if (value[0] == L'\0') {
        return fallback;
    }
    wchar_t* end = nullptr;
    const float parsed = std::wcstof(value.data(), &end);
    return end != value.data() ? parsed : fallback;
}

// "hash,hash,..." in hex (the diagnostic CullHashes key).
std::vector<std::uint64_t> ReadHashList(const wchar_t* key)
{
    std::array<wchar_t, 512> value{};
    GetPrivateProfileStringW(kSection, key, L"", value.data(), static_cast<DWORD>(value.size()),
                             ConfigPath().c_str());
    std::vector<std::uint64_t> hashes;
    wchar_t* cursor = value.data();
    while (*cursor) {
        wchar_t* end = nullptr;
        const unsigned long long parsed = std::wcstoull(cursor, &end, 16);
        if (end == cursor) {
            ++cursor;
            continue;
        }
        hashes.push_back(parsed);
        cursor = end;
    }
    return hashes;
}

int ReadInt(const wchar_t* key, int fallback)
{
    return static_cast<int>(
        GetPrivateProfileIntW(kSection, key, fallback, ConfigPath().c_str()));
}

}  // namespace

std::filesystem::path ConfigPath()
{
    return ModuleDirectory() / L"arma3_native_vr.ini";
}

Config& MutableConfig()
{
    return g_config;
}

void SaveIniValue(const wchar_t* key, double value)
{
    wchar_t text[32]{};
    swprintf_s(text, L"%.3f", value);
    WritePrivateProfileStringW(kSection, key, text, ConfigPath().c_str());
}

const Config& GetConfig()
{
    return g_config;
}

void LoadConfig()
{
    const bool exists = std::filesystem::exists(ConfigPath());
    g_config.enableVR = ReadInt(L"EnableVR", 1) != 0;
    g_config.openXrDiagnosticMode = std::clamp(ReadInt(L"OpenXRDiagnosticMode", 0), 0, 2);
    g_config.quadAnchor = std::clamp(ReadInt(L"QuadAnchor", 0), 0, 1);
    g_config.quadWidthMeters = ClampQuadWidth(ReadFloat(L"QuadWidthMeters", 2.4F));
    g_config.quadDistanceMeters = ClampQuadDistance(ReadFloat(L"QuadDistanceMeters", 2.0F));
    g_config.hudScale = std::clamp(ReadFloat(L"HudScale", 0.6F), 0.2F, 1.2F);
    g_config.hudShiftX = std::clamp(ReadFloat(L"HudShiftX", 0.0F), -0.6F, 0.6F);
    g_config.hudShiftY = std::clamp(ReadFloat(L"HudShiftY", 0.0F), -0.6F, 0.6F);
    g_config.hudDistanceMeters = std::clamp(ReadFloat(L"HudDistanceMeters", 2.0F), 0.4F, 10.0F);
    g_config.worldFovCrop = std::clamp(ReadFloat(L"WorldFovCrop", 0.75F), 0.4F, 1.0F);
    g_config.weaponScale = std::clamp(ReadFloat(L"WeaponSeparationScale", 0.3F), 0.0F, 2.0F);
    g_config.weaponConvergenceMeters =
        std::clamp(ReadFloat(L"WeaponConvergenceMeters", 0.7F), 0.2F, 5.0F);
    g_config.weaponSize = std::clamp(ReadFloat(L"WeaponSizeScale", 0.6F), 0.2F, 1.5F);
    g_config.poseBySequence = ReadInt(L"PoseBySequence", 1) != 0;
    // This build always renders both eyes from the same game frame.
    g_config.projectionLayer = true;
    g_config.steamVrWorldQuad = ReadInt(L"SteamVRWorldQuad", 1) != 0;
    g_config.steamVrMirrorHud = ReadInt(L"SteamVRMirrorHud", 0) != 0;
    g_config.alternateEyeProjection = false;
    g_config.dibrShift = false;
    g_config.dibrForwardHistory = false;
    g_config.aerNative = false;
    g_config.forceGameWidth = std::clamp(ReadInt(L"ForceGameWidth", 0), 0, 8192);
    g_config.forceGameHeight = std::clamp(ReadInt(L"ForceGameHeight", 0), 0, 8192);
    const bool hasManualResolution = g_config.forceGameWidth > 0 && g_config.forceGameHeight > 0;
    g_config.autoHeadsetResolution = ReadInt(L"AutoHeadsetResolution", hasManualResolution ? 0 : 1) != 0;
    g_config.allowOversizeWindow = ReadInt(L"AllowOversizeWindow", 0) != 0;
    g_config.virtualCursor = ReadInt(L"VirtualCursor", 1) != 0;
    g_config.virtualCursorSpeed = std::clamp(ReadFloat(L"VirtualCursorSpeed", 1.0F), 0.2F, 5.0F);
    g_config.monoScreen = ReadInt(L"MonoScreen", 1) != 0;
    g_config.virtualBackBuffer = ReadInt(L"VirtualBackBuffer", 0) != 0;
    g_config.dibrFillMode = std::clamp(ReadInt(L"DibrFillMode", 1), 0, 2);
    g_config.adsLockView = ReadInt(L"AdsLockView", 1) != 0;
    g_config.adsRightMouseToggle = ReadInt(L"AdsRightMouseToggle", 0) != 0;
    g_config.aimEye = std::clamp(ReadInt(L"AimEye", 1), -1, 1);
    g_config.cullGodRays = ReadInt(L"CullGodRays", 1) != 0;
    g_config.cullHashes = ReadHashList(L"CullHashes");
    g_config.scopeLens = ReadInt(L"ScopeLens", 0) != 0;
    g_config.scopeReticle = ReadInt(L"ScopeReticle", 1) != 0;
    g_config.scopeStatic = ReadInt(L"ScopeStatic", 1) != 0;
    g_config.scopeWeaponFlat = ReadInt(L"ScopeWeaponFlat", 1) != 0;
    g_config.scopeLensDegrees = std::clamp(ReadFloat(L"ScopeLensDegrees", 30.0F), 8.0F, 70.0F);
    g_config.hudQuad = ReadInt(L"HudQuad", 1) != 0;
    g_config.hideGameplayHud = ReadInt(L"HideGameplayHud", 0) != 0;
    g_config.hudBodyLock = ReadInt(L"HudBodyLock", 1) != 0;
    g_config.stabilizeHudMarker = ReadInt(L"StabilizeHudMarker", 0) != 0;
    g_config.dlssBalancedExperimental = false;
    g_config.cacheStereoProjection = ReadInt(L"CacheStereoProjection", 0) != 0;
    g_config.staleEyeWarp = false;
    g_config.aerWarpMode = AerWarpMode::Off;
    g_config.aerDibrPreference = 0;
    g_config.stereoShadows = ReadInt(L"StereoShadows", 1) != 0;
    g_config.worldVrs = ReadInt(L"WorldVrs", 0) != 0;
    g_config.singlePassDepth = ReadInt(L"SinglePassDepth", 0) != 0;
    g_config.singlePassWorldPrepass = ReadInt(L"SinglePassWorldPrepass", 0) != 0;
    g_config.weaponBodyLock = ReadInt(L"WeaponBodyLock", 1) != 0;
    g_config.headOffset[0] = std::clamp(ReadFloat(L"HeadOffsetX", 0.0F), -0.6F, 0.6F);
    g_config.headOffset[1] = std::clamp(ReadFloat(L"HeadOffsetY", 0.0F), -0.6F, 0.6F);
    g_config.headOffset[2] = std::clamp(ReadFloat(L"HeadOffsetZ", 0.0F), -0.6F, 0.6F);
    // Vehicle placement defaults to the on-foot one, 6 cm lower, until saved.
    g_config.headOffsetVehicle[0] = std::clamp(ReadFloat(L"HeadOffsetVehicleX", g_config.headOffset[0]), -0.6F, 0.6F);
    g_config.headOffsetVehicle[1] = std::clamp(ReadFloat(L"HeadOffsetVehicleY", g_config.headOffset[1] - 0.06F), -0.6F, 0.6F);
    g_config.headOffsetVehicle[2] = std::clamp(ReadFloat(L"HeadOffsetVehicleZ", g_config.headOffset[2]), -0.6F, 0.6F);
    g_config.vehicleAuto = ReadInt(L"VehicleAuto", 0) != 0;
    g_config.headPlacementMode = std::clamp(ReadInt(L"HeadPlacementMode", 0), 0, 2);
    g_config.weaponHeadPosition = ReadInt(L"WeaponHeadPosition", 0) != 0;
    g_config.weaponProjM22 = ReadFloat(L"WeaponProjM22", 1.00002F);
    g_config.weaponProjM32 = ReadFloat(L"WeaponProjM32", -0.01F);
    g_config.firstPersonClipCm = std::clamp(ReadFloat(L"FirstPersonClipCm", 0.0F), 0.0F, 18.0F);
    g_config.firstPersonHideTorso = ReadInt(L"FirstPersonHideTorso", 0) != 0;
    g_config.headPosition = ReadInt(L"HeadPosition", 0) != 0;
    g_config.headPositionGain[0] = ReadFloat(L"HeadPositionGainX", 0.0F);
    g_config.headPositionGain[1] = ReadFloat(L"HeadPositionGainY", 0.0F);
    g_config.headPositionGain[2] = ReadFloat(L"HeadPositionGainZ", 0.0F);
    g_config.headPositionFrame = std::clamp(ReadInt(L"HeadPositionFrame", 0), 0, 1);
    g_config.headPositionScale = std::clamp(ReadFloat(L"HeadPositionScale", 1.0F), 0.0F, 3.0F);
    g_config.headPositionLimitMeters =
        std::clamp(ReadFloat(L"HeadPositionLimitMeters", 0.25F), 0.0F, 0.3F);
    g_config.worldYawOrbitForward =
        std::clamp(ReadFloat(L"WorldYawOrbitForward", 0.0F), -0.2F, 0.2F);
    g_config.worldYawOrbitRight =
        std::clamp(ReadFloat(L"WorldYawOrbitRight", 0.0F), -0.2F, 0.2F);
    g_config.calibratedRotation = ReadInt(L"CalibratedRotation", 0) != 0;
    g_config.zoomMagnification = std::clamp(ReadFloat(L"ZoomMagnification", 1.0F), 0.0F, 1.0F);
    g_config.quadHeightOffsetMeters =
        std::clamp(ReadFloat(L"QuadHeightOffsetMeters", 0.0F), -1.5F, 1.5F);
    g_config.useSrgbSwapchain = ReadInt(L"UseSrgbSwapchain", 1) != 0;
    g_config.tuningHotkeys = false;
    g_config.statsEveryPresents = std::max(0, ReadInt(L"StatsEveryPresents", 600));
    g_config.perfLog = ReadInt(L"PerfLog", 0) != 0;
    g_config.drawCursor = ReadInt(L"DrawCursor", 1) != 0;
    g_config.headTracking = ReadInt(L"HeadTracking", 1) != 0;
    g_config.headYawLimitDegrees = std::clamp(ReadFloat(L"HeadYawLimitDegrees", 70.0F), 5.0F, 180.0F);
    g_config.headPitchLimitDegrees =
        std::clamp(ReadFloat(L"HeadPitchLimitDegrees", 85.0F), 5.0F, 89.0F);
    g_config.headRotationGain = std::clamp(ReadFloat(L"HeadRotationGain", 0.159155F), 0.05F, 20.0F);
    g_config.headYawSign = ReadInt(L"HeadYawSign", 1) < 0 ? -1 : 1;
    g_config.headPitchSign = ReadInt(L"HeadPitchSign", 1) < 0 ? -1 : 1;
    g_config.poseLagFrames = std::clamp(ReadInt(L"PoseLagFrames", 1), 1, 4);
    g_config.stabilizeArmaRoll = ReadInt(L"StabilizeArmaRoll", 1) != 0;
    g_config.worldQuadDistanceMeters =
        std::clamp(ReadFloat(L"WorldQuadDistanceMeters", 5.0F), 0.5F, 50.0F);
    g_config.worldFovTanVertical = std::clamp(ReadFloat(L"WorldFovTanVertical", 0.75F), 0.05F, 5.0F);
    g_config.stereoMode = 0;
    g_config.stereoShiftMillimeters =
        std::clamp(ReadFloat(L"StereoShiftMillimeters", 30.0F), 0.0F, 2000.0F);
    g_config.stereoSwapEyes = ReadInt(L"StereoSwapEyes", 0) != 0;
    g_config.headsetIpdMeters = std::clamp(ReadFloat(L"HeadsetIpdMeters", 0.064F), 0.04F, 0.08F);
    g_config.stereoScale = std::clamp(ReadFloat(L"StereoScale", 1.0F), 0.0F, 3.0F);
    g_config.autoCalibrate = ReadInt(L"AutoCalibrate", 0) != 0;
    g_config.depthStereo = false;
    g_config.convergenceMeters = std::clamp(ReadFloat(L"ConvergenceMeters", 1.0F), 0.05F, 200.0F);
    g_config.depthReversed = std::clamp(ReadInt(L"DepthReversed", -1), -1, 1);
    g_config.sbsInput = true;
    g_config.frameProbe = ReadInt(L"FrameProbe", 0) != 0;
    g_config.engineStereo = true;
    g_config.engineStereoSeparationMeters =
        std::clamp(ReadFloat(L"EngineStereoSeparationMeters", 0.064F), 0.0F, 0.5F);
    g_config.engineStereoConvergenceMeters =
        std::clamp(ReadFloat(L"EngineStereoConvergenceMeters", 5.0F), 0.2F, 200.0F);
    g_config.frameDumpSeconds = std::clamp(ReadInt(L"FrameDumpSeconds", 0), 0, 3600);
    g_config.detailedCapture = ReadInt(L"DetailedCapture", 0) != 0;
    g_config.frameProbeAutoSeconds = std::clamp(ReadInt(L"FrameProbeAutoSeconds", 0), 0, 3600);
    Log("SbsInput=%d FrameProbe=%d FrameProbeAutoSeconds=%d EngineStereo=%d separation=%.3f "
        "convergence=%.2f FrameDumpSeconds=%d",
        g_config.sbsInput ? 1 : 0, g_config.frameProbe ? 1 : 0, g_config.frameProbeAutoSeconds,
        g_config.engineStereo ? 1 : 0, g_config.engineStereoSeparationMeters,
        g_config.engineStereoConvergenceMeters, g_config.frameDumpSeconds);
    Log("StereoScale=%.2f AutoCalibrate=%d DepthStereo=%d ConvergenceMeters=%.2f DepthReversed=%d",
        g_config.stereoScale, g_config.autoCalibrate ? 1 : 0, g_config.depthStereo ? 1 : 0,
        g_config.convergenceMeters, g_config.depthReversed);
    Log("StereoMode=%d StereoShiftMillimeters=%.1f StereoSwapEyes=%d HeadsetIpdMeters=%.3f",
        g_config.stereoMode, g_config.stereoShiftMillimeters, g_config.stereoSwapEyes ? 1 : 0,
        g_config.headsetIpdMeters);
    Log("DrawCursor=%d HeadTracking=%d HeadYawLimitDegrees=%.1f HeadPitchLimitDegrees=%.1f "
        "HeadRotationGain=%.3f HeadYawSign=%d HeadPitchSign=%d PoseLagFrames=%d "
        "WorldQuadDistanceMeters=%.2f WorldFovTanVertical=%.3f",
        g_config.drawCursor ? 1 : 0, g_config.headTracking ? 1 : 0, g_config.headYawLimitDegrees,
        g_config.headPitchLimitDegrees, g_config.headRotationGain, g_config.headYawSign,
        g_config.headPitchSign, g_config.poseLagFrames, g_config.worldQuadDistanceMeters,
        g_config.worldFovTanVertical);
    Log("Config %s: EnableVR=%d OpenXRDiagnosticMode=%d QuadAnchor=%d QuadWidthMeters=%.3f "
        "QuadDistanceMeters=%.3f UseSrgbSwapchain=%d TuningHotkeys=%d StatsEveryPresents=%d",
        exists ? "loaded" : "file missing, defaults used", g_config.enableVR ? 1 : 0,
        g_config.openXrDiagnosticMode, g_config.quadAnchor, g_config.quadWidthMeters,
        g_config.quadDistanceMeters, g_config.useSrgbSwapchain ? 1 : 0,
        g_config.tuningHotkeys ? 1 : 0, g_config.statsEveryPresents);
}

void SaveStereoTuning(float scale, bool swapEyes, float convergenceMeters)
{
    wchar_t value[32]{};
    swprintf_s(value, L"%.2f", scale);
    WritePrivateProfileStringW(kSection, L"StereoScale", value, ConfigPath().c_str());
    WritePrivateProfileStringW(kSection, L"StereoSwapEyes", swapEyes ? L"1" : L"0",
                               ConfigPath().c_str());
    swprintf_s(value, L"%.2f", convergenceMeters);
    WritePrivateProfileStringW(kSection, L"ConvergenceMeters", value, ConfigPath().c_str());
    Log("TUNING saved: StereoScale=%.2f StereoSwapEyes=%d ConvergenceMeters=%.2f", scale,
        swapEyes ? 1 : 0, convergenceMeters);
}
void SaveHeadOffset(float x, float y, float z)
{
    wchar_t value[32]{};
    const auto write = [&value](const wchar_t* key, double number) {
        swprintf_s(value, L"%.3f", number);
        WritePrivateProfileStringW(kSection, key, value, ConfigPath().c_str());
    };
    write(L"HeadOffsetX", x);
    write(L"HeadOffsetY", y);
    write(L"HeadOffsetZ", z);
    Log("TUNING saved: HeadOffset right/up/forward = %.3f %.3f %.3f m", x, y, z);
}

void SaveHeadOffsetVehicle(float x, float y, float z)
{
    wchar_t value[32]{};
    const auto write = [&value](const wchar_t* key, double number) {
        swprintf_s(value, L"%.3f", number);
        WritePrivateProfileStringW(kSection, key, value, ConfigPath().c_str());
    };
    write(L"HeadOffsetVehicleX", x);
    write(L"HeadOffsetVehicleY", y);
    write(L"HeadOffsetVehicleZ", z);
    Log("TUNING saved: HeadOffsetVehicle right/up/forward = %.3f %.3f %.3f m", x, y, z);
}

void SaveHudTuning(float scale, float shiftX, float shiftY, float distanceMeters, float fovCrop,
                   int poseLagFrames, float weaponScale, float weaponConvergenceMeters,
                   float weaponSize)
{
    wchar_t value[32]{};
    const auto write = [&value](const wchar_t* key, const wchar_t* format, double number) {
        swprintf_s(value, format, number);
        WritePrivateProfileStringW(kSection, key, value, ConfigPath().c_str());
    };
    write(L"HudScale", L"%.3f", scale);
    write(L"HudShiftX", L"%.3f", shiftX);
    write(L"HudShiftY", L"%.3f", shiftY);
    write(L"HudDistanceMeters", L"%.2f", distanceMeters);
    write(L"WorldFovCrop", L"%.3f", fovCrop);
    write(L"PoseLagFrames", L"%.0f", static_cast<double>(poseLagFrames));
    write(L"WeaponSeparationScale", L"%.3f", weaponScale);
    write(L"WeaponConvergenceMeters", L"%.3f", weaponConvergenceMeters);
    write(L"WeaponSizeScale", L"%.3f", weaponSize);
    Log("TUNING saved: HudScale=%.3f HudShiftX=%.3f HudShiftY=%.3f HudDistanceMeters=%.2f "
        "WorldFovCrop=%.3f PoseLagFrames=%d",
        scale, shiftX, shiftY, distanceMeters, fovCrop, poseLagFrames);
}

void SaveQuadTuning(float widthMeters, float distanceMeters, float heightOffsetMeters)
{
    wchar_t value[32]{};
    swprintf_s(value, L"%.3f", widthMeters);
    WritePrivateProfileStringW(kSection, L"QuadWidthMeters", value, ConfigPath().c_str());
    swprintf_s(value, L"%.3f", distanceMeters);
    WritePrivateProfileStringW(kSection, L"QuadDistanceMeters", value, ConfigPath().c_str());
    swprintf_s(value, L"%.3f", heightOffsetMeters);
    WritePrivateProfileStringW(kSection, L"QuadHeightOffsetMeters", value, ConfigPath().c_str());
    Log("TUNING saved: QuadWidthMeters=%.3f QuadDistanceMeters=%.3f QuadHeightOffsetMeters=%.3f",
        widthMeters, distanceMeters, heightOffsetMeters);
}

}  // namespace a3vr
