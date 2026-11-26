#include "OpenXRRuntime.h"
#include "ProjectionCache.h"

#include "Perf.h"
#include "Config.h"
#include "CursorImage.h"
#include "DepthStereo.h"
#include "EngineStereo.h"
#include "FrameProbe.h"
#include "LensMask.h"
#include "Menu.h"
#include "ResolutionHook.h"
#include "Log.h"
#include "QuadMath.h"
#include "WorldShadingRate.h"
#include "XrPoseMath.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace a3vr {
namespace {

bool LuidMatches(const LUID& left, const LUID& right)
{
    return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
}

bool IsTypeless(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R8G8B8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
           format == DXGI_FORMAT_B8G8R8X8_TYPELESS;
}

DXGI_FORMAT UnormOf(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    default:
        return format;
    }
}

// Yaw-only orientation of a pose (Y up, -Z forward in OpenXR).
XrQuaternionf YawOnly(const XrQuaternionf& q)
{
    // Forward vector = q * (0,0,-1).
    const float fx = -2.0F * (q.x * q.z + q.w * q.y);
    const float fz = -(1.0F - 2.0F * (q.x * q.x + q.y * q.y));
    const float yaw = std::atan2(-fx, -fz);
    return {0.0F, std::sin(yaw * 0.5F), 0.0F, std::cos(yaw * 0.5F)};
}

XrCompositionLayerQuad QuadFromProjectionView(const XrCompositionLayerProjectionView& view,
                                             XrSpace space, XrEyeVisibility eye)
{
    // A distant plane preserves the submitted frustum under rotation and limits
    // the apparent shift when the headset moves before the next frame.
    constexpr float distance = 20.0F;
    const auto plane = FrustumQuadAtDistance(view.fov.angleLeft, view.fov.angleRight,
                                             view.fov.angleUp, view.fov.angleDown, distance);
    const XrVector3f offset = Rotate(view.pose.orientation, {plane.centerX, plane.centerY, -distance});
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = space;
    quad.eyeVisibility = eye;
    quad.subImage = view.subImage;
    quad.pose = view.pose;
    quad.pose.position.x += offset.x;
    quad.pose.position.y += offset.y;
    quad.pose.position.z += offset.z;
    quad.size = {plane.width, plane.height};
    return quad;
}

constexpr UINT kSceneSamplesPerAxis = 3;

double NowMs()
{
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return 1000.0 * static_cast<double>(counter.QuadPart) /
           static_cast<double>(frequency.QuadPart);
}

bool IsBgra(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_B8G8R8X8_UNORM || format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

// Positive yaw turns left (counter-clockwise seen from above), positive pitch looks up.
XrQuaternionf YawPitch(float yaw, float pitch)
{
    const XrQuaternionf aroundY{0.0F, std::sin(yaw * 0.5F), 0.0F, std::cos(yaw * 0.5F)};
    const XrQuaternionf aroundX{std::sin(pitch * 0.5F), 0.0F, 0.0F, std::cos(pitch * 0.5F)};
    return Multiply(aroundY, aroundX);
}

bool WindowsCursorShowing()
{
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    return GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING) != 0;
}

constexpr float kDegrees = 57.29578F;

bool KeyEdge(int key, bool& previous)
{
    const bool now = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool edge = now && !previous;
    previous = now;
    return edge;
}

bool KeyHeld(int key)
{
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

}  // namespace

OpenXRRuntime& OpenXRRuntime::Instance()
{
    static OpenXRRuntime runtime;
    return runtime;
}

bool OpenXRRuntime::Check(XrResult result, const char* operation) const
{
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    char name[XR_MAX_RESULT_STRING_SIZE]{};
    if (m_instance != XR_NULL_HANDLE) {
        xrResultToString(m_instance, result, name);
    }
    Log("OpenXR error while %s: %d %s", operation, static_cast<int>(result), name);
    return false;
}

bool OpenXRRuntime::AttachDevice(IDXGISwapChain* swapChain)
{
    DXGI_SWAP_CHAIN_DESC swapDescription{};
    if (FAILED(swapChain->GetDesc(&swapDescription))) {
        return false;
    }
    if (swapDescription.BufferDesc.Width < 640) {
        Log("Ignoring small swapchain %p (%ux%u)", swapChain, swapDescription.BufferDesc.Width,
            swapDescription.BufferDesc.Height);
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device)))) {
        Log("Swapchain %p has no D3D11 device; ignored", swapChain);
        return false;
    }
    m_device = device;
    m_device->GetImmediateContext(&m_context);
    m_mainSwapChain = swapChain;
    m_presentThread = GetCurrentThreadId();

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDescription{};
    if (SUCCEEDED(m_device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        adapter->GetDesc(&adapterDescription);
    }
    Log("RUNG1 main swapchain %p: %ux%u format=%d buffers=%u swapEffect=%d windowed=%d "
        "msaa=%u refresh=%u/%u flags=0x%X hwnd=%p",
        swapChain, swapDescription.BufferDesc.Width, swapDescription.BufferDesc.Height,
        static_cast<int>(swapDescription.BufferDesc.Format), swapDescription.BufferCount,
        static_cast<int>(swapDescription.SwapEffect), swapDescription.Windowed ? 1 : 0,
        swapDescription.SampleDesc.Count, swapDescription.BufferDesc.RefreshRate.Numerator,
        swapDescription.BufferDesc.RefreshRate.Denominator, swapDescription.Flags,
        swapDescription.OutputWindow);
    {
        // Mouse coordinates are window pixels: the window client area has to be the swapchain size.
        RECT client{};
        RECT window{};
        GetClientRect(swapDescription.OutputWindow, &client);
        GetWindowRect(swapDescription.OutputWindow, &window);
        Log("RESOLUTION window client %ldx%ld at (%ld,%ld), swapchain %ux%u %s",
            client.right, client.bottom, window.left, window.top,
            swapDescription.BufferDesc.Width, swapDescription.BufferDesc.Height,
            (client.right == static_cast<LONG>(swapDescription.BufferDesc.Width) &&
             client.bottom == static_cast<LONG>(swapDescription.BufferDesc.Height))
                ? "match" : "MISMATCH (the cursor will be off)");
    }
    Log("RUNG1 D3D11 device %p featureLevel=0x%X createFlags=0x%X adapter=\"%ls\" "
        "presentThread=%lu",
        m_device.Get(), static_cast<unsigned>(m_device->GetFeatureLevel()),
        m_device->GetCreationFlags(), adapterDescription.Description, m_presentThread);
    m_statsStart = GetTickCount64();
    return true;
}

bool OpenXRRuntime::CreateSession()
{
    if (m_instance == XR_NULL_HANDLE) {
        std::uint32_t layerCount = 0;
        xrEnumerateApiLayerProperties(0, &layerCount, nullptr);
        std::vector<XrApiLayerProperties> layers(layerCount, {XR_TYPE_API_LAYER_PROPERTIES});
        xrEnumerateApiLayerProperties(layerCount, &layerCount, layers.data());
        for (const XrApiLayerProperties& layer : layers) {
            Log("OpenXR API layer available: %s (VectorXR/other layers should be off for "
                "lab tests)",
                layer.layerName);
        }

        const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
                                    XR_KHR_VISIBILITY_MASK_EXTENSION_NAME};
        std::uint32_t extensionCount = 0;
        bool haveVisibilityMask = false;
        if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr))) {
            std::vector<XrExtensionProperties> available(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
            if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount,
                                                                    &extensionCount, available.data()))) {
                haveVisibilityMask = std::any_of(available.begin(), available.end(), [](const auto& ext) {
                    return std::strcmp(ext.extensionName, XR_KHR_VISIBILITY_MASK_EXTENSION_NAME) == 0;
                });
            }
        }
        XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        strncpy_s(createInfo.applicationInfo.applicationName, "Arma3NativeVR",
                  XR_MAX_APPLICATION_NAME_SIZE - 1);
        createInfo.applicationInfo.applicationVersion = 1;
        strncpy_s(createInfo.applicationInfo.engineName, "RealVirtuality4",
                  XR_MAX_ENGINE_NAME_SIZE - 1);
        createInfo.applicationInfo.engineVersion = 1;
        // Virtual Desktop rejects apiVersion 1.1 (KCD1VR trap): ask for 1.0.0.
        createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        createInfo.enabledExtensionCount = haveVisibilityMask ? 2U : 1U;
        createInfo.enabledExtensionNames = extensions;
        XrResult created = xrCreateInstance(&createInfo, &m_instance);
        if (created == XR_ERROR_EXTENSION_NOT_PRESENT && haveVisibilityMask) {
            haveVisibilityMask = false;
            createInfo.enabledExtensionCount = 1;
            created = xrCreateInstance(&createInfo, &m_instance);
        }
        if (!Check(created, "creating instance")) {
            m_instance = XR_NULL_HANDLE;
            return false;
        }
        m_getVisibilityMask = nullptr;
        if (haveVisibilityMask) {
            PFN_xrVoidFunction proc = nullptr;
            if (XR_SUCCEEDED(xrGetInstanceProcAddr(m_instance, "xrGetVisibilityMaskKHR", &proc))) {
                m_getVisibilityMask = reinterpret_cast<PFN_xrGetVisibilityMaskKHR>(proc);
            }
        }
        Log("World VRS OpenXR visibility mask: %s", m_getVisibilityMask ? "available" : "unavailable");
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &properties))) {
            m_steamVr = std::strncmp(properties.runtimeName, "SteamVR", 7) == 0;
            Log("RUNG2 OpenXR runtime: %s %u.%u.%u; requested API 1.0.0",
                properties.runtimeName, XR_VERSION_MAJOR(properties.runtimeVersion),
                XR_VERSION_MINOR(properties.runtimeVersion),
                XR_VERSION_PATCH(properties.runtimeVersion));
        }
        if (!Check(xrGetInstanceProcAddr(
                       m_instance, "xrGetD3D11GraphicsRequirementsKHR",
                       reinterpret_cast<PFN_xrVoidFunction*>(&m_getD3D11Requirements)),
                   "loading xrGetD3D11GraphicsRequirementsKHR")) {
            return false;
        }
    }

    if (m_systemId == XR_NULL_SYSTEM_ID) {
        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (!Check(xrGetSystem(m_instance, &systemInfo, &m_systemId),
                   "getting HMD system (headset connected? Virtual Desktop streaming?)")) {
            m_systemId = XR_NULL_SYSTEM_ID;
            return false;
        }
    }

    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!Check(m_getD3D11Requirements(m_instance, m_systemId, &requirements),
               "getting D3D11 requirements")) {
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDescription{};
    if (FAILED(m_device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&adapterDescription))) {
        Log("Unable to query the game's DXGI adapter");
        return false;
    }
    if (!LuidMatches(adapterDescription.AdapterLuid, requirements.adapterLuid)) {
        Log("OpenXR runtime wants a different GPU than the game uses; VR stays off");
        return false;
    }

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = m_device.Get();
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = m_systemId;
    if (!Check(xrCreateSession(m_instance, &sessionInfo, &m_session), "creating session")) {
        m_session = XR_NULL_HANDLE;
        return false;
    }
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0F;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!Check(xrCreateReferenceSpace(m_session, &spaceInfo, &m_localSpace),
               "creating LOCAL space")) {
        DestroySession();
        return false;
    }
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Check(xrCreateReferenceSpace(m_session, &spaceInfo, &m_viewSpace),
               "creating VIEW space")) {
        DestroySession();
        return false;
    }
    Log("RUNG2 OpenXR session created on \"%ls\" (diagnostic mode %d)",
        adapterDescription.Description, GetConfig().openXrDiagnosticMode);
    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(m_instance, m_systemId, &systemProperties))) {
        m_frameMaxWidth = std::min(8192U, systemProperties.graphicsProperties.maxSwapchainImageWidth);
        m_frameMaxHeight = std::min(8192U, systemProperties.graphicsProperties.maxSwapchainImageHeight);
    }
    // The runtime recommends the eye render size after lens distortion and its scaling settings.
    {
        std::uint32_t viewCount = 0;
        xrEnumerateViewConfigurationViews(m_instance, m_systemId,
                                          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount,
                                          nullptr);
        std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        if (viewCount >= 1 &&
            XR_SUCCEEDED(xrEnumerateViewConfigurationViews(
                m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount,
                &viewCount, views.data()))) {
            unsigned maxWidth = 8192, maxHeight = 8192;
            m_eyeRecommendedW = m_eyeRecommendedH = 0;
            for (const auto& view : views) {
                maxWidth = std::min(maxWidth, view.maxImageRectWidth);
                maxHeight = std::min(maxHeight, view.maxImageRectHeight);
                m_eyeRecommendedW = std::max(m_eyeRecommendedW, view.recommendedImageRectWidth);
                m_eyeRecommendedH = std::max(m_eyeRecommendedH, view.recommendedImageRectHeight);
            }
            m_eyeRecommendedW = std::min(m_eyeRecommendedW, maxWidth);
            m_eyeRecommendedH = std::min(m_eyeRecommendedH, maxHeight);
            const auto [frameWidth, frameHeight] = StereoFrameSize(m_eyeRecommendedW, m_eyeRecommendedH,
                100, m_frameMaxWidth, m_frameMaxHeight);
            Log("RESOLUTION OpenXR eye target %ux%u, 16:9 stereo frame %ux%u (auto=%d)",
                m_eyeRecommendedW, m_eyeRecommendedH, frameWidth, frameHeight,
                GetConfig().autoHeadsetResolution ? 1 : 0);
        }
    }
    return true;
}

void OpenXRRuntime::DestroySession()
{
    SetWorldVisibilityContours({});
    m_visibilityPending = true;
    m_dlssWorld.ResetHistory();
    m_staleEye.ResetHistory();
    m_steamVrSceneSample.Reset();
    m_steamVrSceneSampleStarted = false;
    m_hudRetryAfter = m_cursorRetryAfter = 0;
    DestroyQuadSwapchain();
    if (m_viewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(m_viewSpace);
        m_viewSpace = XR_NULL_HANDLE;
    }
    if (m_localSpace != XR_NULL_HANDLE) {
        xrDestroySpace(m_localSpace);
        m_localSpace = XR_NULL_HANDLE;
    }
    if (m_session != XR_NULL_HANDLE) {
        xrDestroySession(m_session);
        m_session = XR_NULL_HANDLE;
    }
    m_sessionRunning = false;
    m_framePrepared = false;
    m_sessionState = XR_SESSION_STATE_UNKNOWN;
    m_localSpaceChanges.clear();
    m_localSpaceOffset = kIdentityXrPose;
    m_worldQuadValid = false;
    m_recenterRequested = true;
}

void OpenXRRuntime::PollEvents()
{
    while (m_instance != XR_NULL_HANDLE) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult result = xrPollEvent(m_instance, &event);
        if (result == XR_EVENT_UNAVAILABLE || !Check(result, "polling events")) {
            return;
        }
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            HandleSessionState(
                reinterpret_cast<const XrEventDataSessionStateChanged*>(&event)->state);
        } else if (event.type == XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR) {
            const auto& change = *reinterpret_cast<const XrEventDataVisibilityMaskChangedKHR*>(&event);
            if (change.session == m_session &&
                change.viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
                SetWorldVisibilityContours({});
                m_visibilityPending = true;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            const auto& change =
                reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(event);
            if (change.session == m_session && change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                const auto next = std::upper_bound(m_localSpaceChanges.begin(), m_localSpaceChanges.end(),
                    change.changeTime, [](XrTime time, const LocalSpaceChange& pending) {
                        return time < pending.time;
                    });
                const bool valid = change.poseValid == XR_TRUE;
                m_localSpaceChanges.insert(next,
                    {change.changeTime, valid ? change.poseInPreviousSpace : kIdentityXrPose, valid});
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            Log("OpenXR instance loss pending; session will be recreated");
            DestroySession();
            xrDestroyInstance(m_instance);
            m_instance = XR_NULL_HANDLE;
            m_systemId = XR_NULL_SYSTEM_ID;
            m_nextSessionAttempt = GetTickCount64() + 5000;
            return;
        }
    }
}

void OpenXRRuntime::ReadWorldVisibility(const XrView (&views)[2], XrQuaternionf headOrientation)
{
    if (!m_getVisibilityMask || !m_visibilityPending) return;
    m_visibilityPending = false;
    WorldVisibilityContours contours;
    for (unsigned eye = 0; eye < 2; ++eye) {
        constexpr auto stereo = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        constexpr auto loop = XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR;
        XrVisibilityMaskKHR mask{XR_TYPE_VISIBILITY_MASK_KHR};
        if (XR_FAILED(m_getVisibilityMask(m_session, stereo, eye, loop, &mask)) ||
            mask.vertexCountOutput < 3 || mask.vertexCountOutput > 4096 ||
            mask.indexCountOutput < 3 || mask.indexCountOutput > 4096) continue;
        std::vector<XrVector2f> vertices(mask.vertexCountOutput);
        std::vector<std::uint32_t> indices(mask.indexCountOutput);
        mask.vertexCapacityInput = mask.vertexCountOutput;
        mask.vertexCountOutput = 0;
        mask.vertices = vertices.data();
        mask.indexCapacityInput = mask.indexCountOutput;
        mask.indexCountOutput = 0;
        mask.indices = indices.data();
        if (XR_FAILED(m_getVisibilityMask(m_session, stereo, eye, loop, &mask)) ||
            mask.indexCountOutput < 3 || mask.indexCountOutput > indices.size() ||
            mask.vertexCountOutput > vertices.size()) continue;

        const auto orientation = Multiply(Conjugate(headOrientation), views[eye].pose.orientation);
        const auto& fov = m_poseRing[0].fov[eye];
        const float left = std::tan(fov.angleLeft), right = std::tan(fov.angleRight);
        const float bottom = std::tan(fov.angleDown), top = std::tan(fov.angleUp);
        auto& contour = contours[eye];
        contour.reserve(mask.indexCountOutput);
        for (unsigned i = 0; i < mask.indexCountOutput; ++i) {
            if (indices[i] >= mask.vertexCountOutput) break;
            const auto v = vertices[indices[i]];
            const auto direction = Rotate(orientation, {v.x, v.y, -1.0F});
            if (direction.z >= -0.05F || right - left <= 0.2F || top - bottom <= 0.2F) break;
            const float x = (2.0F * (direction.x / -direction.z) - right - left) / (right - left);
            const float y = (2.0F * (direction.y / -direction.z) - top - bottom) / (top - bottom);
            if (!std::isfinite(x) || !std::isfinite(y)) break;
            contour.push_back({x, y});
        }
        if (contour.size() != mask.indexCountOutput) contour.clear();
    }
    Log("World VRS OpenXR visibility contours: %zu left, %zu right vertices",
        contours[0].size(), contours[1].size());
    SetWorldVisibilityContours(std::move(contours));
}

void OpenXRRuntime::ApplyLocalSpaceChanges()
{
    if (m_localSpaceChanges.empty() || m_localSpaceChanges.front().time > m_displayTime) {
        return;
    }
    XrPosef offset = m_localSpaceOffset;
    bool preserveAnchor = true;
    do {
        const auto& change = m_localSpaceChanges.front();
        if (change.valid) {
            offset = RebaseLocalOffset(offset, change.newInOld);
        } else {
            offset = kIdentityXrPose;
            preserveAnchor = false;
        }
        m_localSpaceChanges.pop_front();
    } while (!m_localSpaceChanges.empty() && m_localSpaceChanges.front().time <= m_displayTime);

    XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    info.poseInReferenceSpace = offset;
    XrSpace replacement = XR_NULL_HANDLE;
    if (Check(xrCreateReferenceSpace(m_session, &info, &replacement), "rebasing LOCAL space")) {
        xrDestroySpace(m_localSpace);
        m_localSpace = replacement;
        m_localSpaceOffset = offset;
        if (preserveAnchor) {
            Log("OpenXR LOCAL space rebased; preserving head and world anchors");
            return;
        }
    }
    Log("OpenXR LOCAL space continuity unavailable; requesting recenter");
    m_recenterRequested = true;
    m_headRecenterRequested = true;
}

void OpenXRRuntime::HandleSessionState(XrSessionState state)
{
    m_sessionState = state;
    Log("RUNG2 OpenXR session state -> %d", static_cast<int>(state));
    if (state == XR_SESSION_STATE_READY && !m_sessionRunning) {
        XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
        beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (Check(xrBeginSession(m_session, &beginInfo), "beginning session")) {
            m_sessionRunning = true;
            Log("RUNG2 session running");
        }
    } else if (state == XR_SESSION_STATE_STOPPING && m_sessionRunning) {
        if (m_framePrepared) {
            XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = m_displayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            xrEndFrame(m_session, &endInfo);
            m_framePrepared = false;
        }
        xrEndSession(m_session);
        m_sessionRunning = false;
    } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
        DestroySession();
        m_nextSessionAttempt = GetTickCount64() + 5000;
    }
}

