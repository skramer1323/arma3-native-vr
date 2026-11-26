#pragma once

#include "Dibr.h"
#include "DlssWorld.h"
#include "StaleEyeWarp.h"
#include "StereoCas.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include "Calibration.h"
#include "Config.h"
#include "HeadTracking.h"

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>

#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace a3vr {

// In-process OpenXR session driven from IDXGISwapChain::Present (rungs 2-3).
// Everything runs on the thread that presents; m_mutex only guards against
// ResizeBuffers arriving on another thread.
class OpenXRRuntime final {
public:
    static OpenXRRuntime& Instance();

    // Called before the game's Present reaches DXGI.
    // True when the next OpenXR wait should run after DXGI Present returns.
    bool OnPresent(IDXGISwapChain* swapChain);
    bool IsMainSwapChain(IDXGISwapChain* swapChain);
    void OnAfterPresent(IDXGISwapChain* swapChain, double desktopPresentMs, UINT syncInterval);
    // Called before the game's ResizeBuffers reaches DXGI.
    void OnResize(IDXGISwapChain* swapChain, UINT width, UINT height);

private:
    bool AttachDevice(IDXGISwapChain* swapChain);
    bool CreateSession();
    void DestroySession();
    void PollEvents();
    void ReadWorldVisibility(const XrView (&views)[2], XrQuaternionf headOrientation);
    void ApplyLocalSpaceChanges();
    void HandleSessionState(XrSessionState state);
    void PrepareNextFrame();
    void PrepareFollowingFrame();
    bool SubmitPreparedFrame(ID3D11Texture2D* backBuffer, bool withQuad);
    void SaveProjectionEyeImages(ID3D11Texture2D* frame, int renderedEye, bool synthetic, bool fullEye,
                                 int warpedEye);
    void WriteEyeImages(ID3D11Texture2D* frame, int renderedEye, bool synthetic, bool fullEye,
                        int warpedEye, const std::wstring& name, unsigned& mask, int probeId = 0);
    void RequestStereoShots(int pairs);
    struct OpticsSubmitTrace {
        const char* source = "none";
        float unzoomedEyeSpan = 0.0F;
        float windowDivisor = 0.0F;
        int lensPass = 0;
        bool lensSubmitted = false;
    };
    void TraceSubmittedFrame(int renderedEye, std::uint32_t matchedPose, std::uint32_t layerCount,
                             const XrCompositionLayerProjectionView views[2], XrResult result,
                             double submitMs, bool dlssSubmitted, bool staleWarpSubmitted,
                             const char* viewKind, const char* viewSpace, std::uint32_t drawPoseId,
                             float submittedRoll, const OpticsSubmitTrace& optics);
    struct RenderPose;
    bool EnsureQuadSwapchain(const D3D11_TEXTURE2D_DESC& backBuffer);
    void DestroyQuadSwapchain();
    bool CopyIntoQuad(ID3D11Texture2D* backBuffer, const D3D11_TEXTURE2D_DESC& description,
                      bool right);
    bool EnsureRightSwapchain();
    bool EnsureAuxSwapchain(UINT width, XrSwapchain& chain,
                            std::vector<XrSwapchainImageD3D11KHR>& images);
    bool CopyIntoSwapchain(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& description,
                           XrSwapchain chain, const std::vector<XrSwapchainImageD3D11KHR>& images,
                           const D3D11_BOX* sourceBox = nullptr);
    bool CopyProjectionEyes(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& description);
    void SampleSteamVrScene(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& description,
                            bool reprojected);
    void LogSteamVrSceneSample();
    bool RenderDibrEye(ID3D11Texture2D* frame, const RenderPose& pose, const float matrices[32],
                       int renderedEye, XrQuaternionf currentCamera, XrQuaternionf staleCamera,
                       XrCompositionLayerProjectionView& syntheticView);
    void MeasureEyeSeparation(int eye);
    bool RenderDepthStereo(ID3D11Texture2D* backBuffer, bool flat);
    ID3D11RenderTargetView* EyeTarget(ID3D11Texture2D* texture);
    void Recenter(XrTime time);
    bool BuildCursorLayer(const XrCompositionLayerQuad& screen, XrCompositionLayerQuad& cursor);
    HCURSOR ReadGameCursor(POINT& point, int& width, int& height);
    bool DrawCursorIntoScreen(ID3D11Texture2D* image);
    bool BuildHudLayer(XrCompositionLayerQuad& hud, bool inventory);
    bool UploadCursor(HCURSOR handle);
    void PublishHeadPose();
    bool ReadGameFov(float& tanHalfHorizontal, float& tanHalfVertical);

