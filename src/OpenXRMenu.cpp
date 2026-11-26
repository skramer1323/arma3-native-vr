// Settings menu (Insert): the widgets and the panel that shows them in the headset.
// Members of OpenXRRuntime, kept in their own file so OpenXRRuntime.cpp does not grow further.

#include "OpenXRRuntime.h"

#include "Config.h"
#include "EngineStereo.h"
#include "FrameProbe.h"
#include "InputBlock.h"
#include "Log.h"
#include "Menu.h"
#include "ResolutionHook.h"
#include "QuadMath.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

namespace a3vr {
namespace {

constexpr std::uint32_t kMenuWidth = 1280;
constexpr std::uint32_t kMenuHeight = 960;
constexpr float kMenuDistanceMeters = 1.3F;
constexpr float kMenuWidthMeters = 1.6F;
bool g_menuScreenshotRequested = false;

}  // namespace


namespace {

std::string Base64(const std::vector<std::uint8_t>& bytes)
{
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t chunk = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                    (i + 1 < bytes.size() ? bytes[i + 1] << 8 : 0) |
                                    (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        out += kAlphabet[(chunk >> 18) & 63];
        out += kAlphabet[(chunk >> 12) & 63];
        out += i + 1 < bytes.size() ? kAlphabet[(chunk >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? kAlphabet[chunk & 63] : '=';
    }
    return out;
}

// Arma rewrites Arma3.cfg at exit. Persist the new 3D extent afterwards, and include
// the window size only in the legacy path where the headset uses that back buffer.
bool ScheduleCfgResolution(int width, int height, bool resizeWindow)
{
    // Raw string: the PowerShell script has backslashes and regular expressions in it.
    constexpr const wchar_t* kScriptFormat = LR"PS($p = Get-Process -Id %lu -ErrorAction SilentlyContinue; if ($p) { $p.WaitForExit() }; Start-Sleep -Seconds 2; $c = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Arma 3\Arma3.cfg'; if (Test-Path $c) { $t = [IO.File]::ReadAllText($c); if (%ls) { $t = $t -replace '(?m)^displayMode\s*=\s*\d+\s*;','displayMode=1;' -replace '(?m)^winWidth\s*=\s*\d+\s*;','winWidth=%d;' -replace '(?m)^winHeight\s*=\s*\d+\s*;','winHeight=%d;' }; $t = $t -replace '(?m)^renderWidth\s*=\s*\d+\s*;','renderWidth=%d;' -replace '(?m)^renderHeight\s*=\s*\d+\s*;','renderHeight=%d;'; [IO.File]::WriteAllText($c, $t) })PS";
    wchar_t script[1400];
    swprintf_s(script, kScriptFormat, GetCurrentProcessId(), resizeWindow ? L"$true" : L"$false",
               width, height, width, height);
    const std::size_t length = wcslen(script) * sizeof(wchar_t);
    std::vector<std::uint8_t> bytes(length);
    std::memcpy(bytes.data(), script, length);
    const std::string encoded = Base64(bytes);
    std::wstring command =
        L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass "
        L"-EncodedCommand ";
    command.append(encoded.begin(), encoded.end());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &startup, &process)) {
        Log("RESOLUTION could not start the helper that edits Arma3.cfg (error %lu)",
            GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

}  // namespace
// The panel stands in front of where the player looks when the menu opens, facing the player.
void OpenXRRuntime::PlaceMenu()
{
    XrVector3f position = m_poseRing[0].valid ? m_poseRing[0].head.position : XrVector3f{};
    const XrQuaternionf q =
        m_poseRing[0].valid ? m_poseRing[0].head.orientation : XrQuaternionf{0.0F, 0.0F, 0.0F, 1.0F};
    // Forward = q * (0, 0, -1); only the heading matters (the panel stays upright).
    const float fx = -2.0F * (q.x * q.z + q.w * q.y);
    const float fz = -(1.0F - 2.0F * (q.x * q.x + q.y * q.y));
    const float yaw = std::atan2(-fx, -fz);
    m_menuPose.orientation = {0.0F, std::sin(0.5F * yaw), 0.0F, std::cos(0.5F * yaw)};
    m_menuPose.position = {position.x - std::sin(yaw) * kMenuDistanceMeters, position.y - 0.05F,
                           position.z - std::cos(yaw) * kMenuDistanceMeters};
}

unsigned OpenXRRuntime::MenuPixelScale() const
{
    return m_eyeRecommendedW >= 2 * kMenuWidth && m_eyeRecommendedH >= 2 * kMenuHeight ? 2U : 1U;
}

bool OpenXRRuntime::EnsureMenuSwapchain()
{
    if (m_menuSwapchain != XR_NULL_HANDLE) {
        return true;
    }
    if (m_quadFormat == DXGI_FORMAT_UNKNOWN) {
        return false;
    }
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.format = static_cast<std::int64_t>(m_quadFormat);
    createInfo.sampleCount = 1;
    createInfo.width = kMenuWidth * MenuPixelScale();
    createInfo.height = kMenuHeight * MenuPixelScale();
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    if (!Check(xrCreateSwapchain(m_session, &createInfo, &m_menuSwapchain),
               "creating menu swapchain")) {
        m_menuSwapchain = XR_NULL_HANDLE;
        return false;
    }
    std::uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(m_menuSwapchain, 0, &imageCount, nullptr);
    m_menuImages.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!Check(xrEnumerateSwapchainImages(
                   m_menuSwapchain, imageCount, &imageCount,
                   reinterpret_cast<XrSwapchainImageBaseHeader*>(m_menuImages.data())),
               "enumerating menu images")) {
        xrDestroySwapchain(m_menuSwapchain);
        m_menuSwapchain = XR_NULL_HANDLE;
        m_menuImages.clear();
        return false;
    }
    Log("MENU swapchain %ux%u images=%u", createInfo.width, createInfo.height, imageCount);
    return true;
}

bool OpenXRRuntime::RenderMenuLayer(XrCompositionLayerQuad& layer)
{
    if ((!Menu::Instance().IsOpen() && !ShaderBrowseEnabled()) || !EnsureMenuSwapchain()) {
        return false;
    }
    std::uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!Check(xrAcquireSwapchainImage(m_menuSwapchain, &acquireInfo, &index), "acquiring menu")) {
        return false;
    }
    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = 100'000'000;
    bool drawn = false;
    if (Check(xrWaitSwapchainImage(m_menuSwapchain, &waitInfo), "waiting menu")) {
        if (ID3D11RenderTargetView* target = EyeTarget(m_menuImages[index].texture)) {
            drawn = Menu::Instance().Render(m_device.Get(), m_context.Get(), target, kMenuWidth,
                                            kMenuHeight, static_cast<float>(MenuPixelScale()),
                                            [this]() { DrawMenuUi(); });
            if (drawn && g_menuScreenshotRequested) {
                g_menuScreenshotRequested = false;
                const std::wstring path = (ModuleDirectory() / L"arma3_native_vr.menu.jpg").wstring();
                D3D11_TEXTURE2D_DESC menuDesc{};
                m_menuImages[index].texture->GetDesc(&menuDesc);
                const bool saved = DumpTextureJpeg(m_menuImages[index].texture, path.c_str(), nullptr);
                Log("MENU panel dump %s (format %d, %ux%u)", saved ? "written" : "FAILED",
                    static_cast<int>(menuDesc.Format), menuDesc.Width, menuDesc.Height);
            }
        }
    }
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    Check(xrReleaseSwapchainImage(m_menuSwapchain, &releaseInfo), "releasing menu");
    if (!drawn) {
        return false;
    }
    layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space = m_localSpace;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage.swapchain = m_menuSwapchain;
    layer.subImage.imageRect = {{0, 0}, {static_cast<std::int32_t>(kMenuWidth * MenuPixelScale()),
                                         static_cast<std::int32_t>(kMenuHeight * MenuPixelScale())}};
    layer.pose = m_menuPose;
    layer.size = {kMenuWidthMeters, kMenuWidthMeters * kMenuHeight / kMenuWidth};
    return true;
}

// Same as Ctrl+Delete: everything the menu and the hotkeys can change is written to the INI.
void OpenXRRuntime::SaveAllSettings()
{
    SaveQuadTuning(m_quadWidthMeters, m_quadDistanceMeters, m_quadHeightMeters);
    SaveStereoTuning(m_stereoScale, m_stereoSwap, m_convergenceMeters);
    // The placement being edited belongs to the active set; both sets are written.
    float* active = m_vehicleMode ? m_offsetVehicle : m_offsetFoot;
    std::copy(std::begin(m_headOffset), std::end(m_headOffset), active);
    SaveHeadOffset(m_offsetFoot[0], m_offsetFoot[1], m_offsetFoot[2]);
    SaveHeadOffsetVehicle(m_offsetVehicle[0], m_offsetVehicle[1], m_offsetVehicle[2]);
    SaveIniValue(L"HeadPlacementMode", m_vehicleForce);
    SaveHudTuning(m_hudScale, m_hudShiftX, m_hudShiftY, m_hudDistance, m_fovCrop, m_poseLag,
                  m_weaponScale, m_weaponConvergence, m_weaponSize);
    const Config& config = GetConfig();
    SaveIniValue(L"StabilizeHudMarker", config.stabilizeHudMarker ? 1 : 0);
    SaveIniValue(L"HideGameplayHud", config.hideGameplayHud ? 1 : 0);
    SaveIniValue(L"StabilizeArmaRoll", config.stabilizeArmaRoll ? 1 : 0);
    SaveIniValue(L"WorldVrs", config.worldVrs ? 1 : 0);
    SaveIniValue(L"WorldVrsInnerDegrees", config.worldVrsInnerDegrees);
    SaveIniValue(L"WorldVrsCoarseOuter", config.worldVrsCoarseOuter ? 1 : 0);
    SaveIniValue(L"WorldVrsShowRings", config.worldVrsShowRings ? 1 : 0);
    SaveIniValue(L"CasSharpnessPercent", config.casSharpnessPercent);
    SaveIniValue(L"DlssBalancedExperimental", config.dlssBalancedExperimental ? 1 : 0);
    SaveIniValue(L"ScopeLens", config.scopeLens ? 1 : 0);
    SaveIniValue(L"ScopeLensDegrees", config.scopeLensDegrees);
    SaveIniValue(L"ScopeReticle", config.scopeReticle ? 1 : 0);
    SaveIniValue(L"ScopeStatic", config.scopeStatic ? 1 : 0);
    SaveIniValue(L"ScopeWeaponFlat", config.scopeWeaponFlat ? 1 : 0);
    SaveIniValue(L"AlternateEyeProjection", m_alternateEyeProjection ? 1 : 0);
    SaveIniValue(L"DibrShift", m_dibrShift ? 1 : 0);
    SaveIniValue(L"DibrForwardHistory", config.dibrForwardHistory ? 1 : 0);
    SaveIniValue(L"AerWarpMode", static_cast<int>(config.aerWarpMode));
    SaveIniValue(L"AerDibrPreference", config.aerDibrPreference);
    SaveIniValue(L"StaleEyeWarp", config.staleEyeWarp ? 1 : 0);
    SaveIniValue(L"AerNative", config.aerNative ? 1 : 0);
    SaveIniValue(L"DibrFillMode", m_dibrFill);
    SaveIniValue(L"WeaponHeadPosition", config.weaponHeadPosition ? 1 : 0);
    SaveIniValue(L"FirstPersonClipCm", config.firstPersonClipCm);
    SaveIniValue(L"FirstPersonHideTorso", config.firstPersonHideTorso ? 1 : 0);
    Log("MENU all settings saved");
}

void OpenXRRuntime::DrawMenuUi()
{
    const bool wasPreviewingRadius = m_vrsRadiusPreview;
    m_vrsRadiusPreview = false;
    if (!Menu::Instance().IsOpen()) {
        // Shader browser only (menu closed): a small readout, the rest of the panel stays clear.
        ImGui::SetNextWindowPos(ImVec2(20.0F, 20.0F));
        ImGui::SetNextWindowSize(ImVec2(1240.0F, 200.0F));
        ImGui::SetNextWindowBgAlpha(0.8F);
        const ImGuiWindowFlags overlayFlags =
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("a3vr_shaders", nullptr, overlayFlags)) {
            ImGui::TextUnformatted(ShaderBrowseMode() == 2
                                       ? "HUD icons: PgUp/PgDn to hide a draw, Del to mark"
                                       : "Shaders: PgUp/PgDn to hide a draw, Del to mark");
            ImGui::TextUnformatted(ShaderBrowseDescribe().c_str());
            ImGui::Text("Marked: %zu    Ctrl+F1 cycles shader, HUD and off",
                        ShaderBrowseMarkedLines().size());
        }
        ImGui::End();
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kMenuWidth), static_cast<float>(kMenuHeight)));
    if (wasPreviewingRadius && EngineStereoLastWorldVrs()) {
        ImGui::SetNextWindowBgAlpha(0.38F);
    }
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoTitleBar;
    if (!ImGui::Begin("a3vr_menu", nullptr, flags)) {
        ImGui::End();
        return;
    }
    Config& config = MutableConfig();
    const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Arma 3 VR settings");
    const char* recenterLabel = "Recenter view (F11)";
    const auto& style = ImGui::GetStyle();
    const float recenterWidth = ImGui::CalcTextSize(recenterLabel).x + 2.0F * style.FramePadding.x;
    ImGui::SameLine(rightEdge - recenterWidth);
    if (ImGui::Button(recenterLabel)) {
        m_headRecenterRequested = true;
        m_recenterRequested = true;
        EngineStereoResetZoomReference();
        Log("Recentre requested (Insert)");
    }
    ImGui::Separator();

    const float footerHeight = ImGui::GetFrameHeightWithSpacing() + 2.0F * style.ItemSpacing.y;
    ImGui::BeginChild("settings-content", ImVec2(0.0F, -footerHeight));
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.54F);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Rendering")) {
            ImGui::SeparatorText("Peripheral rendering");
            if (ImGui::Checkbox("Enable peripheral shading", &config.worldVrs)) {
                Log("World VRS requested %s at %d deg",
                    config.worldVrs ? "on" : "off", config.worldVrsInnerDegrees);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("This build requires NVIDIA VRS, full stereo and MSAA off. Aiming uses full shading.");
            }
            if (config.worldVrs) {
                ImGui::SameLine(0.0F, 24.0F);
                if (EngineStereoLastWorldVrs()) {
                    ImGui::TextColored(ImVec4(0.45F, 0.90F, 0.72F, 1.0F), "Active");
                } else {
                    ImGui::TextDisabled("Waiting for eligible gameplay");
                }
            }
            ImGui::BeginDisabled(!config.worldVrs);
            ImGui::SliderInt("Sharp center radius", &config.worldVrsInnerDegrees,
                             kWorldShadingMinRadius, kWorldShadingMaxRadius, "%d deg",
                             ImGuiSliderFlags_AlwaysClamp);
            m_vrsRadiusPreview = config.worldVrs && ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                Log("World VRS sharp center radius set to %d deg", config.worldVrsInnerDegrees);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Drag to resize the sharp center. A smaller circle reduces GPU work on more of the scene.");
            }
            ImGui::EndDisabled();

            ImGui::SeparatorText("Image clarity");
            ImGui::SliderInt("FidelityFX CAS sharpness", &config.casSharpnessPercent, 0, 100, "%d%%",
                             ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("At 0%% sharpening is off. When VRS is active, sharpening fades towards the periphery.");
            }

            if (ImGui::CollapsingHeader("Render resolution")) {
                unsigned sceneWidth = 0, sceneHeight = 0;
                EngineStereoSceneSize(sceneWidth, sceneHeight);
                if (sceneWidth && sceneHeight) {
                    ImGui::Text("Current Arma scene  %u x %u", sceneWidth, sceneHeight);
                }
                if (m_startupSceneRequest.first) {
                    ImGui::Text("Startup request  %u x %u", m_startupSceneRequest.first,
                                m_startupSceneRequest.second);
                    if (m_reportedSceneExtent.first && m_reportedSceneExtent != m_startupSceneRequest) {
                        ImGui::TextColored(ImVec4(1.0F, 0.76F, 0.4F, 1.0F),
                                           "Arma allocated a different scene size in this mission.");
                        ImGui::TextDisabled("Use Sampling in Arma's video settings to change the actual scene size.");
                    }
                } else if (GetConfig().virtualBackBuffer) {
                    ImGui::TextDisabled("Arma's Sampling controls the scene size.");
                }
                bool automatic = GetConfig().autoHeadsetResolution;
                const bool resizeWindow = !GetConfig().virtualBackBuffer;
                static int percent = 0;
                static const char* resolutionStatus = nullptr;
                const unsigned recommendedW = m_eyeRecommendedW > 0 ? m_eyeRecommendedW : 2636U;
                const unsigned recommendedH = m_eyeRecommendedH > 0 ? m_eyeRecommendedH : 2804U;
                const auto [autoWidth, autoHeight] = StereoFrameSize(recommendedW, recommendedH, 100,
                    m_frameMaxWidth, m_frameMaxHeight);
                if (ImGui::Checkbox("Automatic 16:9 stereo frame size", &automatic)) {
                    resolutionStatus = "The size mode will apply on restart.";
                    config.autoHeadsetResolution = automatic;
                    SaveIniValue(L"AutoHeadsetResolution", automatic ? 1 : 0);
                    if (automatic && !resizeWindow) {
                        const bool scheduled = ScheduleCfgResolution(static_cast<int>(autoWidth),
                                                                     static_cast<int>(autoHeight), false);
                        resolutionStatus = scheduled ? "Restart Arma and check the actual scene size in a mission."
                                                     : "Could not schedule the new frame size.";
                    }
                }
                const int maxPercent = std::max(100, static_cast<int>(std::min(
                    m_frameMaxWidth * 100 / std::max(1U, autoWidth),
                    m_frameMaxHeight * 100 / std::max(1U, autoHeight))));
                if (percent == 0) {
                    const int wantedW = GetConfig().forceGameWidth;
                    percent = !automatic && wantedW > 0 && autoWidth > 0
                        ? static_cast<int>(std::lround(100.0F * wantedW / autoWidth)) : 100;
                }
                percent = std::clamp(percent, 30, maxPercent);
                ImGui::SliderInt("Manual frame size", &percent, 30, maxPercent, "%d%%",
                                 ImGuiSliderFlags_AlwaysClamp);
                const auto [scaledWidth, scaledHeight] = StereoFrameSize(recommendedW, recommendedH,
                    static_cast<unsigned>(percent), m_frameMaxWidth, m_frameMaxHeight);
                const int wantW = static_cast<int>(scaledWidth);
                const int wantH = static_cast<int>(scaledHeight);
                int fitW = wantW;
                int fitH = wantH;
                if (resizeWindow) {
                    FitWindowToDesktop(wantW, wantH, fitW, fitH);
                }
                ImGui::Text("Automatic: %u x %u. Manual: %d x %d",
                            autoWidth, autoHeight, fitW, fitH);
                if (fitW < wantW) {
                    ImGui::TextDisabled("The window size is limited by your desktop.");
                }
                if (ImGui::Button("Apply on restart")) {
                    if (ScheduleCfgResolution(fitW, fitH, resizeWindow)) {
                        config.autoHeadsetResolution = false;
                        SaveIniValue(L"AutoHeadsetResolution", 0);
                        config.forceGameWidth = wantW;
                        config.forceGameHeight = wantH;
                        SaveIniValue(L"ForceGameWidth", wantW);
                        SaveIniValue(L"ForceGameHeight", wantH);
                        resolutionStatus = "Restart Arma and check the actual scene size in a mission.";
                        Log("RESOLUTION requested from menu: render %dx%d, resize window=%d",
                            fitW, fitH, resizeWindow ? 1 : 0);
                    } else {
                        resolutionStatus = "Could not schedule the new frame size.";
                    }
                }
                if (resolutionStatus) {
                    ImGui::TextUnformatted(resolutionStatus);
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("View and HUD")) {
            ImGui::SeparatorText("Head tracking");
            ImGui::Checkbox("Positional head tracking (6DoF)", &m_headPositionOn);
            if (ImGui::Checkbox("Stabilize horizon", &config.stabilizeArmaRoll)) {
                m_renderEye = -1;
                m_dibr.ResetHistory();
                Log("Camera roll stabilization %s", config.stabilizeArmaRoll ? "on" : "off");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Compensates for the game camera roll, including while aiming or in vehicles.");
            }
            if (ImGui::CollapsingHeader("Head placement and timing")) {
                const char* placements[] = {"Default", "On foot", "Vehicle"};
                ImGui::Combo("Placement profile", &m_vehicleForce, placements, IM_ARRAYSIZE(placements));
                ImGui::TextDisabled("Adjusting the %s profile", m_vehicleMode ? "vehicle" : "on-foot");
                ImGui::SliderFloat("Right (m)", &m_headOffset[0], -0.3F, 0.3F, "%.2f");
                ImGui::SliderFloat("Up (m)", &m_headOffset[1], -0.3F, 0.3F, "%.2f");
                ImGui::SliderFloat("Forward (m)", &m_headOffset[2], -0.3F, 0.3F, "%.2f");
                ImGui::SliderInt("Head pose delay (frames)", &m_poseLag, 1, 4);
            }
            if (ImGui::CollapsingHeader("HUD")) {
                ImGui::Checkbox("Hide gameplay HUD", &config.hideGameplayHud);
                ImGui::Checkbox("Room-anchored HUD panel", &m_hudLockOn);
                ImGui::Checkbox("Center the marked HUD element", &config.stabilizeHudMarker);
                ImGui::SliderFloat("Size", &m_hudScale, 0.2F, 1.2F, "%.2f");
                ImGui::SliderFloat("Horizontal offset", &m_hudShiftX, -0.6F, 0.6F, "%.2f");
                ImGui::SliderFloat("Vertical offset", &m_hudShiftY, -0.6F, 0.6F, "%.2f");
                ImGui::SliderFloat("Distance (m)", &m_hudDistance, 0.4F, 6.0F, "%.2f");
            }
            if (ImGui::CollapsingHeader("Screen and stereo depth")) {
                ImGui::SliderFloat("Stereo depth", &m_stereoScale, 0.0F, 3.0F, "%.2f");
                ImGui::SliderFloat("Virtual screen crop", &m_fovCrop, 0.4F, 1.0F, "%.2f");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Weapon and optics")) {
            ImGui::SeparatorText("Weapon");
            ImGui::Checkbox("Keep weapon aligned with body", &m_weaponLockOn);
            if (ImGui::Checkbox("Lean to inspect weapon", &config.weaponHeadPosition)) {
                if (config.weaponHeadPosition) {
                    m_headPositionOn = true;
                }
                m_renderEye = -1;
                m_dibr.ResetHistory();
                Log("WEAPON inspection mode %s", config.weaponHeadPosition ? "on" : "off");
            }
            if (config.weaponHeadPosition) {
                ImGui::TextDisabled("Available on foot at normal zoom in full stereo.");
            }
            ImGui::SliderFloat("Weapon size", &m_weaponSize, 0.2F, 1.5F, "%.2f");
            ImGui::SliderFloat("Weapon stereo depth", &m_weaponScale, 0.0F, 2.0F, "%.2f");
            ImGui::SliderFloat("Zero parallax distance (m)", &m_weaponConvergence, 0.2F, 5.0F, "%.2f");

            ImGui::SeparatorText("Optics");
            ImGui::Checkbox("Magnified optic window", &config.scopeLens);
            ImGui::SliderFloat("Optic window size (degrees)", &config.scopeLensDegrees, 8.0F, 70.0F, "%.0f");
            ImGui::Checkbox("Weapon-aligned optic reticle", &config.scopeReticle);
            ImGui::Checkbox("Use mouse aim through magnified optics", &config.scopeStatic);
            ImGui::Checkbox("Center the weapon in both eyes when scoped", &config.scopeWeaponFlat);

            if (ImGui::CollapsingHeader("Body clipping")) {
                ImGui::Checkbox("Hide torso and vest (experimental)", &config.firstPersonHideTorso);
                ImGui::SliderFloat("Near body clipping (cm)", &config.firstPersonClipCm,
                                   0.0F, 18.0F, "%.0f");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Diagnostics")) {
            ImGui::SeparatorText("Capture");
            if (m_quadWidth && m_quadHeight) {
                ImGui::Text("VR source  %u x %u", m_quadWidth, m_quadHeight);
            }
            if (m_eyeRecommendedW && m_eyeRecommendedH) {
                ImGui::Text("OpenXR recommended per eye  %u x %u", m_eyeRecommendedW, m_eyeRecommendedH);
            }
            ImGui::BeginDisabled(m_worldViewState != 1 || !m_sbsInput || !m_projectionEnabled);
            if (ImGui::Button("Stereo screenshot")) {
                RequestStereoShots(1);
            }
            ImGui::SameLine();
            if (ImGui::Button("Capture 20 stereo screenshots")) {
                RequestStereoShots(20);
            }
            ImGui::EndDisabled();

            if (config.frameProbe && ImGui::CollapsingHeader("Shader diagnostics")) {
                int browseMode = ShaderBrowseMode();
                const int browseBefore = browseMode;
                ImGui::RadioButton("Off", &browseMode, 0);
                ImGui::RadioButton("Screen-space shaders (shadows, light, glow)", &browseMode, 1);
                ImGui::RadioButton("HUD icons and squad markers", &browseMode, 2);
                if (browseMode != browseBefore) {
                    ShaderBrowseSetMode(browseMode);
                }
                ImGui::TextDisabled("Ctrl+F1 cycles through the modes.");
                ImGui::TextWrapped("Close settings. PgUp/PgDn skips the selected screen-space shader "
                                   "so you can see which effect it draws. When you find a broken effect, "
                                   "press Del to record the shader in "
                                   "arma3_native_vr.shaders.txt.");
                if (ImGui::Button("Previous")) {
                    ShaderBrowseStep(-1);
                }
                ImGui::SameLine();
                if (ImGui::Button("Next")) {
                    ShaderBrowseStep(1);
                }
                ImGui::SameLine();
                if (ImGui::Button("Mark shader")) {
                    ShaderBrowseMark();
                }
                ImGui::TextUnformatted(ShaderBrowseDescribe().c_str());
                ImGui::Separator();
                ImGui::TextUnformatted("Marked shaders");
                for (const std::string& line : ShaderBrowseMarkedLines()) {
                    ImGui::BulletText("%s", line.c_str());
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();
    ImGui::EndChild();

    ImGui::Separator();
    static double savedAt = -10.0;
    if (ImGui::Button("Save settings")) {
        SaveAllSettings();
        savedAt = ImGui::GetTime();
    }
    if (config.frameProbe) {
        ImGui::SameLine();
        if (ImGui::Button("Capture settings panel")) {
            g_menuScreenshotRequested = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Writes arma3_native_vr.menu.jpg in the game folder.");
        }
    }
    if (ImGui::GetTime() - savedAt < 3.0) {
        ImGui::SameLine();
        ImGui::TextDisabled("Saved");
    }
    const char* closeLabel = "Close (Insert or Esc)";
    const float closeWidth = ImGui::CalcTextSize(closeLabel).x + 2.0F * style.FramePadding.x;
    ImGui::SameLine(rightEdge - closeWidth);
    if (ImGui::Button(closeLabel)) {
        Menu::Instance().SetOpen(false);
    }
    ImGui::End();
}

}  // namespace a3vr