void OpenXRRuntime::PrepareNextFrame()
{
    if (!m_sessionRunning || m_framePrepared) {
        return;
    }
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    LARGE_INTEGER start{}, end{}, frequency{};
    QueryPerformanceCounter(&start);
    if (!Check(xrWaitFrame(m_session, &waitInfo, &frameState), "waiting for frame")) {
        return;
    }
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&frequency);
    m_frameWaitMs = 1000.0 * static_cast<double>(end.QuadPart - start.QuadPart) /
                    static_cast<double>(frequency.QuadPart);
    m_waitMsSum += m_frameWaitMs;
    PerfAddXrWait(m_frameWaitMs);
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (!Check(xrBeginFrame(m_session, &beginInfo), "beginning frame")) {
        return;
    }
    m_framePrepared = true;
    m_shouldRender = frameState.shouldRender == XR_TRUE;
    m_displayTime = frameState.predictedDisplayTime;
    m_displayPeriod = frameState.predictedDisplayPeriod;
    ApplyLocalSpaceChanges();
    PublishHeadPose();
}

// Sends the head pose predicted for the next displayed frame to the game's
// FreeTrack input and remembers it: the frame the game renders with it is
// shown on a quad placed at exactly this pose, so the world stays still.
void OpenXRRuntime::PublishHeadPose()
{
    const Config& config = GetConfig();
    if (!config.headTracking || !m_feed.Open()) {
        return;
    }
    EngineStereoSetPoseFeed(&m_feed);
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (!XR_SUCCEEDED(xrLocateSpace(m_viewSpace, m_localSpace, m_displayTime, &location)) ||
        (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
        return;
    }
    const bool havePosition =
        (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    if ((m_headRecenterRequested || !m_headReferenceValid) && havePosition) {
        m_headReference = YawOnly(location.pose.orientation);
        ++m_headReferenceEpoch;
        m_headOrigin = location.pose.position;
        m_adsLean[0] = m_adsLean[1] = m_adsLean[2] = 0.0F;
        m_headReferenceValid = true;
        m_headRecenterRequested = false;
        m_aiming = false;  // the aiming baseline belongs to the old reference
        // Clear the aim latch on recenter because a reload or weapon change can cancel aim in Arma.
        m_adsMouseLatched = false;
        m_adsMousePress = 0;
        m_adsBlend = 0.0F;
        m_adsYaw = 0.0F;
        m_adsPitch = 0.0F;
        Log("HEAD forward direction set (yaw quat=%.4f %.4f)", m_headReference.y,
            m_headReference.w);
    }
    const XrQuaternionf relative =
        Multiply(Conjugate(m_headReference), location.pose.orientation);
    const XrVector3f forward = Rotate(relative, {0.0F, 0.0F, -1.0F});
    for (int i = 7; i > 0; --i) {
        m_poseRing[i] = m_poseRing[i - 1];
    }
    m_poseRing[0] = {};
    if (m_calibration.Active()) {
        // The calibration drives the input; the screen is shown meanwhile.
        float yaw = 0.0F;
        float pitch = 0.0F;
        float shift = 0.0F;
        float lift = 0.0F;
        float depth = 0.0F;
        m_calibration.NextCommand(yaw, pitch, shift, lift, depth);
        m_feed.Publish(yaw, pitch, shift, lift, depth);
        return;
    }
    const float yawLimit = config.headYawLimitDegrees / kDegrees;
    const float pitchLimit = config.headPitchLimitDegrees / kDegrees;
    // Real head angles: yaw positive = left, pitch positive = up.
    float yaw = std::clamp(std::atan2(-forward.x, -forward.z), -yawLimit, yawLimit);
    float pitch =
        std::clamp(std::asin(std::clamp(forward.y, -1.0F, 1.0F)), -pitchLimit, pitchLimit);
    // Alternate-eye stereo: odd frames are rendered for the right eye, even ones
    // for the left; only while the world view is on screen.
    const bool stereo = config.stereoMode == 1 && m_stereoEnabled && m_worldViewState == 1;
    const int eye = stereo ? ((++m_publishCount & 1U) != 0 ? 1 : -1) : 0;
    const float swap = m_stereoSwap ? -1.0F : 1.0F;
    // Head offset from the recentre position (LOCAL space, metres).
    XrVector3f delta{0.0F, 0.0F, 0.0F};
    if (havePosition) {
        delta = {location.pose.position.x - m_headOrigin.x,
                 location.pose.position.y - m_headOrigin.y,
                 location.pose.position.z - m_headOrigin.z};
    }
    const bool rightMouse = KeyHeld(VK_RBUTTON);
    if (config.adsRightMouseToggle) {
        const bool gameplayInput = !m_inventoryOpen && m_worldViewState == 1 && !WindowsCursorShowing() &&
            m_window && GetAncestor(m_window, GA_ROOT) ==
                        GetAncestor(GetForegroundWindow(), GA_ROOT);
        const bool plainClick = !KeyHeld(VK_CONTROL) && !KeyHeld(VK_MENU) && !KeyHeld(VK_SHIFT);
        const ULONGLONG now = GetTickCount64();
        if (!gameplayInput || !plainClick) {
            m_adsMousePress = 0;
        } else if (rightMouse && !m_adsMouseDown) {
            m_adsMousePress = now;
        } else if (!rightMouse && m_adsMouseDown && m_adsMousePress != 0) {
            // Arma's click binding toggles optics on release; a sustained press
            // remains temporary. Ignore modified clicks used to switch optics.
            if (now - m_adsMousePress <= 300) {
                m_adsMouseLatched = !m_adsMouseLatched;
                Log("ADS right mouse toggle: %s", m_adsMouseLatched ? "on" : "off");
            }
            m_adsMousePress = 0;
        }
        m_adsMouseDown = rightMouse;
    }
    const bool opticsInput = !m_inventoryOpen && m_worldViewState == 1 && (rightMouse || m_adsMouseLatched);
    EngineStereoSetOpticsIntent(opticsInput);
    const bool aimingNow = !m_inventoryOpen && config.adsLockView && m_worldViewState == 1 &&
        (opticsInput || EngineStereoZoom() > 1.03F);
    if (aimingNow) {
        m_aimOffFrames = 0;
        if (!m_aiming) {
            m_aiming = true;
            m_adsYaw = yaw;
            m_adsPitch = pitch;
            m_adsLean[0] = delta.x;
            m_adsLean[1] = delta.y;
            m_adsLean[2] = delta.z;
            Log("ADS on: sight line locked at head yaw=%.1f pitch=%.1f deg (right button %d, "
                "toggle %d, zoom %.2f)", yaw * kDegrees, pitch * kDegrees, rightMouse ? 1 : 0,
                m_adsMouseLatched ? 1 : 0, EngineStereoZoom());
        }
    } else if (m_aiming && ++m_aimOffFrames >= 6) {
        m_aiming = false;
        Log("ADS off");
    }
    m_adsBlend = std::clamp(m_adsBlend + (m_aiming ? 0.125F : -0.125F), 0.0F, 1.0F);
    // Scope lens: while a magnifying optic is up the frames alternate. On a lens frame the
    // engine aims exactly along the sight (no head delta), so the centre of its picture is the
    // sight line and the reticle sits there; the lens is then placed in the room at the sight
    // direction (the head direction when aiming started), which makes it follow the mouse/weapon
    // and not the head. On a world frame everything is as before.
    int lensPass = 0;
    if (m_aiming && EngineStereoScopeLens() && m_worldViewState == 1) {
        m_lensParity = !m_lensParity;
        lensPass = m_lensParity ? 2 : 1;
    }
    EngineStereoSetLensPass(lensPass);
    // Static scope (ScopeStatic): through a magnifying optic the head is ignored completely, so
    // head tremor is not magnified and only the mouse aims.
    const bool scopeStatic = config.scopeStatic && !config.scopeLens && m_aiming &&
                             m_worldViewState == 1 && EngineStereoZoom() > 1.03F;
    const float engineYaw = (lensPass == 2 || scopeStatic) ? 0.0F : yaw - m_adsBlend * m_adsYaw;
    const float enginePitch =
        (lensPass == 2 || scopeStatic) ? 0.0F : pitch - m_adsBlend * m_adsPitch;
    const float aimBias =
        (config.aimEye > 0 ? -1.0F : (config.aimEye < 0 ? 1.0F : 0.0F)) * m_adsBlend;
    float sendYaw = 0.0F;
    float sendPitch = 0.0F;
    float sendShift = 0.0F;
    if (m_calibration.Valid() && config.calibratedRotation) {
        // Measured engine response: send exactly what makes the camera follow the head.
        float achievedRight = 0.0F;
        float achievedUp = 0.0F;
        float achievedShift = 0.0F;
        sendYaw = m_calibration.YawToSend(-yaw, achievedRight);
        sendPitch = m_calibration.PitchToSend(pitch, achievedUp);
        yaw = -achievedRight;
        pitch = achievedUp;
        sendShift = m_calibration.ShiftToSend(
            static_cast<float>(eye) * swap * 0.5F * config.headsetIpdMeters * m_stereoScale,
            achievedShift);
    } else {
        sendYaw = static_cast<float>(config.headYawSign) * config.headRotationGain * engineYaw;
        sendPitch =
            static_cast<float>(config.headPitchSign) * config.headRotationGain * enginePitch;
        // Convert the OpenXR right-positive X axis to the FreeTrack sign convention.
        sendShift = -static_cast<float>(eye) * swap * m_stereoShiftMillimeters * m_stereoScale;
    }
    m_poseRing[0].head = location.pose;
    m_poseRing[0].yaw = yaw;
    m_poseRing[0].pitch = pitch;
    m_poseRing[0].eye = eye;
    m_poseRing[0].valid = true;
    m_poseRing[0].scopeStatic = scopeStatic;
    m_poseRing[0].sightActive = m_worldViewState == 1 && (opticsInput || m_adsBlend > 0.0F);
    if (lensPass == 2) {
        m_poseRing[0].lens = true;
        m_poseRing[0].lensOrientation = Multiply(m_headReference, YawPitch(m_adsYaw, m_adsPitch));
    }
    const XrQuaternionf cameraOrientation =
        Multiply(m_headReference, YawPitch(engineYaw, enginePitch));
    UpdateHeadPlacement();
    const bool alternateEyes = !config.staleEyeWarp && m_alternateEyeProjection;
    const bool inspectWeapon = config.weaponHeadPosition && !alternateEyes && m_headPositionOn && havePosition &&
        m_headReferenceValid && !m_vehicleMode && !opticsInput && m_adsBlend == 0.0F && EngineStereoZoom() <= 1.03F;
    // Use the same eye offsets for geometry and the OpenXR projection layer.
    XrVector3f halfBaseline{};
    bool physicalEyes = false;
    {
        const bool wantProjection = m_projectionEnabled && m_sbsInput && config.engineStereo &&
                                    (m_worldViewState == 1 || m_inventoryOpen);
        XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        bool located = false;
        if (wantProjection) {
            XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locateInfo.displayTime = m_displayTime;
            locateInfo.space = m_localSpace;
            XrViewState viewState{XR_TYPE_VIEW_STATE};
            std::uint32_t count = 0;
            located = XR_SUCCEEDED(xrLocateViews(m_session, &locateInfo, &viewState, 2, &count,
                                                 views)) &&
                      count == 2 &&
                      (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0 &&
                      (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        }
        float tangents[8] = {1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
        float half = 0.0F;
        const bool centeredOptics = located && scopeStatic && config.scopeReticle;
        if (located) {
            const float dx = views[1].pose.position.x - views[0].pose.position.x;
            const float dy = views[1].pose.position.y - views[0].pose.position.y;
            const float dz = views[1].pose.position.z - views[0].pose.position.z;
            const float ipd = std::sqrt(dx * dx + dy * dy + dz * dz);
            half = std::clamp(0.5F * ipd, 0.025F, 0.045F) * (m_stereoEnabled ? m_stereoScale : 0.0F);
            if (inspectWeapon && std::isfinite(ipd) && ipd > 0.001F) {
                const float scale = std::min(half, 0.1F) / ipd;
                halfBaseline = Rotate(Conjugate(cameraOrientation), {dx * scale, dy * scale, dz * scale});
                physicalEyes = true;
            }
            for (int i = 0; i < 2; ++i) {
                // The eye displays may be canted against the head: express the frustum in
                // head axes, because the engine draws both eyes with parallel axes.
                const XrQuaternionf eyeRelative =
                    Multiply(Conjugate(location.pose.orientation), views[i].pose.orientation);
                const XrVector3f axis = Rotate(eyeRelative, {0.0F, 0.0F, -1.0F});
                const float cantYaw = std::atan2(axis.x, -axis.z);
                const float cantPitch = std::asin(std::clamp(axis.y, -1.0F, 1.0F));
                const XrFovf& fov = views[i].fov;
                m_poseRing[0].fov[i] = {fov.angleLeft + cantYaw, fov.angleRight + cantYaw,
                                        fov.angleUp + cantPitch, fov.angleDown + cantPitch};
                if (centeredOptics) {
                    m_poseRing[0].fov[i] = CenteredOpticsFov(m_poseRing[0].fov[i]);
                }
                tangents[i * 4 + 0] = -std::tan(m_poseRing[0].fov[i].angleLeft);
                tangents[i * 4 + 1] = std::tan(m_poseRing[0].fov[i].angleRight);
                tangents[i * 4 + 2] = std::tan(m_poseRing[0].fov[i].angleUp);
                tangents[i * 4 + 3] = -std::tan(m_poseRing[0].fov[i].angleDown);
            }
            ReadWorldVisibility(views, location.pose.orientation);
            static int viewLogs = 0;
            if (viewLogs++ < 3) {
                Log("PROJECTION headset eyes: ipd=%.1f mm, left tan l/r/u/d = %.3f %.3f %.3f %.3f, "
                    "right = %.3f %.3f %.3f %.3f",
                    ipd * 1000.0F, tangents[0], tangents[1], tangents[2], tangents[3], tangents[4],
                    tangents[5], tangents[6], tangents[7]);
            }
        }
        m_poseRing[0].halfIpd = half;
        m_poseRing[0].aimBias = aimBias;
        m_poseRing[0].viewsValid = located;
        m_poseRing[0].projection = located;
        EngineStereoSetHeadset(located, half, tangents, aimBias, centeredOptics);
    }
    m_poseRing[0].physicalEyes = physicalEyes;
    m_poseRing[0].halfBaseline = halfBaseline;
    EngineStereoSetPhysicalEyes(physicalEyes, halfBaseline.x, halfBaseline.y, -halfBaseline.z);
    // Head position: the picture is drawn from a camera moved with the head (projection
    // constants, no FreeTrack). Aiming freezes the offset like it freezes the angle.
    {
        float tx = 0.0F;
        float ty = 0.0F;
        float tz = 0.0F;
        if ((config.headPosition || physicalEyes) && m_headPositionOn && havePosition &&
            m_headReferenceValid && !scopeStatic) {
            const XrVector3f offset = {delta.x - m_adsBlend * m_adsLean[0],
                                       delta.y - m_adsBlend * m_adsLean[1],
                                       delta.z - m_adsBlend * m_adsLean[2]};
            // Into the axes of the engine camera (the direction the engine looks).

            const XrVector3f inCamera = Rotate(Conjugate(cameraOrientation), offset);
            const float limit = config.headPositionLimitMeters;
            const float scale = config.headPositionScale;
            tx = std::clamp(inCamera.x * scale, -limit, limit);
            ty = std::clamp(inCamera.y * scale, -limit, limit);
            tz = std::clamp(-inCamera.z * scale, -limit, limit);
        }
        EngineStereoSetLean(tx, ty, tz);
        // Fade horizontal and vertical head offsets while aiming to align the eye with the sight.
        const float placementKeep = 1.0F - m_adsBlend;
        EngineStereoSetHeadOffset(m_appliedOffset[0] * placementKeep,
                                  m_appliedOffset[1] * placementKeep, m_appliedOffset[2]);
        EngineStereoSetHudQuad(((config.hudQuad && m_hudLockOn) || config.hideGameplayHud) &&
                               m_worldViewState == 1 && m_projectionEnabled);
        m_poseRing[0].clipWOffset = -tz;
        m_poseRing[0].leanForward = Rotate(cameraOrientation, {0.0F, 0.0F, -1.0F});
        const XrVector3f renderedOffset = Rotate(cameraOrientation, {tx, ty, -tz});
        m_poseRing[0].projectionCenter = {m_headOrigin.x + renderedOffset.x,
                                         m_headOrigin.y + renderedOffset.y,
                                         m_headOrigin.z + renderedOffset.z};
        EngineStereoSetBodyAngles(engineYaw, enginePitch);
        const bool weaponBodyLock = physicalEyes || (config.weaponBodyLock && m_weaponLockOn);
        const float yawOffset = weaponBodyLock ? engineYaw : 0.0F;
        const float pitchOffset = weaponBodyLock ? enginePitch : 0.0F;
        const float cosYaw = std::cos(yawOffset);
        EngineStereoSetWeaponForward(std::sin(yawOffset), -cosYaw * std::sin(pitchOffset),
                                     cosYaw * std::cos(pitchOffset), weaponBodyLock, m_adsBlend);
    }
    m_feed.Publish(sendYaw, sendPitch, sendShift);
    m_poseRing[0].sequence = m_feed.LastPublishedId();
    m_poseRing[0].headReferenceEpoch = m_headReferenceEpoch;
    if (config.detailedCapture) {
        m_poseRing[0].freeTrackYaw = sendYaw;
        m_poseRing[0].freeTrackPitch = sendPitch;
    }
}
// Half-angle tangents of the game camera, from the exe's own export
// RVGetProjectionViewTransform (no offsets). Follows zoom and optics.
bool OpenXRRuntime::ReadGameFov(float& tanHalfHorizontal, float& tanHalfVertical)
{
    if (!m_gameProjectionViewSearched) {
        m_gameProjectionViewSearched = true;
        m_gameProjectionView = reinterpret_cast<void (*)(float*)>(
            GetProcAddress(GetModuleHandleW(nullptr), "RVGetProjectionViewTransform"));
        Log("FOV export RVGetProjectionViewTransform: %s",
            m_gameProjectionView ? "found" : "NOT found, INI value is used");
    }
    float matrices[32]{};
    if (m_gameProjectionView) {
        m_gameProjectionView(matrices);
    }
    const float p0 = matrices[0];
    const float p5 = matrices[5];
    bool fromGame = std::isfinite(p0) && std::isfinite(p5) && p0 > 0.001F && p5 > 0.001F;
    const char* convention = "ini";
    if (fromGame) {
        // The wider axis has the larger tangent. If the horizontal term is the
        // smaller one, the matrix stores 1/tan (the usual projection scale).
        if (p0 < p5) {
            tanHalfHorizontal = 1.0F / p0;
            tanHalfVertical = 1.0F / p5;
            convention = "1/tan";
        } else {
            tanHalfHorizontal = p0;
            tanHalfVertical = p5;
            convention = "tan";
        }
        fromGame = tanHalfHorizontal > 0.02F && tanHalfHorizontal < 6.0F &&
                   tanHalfVertical > 0.02F && tanHalfVertical < 6.0F;
    }
    if (!fromGame) {
        convention = "ini";
        tanHalfVertical = GetConfig().worldFovTanVertical;
        tanHalfHorizontal = m_quadHeight > 0
                                ? tanHalfVertical * static_cast<float>(m_quadWidth) /
                                      static_cast<float>(m_quadHeight)
                                : tanHalfVertical * 16.0F / 9.0F;
    }
    m_lastTanHorizontal = tanHalfHorizontal;
    m_lastTanVertical = tanHalfVertical;
    if (std::fabs(tanHalfHorizontal - m_loggedTanHorizontal) > 0.02F * tanHalfHorizontal &&
        m_fovLogs < 60) {
        ++m_fovLogs;
        m_loggedTanHorizontal = tanHalfHorizontal;
        Log("FOV p0=%.4f p5=%.4f -> %.1f x %.1f deg (tanH=%.3f tanV=%.3f, %s)", p0, p5,
            2.0F * kDegrees * std::atan(tanHalfHorizontal),
            2.0F * kDegrees * std::atan(tanHalfVertical), tanHalfHorizontal, tanHalfVertical,
            convention);
    }
    return fromGame;
}

// World view: the game image becomes a large quad placed where the camera
// looked when this frame was rendered, sized by the game's field of view.
// eye = -1/+1 centres the quad on that eye, so far objects look far.
void OpenXRRuntime::BuildWorldView(XrCompositionLayerQuad& quad, const RenderPose& pose,
                                   int eye) const
{
    const Config& config = GetConfig();
    // With engine stereo the zero-parallax distance is the convergence distance: the
    // image then shows the world at its true size and depth.
    const float distance = (m_sbsInput && EngineStereoActive())
                               ? EngineStereoConvergenceMeters()
                               : config.worldQuadDistanceMeters;
    const XrQuaternionf orientation =
        Multiply(m_headReference, YawPitch(pose.yaw, pose.pitch));
    const XrVector3f offset = Rotate(
        orientation,
        {0.5F * config.headsetIpdMeters * static_cast<float>(eye), 0.0F, -distance});
    quad.space = m_localSpace;
    quad.pose.orientation = orientation;
    quad.pose.position = {pose.head.position.x + offset.x, pose.head.position.y + offset.y,
                          pose.head.position.z + offset.z};
    quad.size = {2.0F * distance * pose.tanHorizontal, 2.0F * distance * pose.tanVertical};
}

bool OpenXRRuntime::EnsureRightSwapchain()
{
    return EnsureAuxSwapchain(m_quadWidth, m_rightSwapchain, m_rightImages);
}

bool OpenXRRuntime::EnsureAuxSwapchain(
    UINT width, XrSwapchain& chain, std::vector<XrSwapchainImageD3D11KHR>& images)
{
    if (chain != XR_NULL_HANDLE) {
        return true;
    }
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.format = static_cast<std::int64_t>(m_quadFormat);
    createInfo.sampleCount = 1;
    createInfo.width = width;
    createInfo.height = m_quadHeight;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    if (!Check(xrCreateSwapchain(m_session, &createInfo, &chain),
               "creating auxiliary eye swapchain")) {
        chain = XR_NULL_HANDLE;
        return false;
    }
    std::uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(chain, 0, &imageCount, nullptr);
    images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!Check(xrEnumerateSwapchainImages(
                   chain, imageCount, &imageCount,
                   reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
               "enumerating auxiliary eye images")) {
        xrDestroySwapchain(chain);
        chain = XR_NULL_HANDLE;
        images.clear();
        return false;
    }
    Log("STEREO auxiliary eye swapchain %ux%u images=%u", width, m_quadHeight, imageCount);
    return true;
}

ID3D11RenderTargetView* OpenXRRuntime::EyeTarget(ID3D11Texture2D* texture)
{
    auto found = m_eyeTargets.find(texture);
    if (found != m_eyeTargets.end()) {
        return found->second.Get();
    }
    // UNORM view on the sRGB-typed image: the game's bytes go through unchanged,
    // exactly like the plain copy does.
    D3D11_RENDER_TARGET_VIEW_DESC description{};
    description.Format = UnormOf(m_quadFormat);
    description.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;
    if (FAILED(m_device->CreateRenderTargetView(texture, &description, &view))) {
        static int logs = 0;
        if (logs++ < 5) {
            Log("DEPTH could not create a render target on the OpenXR image (format %d)",
                static_cast<int>(description.Format));
        }
        return nullptr;
    }
    m_eyeTargets[texture] = view;
    return view.Get();
}

// Screen with depth: draws the left and the right image from the game frame
// and its depth buffer into the two swapchains. false = use the plain copy.
bool OpenXRRuntime::RenderDepthStereo(ID3D11Texture2D* backBuffer, bool flat)
{
    DepthStereo& depth = DepthStereo::Instance();
    if (!depth.Initialize(m_device.Get(), m_context.Get()) || !EnsureRightSwapchain() ||
        !depth.Capture(m_context.Get(), backBuffer)) {
        return false;
    }
    // Near plane from the game's projection: depth 1 - d is near / distance.
    float tanHorizontal = 1.0F;
    float tanVertical = 1.0F;
    ReadGameFov(tanHorizontal, tanVertical);
    float matrices[32]{};
    if (m_gameProjectionView) {
        m_gameProjectionView(matrices);
    }
    float nearPlane = matrices[10] != 0.0F ? -matrices[14] / matrices[10] : 0.0F;
    if (!std::isfinite(nearPlane) || nearPlane < 0.005F || nearPlane > 5.0F) {
        nearPlane = 0.1F;
    }
    if (std::fabs(nearPlane - m_loggedNear) > 0.1F * nearPlane && m_nearLogs < 30) {
        ++m_nearLogs;
        m_loggedNear = nearPlane;
        Log("DEPTH projection z terms: [10]=%.5f [11]=%.5f [14]=%.5f [15]=%.5f -> near=%.4f m",
            matrices[10], matrices[11], matrices[14], matrices[15], nearPlane);
    }
    DepthStereo::EyeSettings settings;
    settings.strength = m_stereoScale * GetConfig().headsetIpdMeters / m_quadWidthMeters;
    settings.convergence = m_convergenceMeters;
    settings.flat = flat;
    settings.depthView = m_depthDebugView;
    settings.reversedOverride = m_depthReversed;
    for (int eye = 0; eye < 2; ++eye) {
        const XrSwapchain chain = eye == 0 ? m_quadSwapchain : m_rightSwapchain;
        const std::vector<XrSwapchainImageD3D11KHR>& images =
            eye == 0 ? m_quadImages : m_rightImages;
        std::uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!Check(xrAcquireSwapchainImage(chain, &acquireInfo, &index), "acquiring eye image")) {
            return false;
        }
        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = 100'000'000;  // 100 ms
        bool drawn = false;
        if (Check(xrWaitSwapchainImage(chain, &waitInfo), "waiting eye image") &&
            index < images.size()) {
            if (ID3D11RenderTargetView* target = EyeTarget(images[index].texture)) {
                settings.eye = (eye == 0 ? -1.0F : 1.0F) * (m_stereoSwap ? -1.0F : 1.0F);
                depth.RenderEye(m_context.Get(), target, m_quadWidth, m_quadHeight, settings);
                drawn = true;
            }
        }
        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (!Check(xrReleaseSwapchainImage(chain, &releaseInfo), "releasing eye image") ||
            !drawn) {
            return false;
        }
    }
    ++m_depthFrames;
    return true;
}

// Instrumentation: how far the game camera really moved sideways between the
// left and the right frame (metres), from the engine's own view matrix.
// The matrix maps world to camera with row vectors: columns of the 3x3 part
// are the camera axes in the world, the last row is the translation.
void OpenXRRuntime::MeasureEyeSeparation(int eye)
{
    if (!m_gameProjectionView) {
        return;
    }
    float m[32]{};
    m_gameProjectionView(m);
    const double rotation[3][3] = {{m[16], m[17], m[18]}, {m[20], m[21], m[22]}, {m[24], m[25], m[26]}};
    const double translation[3] = {m[28], m[29], m[30]};
    double position[3]{};
    for (int i = 0; i < 3; ++i) {
        position[i] = -(translation[0] * rotation[i][0] + translation[1] * rotation[i][1] +
                        translation[2] * rotation[i][2]);
    }
    if (eye != 0 && m_previousCameraEye == -eye) {
        double sideways = 0.0;
        double total = 0.0;
        for (int i = 0; i < 3; ++i) {
            const double delta = position[i] - m_previousCameraPosition[i];
            sideways += delta * rotation[i][0];  // camera right axis in the world
            total += delta * delta;
        }
        if (total < 0.25) {  // ignore frames where the player moved or teleported
            m_separationSum += sideways * static_cast<double>(eye);
            ++m_separationSamples;
        }
    }
    m_previousCameraEye = eye;
    for (int i = 0; i < 3; ++i) {
        m_previousCameraPosition[i] = position[i];
    }
}
bool OpenXRRuntime::EnsureQuadSwapchain(const D3D11_TEXTURE2D_DESC& backBuffer)
{
    if (m_quadSwapchain != XR_NULL_HANDLE && m_quadWidth == backBuffer.Width &&
        m_quadHeight == backBuffer.Height && m_quadSourceFormat == backBuffer.Format) {
        return true;
    }
    const double reconfigureStarted = PerfNowMs();
    DestroyQuadSwapchain();
    std::uint32_t count = 0;
    xrEnumerateSwapchainFormats(m_session, 0, &count, nullptr);
    std::vector<std::int64_t> formats(count);
    xrEnumerateSwapchainFormats(m_session, count, &count, formats.data());
    const DXGI_FORMAT format =
        SelectQuadFormat(backBuffer.Format, formats, GetConfig().useSrgbSwapchain);
    if (format == DXGI_FORMAT_UNKNOWN) {
        if (!m_quadFormatRejected) {
            Log("Back buffer format %d has no matching OpenXR swapchain format; quad off",
                static_cast<int>(backBuffer.Format));
            m_quadFormatRejected = true;
        }
        return false;
    }
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.format = static_cast<std::int64_t>(format);
    createInfo.sampleCount = 1;
    createInfo.width = backBuffer.Width;
    createInfo.height = backBuffer.Height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    if (!Check(xrCreateSwapchain(m_session, &createInfo, &m_quadSwapchain),
               "creating quad swapchain")) {
        m_quadSwapchain = XR_NULL_HANDLE;
        return false;
    }
    std::uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(m_quadSwapchain, 0, &imageCount, nullptr);
    m_quadImages.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!Check(xrEnumerateSwapchainImages(
                   m_quadSwapchain, imageCount, &imageCount,
                   reinterpret_cast<XrSwapchainImageBaseHeader*>(m_quadImages.data())),
               "enumerating quad images")) {
        DestroyQuadSwapchain();
        return false;
    }
    m_quadWidth = backBuffer.Width;
    m_quadHeight = backBuffer.Height;
    m_quadFormat = format;
    m_quadSourceFormat = backBuffer.Format;
    D3D11_TEXTURE2D_DESC imageDescription{};
    if (!m_quadImages.empty() && m_quadImages[0].texture) {
        m_quadImages[0].texture->GetDesc(&imageDescription);
    }
    Log("RUNG3 quad swapchain %ux%u images=%u source format=%d OpenXR format=%d "
        "image texture format=%d reconfigure=%.2f ms CPU",
        m_quadWidth, m_quadHeight, imageCount, static_cast<int>(backBuffer.Format),
        static_cast<int>(format), static_cast<int>(imageDescription.Format),
        PerfNowMs() - reconfigureStarted);
    return true;
}

void OpenXRRuntime::DestroyQuadSwapchain()
{
    m_stalePairValid = false;
    for (int eye = 0; eye < 2; ++eye) {
        if (m_projectionSwapchains[eye] != XR_NULL_HANDLE) {
            xrDestroySwapchain(m_projectionSwapchains[eye]);
            m_projectionSwapchains[eye] = XR_NULL_HANDLE;
        }
        m_projectionImages[eye].clear();
    }
    if (m_quadSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_quadSwapchain);
        m_quadSwapchain = XR_NULL_HANDLE;
    }
    if (m_rightSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_rightSwapchain);
        m_rightSwapchain = XR_NULL_HANDLE;
    }
    m_rightImages.clear();
    if (m_dibrSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_dibrSwapchain);
        m_dibrSwapchain = XR_NULL_HANDLE;
    }
    m_dibrImages.clear();
    m_dibr.ResetHistory();
    m_retainedViews[0] = m_retainedViews[1] = {};
    m_lensWorldValid = m_lensWindowValid = false;
    m_nativeValid[0] = m_nativeValid[1] = false;
    if (m_menuSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_menuSwapchain);
        m_menuSwapchain = XR_NULL_HANDLE;
    }
    m_menuImages.clear();
    m_eyeTargets.clear();
    m_eyePose[0].valid = false;
    m_eyePose[1].valid = false;
    // The cursor swapchain copies the screen's format, so it goes with it.
    if (m_hudSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_hudSwapchain);
        m_hudSwapchain = XR_NULL_HANDLE;
    }
    m_hudImages.clear();
    m_hudWidth = m_hudHeight = 0;
    if (m_cursorSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_cursorSwapchain);
        m_cursorSwapchain = XR_NULL_HANDLE;
    }
    m_cursorImages.clear();
    m_cursorHandle = nullptr;
    m_cursorUsable = false;
    m_screenCursorHandle = nullptr;
    m_screenCursorView.Reset();
    m_screenCursorContextState.Reset();
    m_quadImages.clear();
    m_quadWidth = 0;
    m_quadHeight = 0;
    m_quadFormat = DXGI_FORMAT_UNKNOWN;
    m_quadSourceFormat = DXGI_FORMAT_UNKNOWN;
}