    void BuildWorldView(XrCompositionLayerQuad& quad, const RenderPose& pose, int eye) const;
    void HandleHotkeys();
    void LogStats();
    bool Check(XrResult result, const char* operation) const;

    std::mutex m_mutex;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    IDXGISwapChain* m_mainSwapChain = nullptr;  // identity only, not owned
    DWORD m_presentThread = 0;
    std::uint32_t m_foreignPresents = 0;

    XrInstance m_instance = XR_NULL_HANDLE;
    PFN_xrGetVisibilityMaskKHR m_getVisibilityMask = nullptr;
    bool m_visibilityPending = true;
    bool m_steamVr = false;
    bool m_steamVrSceneSampleStarted = false;
    bool m_steamVrSceneSampleReprojected = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_steamVrSceneSample;
    XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_localSpace = XR_NULL_HANDLE;
    XrSpace m_viewSpace = XR_NULL_HANDLE;
    struct LocalSpaceChange {
        XrTime time;
        XrPosef newInOld;
        bool valid;
    };
    XrPosef m_localSpaceOffset{{0.0F, 0.0F, 0.0F, 1.0F}, {}};
    std::deque<LocalSpaceChange> m_localSpaceChanges;
    XrSessionState m_sessionState = XR_SESSION_STATE_UNKNOWN;
    bool m_sessionRunning = false;
    bool m_framePrepared = false;
    bool m_shouldRender = false;
    bool m_lookingDown = false;
    int m_renderEye = -1;
    bool m_alternateEyeProjection = false;
    bool m_dibrShift = false;
    int m_dibrFill = 1;  // DIBR hole fill: 0 magenta (diagnostic), 1 stale eye + background, 2 background
    XrSwapchain m_dibrSwapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> m_dibrImages;
    Dibr m_dibr;
    StaleEyeWarp m_staleEye;
    bool m_stalePairValid = false;
    bool m_staleReported = false;
    unsigned m_staleFallbacks = 0;
    XrCompositionLayerProjectionView m_stalePairViews[2]{};
    DlssWorld m_dlssWorld;
    StereoCas m_stereoCas;
    bool m_dibrReported = false;
    int m_eyeCaptureId = 0;
    unsigned m_eyeCaptureMask = 0;
    std::uint64_t m_eyeCaptureStart = 0;
    int m_stereoShotsRemaining = 0;
    int m_stereoShotsWritten = 0;
    unsigned m_stereoShotMask = 0;
    std::uint64_t m_stereoShotRequest = 0;
    XrCompositionLayerProjectionView m_retainedViews[2]{};
    // Scope lens: the latest world frame and the latest lens frame (two layers, alternating frames).
    XrCompositionLayerProjectionView m_lensWorldViews[2]{};
    XrCompositionLayerProjectionView m_lensWindowViews[2]{};
    bool m_lensWorldValid = false;
    bool m_lensWindowValid = false;
    std::function<void(ID3D11Texture2D*)> m_afterCopy;  // runs on the swapchain image before release
    XrQuaternionf m_retainedCamera[2]{};
    XrTime m_displayTime = 0;
    XrDuration m_displayPeriod = 0;
    ULONGLONG m_nextSessionAttempt = 0;
    std::uint32_t m_sessionFailures = 0;

    XrSwapchain m_quadSwapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> m_quadImages;
    XrSwapchain m_projectionSwapchains[2]{};
    std::vector<XrSwapchainImageD3D11KHR> m_projectionImages[2];
    std::uint32_t m_quadWidth = 0;
    std::uint32_t m_quadHeight = 0;
    DXGI_FORMAT m_quadFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT m_quadSourceFormat = DXGI_FORMAT_UNKNOWN;
    bool m_quadFormatRejected = false;
    bool m_loggedFirstQuad = false;