bool OpenXRRuntime::CopyIntoQuad(ID3D11Texture2D* backBuffer,
                                 const D3D11_TEXTURE2D_DESC& description, bool right)
{
    return CopyIntoSwapchain(backBuffer, description, right ? m_rightSwapchain : m_quadSwapchain,
                             right ? m_rightImages : m_quadImages);
}

bool OpenXRRuntime::CopyIntoSwapchain(
    ID3D11Texture2D* backBuffer, const D3D11_TEXTURE2D_DESC& description, XrSwapchain chain,
    const std::vector<XrSwapchainImageD3D11KHR>& images, const D3D11_BOX* sourceBox)
{
    std::uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!Check(xrAcquireSwapchainImage(chain, &acquireInfo, &index),
               "acquiring quad image")) {
        return false;
    }
    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = 100'000'000;  // 100 ms
    const bool profile = PerfEnabled();
    const double waitStart = profile ? PerfNowMs() : 0.0;
    XrResult waitResult = xrWaitSwapchainImage(chain, &waitInfo);
    if (waitResult == XR_TIMEOUT_EXPIRED) {
        // A timed-out image is still acquired. Wait for it before writing or releasing.
        waitInfo.timeout = XR_INFINITE_DURATION;
        waitResult = xrWaitSwapchainImage(chain, &waitInfo);
    }
    if (profile) PerfAddXrImageWait(PerfNowMs() - waitStart);
    if (!Check(waitResult, "waiting quad image") || waitResult == XR_TIMEOUT_EXPIRED) {
        return false;
    }
    if (index >= images.size() || !images[index].texture) {
        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        Check(xrReleaseSwapchainImage(chain, &releaseInfo), "releasing invalid quad image");
        return false;
    }
    ID3D11Texture2D* target = images[index].texture;
    const bool sampleCopy = profile && m_presents % 30 == 0;
    if (sampleCopy) PerfXrCopyGpuTimer().Begin(m_device.Get(), m_context.Get());
    if (description.SampleDesc.Count > 1) {
        // MSAA back buffer: resolve raw bytes (UNORM view), the swapchain is sRGB-typed.
        const DXGI_FORMAT resolveFormat =
            IsTypeless(description.Format) ? UnormOf(description.Format) : description.Format;
        m_context->ResolveSubresource(target, 0, backBuffer, 0, resolveFormat);
    } else {
        m_context->CopySubresourceRegion(target, 0, 0, 0, 0, backBuffer, 0, sourceBox);
    }
    if (sampleCopy) PerfXrCopyGpuTimer().End(m_context.Get());
    if (m_afterCopy) {
        m_afterCopy(target);
    }
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return Check(xrReleaseSwapchainImage(chain, &releaseInfo), "releasing quad image");
}

bool OpenXRRuntime::CopyProjectionEyes(
    ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& description)
{
    if (description.SampleDesc.Count != 1 || description.Width < 2 ||
        description.Width % 2 != 0) {
        return false;
    }
    const UINT eyeWidth = description.Width / 2;
    for (int eye = 0; eye < 2; ++eye) {
        if (!EnsureAuxSwapchain(eyeWidth, m_projectionSwapchains[eye], m_projectionImages[eye])) {
            return false;
        }
    }
    for (int eye = 0; eye < 2; ++eye) {
        const UINT x = static_cast<UINT>(eye) * eyeWidth;
        const D3D11_BOX region{x, 0, 0, x + eyeWidth, description.Height, 1};
        if (!CopyIntoSwapchain(texture, description, m_projectionSwapchains[eye],
                               m_projectionImages[eye], &region)) {
            return false;
        }
    }
    return true;
}

void OpenXRRuntime::SampleSteamVrScene(
    ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& description, bool reprojected)
{
    const DXGI_FORMAT format = UnormOf(description.Format);
    if (m_steamVrSceneSampleStarted || description.SampleDesc.Count != 1 ||
        description.Width < 2 || description.Width % 2 != 0 ||
        (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM &&
         format != DXGI_FORMAT_B8G8R8X8_UNORM)) {
        return;
    }
    m_steamVrSceneSampleStarted = true;
    m_steamVrSceneSampleReprojected = reprojected;
    D3D11_TEXTURE2D_DESC staging{};
    staging.Width = 2 * kSceneSamplesPerAxis;
    staging.Height = kSceneSamplesPerAxis;
    staging.MipLevels = staging.ArraySize = staging.SampleDesc.Count = 1;
    staging.Format = description.Format;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(m_device->CreateTexture2D(&staging, nullptr, &m_steamVrSceneSample))) {
        Log("SteamVR scene sample: unable to allocate readback texture");
        return;
    }
    const UINT half = description.Width / 2;
    for (UINT row = 0; row < kSceneSamplesPerAxis; ++row) {
        const UINT y = description.Height * (row + 1) / (kSceneSamplesPerAxis + 1);
        for (UINT column = 0; column < staging.Width; ++column) {
            const UINT x = (column / kSceneSamplesPerAxis) * half +
                half * (column % kSceneSamplesPerAxis + 1) / (kSceneSamplesPerAxis + 1);
            const D3D11_BOX pixel{x, y, 0, x + 1, y + 1, 1};
            m_context->CopySubresourceRegion(m_steamVrSceneSample.Get(), 0, column, row, 0,
                                             texture, 0, &pixel);
        }
    }
}

void OpenXRRuntime::LogSteamVrSceneSample()
{
    if (!m_steamVrSceneSample) {
        return;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT result = m_context->Map(m_steamVrSceneSample.Get(), 0, D3D11_MAP_READ,
                                         D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
        return;
    }
    if (SUCCEEDED(result)) {
        unsigned maxRgb[2]{};
        unsigned nonBlack[2]{};
        for (UINT row = 0; row < kSceneSamplesPerAxis; ++row) {
            const auto* pixels = static_cast<const std::uint8_t*>(mapped.pData) + row * mapped.RowPitch;
            for (UINT column = 0; column < 2 * kSceneSamplesPerAxis; ++column) {
                const UINT eye = column / kSceneSamplesPerAxis;
                const auto* pixel = pixels + 4 * column;
                const unsigned level = std::max({pixel[0], pixel[1], pixel[2]});
                maxRgb[eye] = std::max(maxRgb[eye], level);
                nonBlack[eye] += level > 8 ? 1U : 0U;
            }
        }
        m_context->Unmap(m_steamVrSceneSample.Get(), 0);
        Log("SteamVR scene sample (%s): left max RGB=%u, nonblack=%u/9; right max RGB=%u, nonblack=%u/9",
            m_steamVrSceneSampleReprojected ? "AER" : "full stereo",
            maxRgb[0], nonBlack[0], maxRgb[1], nonBlack[1]);
    } else {
        Log("SteamVR scene sample: GPU readback failed (0x%08lX)", static_cast<unsigned long>(result));
    }
    m_steamVrSceneSample.Reset();
}

bool OpenXRRuntime::UploadCursor(HCURSOR handle)
{
    const ULONGLONG now = GetTickCount64();
    if (now < m_cursorRetryAfter) {
        return false;
    }
    m_cursorRetryAfter = now + 2000;
    m_cursorHandle = handle;
    m_cursorUsable = false;
    CursorImage image;
    if (!RasterizeCursor(handle, IsBgra(m_quadFormat), image)) {
        if (m_cursorLogs++ < 20) {
            Log("CURSOR handle %p could not be read or is empty", handle);
        }
        return false;
    }
    if (m_cursorSwapchain == XR_NULL_HANDLE) {
        XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                                XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.format = static_cast<std::int64_t>(m_quadFormat);
        createInfo.sampleCount = 1;
        createInfo.width = kCursorMaxSize;
        createInfo.height = kCursorMaxSize;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;
        if (!Check(xrCreateSwapchain(m_session, &createInfo, &m_cursorSwapchain),
                   "creating cursor swapchain")) {
            m_cursorSwapchain = XR_NULL_HANDLE;
            return false;
        }
        std::uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(m_cursorSwapchain, 0, &imageCount, nullptr);
        m_cursorImages.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (!Check(xrEnumerateSwapchainImages(
                       m_cursorSwapchain, imageCount, &imageCount,
                       reinterpret_cast<XrSwapchainImageBaseHeader*>(m_cursorImages.data())),
                   "enumerating cursor images")) {
            xrDestroySwapchain(m_cursorSwapchain);
            m_cursorSwapchain = XR_NULL_HANDLE;
            m_cursorImages.clear();
            return false;
        }
    }
    std::uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!Check(xrAcquireSwapchainImage(m_cursorSwapchain, &acquireInfo, &index),
               "acquiring cursor image")) {
        return false;
    }
    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = 100'000'000;  // 100 ms
    const bool ready =
        Check(xrWaitSwapchainImage(m_cursorSwapchain, &waitInfo), "waiting cursor image");
    if (ready && index < m_cursorImages.size() && m_cursorImages[index].texture) {
        const D3D11_BOX box{0, 0, 0, static_cast<UINT>(image.width),
                            static_cast<UINT>(image.height), 1};
        m_context->UpdateSubresource(m_cursorImages[index].texture, 0, &box, image.pixels.data(),
                                     static_cast<UINT>(image.width) * 4, 0);
    }
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (!Check(xrReleaseSwapchainImage(m_cursorSwapchain, &releaseInfo),
               "releasing cursor image") ||
        !ready) {
        return false;
    }
    m_cursorWidth = image.width;
    m_cursorHeight = image.height;
    m_cursorHotX = image.hotX;
    m_cursorHotY = image.hotY;
    m_cursorUsable = true;
    m_cursorRetryAfter = 0;
    if (m_cursorLogs++ < 20) {
        Log("CURSOR image handle=%p %dx%d hotspot=(%d,%d) visiblePixels=%d", handle, image.width,
            image.height, image.hotX, image.hotY, image.visiblePixels);
    }
    return true;
}

HCURSOR OpenXRRuntime::ReadGameCursor(POINT& point, int& mapWidth, int& mapHeight)
{
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    RECT client{};
    if (!m_window || !GetCursorInfo(&info) || !GetClientRect(m_window, &client) ||
        client.right <= 0 || client.bottom <= 0) {
        return nullptr;
    }
    const bool showing = (info.flags & CURSOR_SHOWING) != 0 && info.hCursor != nullptr;
    if (static_cast<int>(showing) != m_cursorVisibleState) {
        m_cursorVisibleState = static_cast<int>(showing);
        if (m_cursorLogs++ < 20) {
            Log("CURSOR windows cursor %s (flags=0x%lX handle=%p)", showing ? "shown" : "hidden",
                info.flags, info.hCursor);
        }
    }
    if (!showing) {
        return nullptr;
    }
    point = info.ptScreenPos;
    mapWidth = client.right;
    mapHeight = client.bottom;
    // Sample the same pixel and coordinate extent used by the game's click handlers.
    if (!VirtualCursorClientPosition(point, mapWidth, mapHeight) &&
        !ScreenToClient(m_window, &point)) {
        return nullptr;
    }
    if (point.x < 0 || point.y < 0 || point.x >= mapWidth || point.y >= mapHeight) {
        return nullptr;
    }
    return info.hCursor;
}

bool OpenXRRuntime::DrawCursorIntoScreen(ID3D11Texture2D* image)
{
    POINT point{};
    int mapWidth = 0, mapHeight = 0;
    const HCURSOR handle = ReadGameCursor(point, mapWidth, mapHeight);
    if (!handle) {
        return false;
    }
    if (handle != m_screenCursorHandle || !m_screenCursorView) {
        CursorImage cursor;
        if (!RasterizeCursor(handle, false, cursor)) {
            return false;
        }
        // The ImGui blend state expects straight alpha.
        for (std::size_t offset = 0; offset < cursor.pixels.size(); offset += 4) {
            const unsigned alpha = cursor.pixels[offset + 3];
            if (alpha > 0 && alpha < 255) {
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const unsigned value = (cursor.pixels[offset + channel] * 255U + alpha / 2) / alpha;
                    cursor.pixels[offset + channel] = static_cast<std::uint8_t>(std::min(255U, value));
                }
            }
        }
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(cursor.width);
        description.Height = static_cast<UINT>(cursor.height);
        description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA pixels{cursor.pixels.data(), description.Width * 4, 0};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(m_device->CreateTexture2D(&description, &pixels, &texture)) ||
            FAILED(m_device->CreateShaderResourceView(texture.Get(), nullptr, &view))) {
            return false;
        }
        m_screenCursorView = std::move(view);
        m_screenCursorHandle = handle;
        m_screenCursorWidth = cursor.width;
        m_screenCursorHeight = cursor.height;
        m_screenCursorHotX = cursor.hotX;
        m_screenCursorHotY = cursor.hotY;
        Log("CURSOR SteamVR screen texture %dx%d hotspot=(%d,%d)",
            cursor.width, cursor.height, cursor.hotX, cursor.hotY);
    }

    ID3D11RenderTargetView* target = EyeTarget(image);
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context;
    if (!target || FAILED(m_context.As(&context))) {
        return false;
    }
    if (!m_screenCursorContextState) {
        Microsoft::WRL::ComPtr<ID3D11Device1> device;
        const auto featureLevel = m_device->GetFeatureLevel();
        if (FAILED(m_device.As(&device)) ||
            FAILED(device->CreateDeviceContextState(0, &featureLevel, 1, D3D11_SDK_VERSION,
                                                    __uuidof(ID3D11Device), nullptr,
                                                    &m_screenCursorContextState))) {
            return false;
        }
    }
    const float scaleX = static_cast<float>(m_quadWidth) / static_cast<float>(mapWidth);
    const float scaleY = static_cast<float>(m_quadHeight) / static_cast<float>(mapHeight);
    const ImVec2 topLeft{static_cast<float>(point.x - m_screenCursorHotX) * scaleX,
                         static_cast<float>(point.y - m_screenCursorHotY) * scaleY};
    const ImVec2 bottomRight{topLeft.x + static_cast<float>(m_screenCursorWidth) * scaleX,
                             topLeft.y + static_cast<float>(m_screenCursorHeight) * scaleY};
    const ImTextureRef cursorTexture(reinterpret_cast<ImTextureID>(m_screenCursorView.Get()));
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> gameState;
    context->SwapDeviceContextState(m_screenCursorContextState.Get(), &gameState);
    const bool drawn = Menu::Instance().Render(m_device.Get(), m_context.Get(), target,
        m_quadWidth, m_quadHeight, 1.0F, [&] {
            ImGui::GetForegroundDrawList()->AddImage(cursorTexture, topLeft, bottomRight);
        }, false);
    context->SwapDeviceContextState(gameState.Get(), nullptr);
    return drawn;
}

// Used by runtimes that show the hardware cursor as a separate composition layer.
bool OpenXRRuntime::BuildCursorLayer(const XrCompositionLayerQuad& screen,
                                     XrCompositionLayerQuad& cursor)
{
    POINT point{};
    int mapWidth = 0, mapHeight = 0;
    const HCURSOR handle = ReadGameCursor(point, mapWidth, mapHeight);
    if (!handle || ((!m_cursorUsable || handle != m_cursorHandle) && !UploadCursor(handle))) {
        return false;
    }
    if (!m_cursorUsable) {
        return false;
    }
    const float metersPerPixelX = screen.size.width / static_cast<float>(mapWidth);
    const float metersPerPixelY = screen.size.height / static_cast<float>(mapHeight);
    const float centerX = CursorBitmapCenter(point.x, m_cursorHotX, m_cursorWidth, mapWidth);
    const float centerY = CursorBitmapCenter(point.y, m_cursorHotY, m_cursorHeight, mapHeight);
    const XrVector3f offset = Rotate(
        screen.pose.orientation,
        {(centerX - 0.5F) * screen.size.width, (0.5F - centerY) * screen.size.height, 0.0F});
    cursor.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    cursor.space = screen.space;
    cursor.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    cursor.subImage.swapchain = m_cursorSwapchain;
    cursor.subImage.imageRect = {{0, 0}, {m_cursorWidth, m_cursorHeight}};
    cursor.subImage.imageArrayIndex = 0;
    cursor.pose.orientation = screen.pose.orientation;
    cursor.pose.position = {screen.pose.position.x + offset.x, screen.pose.position.y + offset.y,
                            screen.pose.position.z + offset.z};
    cursor.size = {static_cast<float>(m_cursorWidth) * metersPerPixelX,
                   static_cast<float>(m_cursorHeight) * metersPerPixelY};
    return true;
}

void OpenXRRuntime::Recenter(XrTime time)
{
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (!XR_SUCCEEDED(xrLocateSpace(m_viewSpace, m_localSpace, time, &location)) ||
        (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
        return;
    }
    const XrQuaternionf yaw = YawOnly(location.pose.orientation);
    const XrVector3f forward = Rotate(yaw, {0.0F, 0.0F, -m_quadDistanceMeters});
    m_worldQuadPose.orientation = yaw;
    m_worldQuadPose.position = {location.pose.position.x + forward.x,
                                location.pose.position.y + forward.y + m_quadHeightMeters,
                                location.pose.position.z + forward.z};
    m_worldQuadValid = true;
    m_recenterRequested = false;
    Log("World quad recentred: pos=(%.3f %.3f %.3f) yaw quat=(%.4f %.4f)",
        m_worldQuadPose.position.x, m_worldQuadPose.position.y, m_worldQuadPose.position.z,
        yaw.y, yaw.w);
}

namespace {

// Self-check of the head look (jitter): per frame, how much the engine camera turned against how
// much the head pose that goes with that frame turned. A perfect pipeline gives a straight line
// (residual ~0); noise or a pose one frame off shows as a residual.
void JitterSample(const float* m, float headYawDeg, float headPitchDeg)
{
    static bool havePrevious = false;
    static double prevEngineYaw = 0.0;
    static double prevEnginePitch = 0.0;
    static double prevHeadYaw = 0.0;
    static double prevHeadPitch = 0.0;
    static double sums[2][3]{};  // per axis: sum dE*dH, sum dH*dH, sum dE*dE
    static int count = 0;
    constexpr double kDeg = 57.295779513;
    const double fx = m[18];
    const double fy = m[22];
    const double fz = m[26];
    const double engineYaw = kDeg * std::atan2(fx, fz);
    const double enginePitch = kDeg * std::asin(std::clamp(fy, -1.0, 1.0));
    const auto unwrap = [](double d) {
        while (d > 180.0) {
            d -= 360.0;
        }
        while (d < -180.0) {
            d += 360.0;
        }
        return d;
    };
    if (havePrevious) {
        const double e[2] = {unwrap(engineYaw - prevEngineYaw), enginePitch - prevEnginePitch};
        const double h[2] = {headYawDeg - prevHeadYaw, headPitchDeg - prevHeadPitch};
        if (std::fabs(e[0]) < 20.0 && std::fabs(e[1]) < 20.0) {
            for (int a = 0; a < 2; ++a) {
                sums[a][0] += e[a] * h[a];
                sums[a][1] += h[a] * h[a];
                sums[a][2] += e[a] * e[a];
            }
            ++count;
        }
    }
    havePrevious = true;
    prevEngineYaw = engineYaw;
    prevEnginePitch = enginePitch;
    prevHeadYaw = headYawDeg;
    prevHeadPitch = headPitchDeg;
    if (count >= 600) {
        double slope[2];
        double residual[2];
        double engineRms[2];
        double headRms[2];
        for (int a = 0; a < 2; ++a) {
            slope[a] = sums[a][1] > 1e-9 ? sums[a][0] / sums[a][1] : 0.0;
            residual[a] = std::sqrt(std::max(0.0, sums[a][2] - slope[a] * sums[a][0]) / count);
            engineRms[a] = std::sqrt(sums[a][2] / count);
            headRms[a] = std::sqrt(sums[a][1] / count);
            sums[a][0] = sums[a][1] = sums[a][2] = 0.0;
        }
        count = 0;
        Log("JITTER per-frame turn (deg): yaw engine %.4f head %.4f slope %.2f residual %.4f | "
            "pitch engine %.4f head %.4f slope %.2f residual %.4f",
            engineRms[0], headRms[0], slope[0], residual[0], engineRms[1], headRms[1], slope[1],
            residual[1]);
    }
}

// Compare the pose offsets against the camera sampled in the world draw.
// Use per-frame deltas so a change in the player's starting direction has no effect.
void PoseLagSample(std::uint64_t present, const std::array<float, 16>& view,
                   const std::array<std::array<float, 2>, 4>& head)
{
    static std::uint64_t lastPresent = 0;
    static std::array<std::array<float, 2>, 4> previousHead{};
    static std::array<std::array<double, 2>, 4> squaredError{};
    static std::array<float, 2> previousCamera{};
    static unsigned samples = 0;
    const std::array<float, 2> camera{
        -std::atan2(view[2], view[10]),
        std::asin(std::clamp(view[6], -1.0F, 1.0F))
    };
    if (lastPresent + 1 != present) {
        squaredError = {};
        samples = 0;
    } else if (lastPresent != 0) {
        for (std::size_t i = 0; i < head.size(); ++i) {
            for (std::size_t axis = 0; axis < camera.size(); ++axis) {
                const float motion = camera[axis] - previousCamera[axis];
                const float expected = head[i][axis] - previousHead[i][axis];
                const float error = std::remainder(motion - expected, 2.0F * 3.14159265F);
                squaredError[i][axis] += static_cast<double>(error) * error;
            }
        }
        if (++samples == 200) {
            std::array<std::array<double, 2>, 4> rms{};
            for (std::size_t i = 0; i < rms.size(); ++i) {
                for (std::size_t axis = 0; axis < camera.size(); ++axis) {
                    rms[i][axis] = 57.295779513 * std::sqrt(squaredError[i][axis] / samples);
                }
            }
            Log("POSE world draw error deg (yaw/pitch), offsets "
                "1=%.3f/%.3f 2=%.3f/%.3f 3=%.3f/%.3f 4=%.3f/%.3f",
                rms[0][0], rms[0][1], rms[1][0], rms[1][1],
                rms[2][0], rms[2][1], rms[3][0], rms[3][1]);
            squaredError = {};
            samples = 0;
        }
    }
    lastPresent = present;
    previousCamera = camera;
    previousHead = head;
}

}  // namespace

// Why a DIBR frame fell back to the last real eye image (rate limited, named like the SnowRunner
// mod's decline outcomes).
static void DibrDecline(const char* reason)
{
    static int counts[8]{};
    static const char* names[8]{};
    int slot = 0;
    while (slot < 7 && names[slot] && names[slot] != reason) {
        ++slot;
    }
    names[slot] = reason;
    if (counts[slot]++ < 3 || counts[slot] % 2000 == 0) {
        Log("DIBR declined: %s (%d so far)", reason, counts[slot]);
    }
}

bool OpenXRRuntime::RenderDibrEye(ID3D11Texture2D* frame, const RenderPose& pose,
                                  const float matrices[32], int renderedEye,
                                  XrQuaternionf currentCamera, XrQuaternionf staleCamera,
                                  XrCompositionLayerProjectionView& syntheticView)
{
    if (!m_dibr.Retain(m_device.Get(), m_context.Get(), frame, renderedEye) ||
        UnormOf(m_quadFormat) != DXGI_FORMAT_R8G8B8A8_UNORM) {
        DibrDecline("frame retain failed or colour format is not RGBA8");
        return false;
    }

    const DibrSceneDepth sceneDepth = EngineStereoLastDibrDepth();
    if (!sceneDepth.texture) {
        static int missingDepthLogs = 0;
        if (missingDepthLogs++ < 4) {
            Log("DIBR declined: no matching scene depth; using the last real eye image");
        }
        return false;
    }
    // Arma maps world and first-person geometry to distinct viewport depth ranges.
    // Both depth terms were translated together. In eye coordinates the
    // native projection is still z_ndc = A + B / eyeDepth.
    const WorldDepthProjection worldDepth = EngineStereoLastWorldDepthProjection();
    const float worldRange = sceneDepth.worldMax - sceneDepth.worldMin;
    const float weaponRange = sceneDepth.weaponMax - sceneDepth.weaponMin;
    const float worldA = sceneDepth.worldMin + worldRange *
                         (worldDepth.valid ? worldDepth.depthScale : matrices[10]);
    const float worldB = worldRange * (worldDepth.valid ? worldDepth.depthBias : matrices[14]);

    if (!std::isfinite(worldA) || !std::isfinite(worldB) || worldB >= -0.001F ||
        std::fabs(matrices[11] - 1.0F) > 0.001F || pose.halfIpd <= 0.0F) {
        DibrDecline("bad depth projection or eye separation");
        return false;
    }

    const int staleEye = 1 - renderedEye;
    Dibr::Settings settings{};
    const auto setProjection = [](const XrFovf& fov, float out[4]) {
        const float left = std::tan(fov.angleLeft), right = std::tan(fov.angleRight);
        const float down = std::tan(fov.angleDown), up = std::tan(fov.angleUp);
        const float width = std::max(0.2F, right - left);
        const float height = std::max(0.2F, up - down);
        out[0] = 2.0F / width;
        out[1] = 2.0F / height;
        out[2] = -(right + left) / width;
        out[3] = -(up + down) / height;
    };
    setProjection(pose.fov[renderedEye], settings.sourceProjection);
    setProjection(pose.fov[staleEye], settings.targetProjection);
    setProjection(m_retainedViews[staleEye].fov, settings.staleProjection);
    // The forward scatter is horizontal. Use rotated AER on headsets whose
    // two images have incompatible vertical frusta.
    const float verticalScale = settings.targetProjection[1] / settings.sourceProjection[1];
    const float verticalOffset = settings.targetProjection[3] -
                                 verticalScale * settings.sourceProjection[3];
    if ((std::fabs(verticalScale - 1.0F) + std::fabs(verticalOffset)) * m_quadHeight > 1.0F) {
        DibrDecline("eye frusta differ vertically (canted-eye headset)");
        return false;
    }

    settings.depth[0] = worldA;
    settings.depth[1] = 1.0F / worldB;
    if (weaponRange > 0.0F) {
        settings.nearDepth[0] = sceneDepth.weaponMin;
        settings.nearDepth[1] = sceneDepth.weaponMax;
    }
    settings.separation[0] = (renderedEye == 0 ? 2.0F : -2.0F) * pose.halfIpd;
    const auto warp = GetConfig().aerWarpMode;
    settings.separation[1] = static_cast<float>(
        warp == AerWarpMode::Off && m_dibrFill == 1 ? 2 : m_dibrFill);
    settings.separation[2] = 2.0F / static_cast<float>(m_quadWidth);
    settings.separation[3] = 1.0F / static_cast<float>(m_quadHeight);

    const XrQuaternionf relative = Multiply(Conjugate(staleCamera), currentCamera);
    const XrVector3f x = Rotate(relative, {1.0F, 0.0F, 0.0F});
    const XrVector3f y = Rotate(relative, {0.0F, 1.0F, 0.0F});
    const XrVector3f z = Rotate(relative, {0.0F, 0.0F, -1.0F});
    settings.staleRotation[0][0] = x.x;
    settings.staleRotation[0][1] = y.x;
    settings.staleRotation[0][2] = z.x;
    settings.staleRotation[1][0] = x.y;
    settings.staleRotation[1][1] = y.y;
    settings.staleRotation[1][2] = z.y;
    settings.staleRotation[2][0] = -x.z;
    settings.staleRotation[2][1] = -y.z;
    settings.staleRotation[2][2] = -z.z;

    DlssWorld::Camera historyCamera{};
    const bool depthHistory = worldDepth.valid && warp == AerWarpMode::Camera6DoF;
    if (depthHistory) {
        historyCamera.view = worldDepth.view;
        float records[2][8]{};
        EngineStereoReadWorldEyeRecords(records);
        for (int i = 0; i < 2; ++i) {
            std::copy_n(records[i], historyCamera.eye[i].size(), historyCamera.eye[i].begin());
        }
        historyCamera.depthScale = worldDepth.depthScale;
        historyCamera.worldMin = sceneDepth.worldMin;
        historyCamera.worldMax = sceneDepth.worldMax;
    }
    const bool profile = PerfEnabled();
    auto& timer = PerfDibrGpuTimer();
    if (profile) timer.Begin(m_device.Get(), m_context.Get());
    ID3D11Texture2D* result = m_dibr.Render(m_context.Get(), frame, sceneDepth.texture,
        renderedEye, settings, depthHistory ? &historyCamera : nullptr, GetConfig().dibrForwardHistory);
    if (profile) timer.End(m_context.Get());
    if (!result || !EnsureAuxSwapchain(m_quadWidth / 2, m_dibrSwapchain, m_dibrImages)) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    result->GetDesc(&desc);
    if (!CopyIntoSwapchain(result, desc, m_dibrSwapchain, m_dibrImages)) {
        return false;
    }
    syntheticView.subImage.swapchain = m_dibrSwapchain;
    syntheticView.subImage.imageRect.offset.x = 0;
    if (!m_dibrReported) {
        m_dibrReported = true;
        Log("DIBR active: %ux%u, reconstructed world depth %.6f..%.6f, real near layer %.6f..%.6f, "
            "history=%s", desc.Width, desc.Height, sceneDepth.worldMin, sceneDepth.worldMax,
            sceneDepth.weaponMin, sceneDepth.weaponMax, m_dibr.ForwardHistoryActive() ? "6DoF forward" :
                m_dibr.HistoryWarpActive() ? "6DoF" : "rotation");
    }
    return true;
}

void OpenXRRuntime::SaveProjectionEyeImages(ID3D11Texture2D* frame, int renderedEye,
                                             bool synthetic, bool fullEye, int warpedEye)
{
    if (m_stereoShotsRemaining > 0 && !Menu::Instance().IsOpen()) {
        if (m_presents - m_stereoShotRequest > 240) {
            Log("STEREO shots expired after %d pairs", m_stereoShotsWritten);
            m_stereoShotsRemaining = 0;
        } else {
            const auto name = L"arma3_native_vr.shot-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(m_stereoShotRequest) +
                L"-" + std::to_wstring(m_stereoShotsWritten + 1);
            WriteEyeImages(frame, renderedEye, synthetic, fullEye, warpedEye, name, m_stereoShotMask);
            if (m_stereoShotMask == 3U) {
                m_stereoShotMask = 0;
                ++m_stereoShotsWritten;
                if (--m_stereoShotsRemaining == 0) {
                    Log("STEREO shots finished: %d pairs", m_stereoShotsWritten);
                }
            }
        }
    }
    if (!GetConfig().frameProbe) {
        return;
    }
    const int capture = FrameProbeCaptureNumber();
    if (capture <= 0) {
        return;
    }
    if (capture != m_eyeCaptureId) {
        m_eyeCaptureId = capture;
        m_eyeCaptureMask = 0;
        m_eyeCaptureStart = m_presents;
    }
    if (m_presents - m_eyeCaptureStart > 4 || m_eyeCaptureMask == 3U) {
        return;
    }
    const std::wstring name = L"arma3_native_vr.frame" + std::to_wstring(capture);
    WriteEyeImages(frame, renderedEye, synthetic, fullEye, warpedEye, name, m_eyeCaptureMask, capture);
    if (m_eyeCaptureMask == 3U) {
        UINT width = 0, height = 0;
        if (auto* hud = EngineStereoHudTexture(width, height)) {
            const auto path = (ModuleDirectory() / (name + L".hud.jpg")).wstring();
            if (DumpTextureJpeg(hud, path.c_str())) {
                Log("PROBE HUD image written for capture %d (%ux%u)", capture, width, height);
            }
        }
    }
}

void OpenXRRuntime::RequestStereoShots(int pairs)
{
    m_stereoShotsRemaining = pairs;
    m_stereoShotsWritten = 0;
    m_stereoShotMask = 0;
    m_stereoShotRequest = m_presents;
    Menu::Instance().SetOpen(false);
    Log("STEREO shots requested: %d pairs", pairs);
}

void OpenXRRuntime::WriteEyeImages(ID3D11Texture2D* frame, int renderedEye, bool synthetic,
                                   bool fullEye, int warpedEye, const std::wstring& name,
                                   unsigned& mask, int probeId)
{
    const UINT half = m_quadWidth / 2;
    for (int eye = 0; eye < 2; ++eye) {
        const unsigned bit = 1U << eye;
        const bool reprojected = synthetic && eye != renderedEye;
        if ((mask & bit) != 0 ||
            (renderedEye >= 0 && eye != renderedEye && !reprojected)) {
            continue;
        }
        const UINT x = eye == 0 ? 0U : half;
        const D3D11_BOX rect{x, 0, 0, x + half, m_quadHeight, 1};
        ID3D11Texture2D* texture = reprojected ? m_dibr.OutputTexture() : frame;
        const auto path = ModuleDirectory() / (name + (eye == 0 ? L".eye-L.jpg" : L".eye-R.jpg"));
        const bool saved = texture &&
            DumpTextureJpeg(texture, path.c_str(), (reprojected || fullEye) ? nullptr : &rect);
        const char* kind = reprojected ? "DIBR" : eye == warpedEye ? "stale-eye warp" : "real";
        const char eyeName = eye == 0 ? 'L' : 'R';
        const auto present = static_cast<unsigned long long>(m_presents);
        if (probeId != 0) {
            Log("PROBE eye %c capture %d: %s, %s, Present %llu",
                eyeName, probeId, kind, saved ? "written" : "failed", present);
        } else {
            Log("STEREO image eye %c: %s, %s, Present %llu",
                eyeName, kind, saved ? "written" : "failed", present);
        }
        if (saved) {
            mask |= bit;
        }
    }
}

// Present the captured HUD or inventory texture on the room-anchored panel.
bool OpenXRRuntime::BuildHudLayer(XrCompositionLayerQuad& hud, bool inventory)
{
    UINT width = 0;
    UINT height = 0;
    ID3D11Texture2D* texture = EngineStereoHudTexture(width, height);
    if (!texture || width == 0 || height == 0 || !m_headReferenceValid) {
        return false;
    }
    if (m_hudSwapchain != XR_NULL_HANDLE && (m_hudWidth != width || m_hudHeight != height)) {
        xrDestroySwapchain(m_hudSwapchain);
        m_hudSwapchain = XR_NULL_HANDLE;
        m_hudImages.clear();
    }
    if (m_hudSwapchain == XR_NULL_HANDLE) {
        const ULONGLONG now = GetTickCount64();
        if (now < m_hudRetryAfter) {
            return false;
        }
        m_hudRetryAfter = now + 2000;
        XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                                XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.format = static_cast<std::int64_t>(m_quadFormat);
        createInfo.sampleCount = 1;
        createInfo.width = width;
        createInfo.height = height;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;
        if (!Check(xrCreateSwapchain(m_session, &createInfo, &m_hudSwapchain),
                   "creating HUD swapchain")) {
            m_hudSwapchain = XR_NULL_HANDLE;
            return false;
        }
        std::uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(m_hudSwapchain, 0, &imageCount, nullptr);
        m_hudImages.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (!Check(xrEnumerateSwapchainImages(
                       m_hudSwapchain, imageCount, &imageCount,
                       reinterpret_cast<XrSwapchainImageBaseHeader*>(m_hudImages.data())),
                   "enumerating HUD images")) {
            xrDestroySwapchain(m_hudSwapchain);
            m_hudSwapchain = XR_NULL_HANDLE;
            m_hudImages.clear();
            return false;
        }
        m_hudWidth = width;
        m_hudHeight = height;
        m_hudRetryAfter = 0;
        Log("HUD quad: swapchain %ux%u, %u images", width, height, imageCount);
    }
    std::uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!Check(xrAcquireSwapchainImage(m_hudSwapchain, &acquireInfo, &index),
               "acquiring HUD image")) {
        return false;
    }
    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = 100'000'000;
    const bool ready = Check(xrWaitSwapchainImage(m_hudSwapchain, &waitInfo), "waiting HUD image");
    if (ready && index < m_hudImages.size() && m_hudImages[index].texture) {
        m_context->CopySubresourceRegion(m_hudImages[index].texture, 0, 0, 0, 0, texture, 0,
                                         nullptr);
    }
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const bool released = Check(xrReleaseSwapchainImage(m_hudSwapchain, &releaseInfo),
                                "releasing HUD image");
    if (!ready || !released) {
        return false;
    }
    // Both panels use the saved position and forward direction from F11. Inventory
    // keeps the screen's angular size and is centred at the saved eye height.
    const float distance = inventory ? m_quadDistanceMeters : std::max(0.4F, m_hudDistance);
    const float spanX = inventory ? m_quadWidthMeters : 1.9F * distance * m_hudScale;
    const float shiftX = inventory ? 0.0F : m_hudShiftX * 1.9F * distance;
    const float shiftY = inventory ? 0.0F : m_hudShiftY * 2.0F * distance;
    const XrVector3f offset = Rotate(m_headReference, {shiftX, shiftY, -distance});
    hud.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    hud.space = m_localSpace;
    hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    hud.subImage.swapchain = m_hudSwapchain;
    hud.subImage.imageRect = {{0, 0}, {static_cast<std::int32_t>(width),
                                       static_cast<std::int32_t>(height)}};
    hud.subImage.imageArrayIndex = 0;
    hud.pose.orientation = m_headReference;
    hud.pose.position = {m_headOrigin.x + offset.x, m_headOrigin.y + offset.y,
                         m_headOrigin.z + offset.z};
    hud.size = {spanX, QuadHeightMeters(spanX, width, height)};
    if (!inventory && m_hudLogs++ < 3) {
        Log("HUD quad: %.2f x %.2f m at %.2f m, fixed in the room in front of the recentre "
            "direction",
            hud.size.width, hud.size.height, distance);
    }
    return true;
}