    // Live tuning values (start from the INI).
    float m_quadWidthMeters = 2.4F;
    float m_quadDistanceMeters = 2.0F;
    float m_quadHeightMeters = 0.0F;
    float m_hudScale = 0.6F;
    float m_hudShiftX = 0.0F;
    float m_hudShiftY = 0.0F;
    float m_hudDistance = 2.0F;
    float m_fovCrop = 0.75F;
    bool m_projectionEnabled = true;
    bool m_aiming = false;       // sight line locked (ADS)
    int m_aimOffFrames = 0;
    bool m_adsMouseDown = false;
    bool m_adsMouseLatched = false;
    ULONGLONG m_adsMousePress = 0;
    bool m_weaponLockOn = true;    // Ctrl+F5
    float m_headOffset[3]{0.0F, 0.0F, 0.0F};  // right, up, forward (m): Ctrl+arrows/PgUp/PgDn
    // Two saved placements: on foot and in a vehicle. m_headOffset is the active one being edited;
    // m_appliedOffset eases towards it so a switch is not a jump.
    float m_offsetFoot[3]{0.0F, 0.0F, 0.0F};
    float m_offsetVehicle[3]{0.0F, 0.0F, 0.0F};
    float m_appliedOffset[3]{0.0F, 0.0F, 0.0F};
    bool m_vehicleMode = false;
    unsigned m_eyeRecommendedW = 0;  // headset per-eye size from OpenXR (frame size slider)
    unsigned m_eyeRecommendedH = 0;
    unsigned m_frameMaxWidth = 8192;
    unsigned m_frameMaxHeight = 8192;
    std::pair<unsigned, unsigned> m_startupSceneRequest{};
    std::pair<unsigned, unsigned> m_reportedSceneExtent{};
    bool m_lensParity = false;  // alternates world/lens frames while a scope is up
    bool m_nativeValid[2]{false, false};  // AER native: each eye has a real native frame
    int m_vehicleForce = 0;  // 0 automatic, 1 on foot, 2 vehicle (Ctrl+Insert cycles)
    int m_noWeaponFrames = 0;
    int m_weaponFrames = 0;
    void UpdateHeadPlacement();
    // Settings menu (Insert): panel in the room, widgets in OpenXRMenu.cpp.
    bool EnsureMenuSwapchain();
    unsigned MenuPixelScale() const;
    bool RenderMenuLayer(XrCompositionLayerQuad& layer);
    void DrawMenuUi();
    void PlaceMenu();
    void SaveAllSettings();
    bool m_vrsRadiusPreview = false;
    XrSwapchain m_menuSwapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> m_menuImages;
    XrPosef m_menuPose{{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, -1.3F}};
    bool m_hudLockOn = true;       // Ctrl+F6
    bool m_headPositionOn = true;  // Ctrl+F7
    float m_adsBlend = 0.0F;     // 0..1, eased in and out
    XrVector3f m_headOrigin{};   // head position at recentre (LOCAL space)
    float m_adsLean[3]{};        // body-frame head offset when aiming started
    float m_adsYaw = 0.0F;       // real head angles when aiming started
    float m_adsPitch = 0.0F;
    float m_weaponScale = 0.3F;
    float m_weaponConvergence = 0.7F;
    float m_weaponSize = 0.6F;
    int m_poseLag = 1;  // Pose matching delay in frames.
    bool m_tuningInitialized = false;

    // World-fixed quad pose in LOCAL space (QuadAnchor=1).
    XrPosef m_worldQuadPose{{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, -2.0F}};
    bool m_worldQuadValid = false;
    bool m_recenterRequested = true;

    // Statistics.
    std::uint64_t m_presents = 0;
    std::uint64_t m_framesSubmitted = 0;
    std::uint64_t m_quadFramesSubmitted = 0;
    double m_waitMsSum = 0.0;
    double m_submitMsSum = 0.0;
    double m_maxPresentGapMs = 0.0;
    double m_lastPresentMs = 0.0;
    double m_presentGapMs = 0.0;
    double m_frameWaitMs = 0.0;
    std::uint64_t m_steamVrLastWorldPresent = 0;
    unsigned m_steamVrWorldPresents = 0;
    unsigned m_steamVrLongGaps = 0;
    UINT m_steamVrDesktopSyncInterval = 0;
    double m_steamVrDesktopPresentSum = 0.0;
    double m_steamVrDesktopPresentMax = 0.0;
    bool m_frameTraceAttempted = false;
    int m_opticsProbeCapture = 0;
    std::ofstream m_frameTrace;
    std::uint64_t m_notRendered = 0;
    std::uint64_t m_cursorFrames = 0;

    // Windows cursor shown as a second quad (DrawCursor=1).
    HWND m_window = nullptr;
    XrSwapchain m_hudSwapchain = XR_NULL_HANDLE;  // the HUD quad (room-fixed panel)
    ULONGLONG m_hudRetryAfter = 0;
    std::vector<XrSwapchainImageD3D11KHR> m_hudImages;
    UINT m_hudWidth = 0;
    UINT m_hudHeight = 0;
    int m_hudLogs = 0;
    XrSwapchain m_cursorSwapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> m_cursorImages;
    HCURSOR m_cursorHandle = nullptr;
    ULONGLONG m_cursorRetryAfter = 0;
    HCURSOR m_screenCursorHandle = nullptr;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_screenCursorView;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> m_screenCursorContextState;
    int m_screenCursorWidth = 0;
    int m_screenCursorHeight = 0;
    int m_screenCursorHotX = 0;
    int m_screenCursorHotY = 0;
    bool m_cursorUsable = false;
    int m_cursorWidth = 0;
    int m_cursorHeight = 0;
    int m_cursorHotX = 0;
    int m_cursorHotY = 0;
    int m_cursorVisibleState = -1;
    std::uint32_t m_cursorLogs = 0;
    ULONGLONG m_statsStart = 0;