bool OpenXRRuntime::SubmitPreparedFrame(ID3D11Texture2D* backBuffer, bool withQuad)
{
    if (!m_framePrepared) {
        return false;
    }
    LogSteamVrSceneSample();
    if (m_calibration.Active() && m_gameProjectionView) {
        float matrices[32]{};
        m_gameProjectionView(matrices);
        m_calibration.Sample(matrices);
    }
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad cursorQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad hudQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad rightQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjection lensProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView projectionViews[2]{};
    const XrCompositionLayerBaseHeader* layers[6] = {};
    XrCompositionLayerQuad menuQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    std::uint32_t layerCount = 0;
    bool submittedSteamVrQuads = false;
    std::uint32_t drawPoseId = 0;
    float submittedRoll = 0.0F;
    OpticsSubmitTrace opticsTrace;

    if (m_poseRing[0].valid) {
        const XrVector3f forward = Rotate(m_poseRing[0].head.orientation, {0.0F, 0.0F, -1.0F});
        const float pitch = std::asin(std::clamp(forward.y, -1.0F, 1.0F)) * kDegrees;
        const bool lookingDown = pitch < (m_lookingDown ? -40.0F : -50.0F);
        if (lookingDown != m_lookingDown) {
            m_lookingDown = lookingDown;
            Log("LOOKDOWN %s: headset pitch %.1f deg, camera target %.1f deg, weapon draws %u, "
                "render %d, projection %d, state %d",
                lookingDown ? "start" : "end", pitch, m_poseRing[0].pitch * kDegrees,
                EngineStereoLastWeaponDraws(), m_shouldRender ? 1 : 0,
                m_poseRing[0].projection ? 1 : 0, static_cast<int>(m_sessionState));
        }
    }
    if (!m_shouldRender) {
        ++m_notRendered;
    }
    const bool captureDetails = GetConfig().detailedCapture;
    const double traceStart = captureDetails ? NowMs() : 0.0;
    std::uint32_t matchedPose = 0;
    bool dlssApplied = false;
    bool staleWarpApplied = false;
    bool rememberStalePair = false;
    int nextStaleEye = -1;
    const int renderedEye = m_renderEye;
    const bool staleMode = GetConfig().staleEyeWarp;
    const int legacyEye = staleMode ? -1 : renderedEye;
    m_renderEye = -1;
    EngineStereoSetRenderEye(-1);
    if (withQuad && m_shouldRender && backBuffer) {
        D3D11_TEXTURE2D_DESC windowDescription{};  // the game's window-sized back buffer
        backBuffer->GetDesc(&windowDescription);
        VirtualCursorSetPicture(static_cast<int>(windowDescription.Width),
                                static_cast<int>(windowDescription.Height));
        // The inventory is an interactive panel over the world. Other cursor-driven
        // displays still use their original flat screen.
        const bool cursorShowing = WindowsCursorShowing();
        m_inventoryOpen = GetConfig().inventoryInVr && GetConfig().monoScreen &&
            GetConfig().virtualBackBuffer && GetConfig().headTracking && m_worldViewEnabled &&
            m_projectionEnabled && m_sbsInput && EngineStereoLastInventoryDraw();
        ID3D11Texture2D* independentScene = EngineStereoVirtualTexture();
        UINT inventoryWidth = 0, inventoryHeight = 0;
        const bool inventoryWorld = m_inventoryOpen && independentScene &&
            m_poseRing[0].projection && m_poseRing[0].viewsValid &&
            EngineStereoHudTexture(inventoryWidth, inventoryHeight);
        int poseBaseIndex = 0;
        RenderPose renderPose = m_poseRing[std::clamp(m_poseLag, 1, 4) - 1];
        if (GetConfig().poseBySequence && GetConfig().headTracking) {
            // The engine polls the head sample at its own rate (not once per frame): show the
            // frame at the sample it actually received. Alt+Home/End adds frames on top.
            const std::uint32_t used = m_feed.LastReturnedId();
            int found = -1;
            for (int i = 0; used != 0 && i < 8; ++i) {
                if (m_poseRing[i].valid && m_poseRing[i].sequence == used) {
                    found = i;
                    break;
                }
            }
            if (found >= 0) {
                poseBaseIndex = found;
                renderPose = m_poseRing[std::min(found + std::clamp(m_poseLag, 1, 4) - 1, 7)];
                m_lastConsumedPose = m_poseRing[found];
                ++m_poseHits[found];
            } else {
                // While a dialog owns input the game can stop polling FreeTrack. Keep the
                // pose behind the rendered image even after it has aged out of the ring.
                if (m_inventoryOpen && used != 0 && m_lastConsumedPose.sequence == used &&
                    m_lastConsumedPose.headReferenceEpoch == m_headReferenceEpoch) {
                    renderPose = m_lastConsumedPose;
                }
                ++m_poseHits[8];
            }
        }
        matchedPose = renderPose.sequence;
        const bool worldView = GetConfig().headTracking && m_worldViewEnabled &&
                               renderPose.valid && m_headReferenceValid &&
                               (inventoryWorld || (!cursorShowing && !m_inventoryOpen));
        ID3D11Texture2D* worldTexture = worldView ? independentScene : nullptr;
        opticsTrace.source = worldView && worldTexture ? "independent_scene" : "desktop_backbuffer";
        if (worldTexture) {
            backBuffer = worldTexture;
        }
        D3D11_TEXTURE2D_DESC description{};
        backBuffer->GetDesc(&description);
        if (worldView && GetConfig().virtualBackBuffer) {
            static UINT lastWidth = 0, lastHeight = 0;
            static bool lastSeparate = false;
            const bool separate = worldTexture != nullptr;
            if (lastWidth != description.Width || lastHeight != description.Height ||
                lastSeparate != separate) {
                lastWidth = description.Width;
                lastHeight = description.Height;
                lastSeparate = separate;
                Log("VR world source %s %ux%u; desktop %ux%u",
                    separate ? "independent scene" : "desktop fallback",
                    description.Width, description.Height, windowDescription.Width, windowDescription.Height);
            }
        }
        int eye = worldView && !m_sbsInput ? renderPose.eye : 0;
        if (eye != 0 && !(EnsureQuadSwapchain(description) && EnsureRightSwapchain())) {
            eye = 0;
        }
        // Screen mode with depth: both eye images are drawn from this one frame.
        // Menus, map and inventory (cursor shown) stay flat so the UI is clean.
        const bool depthStereo =
            GetConfig().depthStereo && !m_sbsInput && !worldView && !m_calibration.Active() &&
            EnsureQuadSwapchain(description) &&
            RenderDepthStereo(backBuffer, cursorShowing || !m_stereoEnabled);
        // Finish a single-eye frame already drawn before checking whether to
        // seed a new pair. Use full stereo while a sight is active.
        const bool aerPair = renderedEye >= 0 ||
            (!m_poseRing[0].sightActive && !m_poseRing[0].scopeStatic &&
             !EngineStereoScopeUi() &&
             (!m_dibrShift || std::fabs(EngineStereoDrawZoom() - 1.0F) <= 0.001F));
        const bool useAer = !inventoryWorld && !staleMode && m_alternateEyeProjection && aerPair &&
                            m_sbsInput && m_projectionEnabled && worldView && m_poseRing[0].projection &&
                            m_stereoEnabled && !m_headRecenterRequested && !m_calibration.Active() &&
                            !(m_poseRing[0].scopeStatic && renderedEye < 0);
        const auto depth = EngineStereoLastDibrDepth();
        const auto sceneProjection = EngineStereoLastWorldDepthProjection();
        if (worldView && sceneProjection.valid && poseBaseIndex <= 4 &&
            m_poseRing[poseBaseIndex + 3].valid && !m_poseRing[0].sightActive &&
            !Menu::Instance().IsOpen() && !m_calibration.Active()) {
            std::array<std::array<float, 2>, 4> head{};
            for (int i = 0; i < 4; ++i) {
                const auto& pose = m_poseRing[poseBaseIndex + i];
                head[i] = {pose.yaw, pose.pitch};
            }
            PoseLagSample(m_presents, sceneProjection.view, head);
        }
        const bool regularProjection = worldTexture && m_sbsInput && m_projectionEnabled &&
            m_poseRing[0].projection && m_poseRing[0].viewsValid && !EngineStereoScopeUi() &&
            EngineStereoDrawZoom() <= 1.03F && !m_poseRing[0].scopeStatic;
        const bool staleCandidate = !inventoryWorld && staleMode && regularProjection &&
            !m_poseRing[0].sightActive && m_stereoEnabled &&
            !m_headRecenterRequested && !m_recenterRequested && !m_calibration.Active();
        const bool hasNativeNear = depth.weaponMax > depth.weaponMin || EngineStereoLastWeaponDraws() == 0;
        const auto stereoCamera = [&](bool capturedView) {
            DlssWorld::Camera camera{};
            if (capturedView) {
                camera.view = sceneProjection.view;
            } else if (m_gameProjectionView) {
                float matrices[32]{};
                m_gameProjectionView(matrices);
                std::copy_n(matrices + 16, camera.view.size(), camera.view.begin());
            }
            float records[2][8]{};
            EngineStereoReadWorldEyeRecords(records);
            for (int i = 0; i < 2; ++i) {
                std::copy_n(records[i], camera.eye[i].size(), camera.eye[i].begin());
            }
            camera.depthScale = sceneProjection.depthScale;
            camera.depthBias = sceneProjection.depthBias;
            camera.worldMin = depth.worldMin;
            camera.worldMax = depth.worldMax;
            camera.weaponMin = depth.weaponMin;
            camera.weaponMax = depth.weaponMax;
            return camera;
        };

        StaleEyeWarp::Pair reconstructed{};
        if (staleCandidate && depth.texture && sceneProjection.valid && hasNativeNear) {
            const auto camera = stereoCamera(true);
            const auto time = NowMs();
            if (renderedEye < 0) {
                if (m_staleEye.Seed(m_device.Get(), m_context.Get(), worldTexture, depth.texture,
                                    camera, m_presents, time)) {
                    nextStaleEye = 0;
                    if (!m_staleReported && m_staleFallbacks < 4) {
                        Log("AER 6DoF history ready at present %llu: world depth %.6f..%.6f, near %.6f..%.6f",
                            static_cast<unsigned long long>(m_presents), camera.worldMin, camera.worldMax,
                            camera.weaponMin, camera.weaponMax);
                    }
                }
            } else {
                auto& timer = PerfStaleEyeGpuTimer();
                const bool perf = PerfEnabled();
                if (perf) timer.Begin(m_device.Get(), m_context.Get());
                reconstructed = m_staleEye.Reproject(m_device.Get(), m_context.Get(), worldTexture,
                                                      depth.texture, camera, renderedEye, m_presents, time);
                if (perf) timer.End(m_context.Get());
                if (reconstructed.color) {
                    nextStaleEye = 1 - renderedEye;
                    staleWarpApplied = true;
                    backBuffer = reconstructed.color;
                    if (!m_staleReported) {
                        Log("AER 6DoF active: %ux%u stereo pair, current first-person layer for both eyes",
                            description.Width, description.Height);
                        m_staleReported = true;
                    }
                }
            }
        } else if (staleCandidate && renderedEye < 0 && !m_staleReported) {
            static unsigned missingDepthLogs = 0;
            if (missingDepthLogs++ < 4) {
                Log("AER 6DoF waiting for render inputs: world depth=%d, camera=%d, first-person depth=%d",
                    depth.texture != nullptr, sceneProjection.valid, hasNativeNear);
            }
        } else if (staleMode && worldView && worldTexture && !staleCandidate && !m_staleReported) {
            static unsigned blockedLogs = 0;
            if (blockedLogs++ < 4) {
                Log("AER 6DoF gated: sbs=%d projection=%d eyes=%d stereo=%d zoom=%.3f scope=%d sight=%d "
                    "head recenter=%d quad recenter=%d calibration=%d",
                    m_sbsInput, m_projectionEnabled && m_poseRing[0].projection, m_poseRing[0].viewsValid,
                    m_stereoEnabled, EngineStereoDrawZoom(), EngineStereoScopeUi() || m_poseRing[0].scopeStatic,
                    m_poseRing[0].sightActive, m_headRecenterRequested, m_recenterRequested, m_calibration.Active());
            }
        }
        const bool incompleteStalePair = staleMode && renderedEye >= 0 && worldView && !staleWarpApplied;
        const bool reuseStalePair = incompleteStalePair && m_stalePairValid;
        if (incompleteStalePair) {
            m_staleEye.ResetHistory();
            if (m_staleFallbacks++ < 10) {
                Log("AER 6DoF reset at present %llu (eligible=%d depth=%d projection=%d near=%d); restoring full stereo",
                    static_cast<unsigned long long>(m_presents), staleCandidate, depth.texture != nullptr,
                    sceneProjection.valid, hasNativeNear);
            }
        }
        const bool tryDlss = GetConfig().dlssBalancedExperimental && worldView;
        const bool dlssEligible = worldTexture && !useAer && (renderedEye < 0 || staleWarpApplied) &&
            m_poseRing[0].projection && !EngineStereoScopeUi() &&
            EngineStereoDrawZoom() <= 1.03F && (m_gameProjectionView || staleMode);
        if (tryDlss && dlssEligible) {
            if (depth.texture && sceneProjection.valid) {
                auto camera = stereoCamera(staleMode);
                camera.synthesized = staleWarpApplied;
                const bool perfOn = PerfEnabled();
                const double perfDlssT0 = perfOn ? PerfNowMs() : 0.0;
                if (perfOn) {
                    PerfDlssGpuTimer().Begin(m_device.Get(), m_context.Get());
                }
                ID3D11Texture2D* upscaled = m_dlssWorld.Render(m_device.Get(), m_context.Get(),
                        backBuffer, reconstructed.depth ? reconstructed.depth : depth.texture,
                        camera, {2560, 2880}, m_presents);
                if (perfOn) {
                    PerfDlssGpuTimer().End(m_context.Get());
                    PerfAddDlssCpu(PerfNowMs() - perfDlssT0);
                }
                if (upscaled) {
                    backBuffer = upscaled;
                    backBuffer->GetDesc(&description);
                    dlssApplied = true;
                }
            } else {
                static bool reportedMissingDepth = false;
                if (!reportedMissingDepth) {
                    Log("DLSS Balanced waiting for the main stereo depth pass");
                    reportedMissingDepth = true;
                }
            }
        } else if (tryDlss) {
            static bool reportedIneligible = false;
            if (!reportedIneligible) {
                Log("DLSS Balanced waiting for eligible gameplay (scene=%d eye=%d projection=%d zoom=%.3f)",
                    worldTexture ? 1 : 0, renderedEye, m_poseRing[0].projection ? 1 : 0, EngineStereoDrawZoom());
                reportedIneligible = true;
            }
        }
        if (staleWarpApplied && !dlssApplied) {
            backBuffer->GetDesc(&description);
        }
        const int casStrength = GetConfig().casSharpnessPercent;
        const bool vrsActive = EngineStereoLastWorldVrs();
        const bool previewRadius = vrsActive &&
            (GetConfig().worldVrsShowRings || (Menu::Instance().IsOpen() && m_vrsRadiusPreview));
        if ((casStrength > 0 || previewRadius) && regularProjection && m_stereoEnabled && !useAer && legacyEye < 0 &&
            !staleMode && !dlssApplied && !m_poseRing[0].sightActive && !m_calibration.Active()) {
            WorldShadingGuide guide;
            const bool hasGuide = vrsActive && GetWorldShadingGuide(description.Width, description.Height, guide);
            const bool sample = PerfEnabled() && m_presents % 30 == 0;
            if (sample) PerfCasGpuTimer().Begin(m_device.Get(), m_context.Get());
            ID3D11Texture2D* sharpened = m_stereoCas.Render(m_context.Get(), backBuffer, casStrength,
                                                          hasGuide ? &guide : nullptr,
                                                          previewRadius && hasGuide);
            if (sample) PerfCasGpuTimer().End(m_context.Get());
            if (sharpened) backBuffer = sharpened;
        }
        // Scope lens: alternating frames. Pass 1 = world at normal scale, pass 2 = the magnified
        // window; each goes to its own swapchain and both are submitted every frame.
        const int lensPass =
            (worldView && m_sbsInput && m_projectionEnabled && m_poseRing[0].projection &&
             m_poseRing[0].viewsValid && m_stereoEnabled && !useAer && renderedEye < 0) ?
                EngineStereoLensPass() : 0;
        opticsTrace.lensPass = lensPass;
        if (lensPass == 0) {
            m_lensWorldValid = false;
            m_lensWindowValid = false;
        }
        // The window of the eye image that the lens shows: centred on the aim axis (tangent 0),
        // +-a around it, a = tan(half of the apparent field). Pixel rectangle in the whole
        // image (eye offset included) and the field of view of exactly that rectangle.
        const auto lensWindow = [&](int i, float rect[4], XrFovf* fov) {
            const XrFovf& view = m_poseRing[0].fov[i];
            const float tanL = -std::tan(view.angleLeft);
            const float tanR = std::tan(view.angleRight);
            const float tanU = std::tan(view.angleUp);
            const float tanD = -std::tan(view.angleDown);
            const float spanX = std::max(0.2F, tanL + tanR);
            const float spanY = std::max(0.2F, tanU + tanD);
            const float a = std::tan(0.5F * GetConfig().scopeLensDegrees * 0.0174533F);
            const float halfWidth = static_cast<float>(description.Width / 2);
            const float height = static_cast<float>(description.Height);
            const float x0 = std::clamp((tanL - a) / spanX * halfWidth, 0.0F, halfWidth - 2.0F);
            const float x1 = std::clamp((tanL + a) / spanX * halfWidth, x0 + 2.0F, halfWidth);
            const float y0 = std::clamp((tanU - a) / spanY * height, 0.0F, height - 2.0F);
            const float y1 = std::clamp((tanU + a) / spanY * height, y0 + 2.0F, height);
            rect[0] = (i == 0 ? 0.0F : halfWidth) + std::floor(x0);
            rect[1] = std::floor(y0);
            rect[2] = std::floor(x1) - std::floor(x0);
            rect[3] = std::floor(y1) - std::floor(y0);
            if (fov) {
                const float ux0 = std::floor(x0) / halfWidth * spanX - tanL;
                const float ux1 = std::floor(x1) / halfWidth * spanX - tanL;
                const float vy0 = tanU - std::floor(y0) / height * spanY;
                const float vy1 = tanU - std::floor(y1) / height * spanY;
                *fov = {std::atan(ux0), std::atan(ux1), std::atan(vy0), std::atan(vy1)};
            }
        };
        bool cursorDrawnIntoScreen = false;
        if (lensPass == 2) {
            m_afterCopy = [&](ID3D11Texture2D* texture) {
                if (ID3D11RenderTargetView* target = EyeTarget(texture)) {
                    for (int i = 0; i < 2; ++i) {
                        float rect[4];
                        lensWindow(i, rect, nullptr);
                        LensMaskWriteAlpha(m_device.Get(), m_context.Get(), target, rect);
                    }
                }
            };
        } else if (m_steamVr && !worldView && cursorShowing && GetConfig().monoScreen &&
                   GetConfig().drawCursor && !Menu::Instance().IsOpen()) {
            m_afterCopy = [&](ID3D11Texture2D* texture) {
                cursorDrawnIntoScreen = DrawCursorIntoScreen(texture);
            };
        }
        const bool projectionReady = worldView && m_sbsInput && m_projectionEnabled &&
                                     m_poseRing[0].projection && m_poseRing[0].viewsValid;
        const bool splitProjection = m_steamVr && projectionReady && !reuseStalePair &&
            !incompleteStalePair && !useAer && legacyEye < 0 && lensPass == 0 &&
            EnsureQuadSwapchain(description) && CopyProjectionEyes(backBuffer, description);
        const bool copied = reuseStalePair || depthStereo || splitProjection ||
            (!incompleteStalePair && EnsureQuadSwapchain(description) &&
             (legacyEye < 0 || (useAer && EnsureRightSwapchain() &&
                 m_retainedViews[0].subImage.swapchain == m_quadSwapchain &&
                 m_retainedViews[1].subImage.swapchain == m_rightSwapchain)) &&
             (lensPass != 2 || EnsureRightSwapchain()) &&
             CopyIntoQuad(backBuffer, description,
                          legacyEye == 1 || eye > 0 || lensPass == 2));
        m_afterCopy = nullptr;
        if (copied) {
            if (m_steamVr && worldTexture && sceneProjection.valid && !reuseStalePair &&
                (!staleCandidate || staleWarpApplied || m_staleReported)) {
                SampleSteamVrScene(backBuffer, description, staleWarpApplied);
            }
            const bool world = GetConfig().quadAnchor == 1;
            if (world && (m_recenterRequested || !m_worldQuadValid)) {
                Recenter(m_displayTime);
            }
            quad.layerFlags = 0;  // opaque: the back buffer alpha is not meaningful
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = eye > 0 ? m_rightSwapchain : m_quadSwapchain;
            quad.subImage.imageRect = {{0, 0},
                                       {static_cast<std::int32_t>(m_quadWidth),
                                        static_cast<std::int32_t>(m_quadHeight)}};
            quad.subImage.imageArrayIndex = 0;
            quad.size = {m_quadWidthMeters,
                         QuadHeightMeters(m_quadWidthMeters, windowDescription.Width,
                                          windowDescription.Height)};
            if (world && m_worldQuadValid) {
                quad.space = m_localSpace;
                quad.pose = m_worldQuadPose;
            } else {
                quad.space = m_viewSpace;
                quad.pose = {{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, m_quadHeightMeters, -m_quadDistanceMeters}};
            }
            layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
            layerCount = 1;
            // Menus, map, inventory: the engine drew ONE normal frame (MonoScreen), shown to both
            // eyes on the flat screen instead of two doubled halves.
            const bool monoScreen = GetConfig().monoScreen && cursorShowing && !worldView;
            if (depthStereo) {
                rightQuad = quad;
                quad.eyeVisibility = XR_EYE_VISIBILITY_LEFT;
                quad.subImage.swapchain = m_quadSwapchain;
                rightQuad.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
                rightQuad.subImage.swapchain = m_rightSwapchain;
                layers[1] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&rightQuad);
                layerCount = 2;
            } else if (m_sbsInput && !monoScreen) {
                // The frame is side-by-side stereo (EngineStereo or an external tool):
                // the left half goes to the left eye, the right half to the right eye.
                // In the world view both quads are then placed by BuildWorldView below.
                const std::int32_t half = static_cast<std::int32_t>(m_quadWidth / 2);
                rightQuad = quad;
                quad.eyeVisibility = XR_EYE_VISIBILITY_LEFT;
                quad.subImage.imageRect = {{0, 0}, {half, static_cast<std::int32_t>(m_quadHeight)}};
                rightQuad.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
                rightQuad.subImage.imageRect = {{half, 0},
                                                {half, static_cast<std::int32_t>(m_quadHeight)}};
                layers[1] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&rightQuad);
                layerCount = 2;
                ++m_sbsFrames;
            }
            if (worldView) {
                ++m_worldFrames;
                // Two seconds into the first world view: measure the engine once.
                if (++m_worldFramesTotal == 160 && GetConfig().autoCalibrate &&
                    !m_calibrationTried) {
                    m_calibrationTried = true;
                    m_headRecenterRequested = true;
                    m_calibration.Start();
                }
                if (m_gameProjectionView && !m_aiming) {
                    float jitterMatrices[32]{};
                    m_gameProjectionView(jitterMatrices);
                    JitterSample(jitterMatrices, renderPose.yaw * 57.29578F,
                                 renderPose.pitch * 57.29578F);
                }
                ReadGameFov(renderPose.tanHorizontal, renderPose.tanVertical);
                if (!m_sbsInput) {
                    MeasureEyeSeparation(eye);
                }
                // FreeTrack may use an older rotation sample. The D3D projection patch
                // uses the latest frustum, lean and eye offsets published before rendering.
                const RenderPose& stereoPose = m_poseRing[0];
                if (m_sbsInput && layerCount == 2 && stereoPose.projection &&
                    stereoPose.viewsValid && m_projectionEnabled) {
                    // Projection layer: the halves are the two eyes drawn with the headset's own
                    // frustum; the layer carries the pose and field of view they were drawn with.
                    // Static scope: the picture is glued to the face (view space), the head does
                    // not move it; the frame was drawn with no head rotation or position at all.
                    const bool staticView = stereoPose.scopeStatic && lensPass == 0 && !useAer;
                    const XrQuaternionf orientation =
                        staticView ? XrQuaternionf{0.0F, 0.0F, 0.0F, 1.0F}
                        : ((lensPass == 2 && stereoPose.lens)
                               ? stereoPose.lensOrientation
                               : Multiply(m_headReference,
                                          YawPitch(renderPose.yaw, renderPose.pitch)));
                    XrQuaternionf cameraRoll{0.0F, 0.0F, 0.0F, 1.0F};
                    if (GetConfig().stabilizeArmaRoll && lensPass == 0) {
                        float roll = 0.0F;
                        const auto measureRoll =
                            m_vehicleMode ? CameraRollFromView : OnFootCameraRollFromView;
                        if (sceneProjection.valid) {
                            roll = measureRoll(sceneProjection.view[4], sceneProjection.view[5]);
                        } else if (m_gameProjectionView) {
                            float matrices[32]{};
                            m_gameProjectionView(matrices);
                            roll = measureRoll(matrices[20], matrices[21]);
                        }
                        cameraRoll = {0.0F, 0.0F, std::sin(roll * 0.5F), std::cos(roll * 0.5F)};
                        submittedRoll = roll * kDegrees;
                    }
                    XrQuaternionf frameOrientation = orientation;
                    if (regularProjection && renderedEye < 0 && !staleMode &&
                        !m_alternateEyeProjection && m_stereoEnabled && lensPass == 0 &&
                        !m_vehicleMode && !m_aiming && !stereoPose.sightActive &&
                        !m_headRecenterRequested && !m_recenterRequested && !m_calibration.Active() &&
                        !dlssApplied && GetConfig().poseBySequence && m_poseLag == 1) {
                        const auto draw = EngineStereoLastWorldDrawPoseTrace();
                        // FreeTrack can be polled again between rendering and xrEndFrame.
                        if (draw.returnedBefore != 0 && draw.returnedBefore == draw.returnedAfter) {
                            for (const auto& sample : m_poseRing) {
                                if (sample.valid && sample.sequence == draw.returnedAfter &&
                                    sample.headReferenceEpoch == m_headReferenceEpoch &&
                                    !sample.sightActive && !sample.scopeStatic) {
                                    frameOrientation = Multiply(m_headReference,
                                        YawPitch(sample.yaw, sample.pitch));
                                    drawPoseId = sample.sequence;
                                    break;
                                }
                            }
                        }
                    }
                    // This experiment only changes the submitted image rotation.
                    const XrQuaternionf imageOrientation = Multiply(frameOrientation, cameraRoll);
                    const float zoom = EngineStereoDrawZoom();
                    // The picture was drawn zoom times narrower; shown at zoom^(1-m) times
                    // narrower it is magnified zoom^m times (a window, like a scope lens).
                    // Scope lens: the whole picture is drawn zoom times narrower and shown at full
                    // magnification, but only a window around the aim axis is submitted (below).
                    const bool scopeLens = EngineStereoScopeLens();
                    const float windowDivisor =
                        scopeLens ? 1.0F :
                        (zoom > 1.0F ? std::pow(zoom, 1.0F - GetConfig().zoomMagnification) : zoom);
                    opticsTrace.windowDivisor = windowDivisor;
                    if (captureDetails) {
                        opticsTrace.unzoomedEyeSpan =
                            std::tan(stereoPose.fov[0].angleRight) - std::tan(stereoPose.fov[0].angleLeft);
                    }
                    const float separationDivisor =
                        zoom > 1.0F ? std::pow(zoom, 0.5F * GetConfig().zoomMagnification) : 1.0F;
                    const std::int32_t half = static_cast<std::int32_t>(m_quadWidth / 2);
                    // AER native: the frame is a normal single-camera frame of one eye (full target,
                    // the engine's own projection), shown with that projection's field of view.
                    const bool nativeCfg =
                        GetConfig().aerNative && m_alternateEyeProjection && !m_dibrShift;
                    float nativeMatrices[32]{};
                    bool haveNative = false;
                    if (nativeCfg && useAer && renderedEye >= 0 && m_gameProjectionView) {
                        m_gameProjectionView(nativeMatrices);
                        haveNative = nativeMatrices[0] > 0.2F && nativeMatrices[5] > 0.2F;
                    }
                    const bool nativeView = haveNative;
                    const float appliedClipW = EngineStereoLastWorldDepthProjection().clipWOffset;
                    const float forwardCorrection = stereoPose.clipWOffset - appliedClipW;
                    const XrVector3f projectionCenter =
                        staticView ? XrVector3f{0.0F, 0.0F, 0.0F}
                                   : XrVector3f{stereoPose.projectionCenter.x +
                                                    forwardCorrection * stereoPose.leanForward.x,
                                                stereoPose.projectionCenter.y +
                                                    forwardCorrection * stereoPose.leanForward.y,
                                                stereoPose.projectionCenter.z +
                                                    forwardCorrection * stereoPose.leanForward.z};
                    for (int i = 0; i < 2; ++i) {
                        projectionViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                        const float eyeSign = i == 0 ? -1.0F : 1.0F;
                        const float eyeX =
                            (eyeSign + stereoPose.aimBias) * stereoPose.halfIpd / separationDivisor;
                        const XrVector3f baseline = stereoPose.halfBaseline;
                        const XrVector3f eyeOffset = stereoPose.physicalEyes
                            ? XrVector3f{eyeSign * baseline.x, eyeSign * baseline.y, eyeSign * baseline.z}
                            : XrVector3f{eyeX, 0.0F, 0.0F};
                        const XrVector3f offset = Rotate(orientation, eyeOffset);
                        projectionViews[i].pose.orientation = imageOrientation;
                        projectionViews[i].pose.position = {projectionCenter.x + offset.x,
                                                            projectionCenter.y + offset.y,
                                                            projectionCenter.z + offset.z};
                        XrFovf fov = stereoPose.fov[i];
                        fov.angleLeft = std::atan(std::tan(fov.angleLeft) / windowDivisor);
                        fov.angleRight = std::atan(std::tan(fov.angleRight) / windowDivisor);
                        fov.angleUp = std::atan(std::tan(fov.angleUp) / windowDivisor);
                        fov.angleDown = std::atan(std::tan(fov.angleDown) / windowDivisor);
                        projectionViews[i].fov = fov;
                        projectionViews[i].subImage.swapchain = m_quadSwapchain;
                        projectionViews[i].subImage.imageRect = {
                            {i == 0 ? 0 : half, 0}, {half, static_cast<std::int32_t>(m_quadHeight)}};
                        projectionViews[i].subImage.imageArrayIndex = 0;
                        if (splitProjection) {
                            projectionViews[i].subImage.swapchain = m_projectionSwapchains[i];
                            projectionViews[i].subImage.imageRect.offset.x = 0;
                        }
                        if (nativeView) {
                            const float shownX = std::max(1.0F, zoom) / nativeMatrices[0];
                            const float shownY = std::max(1.0F, zoom) / nativeMatrices[5];
                            projectionViews[i].fov = {-std::atan(shownX), std::atan(shownX),
                                                      std::atan(shownY), -std::atan(shownY)};
                            projectionViews[i].subImage.imageRect = {
                                {0, 0}, {static_cast<std::int32_t>(m_quadWidth),
                                         static_cast<std::int32_t>(m_quadHeight)}};
                        }
                        if (lensPass == 2) {
                            // The lens frame: only the window around the aim axis, in the aux
                            // swapchain (its alpha is the round mask written after the copy).
                            float rect[4];
                            lensWindow(i, rect, &projectionViews[i].fov);
                            projectionViews[i].subImage.swapchain = m_rightSwapchain;
                            projectionViews[i].subImage.imageRect = {
                                {static_cast<std::int32_t>(rect[0]), static_cast<std::int32_t>(rect[1])},
                                {static_cast<std::int32_t>(rect[2]), static_cast<std::int32_t>(rect[3])}};
                        }
                    }
                    XrQuaternionf camera = imageOrientation;
                    float matrices[32]{};
                    const auto warp = GetConfig().aerWarpMode;
                    if (useAer && m_gameProjectionView) {
                        m_gameProjectionView(matrices);
                        const float* view = sceneProjection.valid ? sceneProjection.view.data() : matrices + 16;
                        const float x = view[2];
                        const float y = view[6];
                        const float z = view[10];
                        const float length = std::sqrt(x * x + y * y + z * z);
                        if (std::isfinite(length) && length > 0.5F) {
                            // Retained images need the roll of their own render. Including it
                            // in the camera delta prevents the current roll leaking to that eye.
                            camera = Multiply(YawPitch(-std::atan2(x, z),
                                std::asin(std::clamp(y / length, -1.0F, 1.0F))), cameraRoll);
                        }
                    }
                    // With headset-only warping, use the tracked view as the
                    // relative rotation source for DIBR history.
                    const XrQuaternionf historyCamera =
                        warp == AerWarpMode::HeadsetRotation ? orientation : camera;
                    bool synthesized = false;
                    if (useAer && renderedEye < 0 && EnsureRightSwapchain() &&
                        CopyIntoQuad(backBuffer, description, true)) {
                        m_dibr.ResetHistory();
                        m_retainedViews[0] = projectionViews[0];
                        m_retainedViews[1] = projectionViews[1];
                        m_retainedViews[1].subImage.swapchain = m_rightSwapchain;
                        m_retainedCamera[0] = m_retainedCamera[1] = historyCamera;
                        m_nativeValid[0] = m_nativeValid[1] = false;
                        m_renderEye = 0;
                        Log("AER projection started: one scene eye per frame, camera rotation reprojection");
                    } else if (useAer && renderedEye >= 0 &&
                               m_retainedViews[0].subImage.swapchain == m_quadSwapchain &&
                               m_retainedViews[1].subImage.swapchain == m_rightSwapchain) {
                        projectionViews[renderedEye].subImage.swapchain =
                            renderedEye == 0 ? m_quadSwapchain : m_rightSwapchain;
                        m_retainedViews[renderedEye] = projectionViews[renderedEye];
                        m_retainedCamera[renderedEye] = historyCamera;
                        if (nativeView) {
                            m_nativeValid[renderedEye] = true;
                        }
                        const int staleEye = 1 - renderedEye;
                        synthesized =
                            m_dibrShift && std::fabs(zoom - 1.0F) <= 0.001F &&
                            RenderDibrEye(backBuffer, stereoPose, matrices, renderedEye, historyCamera,
                                          m_retainedCamera[staleEye], projectionViews[staleEye]);
                        if (!synthesized) {
                            if (m_dibrShift && std::fabs(zoom - 1.0F) > 0.001F) {
                                m_dibr.ResetHistory();
                            }
                            const XrPosef freshPose = projectionViews[staleEye].pose;
                            projectionViews[staleEye] = m_retainedViews[staleEye];
                            if (warp == AerWarpMode::Off) {
                                projectionViews[staleEye].pose = freshPose;
                            } else if (warp != AerWarpMode::HeadsetRotation) {
                                // Add the game camera's movement to OpenXR's head reprojection.
                                projectionViews[staleEye].pose.orientation = Multiply(
                                    Multiply(imageOrientation, Conjugate(historyCamera)),
                                    m_retainedCamera[staleEye]);
                            }
                            if (nativeView && !m_nativeValid[staleEye]) {
                                // The other eye has no native frame yet: show this eye's picture in
                                // both for the first frame instead of an image of the old layout.
                                projectionViews[staleEye].subImage = projectionViews[renderedEye].subImage;
                                projectionViews[staleEye].fov = projectionViews[renderedEye].fov;
                                projectionViews[staleEye].pose = projectionViews[renderedEye].pose;
                            }
                        }
                        m_renderEye = staleEye;
                    }
                    if (reuseStalePair) {
                        projectionViews[0] = m_stalePairViews[0];
                        projectionViews[1] = m_stalePairViews[1];
                    } else {
                        rememberStalePair = nextStaleEye >= 0;
                        SaveProjectionEyeImages(backBuffer, legacyEye, synthesized, nativeView,
                                                staleWarpApplied ? 1 - renderedEye : -1);
                    }
                    if (lensPass == 1) {
                        m_lensWorldViews[0] = projectionViews[0];
                        m_lensWorldViews[1] = projectionViews[1];
                        m_lensWorldValid = true;
                    } else if (lensPass == 2) {
                        m_lensWindowViews[0] = projectionViews[0];
                        m_lensWindowViews[1] = projectionViews[1];
                        m_lensWindowValid = true;
                        if (m_lensWorldValid) {
                            // The world layer is the latest world frame; this frame is the lens.
                            projectionViews[0] = m_lensWorldViews[0];
                            projectionViews[1] = m_lensWorldViews[1];
                        }
                    }
                    projection.layerFlags = 0;
                    projection.space = staticView ? m_viewSpace : m_localSpace;
                    projection.viewCount = 2;
                    projection.views = projectionViews;
                    const bool steamVrWorldQuad = m_steamVr && GetConfig().steamVrWorldQuad && lensPass == 0;
                    if (!reuseStalePair) {
                        const int layout = steamVrWorldQuad ? 3 : splitProjection ? 2 : 1;
                        static int lastLayout = 0;
                        static unsigned logs = 0;
                        if (layout != lastLayout && logs++ < 8) {
                            Log("XR world: %s, %s swapchain, frame %ux%u, source=%s",
                                steamVrWorldQuad ? "stereo quads" : "projection",
                                splitProjection ? "per-eye" : "shared",
                                description.Width, description.Height,
                                worldTexture ? "independent scene" : "desktop back buffer");
                        }
                        lastLayout = layout;
                    }
                    if (steamVrWorldQuad) {
                        quad = QuadFromProjectionView(projectionViews[0], projection.space, XR_EYE_VISIBILITY_LEFT);
                        rightQuad = QuadFromProjectionView(projectionViews[1], projection.space, XR_EYE_VISIBILITY_RIGHT);
                        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                        layers[1] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&rightQuad);
                        layerCount = 2;
                        submittedSteamVrQuads = true;
                    } else {
                        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
                        layerCount = 1;
                    }
                    if (lensPass != 0 && m_lensWorldValid && m_lensWindowValid) {
                        lensProjection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                        lensProjection.space = m_localSpace;
                        lensProjection.viewCount = 2;
                        lensProjection.views = m_lensWindowViews;
                        layers[layerCount++] =
                            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&lensProjection);
                        opticsTrace.lensSubmitted = true;
                    }
                    ++m_stereoFrames;
                } else if (m_sbsInput && layerCount == 2) {
                    // Side-by-side halves: both eyes come from the same camera pose, the
                    // parallax is already in the images (EngineStereo).
                    BuildWorldView(quad, renderPose, 0);
                    BuildWorldView(rightQuad, renderPose, 0);
                    // The game picture is wider than the headset sees and stretches at the
                    // edges: keep only the central part of each half on the quad.
                    const float crop = m_fovCrop;
                    const std::int32_t half = static_cast<std::int32_t>(m_quadWidth / 2);
                    const std::int32_t height = static_cast<std::int32_t>(m_quadHeight);
                    const std::int32_t cropWidth = static_cast<std::int32_t>(half * crop);
                    const std::int32_t cropHeight = static_cast<std::int32_t>(height * crop);
                    const std::int32_t x0 = (half - cropWidth) / 2;
                    const std::int32_t y0 = (height - cropHeight) / 2;
                    quad.subImage.imageRect = {{x0, y0}, {cropWidth, cropHeight}};
                    rightQuad.subImage.imageRect = {{half + x0, y0}, {cropWidth, cropHeight}};
                    quad.size = {quad.size.width * crop, quad.size.height * crop};
                    rightQuad.size = {rightQuad.size.width * crop, rightQuad.size.height * crop};
                    ++m_stereoFrames;
                } else if (eye == 0) {
                    m_eyePose[0].valid = false;
                    m_eyePose[1].valid = false;
                    BuildWorldView(quad, renderPose, 0);
                } else {
                    m_eyePose[eye > 0 ? 1 : 0] = renderPose;
                    if (m_eyePose[0].valid && m_eyePose[1].valid) {
                        // Each eye keeps its own last frame at the pose it was rendered with.
                        rightQuad = quad;
                        quad.eyeVisibility = XR_EYE_VISIBILITY_LEFT;
                        quad.subImage.swapchain = m_quadSwapchain;
                        BuildWorldView(quad, m_eyePose[0], -1);
                        rightQuad.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
                        rightQuad.subImage.swapchain = m_rightSwapchain;
                        BuildWorldView(rightQuad, m_eyePose[1], 1);
                        layers[1] =
                            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&rightQuad);
                        layerCount = 2;
                        ++m_stereoFrames;
                    } else {
                        BuildWorldView(quad, renderPose, 0);  // first frame of a pair
                    }
                }
            }
            if (static_cast<int>(worldView) != m_worldViewState) {
                m_worldViewState = static_cast<int>(worldView);
                static int viewLogs = 0;
                if (viewLogs++ < 60) {
                    Log("VIEW %s", worldView ? "world (head look)" : "screen");
                }
            }
            const bool inventoryPanel = worldView && inventoryWorld && BuildHudLayer(hudQuad, true);
            if (inventoryPanel || (worldView && GetConfig().hudQuad && !GetConfig().hideGameplayHud &&
                m_hudLockOn && layerCount < 4 && !EngineStereoScopeUi() && BuildHudLayer(hudQuad, false))) {
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudQuad);
            }
            if (m_inventoryPresented != inventoryPanel) {
                m_inventoryPresented = inventoryPanel;
                Log("Inventory VR: %s", inventoryPanel ? "stereo world + F11-anchored panel" : "panel closed");
            }
            if (cursorDrawnIntoScreen) {
                ++m_cursorFrames;
            } else if ((inventoryPanel || !worldView) && !Menu::Instance().IsOpen() && GetConfig().drawCursor &&
                       BuildCursorLayer(inventoryPanel ? hudQuad : quad, cursorQuad)) {
                layers[layerCount++] =
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cursorQuad);
                ++m_cursorFrames;
            }
            ++m_quadFramesSubmitted;
            if (!m_loggedFirstQuad) {
                Log("RUNG3 first quad frame: %ux%u, %.3f x %.3f m at %.2f m (%.1f deg), %s",
                    m_quadWidth, m_quadHeight, quad.size.width, quad.size.height,
                    m_quadDistanceMeters,
                    HorizontalDegrees(m_quadWidthMeters, m_quadDistanceMeters),
                    world ? "world-fixed" : "head-locked");
                m_loggedFirstQuad = true;
            }
        }
    }
    // Settings menu: a panel in the room on top of whatever the frame shows.
    if (m_shouldRender && layerCount > 0 && layerCount < 6 &&
        (Menu::Instance().IsOpen() || ShaderBrowseEnabled()) &&
        RenderMenuLayer(menuQuad)) {
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menuQuad);
    }


    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = m_displayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layerCount ? layers : nullptr;
    m_framePrepared = false;
    if (submittedSteamVrQuads) {
        // Hand the eye copies to the GPU before SteamVR picks up this frame.
        m_context->Flush();
    }
    const XrResult result = xrEndFrame(m_session, &endInfo);
    if (captureDetails) {
        const bool projected = layerCount > 0 &&
            layers[0] == reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        const bool dlssSubmitted = dlssApplied && projected && XR_SUCCEEDED(result);
        const bool staleWarpSubmitted = staleWarpApplied && rememberStalePair && XR_SUCCEEDED(result);
        const char* viewKind = projected ? "projection" : submittedSteamVrQuads ? "steamvr_eye_quads" :
                               layerCount > 0 ? "other" : "none";
        const XrSpace space = projected ? projection.space :
                              submittedSteamVrQuads ? quad.space : XR_NULL_HANDLE;
        const char* viewSpace = space == XR_NULL_HANDLE ? "" : space == m_localSpace ? "local" :
                                space == m_viewSpace ? "view" : "other";
        TraceSubmittedFrame(renderedEye, matchedPose, layerCount, projectionViews,
                            result, NowMs() - traceStart, dlssSubmitted, staleWarpSubmitted,
                            viewKind, viewSpace, projected || submittedSteamVrQuads ? drawPoseId : 0,
                            submittedRoll, opticsTrace);
    }
    if (Check(result, "ending frame")) {
        if (rememberStalePair && GetConfig().staleEyeWarp) {
            m_stalePairViews[0] = projectionViews[0];
            m_stalePairViews[1] = projectionViews[1];
            m_stalePairValid = true;
            m_renderEye = nextStaleEye;
        }
        if (m_framesSubmitted++ == 0) {
            Log("RUNG2 first xrEndFrame ok (layers=%u)", layerCount);
        }
    }
    return submittedSteamVrQuads;
}

void OpenXRRuntime::TraceSubmittedFrame(
    int renderedEye, std::uint32_t matchedPose, std::uint32_t layerCount,
    const XrCompositionLayerProjectionView views[2], XrResult result, double submitMs,
    bool dlssSubmitted, bool staleWarpSubmitted, const char* viewKind, const char* viewSpace,
    std::uint32_t drawPoseId, float submittedRoll, const OpticsSubmitTrace& optics)
{
    if (!m_frameTraceAttempted) {
        m_frameTraceAttempted = true;
        m_frameTrace.open(ModuleDirectory() / L"arma3_native_vr.frames.csv", std::ios::trunc);
        Log("FRAME TRACE %s", m_frameTrace ? "writing arma3_native_vr.frames.csv" : "open failed");
        if (m_frameTrace) {
            m_frameTrace << "present,qpc_ms,present_gap_ms,xr_wait_ms,submit_ms,display_ns,"
                            "xr_state,should_render,xr_result,layers,world,eye,dibr,weapon_draws,"
                            "published,consumed,matched,zoom,aiming,head_yaw,head_pitch,"
                            "head_x,head_y,head_z,head_qx,head_qy,head_qz,head_qw";
            for (int i = 0; i < 32; ++i) {
                m_frameTrace << ",arma_m" << i;
            }
            for (int i = 0; i < 2; ++i) {
                for (const char* field : {"x", "y", "z", "qx", "qy", "qz", "qw",
                                          "fov_l", "fov_r", "fov_u", "fov_d"}) {
                    m_frameTrace << ",eye" << i << '_' << field;
                }
            }
            m_frameTrace << ",dlss_requested,dlss_submitted,stale_eye_warp,sight_active,world_vrs,"
                            "stereo_shadows,render_mode,dibr_history_6dof,pose_lag,dibr_forward_history,"
                            "render_camera_roll_deg,stabilize_camera_roll,aer_warp_mode,"
                            "submit_view_kind,submit_view_space,world_draw_sampled,world_draw_frame,"
                            "world_draw_view_valid,world_draw_ft_before,world_draw_ft_after,"
                            "world_draw_ft_matches_submit,world_draw_ft_in_ring,"
                            "world_draw_head_yaw_deg,world_draw_head_pitch_deg,"
                            "world_draw_ft_yaw_rad,world_draw_ft_pitch_rad";
            for (int i = 0; i < 16; ++i) {
                m_frameTrace << ",world_draw_v" << i;
            }
            m_frameTrace << ",submit_draw_pose_id,submit_camera_roll_deg,"
                            "world_yaw_correction_right_m,world_yaw_correction_up_m,"
                            "world_yaw_correction_forward_m,world_vrs_4x4,"
                            "probe_capture,zoom_draw_p00,zoom_draw_p11,zoom_present_p00,zoom_present_p11,"
                            "zoom_reference_before,zoom_reference,zoom_raw_ratio,zoom_reference_updated,zoom_optics_intent,"
                            "zoom_gate,zoom_selected,zoom_config,zoom_window_divisor,zoom_xr_fov_ratio,"
                            "zoom_source,zoom_scope_ui,zoom_scope_static,zoom_lens_pass,zoom_lens_submitted\n";
            m_frameTrace.precision(12);
        }
    }
    if (!m_frameTrace) {
        return;
    }

    const RenderPose& pose = m_poseRing[0];
    const bool dibr = m_dibrSwapchain != XR_NULL_HANDLE &&
        (views[0].subImage.swapchain == m_dibrSwapchain ||
         views[1].subImage.swapchain == m_dibrSwapchain);
    m_frameTrace << m_presents << ',' << m_lastPresentMs << ',' << m_presentGapMs << ','
                 << m_frameWaitMs << ',' << submitMs << ',' << m_displayTime << ','
                 << static_cast<int>(m_sessionState) << ',' << m_shouldRender << ','
                 << static_cast<int>(result) << ',' << layerCount << ',' << m_worldViewState << ','
                 << renderedEye << ',' << dibr << ',' << EngineStereoLastWeaponDraws() << ','
                 << pose.sequence << ',' << m_feed.LastReturnedId() << ',' << matchedPose << ','
                 << EngineStereoDrawZoom() << ',' << m_aiming << ','
                 << pose.yaw * kDegrees << ',' << pose.pitch * kDegrees;
    const auto writePose = [this](const XrPosef& p) {
        m_frameTrace << ',' << p.position.x << ',' << p.position.y << ',' << p.position.z
                     << ',' << p.orientation.x << ',' << p.orientation.y << ','
                     << p.orientation.z << ',' << p.orientation.w;
    };
    writePose(pose.head);
    float matrices[32]{};
    if (m_gameProjectionView) {
        m_gameProjectionView(matrices);
    }
    for (float element : matrices) {
        m_frameTrace << ',' << element;
    }
    for (int i = 0; i < 2; ++i) {
        writePose(views[i].pose);
        const XrFovf& fov = views[i].fov;
        m_frameTrace << ',' << fov.angleLeft << ',' << fov.angleRight << ','
                     << fov.angleUp << ',' << fov.angleDown;
    }
    const auto& config = GetConfig();
    const char* renderMode = "stereo";
    if (config.staleEyeWarp) {
        renderMode = "aer_6dof";
    } else if (m_alternateEyeProjection) {
        renderMode = m_dibrShift ? (config.dibrForwardHistory ? "dibr_forward" : "dibr") : "aer";
    }
    m_frameTrace << ',' << config.dlssBalancedExperimental << ',' << dlssSubmitted << ','
                 << staleWarpSubmitted << ',' << pose.sightActive << ',' << EngineStereoLastWorldVrs() << ','
                 << EngineStereoLastStereoShadow() << ',' << renderMode << ','
                 << (dibr && m_dibr.HistoryWarpActive()) << ',' << m_poseLag << ','
                 << (dibr && m_dibr.ForwardHistoryActive()) << ',';
    const auto worldCamera = EngineStereoLastWorldDepthProjection();
    if (worldCamera.valid) {
        m_frameTrace << CameraRollFromView(worldCamera.view[4], worldCamera.view[5]) * kDegrees;
    }
    const auto draw = EngineStereoLastWorldDrawPoseTrace();
    const RenderPose* drawPose = nullptr;
    const bool observedStableId = draw.returnedBefore != 0 && draw.returnedBefore == draw.returnedAfter;
    if (observedStableId) {
        for (const auto& entry : m_poseRing) {
            if (entry.valid && entry.sequence == draw.returnedAfter) {
                drawPose = &entry;
                break;
            }
        }
    }
    m_frameTrace << ',' << config.stabilizeArmaRoll << ',' << static_cast<int>(config.aerWarpMode)
                 << ',' << viewKind << ',' << viewSpace << ',' << draw.sampled << ',' << draw.frame
                 << ',' << draw.viewValid << ',' << draw.returnedBefore << ',' << draw.returnedAfter
                 << ',' << (observedStableId && draw.returnedAfter == matchedPose)
                 << ',' << (drawPose != nullptr);
    if (drawPose) {
        m_frameTrace << ',' << drawPose->yaw * kDegrees << ',' << drawPose->pitch * kDegrees
                     << ',' << drawPose->freeTrackYaw << ',' << drawPose->freeTrackPitch;
    } else {
        m_frameTrace << ",,,,";
    }
    for (float element : draw.view) {
        m_frameTrace << ',';
        if (draw.viewValid) {
            m_frameTrace << element;
        }
    }
    m_frameTrace << ',' << drawPoseId << ',' << submittedRoll;
    for (float offset : worldCamera.yawOrbitCorrection) {
        m_frameTrace << ',' << offset;
    }
    const auto zoom = EngineStereoLastZoomTrace();
    const float submittedSpan = optics.windowDivisor > 0.0F
        ? std::tan(views[0].fov.angleRight) - std::tan(views[0].fov.angleLeft) : 0.0F;
    const float fovRatio = optics.unzoomedEyeSpan > 0.0F && submittedSpan > 0.0F
        ? optics.unzoomedEyeSpan / submittedSpan : 0.0F;
    const int capture = FrameProbeCaptureNumber();
    const bool captureStarted = capture > m_opticsProbeCapture;
    m_frameTrace << ',' << EngineStereoLastWorldVrsCoarse() << ',' << (captureStarted ? capture : 0)
                 << ',' << draw.nativeP00 << ',' << draw.nativeP11 << ',' << zoom.nativeP00
                 << ',' << zoom.nativeP11 << ',' << zoom.previousReference << ',' << zoom.reference
                 << ',' << zoom.nativeRatio
                 << ',' << zoom.referenceUpdated << ',' << zoom.opticsIntent << ',' << zoom.decision
                 << ',' << EngineStereoZoom() << ',' << config.zoomMagnification
                 << ',' << optics.windowDivisor << ',' << fovRatio << ',' << optics.source
                 << ',' << EngineStereoScopeUi() << ',' << pose.scopeStatic << ',' << optics.lensPass
                 << ',' << optics.lensSubmitted << '\n';
    if (captureStarted) {
        m_opticsProbeCapture = capture;
        Log("OPTICS probe %d present=%llu source=%s view=%s: draw P00/P11=%.4f/%.4f "
            "Present P00/P11=%.4f/%.4f base=%.4f->%.4f raw=%.2fx input=%d baseUpdated=%d decision=%s "
            "selected=%.2fx drawn=%.2fx window=%.3f xrFovRatio=%.2f lensPass=%d lensLayer=%d",
            capture, static_cast<unsigned long long>(m_presents), optics.source, viewKind,
            draw.nativeP00, draw.nativeP11, zoom.nativeP00, zoom.nativeP11, zoom.previousReference, zoom.reference,
            zoom.nativeRatio, zoom.opticsIntent, zoom.referenceUpdated, zoom.decision,
            EngineStereoZoom(), EngineStereoDrawZoom(), optics.windowDivisor, fovRatio,
            optics.lensPass, optics.lensSubmitted);
    }
    if (captureStarted || m_presents % 60 == 0) {
        m_frameTrace.flush();
    }
}