    // Head look (HeadTracking=1).
    struct RenderPose {
        XrPosef head{{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 0.0F}};
        float yaw = 0.0F;    // radians, real head angle after the limits
        // Scope-lens frame: the engine aims exactly along the sight; the picture is placed in the
        // room at the sight direction (fixed to the weapon, follows the mouse, not the head).
        bool lens = false;
        bool scopeStatic = false;  // magnified optic up: view glued to the face, head ignored
        bool sightActive = false;  // optics input or ADS transition for this render
        XrQuaternionf lensOrientation{0.0F, 0.0F, 0.0F, 1.0F};
        float pitch = 0.0F;
        float freeTrackYaw = 0.0F;
        float freeTrackPitch = 0.0F;
        bool valid = false;
        int eye = 0;  // -1 left, +1 right, 0 mono
        std::uint32_t sequence = 0;  // FreeTrack dataId this pose was published with
        std::uint64_t headReferenceEpoch = 0;
        // Projection layer mode: headset frustum per eye (in head axes) and half eye
        // separation used to draw this frame.
        XrFovf fov[2]{};
        float halfIpd = 0.0F;
        bool physicalEyes = false;
        XrVector3f halfBaseline{};  // Center to right eye, in the render camera's OpenXR axes.
        float aimBias = 0.0F;
        float clipWOffset = 0.0F;
        XrVector3f leanForward{};
        XrVector3f projectionCenter{};
        bool viewsValid = false;
        bool projection = false;
        float tanHorizontal = 1.0F;
        float tanVertical = 0.75F;
    };
    // Alternate-eye stereo (StereoMode=1).
    XrSwapchain m_rightSwapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> m_rightImages;
    RenderPose m_eyePose[2];  // [0] left, [1] right: the last frame shown to each eye
    bool m_stereoEnabled = true;
    bool m_stereoSwap = false;
    float m_stereoShiftMillimeters = 30.0F;
    float m_stereoScale = 1.0F;
    TrackerCalibration m_calibration;
    // Depth-based stereo on the screen (DepthStereo=1).
    std::unordered_map<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11RenderTargetView>>
        m_eyeTargets;
    float m_convergenceMeters = 1.0F;
    int m_depthReversed = -1;
    bool m_depthDebugView = false;
    bool m_sbsInput = false;
    std::uint64_t m_sbsFrames = 0;
    float m_loggedNear = 0.0F;
    std::uint32_t m_nearLogs = 0;
    std::uint64_t m_depthFrames = 0;
    bool m_calibrationTried = false;
    std::uint64_t m_worldFramesTotal = 0;
    std::uint64_t m_publishCount = 0;
    std::uint64_t m_stereoFrames = 0;
    double m_previousCameraPosition[3]{};
    int m_previousCameraEye = 0;
    double m_separationSum = 0.0;
    std::uint32_t m_separationSamples = 0;
    FreeTrackFeed m_feed;
    std::uint64_t m_headReferenceEpoch = 0;
    RenderPose m_poseRing[8];  // [0] = newest published pose
    std::uint32_t m_poseHits[9]{};  // frames shown with ring pose i; [8] = no match
    XrQuaternionf m_headReference{0.0F, 0.0F, 0.0F, 1.0F};
    bool m_headReferenceValid = false;
    bool m_headRecenterRequested = true;
    bool m_worldViewEnabled = true;
    int m_worldViewState = -1;
    RenderPose m_lastConsumedPose{};
    bool m_inventoryOpen = false;
    bool m_inventoryPresented = false;
    std::uint64_t m_worldFrames = 0;
    std::uint32_t m_lastClientCalls = 0;
    std::uint32_t m_fovLogs = 0;
    float m_loggedTanHorizontal = 0.0F;
    float m_lastTanHorizontal = 0.0F;
    float m_lastTanVertical = 0.0F;
    void (*m_gameProjectionView)(float*) = nullptr;
    bool m_gameProjectionViewSearched = false;

    PFN_xrGetD3D11GraphicsRequirementsKHR m_getD3D11Requirements = nullptr;
};

}  // namespace a3vr