// On foot or in a vehicle: a vehicle has a roof, so the head sits lower there. Automatic switch:
// no first-person weapon drawn for 1.5 s in the world view = vehicle, a weapon again = on foot.
// The Insert menu can override automatic placement.
void OpenXRRuntime::UpdateHeadPlacement()
{
    bool wantVehicle = m_vehicleMode;
    if (m_vehicleForce == 1) {
        wantVehicle = false;
    } else if (m_vehicleForce == 2) {
        wantVehicle = true;
    } else if (!GetConfig().vehicleAuto) {
        wantVehicle = false;  // no reliable automatic signal: a cockpit is drawn like a weapon
    } else if (GetConfig().vehicleAuto && m_worldViewState == 1) {
        if (EngineStereoLastWeaponDraws() == 0) {
            m_weaponFrames = 0;
            if (++m_noWeaponFrames >= 120) {
                wantVehicle = true;
            }
        } else {
            m_noWeaponFrames = 0;
            if (++m_weaponFrames >= 10) {
                wantVehicle = false;
            }
        }
    }
    if (wantVehicle != m_vehicleMode) {
        // Keep unsaved edits of the placement we leave.
        float* leaving = m_vehicleMode ? m_offsetVehicle : m_offsetFoot;
        std::copy(std::begin(m_headOffset), std::end(m_headOffset), leaving);
        m_vehicleMode = wantVehicle;
        const float* entering = m_vehicleMode ? m_offsetVehicle : m_offsetFoot;
        std::copy(entering, entering + 3, std::begin(m_headOffset));
        Log("HEAD placement: %s (right %+.2f up %+.2f forward %+.2f m)",
            m_vehicleMode ? "vehicle" : "on foot", m_headOffset[0], m_headOffset[1],
            m_headOffset[2]);
    }
    for (int i = 0; i < 3; ++i) {
        m_appliedOffset[i] += (m_headOffset[i] - m_appliedOffset[i]) * 0.12F;
    }
    EngineStereoSetVehicle(m_vehicleMode);
}

void OpenXRRuntime::HandleHotkeys()
{
    static bool recenterHeld = false;
    static bool insertHeld = false;
    static bool escapeHeld = false;
    const bool recenter = KeyEdge(VK_F11, recenterHeld);
    const bool insert = KeyEdge(VK_INSERT, insertHeld);
    const bool escape = KeyEdge(VK_ESCAPE, escapeHeld);
    static bool browserToggleHeld = false;
    static bool previousHeld = false;
    static bool nextHeld = false;
    static bool markHeld = false;
    const bool cycleBrowser = KeyEdge(VK_F1, browserToggleHeld) && KeyHeld(VK_CONTROL);
    const bool previousShader = KeyEdge(VK_PRIOR, previousHeld);
    const bool nextShader = KeyEdge(VK_NEXT, nextHeld);
    const bool markShader = KeyEdge(VK_DELETE, markHeld);
    const bool menuOpen = Menu::Instance().IsOpen();
    const bool browseKeys = ShaderBrowseEnabled() && !menuOpen &&
                            (previousShader || nextShader || markShader);
    if ((!recenter && !insert && !escape && !browseKeys && !cycleBrowser) ||
        KeyHeld(VK_MENU) || KeyHeld(VK_SHIFT) || !m_window ||
        GetAncestor(m_window, GA_ROOT) != GetAncestor(GetForegroundWindow(), GA_ROOT)) {
        return;
    }
    if (KeyHeld(VK_CONTROL)) {
        if (cycleBrowser && GetConfig().frameProbe && !menuOpen) {
            ShaderBrowseSetMode((ShaderBrowseMode() + 1) % 3);
        }
        return;
    }
    if (insert) {
        if (!menuOpen) PlaceMenu();
        Menu::Instance().Toggle();
    } else if (escape && menuOpen) {
        Menu::Instance().SetOpen(false);
    } else if (browseKeys) {
        if (previousShader) ShaderBrowseStep(-1);
        if (nextShader) ShaderBrowseStep(1);
        if (markShader) ShaderBrowseMark();
    }
    if (recenter) {
        m_headRecenterRequested = true;
        m_recenterRequested = true;
        EngineStereoResetZoomReference();
        Log("Recentre requested (F11)");
    }
}

void OpenXRRuntime::LogStats()
{
    const int every = GetConfig().statsEveryPresents;
    if (every <= 0 || m_presents % static_cast<std::uint64_t>(every) != 0) {
        return;
    }
    {
        // Mouse coordinates are window pixels: say so when the window and the picture disagree.
        static int lastClientW = 0;
        static int lastClientH = 0;
        static unsigned lastBackW = 0;
        static unsigned lastBackH = 0;
        RECT client{};
        if (m_window && GetClientRect(m_window, &client) &&
            (client.right != lastClientW || client.bottom != lastClientH ||
             lastBackW != m_quadWidth || lastBackH != m_quadHeight)) {
            lastClientW = client.right;
            lastClientH = client.bottom;
            lastBackW = m_quadWidth;
            lastBackH = m_quadHeight;
            Log("RESOLUTION window client now %ldx%ld, picture %ux%u%s", client.right,
                client.bottom, m_quadWidth, m_quadHeight,
                (client.right == static_cast<LONG>(m_quadWidth) &&
                 client.bottom == static_cast<LONG>(m_quadHeight))
                    ? " (match)" : " (differs: the virtual cursor maps the whole picture)");
        }
    }
    if (m_worldViewState == 1 && EngineStereoPerfWorld() && m_startupSceneRequest.first) {
        std::pair<unsigned, unsigned> actual{};
        EngineStereoSceneSize(actual.first, actual.second);
        if (actual.first && actual.second && actual != m_reportedSceneExtent) {
            m_reportedSceneExtent = actual;
            Log("RESOLUTION mission scene actual %ux%u, startup request %ux%u (%s)",
                actual.first, actual.second, m_startupSceneRequest.first, m_startupSceneRequest.second,
                actual == m_startupSceneRequest ? "applied" : "different");
        }
    }
    const ULONGLONG now = GetTickCount64();
    const double seconds = static_cast<double>(now - m_statsStart) / 1000.0;
    // avgWaitFrame = time the game waits for the headset; avgSubmit = our copy + xrEndFrame;
    // maxGap = longest pause between two presents (a hitch); notRendered = frames the
    // runtime asked us to skip in this window.
    Log("STATS presents=%llu fps=%.1f xrFrames=%llu quadFrames=%llu cursorFrames=%llu "
        "avgWaitFrame=%.2f ms avgSubmit=%.2f ms maxGap=%.1f ms notRendered=%llu state=%d",
        m_presents, seconds > 0.0 ? every / seconds : 0.0, m_framesSubmitted,
        m_quadFramesSubmitted, m_cursorFrames, m_waitMsSum / every, m_submitMsSum / every,
        m_maxPresentGapMs, m_notRendered, static_cast<int>(m_sessionState));
    if (m_steamVrWorldPresents > 0) {
        Log("SteamVR pacing: period=%.2f ms, world frames=%u, gaps over 1.5 periods=%u, "
            "desktop Present avg=%.2f ms max=%.2f ms sync=%u",
            static_cast<double>(m_displayPeriod) / 1e6, m_steamVrWorldPresents, m_steamVrLongGaps,
            m_steamVrDesktopPresentSum / m_steamVrWorldPresents, m_steamVrDesktopPresentMax,
            m_steamVrDesktopSyncInterval);
        m_steamVrWorldPresents = m_steamVrLongGaps = 0;
        m_steamVrDesktopPresentSum = m_steamVrDesktopPresentMax = 0.0;
    }
    if (GetConfig().depthStereo) {
        Log("DEPTH3D frames=%llu on=%d scale=%.2f screenPlane=%.2f m near=%.4f m swap=%d",
            m_depthFrames, m_stereoEnabled ? 1 : 0, m_stereoScale, m_convergenceMeters,
            m_loggedNear, m_stereoSwap ? 1 : 0);
        DepthStereo::Instance().LogStats();
        Log("SBS input=%d frames=%llu", m_sbsInput ? 1 : 0, m_sbsFrames);
        m_sbsFrames = 0;
        m_depthFrames = 0;
    }
    if (GetConfig().headTracking) {
        Log("POSE shown with ring index 0..7 (0 = newest): %u %u %u %u %u %u %u %u, no match %u "
            "(sequence matching %s, lag offset %d)",
            m_poseHits[0], m_poseHits[1], m_poseHits[2], m_poseHits[3], m_poseHits[4],
            m_poseHits[5], m_poseHits[6], m_poseHits[7], m_poseHits[8],
            GetConfig().poseBySequence ? "on" : "off", m_poseLag - 1);
        std::memset(m_poseHits, 0, sizeof(m_poseHits));
        // ftCalls = how often the game polled our FreeTrack client in this window
        // (0 = FreeTrack is not active in the game). view = the game's camera
        // matrix, to compare the head angle we sent with what the engine applied.
        const std::uint32_t calls = m_feed.ClientCalls();
        float matrices[32]{};
        if (m_gameProjectionView) {
            m_gameProjectionView(matrices);
        }
        Log("HEAD worldFrames=%llu ftCalls=+%u sentYaw=%.1f sentPitch=%.1f deg tanH=%.3f "
            "tanV=%.3f view=[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
            m_worldFrames, calls - m_lastClientCalls, m_poseRing[0].yaw * kDegrees,
            m_poseRing[0].pitch * kDegrees, m_lastTanHorizontal, m_lastTanVertical, matrices[16],
            matrices[17], matrices[18], matrices[19], matrices[20], matrices[21], matrices[22],
            matrices[23], matrices[24], matrices[25], matrices[26], matrices[27]);
        // measured = sideways camera move between the two eyes as the engine applied
        // it (metres, sign: + = right eye is on the right). Target is about 0.064.
        Log("STEREO frames=%llu calibrated=%d scale=%.2f swap=%d measuredSeparation=%.4f m "
            "(samples=%u)",
            m_stereoFrames, m_calibration.Valid() ? 1 : 0, m_stereoScale, m_stereoSwap ? 1 : 0,
            m_separationSamples ? m_separationSum / m_separationSamples : 0.0,
            m_separationSamples);
        m_stereoFrames = 0;
        m_separationSum = 0.0;
        m_separationSamples = 0;
        m_lastClientCalls = calls;
        m_worldFrames = 0;
    }
    m_statsStart = now;
    m_waitMsSum = 0.0;
    m_submitMsSum = 0.0;
    m_maxPresentGapMs = 0.0;
    m_notRendered = 0;
}

bool OpenXRRuntime::IsMainSwapChain(IDXGISwapChain* swapChain)
{
    std::scoped_lock lock(m_mutex);
    return m_mainSwapChain == swapChain;
}

bool OpenXRRuntime::OnPresent(IDXGISwapChain* swapChain)
{
    std::scoped_lock lock(m_mutex);
    if (!swapChain) {
        return false;
    }
    if (!m_mainSwapChain) {
        if (!AttachDevice(swapChain)) {
            return false;
        }
    } else if (swapChain != m_mainSwapChain) {
        // Arma may recreate its swapchain (video options, window mode). When
        // another swapchain presents 30 times in a row, it becomes the main one.
        if (++m_foreignPresents < 30) {
            return false;
        }
        m_foreignPresents = 0;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        swapChain->GetDevice(IID_PPV_ARGS(&device));
        Log("Main swapchain replaced: %p -> %p (device %s)", m_mainSwapChain, swapChain,
            device.Get() == m_device.Get() ? "unchanged" : "CHANGED, session restarts");
        if (device.Get() != m_device.Get()) {
            DestroySession();
            m_nextSessionAttempt = 0;
        }
        DestroyQuadSwapchain();
        m_mainSwapChain = nullptr;
        if (!AttachDevice(swapChain)) {
            return false;
        }
    }
    m_foreignPresents = 0;
    ++m_presents;
    const double presentMs = NowMs();
    m_presentGapMs = m_lastPresentMs > 0.0 ? presentMs - m_lastPresentMs : 0.0;
    m_maxPresentGapMs = std::max(m_maxPresentGapMs, m_presentGapMs);
    m_lastPresentMs = presentMs;
    if (GetCurrentThreadId() != m_presentThread) {
        Log("Present arrived on thread %lu (first was %lu)", GetCurrentThreadId(),
            m_presentThread);
        m_presentThread = GetCurrentThreadId();
    }
    LogStats();
    if (!GetConfig().enableVR) {
        return false;
    }
    if (!m_tuningInitialized) {
        m_quadWidthMeters = GetConfig().quadWidthMeters;
        m_quadDistanceMeters = GetConfig().quadDistanceMeters;
        m_quadHeightMeters = GetConfig().quadHeightOffsetMeters;
        m_hudScale = GetConfig().hudScale;
        m_hudShiftX = GetConfig().hudShiftX;
        m_hudShiftY = GetConfig().hudShiftY;
        m_hudDistance = GetConfig().hudDistanceMeters;
        m_fovCrop = GetConfig().worldFovCrop;
        m_projectionEnabled = GetConfig().projectionLayer;
        m_alternateEyeProjection = GetConfig().alternateEyeProjection;
        m_dibrShift = GetConfig().dibrShift;
        m_dibrFill = GetConfig().dibrFillMode;
        Log("RENDER startup: full stereo locked");
        m_weaponScale = GetConfig().weaponScale;
        m_weaponConvergence = GetConfig().weaponConvergenceMeters;
        m_weaponSize = GetConfig().weaponSize;
        m_poseLag = GetConfig().poseLagFrames;
        m_vehicleForce = GetConfig().headPlacementMode;
        m_stereoShiftMillimeters = GetConfig().stereoShiftMillimeters;
        m_stereoScale = GetConfig().stereoScale;
        std::copy(std::begin(GetConfig().headOffset), std::end(GetConfig().headOffset),
                  std::begin(m_headOffset));
        std::copy(std::begin(m_headOffset), std::end(m_headOffset), std::begin(m_offsetFoot));
        std::copy(std::begin(GetConfig().headOffsetVehicle), std::end(GetConfig().headOffsetVehicle),
                  std::begin(m_offsetVehicle));
        std::copy(std::begin(m_headOffset), std::end(m_headOffset), std::begin(m_appliedOffset));
        m_convergenceMeters = GetConfig().convergenceMeters;
        m_depthReversed = GetConfig().depthReversed;
        m_sbsInput = GetConfig().sbsInput;
        m_stereoSwap = GetConfig().stereoSwapEyes;
        const Config& resolution = GetConfig();
        if (resolution.virtualBackBuffer && !resolution.autoHeadsetResolution &&
            resolution.forceGameWidth > 0 && resolution.forceGameHeight > 0) {
            m_startupSceneRequest = {static_cast<unsigned>(resolution.forceGameWidth),
                                     static_cast<unsigned>(resolution.forceGameHeight)};
        }
        m_tuningInitialized = true;
    }
    // The stereo buffer keeps both eye regions. Menus use matching views without parallax.
    EngineStereoSetTuning(true, (WindowsCursorShowing() && !m_inventoryOpen) || !m_stereoEnabled, m_stereoScale,
                          m_convergenceMeters);
    EngineStereoSetHud(m_worldViewState == 1 && m_sbsInput, m_hudScale, m_hudShiftX, m_hudShiftY,
                       m_hudDistance);
    EngineStereoSetWeapon(m_weaponScale, m_weaponConvergence, m_weaponSize);
    if (m_session == XR_NULL_HANDLE) {
        const ULONGLONG now = GetTickCount64();
        if (now < m_nextSessionAttempt) {
            return false;
        }
        if (!CreateSession()) {
            ++m_sessionFailures;
            m_nextSessionAttempt = now + 5000;
            Log("OpenXR session not available (attempt %u); retrying every 5 s",
                m_sessionFailures);
            return false;
        }
    }
    PollEvents();
    if (!m_sessionRunning) {
        return false;
    }
    if (m_framePrepared) {
        ApplyLocalSpaceChanges();
    }
    const int mode = GetConfig().openXrDiagnosticMode;
    if (mode == 2) {
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (mode == 0) {
        swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    }
    DXGI_SWAP_CHAIN_DESC swapDescription{};
    if (SUCCEEDED(swapChain->GetDesc(&swapDescription))) {
        m_window = swapDescription.OutputWindow;
        const Config& config = GetConfig();
        const auto [frameWidth, frameHeight] = StereoFrameSize(m_eyeRecommendedW, m_eyeRecommendedH,
            100, m_frameMaxWidth, m_frameMaxHeight);
        const int width = config.autoHeadsetResolution ? static_cast<int>(frameWidth)
                                                        : config.forceGameWidth;
        const int height = config.autoHeadsetResolution ? static_cast<int>(frameHeight)
                                                         : config.forceGameHeight;
        const bool resizeDesktop = swapDescription.Windowed && !config.virtualBackBuffer;
        AdoptMainWindow(m_window, resizeDesktop ? width : 0, resizeDesktop ? height : 0);
    }
    const double submitStart = NowMs();
    const bool deferWait = SubmitPreparedFrame(backBuffer.Get(), mode == 0);
    const double submitMs = NowMs() - submitStart;
    m_submitMsSum += submitMs;
    PerfAddPresentSide(0.0, 0.0, submitMs);
    if (!deferWait) {
        PrepareFollowingFrame();
    }
    return deferWait;
}

void OpenXRRuntime::OnAfterPresent(IDXGISwapChain* swapChain, double desktopPresentMs, UINT syncInterval)
{
    std::scoped_lock lock(m_mutex);
    if (swapChain != m_mainSwapChain || !m_sessionRunning) {
        return;
    }
    const double periodMs = static_cast<double>(m_displayPeriod) / 1e6;
    if (m_steamVrLastWorldPresent + 1 == m_presents && periodMs > 0.0 &&
        m_presentGapMs > 1.5 * periodMs) {
        ++m_steamVrLongGaps;
    }
    m_steamVrLastWorldPresent = m_presents;
    ++m_steamVrWorldPresents;
    m_steamVrDesktopPresentSum += desktopPresentMs;
    m_steamVrDesktopPresentMax = std::max(m_steamVrDesktopPresentMax, desktopPresentMs);
    m_steamVrDesktopSyncInterval = syncInterval;
    PrepareFollowingFrame();
}

void OpenXRRuntime::PrepareFollowingFrame()
{
    PrepareNextFrame();
    HandleHotkeys();
    const auto& config = GetConfig();
    const bool cursorShowing = WindowsCursorShowing();
    EngineStereoSetDesktopHudMirror(cursorShowing || !m_steamVr || !config.steamVrWorldQuad || config.steamVrMirrorHud);
    m_renderEye = -1;
    EngineStereoSetRenderEye(-1);
    EngineStereoSetAerNative(false);
    EngineStereoSetInventoryHud(m_inventoryOpen);
    EngineStereoSetMonoScreen(config.monoScreen && cursorShowing && !m_inventoryOpen);
    if (m_inventoryOpen) {
        EngineStereoSetTuning(true, !m_stereoEnabled, m_stereoScale, m_convergenceMeters);
    }
    EngineStereoSetVirtualBack(GetConfig().virtualBackBuffer && GetConfig().monoScreen &&
                               m_projectionEnabled && m_sbsInput && m_poseRing[0].projection);
}

void OpenXRRuntime::OnResize(IDXGISwapChain* swapChain, UINT width, UINT height)
{
    std::scoped_lock lock(m_mutex);
    if (swapChain != m_mainSwapChain) {
        return;
    }
    // The quad swapchain is recreated at the next Present with the new size.
    // No back-buffer reference is held across frames, so ResizeBuffers can succeed.
    Log("ResizeBuffers on main swapchain: %ux%u", width, height);
}

}  // namespace a3vr
