#include "EngineStereo.h"
#include "PerfPasses.h"

#include "BodyMask.h"
#include "Config.h"
#include "FrameProbe.h"
#include "HeadTracking.h"
#include "Log.h"
#include "OpticsZoom.h"
#include "ProjectionCache.h"
#include "QuadMath.h"
#include "StereoShadowShader.h"
#include "VrsPixelShader.h"
#include "WorldShadingRate.h"
#include "SinglePassDepth.h"

#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <ocidl.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace a3vr {
namespace {

using Microsoft::WRL::ComPtr;

enum class TargetClass { None, Scene, BackBuffer };

constexpr UINT kProjectionSlot = 2;
constexpr UINT kProjectionBytes = 112;
constexpr UINT kShadowProjectionSlot = 12;
constexpr UINT kMatrix20Offset = 32;  // cb2[2].x
constexpr UINT kMatrix30Offset = 48;  // cb2[3].x

std::atomic<bool> g_tuneEnabled{true};
std::atomic<bool> g_flat{false};
std::atomic<float> g_separationScale{1.0F};
std::atomic<float> g_convergenceFactor{1.0F};
std::atomic<float> g_projectionX{0.9F};
std::atomic<bool> g_projMode{false};
std::atomic<bool> g_centeredOptics{false};
std::atomic<int> g_renderEye{-1};
std::atomic<bool> g_captureDibrDepth{false};
ComPtr<ID3D11Texture2D> g_dibrSceneDepth;
ComPtr<ID3D11Texture2D> g_lastDibrSceneDepth;
ID3D11DepthStencilView* g_dibrDepthView = nullptr;
UINT g_capturedDepthWidth = 0;
DibrSceneDepth g_dibrDepthInfo;
DibrSceneDepth g_lastDibrDepthInfo;
std::atomic<float> g_halfIpd{0.032F};
std::atomic<float> g_aimBias{0.0F};
std::atomic<bool> g_physicalEyes{false};
std::atomic<float> g_rightEyeOffset[3]{};
std::atomic<float> g_headOffset[3]{{0.0F}, {0.0F}, {0.0F}};  // user placement of the head (m)
std::atomic<bool> g_vehicleView{false};
// HUD quad (UI drawn once into its own texture).
std::atomic<bool> g_hudQuadEnabled{false};
std::atomic<bool> g_desktopHudMirror{true};
std::atomic<int> g_hudQuadDraws{0};
bool g_hudLastReady = false;
ComPtr<ID3D11Texture2D> g_hudTexture;
ComPtr<ID3D11RenderTargetView> g_hudRtv;
UINT g_hudTextureWidth = 0;
UINT g_hudTextureHeight = 0;
struct HudBlendEntry {
    D3D11_BLEND_DESC desc{};
    ComPtr<ID3D11BlendState> state;
};
std::vector<HudBlendEntry> g_hudBlends;
ComPtr<ID3D11GeometryShader> g_hudMarkerShader;
bool g_hudMarkerShaderFailed = false;
std::atomic<bool> g_hudLock{true};
std::atomic<bool> g_weaponBodyLock{false};
std::atomic<float> g_weaponAimingBlend{0.0F};
std::atomic<float> g_weaponForward[3]{{0.0F}, {0.0F}, {1.0F}};
std::atomic<float> g_lean[3]{{0.0F}, {0.0F}, {0.0F}};  // camera moves right, up, forward (m)
std::atomic<float> g_eyeTan[8] = {1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
std::atomic<float> g_baseP00{0.0F};
std::atomic<float> g_zoom{1.0F};
std::atomic<bool> g_opticsIntent{false};
std::atomic<float> g_drawZoom{1.0F};
EngineZoomTrace g_zoomTrace;
std::atomic<bool> g_scopeLens{false};  // magnifying optic in use and ScopeLens=1
std::atomic<bool> g_scopeUi{false};    // magnifying optic up: the UI (reticle) is drawn in the picture at the aim axis
std::atomic<int> g_lensPass{0};        // 0 none, 1 world frame, 2 lens frame (alternating)
std::atomic<int> g_lensPassSet{0};     // pass chosen by the head pose publisher for the next frame
std::atomic<bool> g_aerNative{false};  // AER with the engine's own projection and the full target per eye
std::atomic<bool> g_monoScreen{false};  // menus/inventory: the engine draws one normal frame, no doubling
std::atomic<bool> g_inventoryHud{false};
bool g_inventoryDraw = false;
bool g_lastInventoryDraw = false;
std::atomic<bool> g_virtualBack{false};
ComPtr<ID3D11Texture2D> g_virtTexture;
ComPtr<ID3D11RenderTargetView> g_virtRtv;
UINT g_virtWidth = 0;
UINT g_virtHeight = 0;
DXGI_FORMAT g_virtFormat = DXGI_FORMAT_UNKNOWN;
UINT g_frameSceneWidth = 0;
UINT g_frameSceneHeight = 0;
bool g_virtualFrameReady = false;
bool g_virtualLastReady = false;
const char* g_virtualSkipReason = nullptr;
std::atomic<float> g_weaponScale{0.3F};
std::atomic<float> g_weaponSize{0.6F};
std::atomic<float> g_weaponConvergence{0.7F};
std::atomic<bool> g_hudActive{false};
std::atomic<float> g_hudScale{0.6F};
std::atomic<float> g_hudShiftX{0.0F};
std::atomic<float> g_hudShiftY{0.0F};
std::atomic<float> g_hudDistance{2.0F};
std::atomic<UINT> g_sceneWidth{0};
std::atomic<UINT> g_sceneHeight{0};
std::atomic<void*> g_backBuffer{nullptr};
std::atomic<UINT> g_backWidth{0};
std::atomic<UINT> g_backHeight{0};
std::atomic<std::uint64_t> g_frame{1};

std::mutex g_shaderMutex;
std::unordered_set<void*> g_blitShaders;
std::unordered_set<void*> g_projectedShaders;
std::unordered_set<ID3D11VertexShader*> g_hudMarkerVertexShaders;
std::unordered_set<ID3D11PixelShader*> g_hudMarkerPixelShaders;
std::unordered_set<ID3D11PixelShader*> g_shadowResolveShaders;
struct SkinnedShader {
    std::vector<unsigned char> vertexCode;
    ComPtr<ID3D11GeometryShader> mask;
};
std::unordered_map<void*, SkinnedShader> g_skinnedShaders;

// Device resources, created on first use (render thread).
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11Buffer> g_eyeBuffer[2];
ComPtr<ID3D11Buffer> g_patchBuffer;
ComPtr<ID3D11PixelShader> g_stereoShadowShader;
bool g_stereoShadowShaderFailed = false;
bool g_worldVrsThisFrame = false;
bool g_worldVrsLastFrame = false;
bool g_worldVrsCoarseThisFrame = false;
bool g_worldVrsCoarseLastFrame = false;
unsigned g_worldVrsAppliedDraws = 0;
unsigned g_worldVrsFullRateDraws = 0;
unsigned g_worldVrsTargetRejected = 0;
unsigned g_worldVrsShaderRejected = 0;
unsigned g_worldVrsBlendRejected = 0;
unsigned g_worldVrsBindFailed = 0;
bool g_stereoShadowThisFrame = false;
bool g_stereoShadowLastFrame = false;
// Partial projection records contain P00, P11, M20, M21, M30, M31, M32, M33.
constexpr UINT kProjectionRecordFloats = 8;
constexpr UINT kAuxWorldOffset = 4 * kProjectionRecordFloats;
constexpr UINT kClippedBodyOffset = kAuxWorldOffset + 2 * kProjectionRecordFloats;
constexpr UINT kPatchFloats = kClippedBodyOffset + 2 * kProjectionRecordFloats;
bool g_firstPersonClipPrepared = false;
bool g_bodyMaskPrepared = false;
std::atomic<float> g_bodyYaw{0.0F};
std::atomic<float> g_bodyPitch{0.0F};
ComPtr<ID3D11Buffer> g_bodyMaskConstants[2];
std::uint64_t g_bodyMaskFrame[2]{};
constexpr UINT kPatchBytes = kPatchFloats * sizeof(float);
float g_uploaded[kPatchFloats] = {1e9F, 1e9F, 1e9F, 1e9F, 1e9F, 1e9F, 1e9F, 1e9F};
WorldDepthProjection g_preparedWorldDepth;
WorldDepthProjection g_frameWorldDepth;
WorldDepthProjection g_lastWorldDepth;
WorldDrawPoseTrace g_frameWorldDrawPose;
WorldDrawPoseTrace g_lastWorldDrawPose;
std::atomic<const FreeTrackFeed*> g_poseFeed{nullptr};
bool g_resourcesFailed = false;

// Per-frame statistics (render thread).
std::uint32_t g_statDoubled = 0;
std::uint32_t g_singlePassDepthDraws = 0;
std::uint32_t g_singlePassWorldDraws = 0;
std::uint32_t g_singlePassDepthFrames = 0;
std::uint32_t g_statSceneDoubled = 0;
std::uint32_t g_statUiDoubled = 0;
std::uint64_t g_skippedDesktopHudDraws = 0;
std::uint32_t g_statUnpatched = 0;
std::uint32_t g_statScissor = 0;
std::uint32_t g_statWeapon = 0;
std::uint32_t g_lastWeapon = 0;
std::uint32_t g_statComposite = 0;
std::uint32_t g_lastComposite = 0;
std::uint32_t g_lastDoubled = 0;
std::uint32_t g_lastScene = 0;
std::uint32_t g_lastUi = 0;
std::uint32_t g_lastUnpatched = 0;
std::uint32_t g_lastScissor = 0;
std::uint64_t g_presents = 0;
ULONGLONG g_firstPresentTick = 0;
int g_dumps = 0;
bool g_loggedFirst = false;

struct ClassCache {
    void* rtv = nullptr;
    void* dsv = nullptr;
    std::uint64_t frame = 0;
    TargetClass cls = TargetClass::None;
    UINT width = 0;
};
thread_local ClassCache t_cache;

bool IsBlitShader(ID3D11VertexShader* shader)
{
    if (!shader) {
        return false;
    }
    std::scoped_lock lock(g_shaderMutex);
    return g_blitShaders.count(shader) != 0;
}

bool UsesSceneProjection(ID3D11VertexShader* shader)
{
    std::scoped_lock lock(g_shaderMutex);
    return g_projectedShaders.count(shader) != 0;
}

std::unordered_set<ID3D11VertexShader*> g_depthOnlyVertexShaders;
std::unordered_set<ID3D11PixelShader*> g_emptyDepthPixelShaders;
std::unordered_set<ID3D11VertexShader*> g_prepassVertexShaders;
std::unordered_set<ID3D11PixelShader*> g_prepassPixelShaders;
std::unordered_set<ID3D11PixelShader*> g_opaqueVrsPixelShaders;

bool CanShadeOpaqueMaterialCoarsely(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11PixelShader> shader;
    context->PSGetShader(&shader, nullptr, nullptr);
    {
        std::scoped_lock lock(g_shaderMutex);
        if (!g_opaqueVrsPixelShaders.contains(shader.Get())) {
            ++g_worldVrsShaderRejected;
            return false;
        }
    }
    ComPtr<ID3D11BlendState> blend;
    float blendFactor[4]{};
    UINT sampleMask = 0;
    context->OMGetBlendState(&blend, blendFactor, &sampleMask);
    if (!blend) return true;
    D3D11_BLEND_DESC state{};
    blend->GetDesc(&state);
    if (state.AlphaToCoverageEnable || state.RenderTarget[0].BlendEnable) {
        ++g_worldVrsBlendRejected;
        return false;
    }
    return true;
}

bool IsVerifiedSinglePassDraw(ID3D11DeviceContext* context, ID3D11VertexShader* vertex,
                              ID3D11RenderTargetView* target)
{
    if (target) {
        if (!GetConfig().singlePassWorldPrepass) return false;
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        target->GetDesc(&view);
        if (view.Format != DXGI_FORMAT_R32_FLOAT ||
            (view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D &&
             view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2DMS)) return false;
    } else if (!GetConfig().singlePassDepth) {
        return false;
    }
    {
        std::scoped_lock lock(g_shaderMutex);
        const auto& shaders = target ? g_prepassVertexShaders : g_depthOnlyVertexShaders;
        if (!shaders.contains(vertex)) return false;
    }
    ComPtr<ID3D11PixelShader> pixel;
    context->PSGetShader(&pixel, nullptr, nullptr);
    std::scoped_lock lock(g_shaderMutex);
    if (target) return pixel && g_prepassPixelShaders.contains(pixel.Get());
    return !pixel || g_emptyDepthPixelShaders.contains(pixel.Get());
}

TargetClass Classify(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, UINT& width)
{
    const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
    if (t_cache.frame == frame && t_cache.rtv == rtv && t_cache.dsv == dsv) {
        width = t_cache.width;
        return t_cache.cls;
    }
    TargetClass result = TargetClass::None;
    width = 0;
    const UINT backWidth = g_backWidth.load();
    const UINT backHeight = g_backHeight.load();
    if (backWidth > 0 && backHeight > 0) {
        if (dsv) {
            ComPtr<ID3D11Resource> resource;
            dsv->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;
            if (resource && SUCCEEDED(resource.As(&texture))) {
                D3D11_TEXTURE2D_DESC description{};
                texture->GetDesc(&description);
                const double aspect = static_cast<double>(description.Width) / description.Height;
                const double backAspect = static_cast<double>(backWidth) / backHeight;
                const bool matchesWindow = std::fabs(aspect - backAspect) < 0.03 * backAspect;
                const bool wideIndependent = g_virtualBack.load() && std::fabs(aspect - 16.0 / 9.0) < 0.04;
                if (description.Width * 5 >= backWidth * 2 && (matchesWindow || wideIndependent)) {
                    result = TargetClass::Scene;
                    width = description.Width;
                    g_sceneWidth.store(description.Width);
                    g_sceneHeight.store(description.Height);
                }
            }
        }
        if (result == TargetClass::None && rtv) {
            ComPtr<ID3D11Resource> resource;
            rtv->GetResource(&resource);
            if (resource.Get() == g_backBuffer.load()) {
                result = TargetClass::BackBuffer;
                width = backWidth;
            }
        }
    }
    t_cache = {rtv, dsv, frame, result, width};
    return result;
}

// A draw into the back buffer that reads a scene-sized texture is the composite of
// the finished 3D image: that image is already side by side, it must be drawn once.
bool ReadsSceneTexture(ID3D11DeviceContext* context)
{
    const UINT backWidth = g_backWidth.load();
    const UINT backHeight = g_backHeight.load();
    if (backWidth == 0 || backHeight == 0) {
        return false;
    }
    ID3D11ShaderResourceView* views[4]{};
    context->PSGetShaderResources(0, 4, views);
    bool found = false;
    for (ID3D11ShaderResourceView* view : views) {
        if (!view) {
            continue;
        }
        if (!found) {
            ComPtr<ID3D11Resource> resource;
            view->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;
            if (resource && SUCCEEDED(resource.As(&texture))) {
                D3D11_TEXTURE2D_DESC description{};
                texture->GetDesc(&description);
                const double aspect = static_cast<double>(description.Width) / description.Height;
                const double backAspect = static_cast<double>(backWidth) / backHeight;
                found = (g_virtualBack.load() && description.Width == g_frameSceneWidth &&
                         description.Height == g_frameSceneHeight) ||
                        (description.Width * 5 >= backWidth * 2 &&
                         std::fabs(aspect - backAspect) < 0.03 * backAspect);
            }
        }
        view->Release();
    }
    return found;
}

bool ReadRenderProjectionView(float (&matrices)[32])
{
    using ProjectionViewFn = void (*)(float*);
    static const auto projectionView = reinterpret_cast<ProjectionViewFn>(
        GetProcAddress(GetModuleHandleW(nullptr), "RVGetProjectionViewTransform"));
    if (!projectionView) {
        return false;
    }
    projectionView(matrices);
    return true;
}

void SampleWorldDrawPose(bool captureView)
{
    auto& trace = g_frameWorldDrawPose;
    const auto* feed = g_poseFeed.load(std::memory_order_acquire);
    trace.sampled = true;
    trace.frame = g_frame.load(std::memory_order_relaxed);
    trace.returnedBefore = feed ? feed->LastReturnedId() : 0;
    trace.returnedAfter = trace.returnedBefore;
    if (!captureView) {
        return;
    }
    float matrices[32]{};
    const bool haveView = ReadRenderProjectionView(matrices);
    trace.returnedAfter = feed ? feed->LastReturnedId() : 0;
    if (haveView) {
        trace.nativeP00 = matrices[0];
        trace.nativeP11 = matrices[5];
    }
    if (haveView && std::all_of(matrices + 16, matrices + 32, [](float value) {
            return std::isfinite(value);
        }) && std::fabs(matrices[31] - 1.0F) < 0.01F) {
        std::copy_n(matrices + 16, trace.view.size(), trace.view.begin());
        trace.viewValid = true;
    }
}

bool UploadPatch(ID3D11DeviceContext* context)
{
    const float p00Game = g_projectionX.load();
    float data[kPatchFloats]{};
    const auto record = [&data](int layer, int eye) {
        return data + (layer * 2 + eye) * kProjectionRecordFloats;
    };
    g_preparedWorldDepth = {};
    g_firstPersonClipPrepared = false;
    g_bodyMaskPrepared = false;
    const bool vehicle = g_vehicleView.load();
    if (g_projMode.load()) {
        const bool physicalEyes = g_physicalEyes.load();
        // Read on the render thread when drawing starts. Arma can change the near
        // plane as the camera moves, even when its FOV stays constant.
        float matrices[32]{};
        if (ReadRenderProjectionView(matrices) &&
            matrices[10] >= 1.0F && matrices[10] < 1.1F &&
            matrices[14] < 0.0F && std::isfinite(matrices[14]) &&
            std::fabs(matrices[11] - 1.0F) < 0.0001F &&
            std::fabs(matrices[15]) < 0.0001F) {
            g_preparedWorldDepth = {matrices[10], matrices[14],
                                    -(g_lean[2].load() + g_headOffset[2].load()), true};
            std::copy_n(matrices + 16, 16, g_preparedWorldDepth.view.begin());
        }
        // Headset frustum per eye. Row-vector clip.x = x*P00 + z*M20 + M30 (w = z):
        // M20 = -(tanR-tanL)/(tanR+tanL) puts the optical axis where the headset has it,
        // M30 = -s*P00*d moves the camera by s*d (s = -1 left, +1 right).
        const float half = g_halfIpd.load();
        const float measuredZoom = std::max(0.2F, g_zoom.load());
        const auto& config = GetConfig();
        if ((config.worldYawOrbitForward != 0.0F || config.worldYawOrbitRight != 0.0F) &&
            g_preparedWorldDepth.valid && !vehicle && measuredZoom <= 1.03F &&
            g_renderEye.load() < 0 && !g_opticsIntent.load() &&
            g_weaponAimingBlend.load() == 0.0F) {
            g_preparedWorldDepth.yawOrbitCorrection = WorldYawOrbitCorrection(
                g_preparedWorldDepth.view, g_bodyYaw.load(),
                config.worldYawOrbitForward, config.worldYawOrbitRight);
        }
        const float aim = std::clamp(g_weaponAimingBlend.load(), 0.0F, 1.0F);
        const bool clipOnFoot = !vehicle && measuredZoom <= 1.03F;
        g_bodyMaskPrepared = clipOnFoot && config.firstPersonHideTorso;
        const float clearance = clipOnFoot
            ? 0.01F * config.firstPersonClipCm * (1.0F - 0.35F * aim) : 0.0F;
        // Scope lens: frames alternate. Pass 1 = the world at normal scale (the surroundings),
        // pass 2 = the magnified picture for the lens window. Both come from the engine's own
        // zoomed frame, so objects outside its narrow view cone can be missing in pass 1.
        const bool lensOn = GetConfig().scopeLens && measuredZoom > 1.03F;
        // The pass of this frame was chosen when the head pose was published (it decides what
        // the engine is told to aim at), so both stay in step.
        int lensPass = 0;
        if (lensOn) {
            lensPass = g_lensPassSet.load() == 2 ? 2 : 1;
        }
        g_lensPass.store(lensPass);
        g_scopeLens.store(lensOn);
        g_scopeUi.store((GetConfig().scopeLens || GetConfig().scopeReticle) && measuredZoom > 1.03F);
        const float zoom = lensPass == 1 ? 1.0F : measuredZoom;
        g_drawZoom.store(zoom);
        // Magnified picture: the eye separation shrinks with the square root of the magnification
        // (natural depth at the shown scale instead of exaggerated hyper-stereo).
        const float separationDivisor =
            zoom > 1.0F ? std::pow(zoom, 0.5F * GetConfig().zoomMagnification) : 1.0F;
        const bool native = g_aerNative.load() && g_renderEye.load() >= 0 &&
                            matrices[0] > 0.2F && matrices[5] > 0.2F;
        for (int eye = 0; eye < 2; ++eye) {
            const float tanLeft = g_eyeTan[eye * 4 + 0].load();
            const float tanRight = g_eyeTan[eye * 4 + 1].load();
            const float tanUp = g_eyeTan[eye * 4 + 2].load();
            const float tanDown = g_eyeTan[eye * 4 + 3].load();
            const float spanX = std::max(0.2F, tanLeft + tanRight);
            const float spanY = std::max(0.2F, tanUp + tanDown);
            // AER native: keep the engine's own projection (its zoom included) and only move the
            // camera, so the engine's screen-space passes (shadows, AO, glow) see a normal frame.
            const float p00 = native ? matrices[0] : 2.0F / spanX * zoom;
            const float p11 = native ? matrices[5] : 2.0F / spanY * zoom;
            const float shift = eye == 0 ? -1.0F : 1.0F;
            for (int layer = 0; layer < 2; ++layer) {
                // Arma draws the cockpit in its near layer. Share the world's
                // camera projection while retaining that layer's depth range.
                const bool infantryViewmodel = layer == 1 && !vehicle;
                // Through a magnifying optic (not a collimator) the weapon gets no eye separation:
                // it is the same in both eyes, centred, so both eyes can aim through the sight.
                const bool flatWeapon =
                    infantryViewmodel && measuredZoom > 1.03F && config.scopeWeaponFlat;
                const float layerHalf = infantryViewmodel ? half * g_weaponScale.load() : half;
                const float d = flatWeapon ? 0.0F : layerHalf / separationDivisor;
                float* r = record(layer, eye);
                // The camera-relative mode keeps the legacy magnification. In the
                // body-relative mode size is set by the distance to the viewmodel.
                const float aiming = g_weaponAimingBlend.load();
                const float size = infantryViewmodel ? g_weaponSize.load() : 1.0F;
                const float angularSize = size + aiming * (1.0F - size);
                const bool bodyRelativeWeapon = infantryViewmodel && g_weaponBodyLock.load();
                r[0] = bodyRelativeWeapon ? p00 : p00 * angularSize;
                r[1] = bodyRelativeWeapon ? p11 : p11 * angularSize;
                r[2] = native ? 0.0F : -(tanRight - tanLeft) / spanX;
                r[3] = native ? 0.0F : -(tanUp - tanDown) / spanY;
                // Eye position relative to the game camera: s*d, moved by the aim bias so that
                // the aiming eye sits exactly on the sight line (the camera).
                // Head position: the camera moves by (tx, ty, tz) in its own axes. For the
                // row-vector clip = (x*P00 + z*M20 + M30, y*P11 + z*M21 + M31, .., z + M33) this
                // is M30 -= P00*tx + M20*tz, M31 -= P11*ty + M21*tz, M33 = -tz.
                // During inspection, the world and first-person geometry use the same camera.
                // Head roll can put one eye above the other in Arma's yaw/pitch camera axes.
                const bool applyLean = !infantryViewmodel || physicalEyes;
                const float eyeX = physicalEyes ? shift * g_rightEyeOffset[0].load()
                                                : (shift + g_aimBias.load()) * d;
                const float eyeY = physicalEyes ? shift * g_rightEyeOffset[1].load() : 0.0F;
                const float eyeZ = physicalEyes && (layer == 1 || g_preparedWorldDepth.valid)
                    ? shift * g_rightEyeOffset[2].load() : 0.0F;
                const auto& orbit = g_preparedWorldDepth.yawOrbitCorrection;
                const float tx = (applyLean ? g_lean[0].load() : 0.0F) + g_headOffset[0].load() +
                    eyeX + (layer == 0 ? orbit[0] : 0.0F);
                const float ty = (applyLean ? g_lean[1].load() : 0.0F) + g_headOffset[1].load() +
                    eyeY + (layer == 0 ? orbit[1] : 0.0F);
                const float centerZ = layer == 0 ? -g_preparedWorldDepth.clipWOffset :
                    (applyLean ? g_lean[2].load() : 0.0F) + g_headOffset[2].load();
                const float tz = centerZ + eyeZ + (layer == 0 ? orbit[2] : 0.0F);
                r[4] = -p00 * tx;
                r[5] = -p11 * ty;
                if (layer == 0) {
                    // Auxiliary passes can use a different depth projection.
                    float* aux = data + kAuxWorldOffset + eye * kProjectionRecordFloats;
                    std::memcpy(aux, r, kProjectionRecordFloats * sizeof(float));
                    r[6] = g_preparedWorldDepth.depthBias - g_preparedWorldDepth.depthScale * tz;
                }
                r[4] -= r[2] * tz;
                r[5] -= r[3] * tz;
                r[7] = -tz;
                if (bodyRelativeWeapon) {
                    // Arma's model-to-view matrix already accounts for FreeTrack
                    // rotation. Rotate only the distance offset into view space.
                    constexpr float referenceDistance = 0.30F;
                    const float forwardOffset = referenceDistance * (1.0F / size - 1.0F) *
                                                (1.0F - aiming);
                    const float modelX = forwardOffset * g_weaponForward[0].load();
                    const float modelY = forwardOffset * g_weaponForward[1].load();
                    const float modelZ = forwardOffset * g_weaponForward[2].load();
                    r[4] += modelX * p00 + modelZ * r[2];
                    r[5] += modelY * p11 + modelZ * r[3];
                    r[7] += modelZ;
                }
                if (layer == 1) {
                    r[6] = config.weaponProjM32 + config.weaponProjM22 * r[7];
                    float* clipped = data + kClippedBodyOffset + eye * kProjectionRecordFloats;
                    std::memcpy(clipped, r, kProjectionRecordFloats * sizeof(float));
                    clipped[6] = FirstPersonDepthBias(config.weaponProjM22, config.weaponProjM32,
                                                    r[7], clearance);
                    g_firstPersonClipPrepared = clipped[6] < r[6];
                }
            }
        }
    } else {
        const float separation =
            GetConfig().engineStereoSeparationMeters * g_separationScale.load();
        const float convergence =
            std::max(0.1F, GetConfig().engineStereoConvergenceMeters * g_convergenceFactor.load());
        const float half = g_flat.load() ? 0.0F : 0.5F * separation;
        // The weapon layer (viewport depth 0..0.01) sits within arm reach: its own, smaller
        // separation and zero-parallax distance keep it from splitting into two images.
        const float nearHalf = vehicle ? half : half * g_weaponScale.load();
        const float nearConvergence =
            vehicle ? convergence : std::max(0.1F, g_weaponConvergence.load());
        for (int eye = 0; eye < 2; ++eye) {
            const float shift = eye == 0 ? -1.0F : 1.0F;
            float* scene = record(0, eye);
            scene[2] = shift * p00Game * half / convergence;
            scene[4] = -shift * p00Game * half;
            float* nearLayer = record(1, eye);
            nearLayer[2] = shift * p00Game * nearHalf / nearConvergence;
            nearLayer[4] = -shift * p00Game * nearHalf;
        }
    }
    if (std::memcmp(data, g_uploaded, sizeof(data)) == 0) {
        return true;
    }
    context->UpdateSubresource(g_patchBuffer.Get(), 0, nullptr, data, 0, 0);
    std::memcpy(g_uploaded, data, sizeof(data));
    return true;
}
bool EnsureResources(ID3D11DeviceContext* context)
{
    if (g_resourcesFailed) {
        return false;
    }
    if (!g_patchBuffer) {
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = kProjectionBytes;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bool ok = device && SUCCEEDED(device->CreateBuffer(&description, nullptr,
                                                           &g_eyeBuffer[0])) &&
                  SUCCEEDED(device->CreateBuffer(&description, nullptr, &g_eyeBuffer[1]));
        description.ByteWidth = kPatchBytes;
        ok = ok && SUCCEEDED(device->CreateBuffer(&description, nullptr, &g_patchBuffer));
        if (!ok) {
            g_resourcesFailed = true;
            g_eyeBuffer[0].Reset();
            g_eyeBuffer[1].Reset();
            g_patchBuffer.Reset();
            Log("STEREO could not create the eye constant buffers; eyes are not shifted");
            return false;
        }
        g_device = device;
        if (GetConfig().cacheStereoProjection) {
            Log("STEREO projection cache %s", EnableProjectionCacheForContext(context)
                ? "enabled" : "unavailable on the game context; using normal copies");
        }
        Log("STEREO eye constant buffers created");
    }
    // Projection inputs are published at Present and shared by the scene draws of a frame.
    static std::uint64_t preparedFrame = 0;
    const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
    if (preparedFrame == frame) {
        return true;
    }
    if (!UploadPatch(context)) {
        return false;
    }
    preparedFrame = frame;
    return true;
}

}  // namespace

StereoDraw::StereoDraw(ID3D11DeviceContext* context, UINT elementCount, bool instancedOrIndirect)
    : m_context(context)
{
    if (GetConfig().engineStereo && g_tuneEnabled.load(std::memory_order_relaxed) &&
        !g_monoScreen.load(std::memory_order_relaxed)) {
        Begin(elementCount, instancedOrIndirect);
    }
    if (m_foveatedWorld) {
        const auto& config = GetConfig();
        const WorldShadingProjection eyes[2]{
            {g_uploaded[0], g_uploaded[1], g_uploaded[2], g_uploaded[3]},
            {g_uploaded[8], g_uploaded[9], g_uploaded[10], g_uploaded[11]}};
        if (SetWorldShadingRate(context, m_targetWidth, static_cast<UINT>(m_viewport.Height), eyes,
                                config.worldVrsInnerDegrees,
                                WorldShadingTransitionEnd(config.worldVrsInnerDegrees),
                                config.worldVrsCoarseOuter)) {
            g_worldVrsThisFrame = true;
            g_worldVrsCoarseThisFrame |= WorldShadingCoarseActive();
            ++g_worldVrsAppliedDraws;
        } else {
            ++g_worldVrsBindFailed;
        }
    } else if (m_vrsWorldTarget) {
        SuspendWorldShadingRate(context);
    } else {
        ClearWorldShadingRate(context);
    }
}

namespace {

// Repeat the engine's final color pass at the actual 3D target size.
bool EnsureVirtualTarget(ID3D11DeviceContext* context, DXGI_FORMAT format)
{
    const UINT width = g_frameSceneWidth;
    const UINT height = g_frameSceneHeight;
    if (g_virtTexture && g_virtWidth == width && g_virtHeight == height && g_virtFormat == format) {
        return true;
    }
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> view;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &texture)) ||
        FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, &view))) {
        Log("VIRTUAL back buffer: could not create %ux%u", width, height);
        return false;
    }
    const FLOAT clear[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    context->ClearRenderTargetView(view.Get(), clear);
    g_virtTexture = texture;
    g_virtRtv = view;
    g_virtWidth = width;
    g_virtHeight = height;
    g_virtFormat = format;
    Log("VR scene output %ux%u created (desktop buffer %ux%u)", width, height,
        g_backWidth.load(), g_backHeight.load());
    return true;
}

bool EnsureHudTexture(ID3D11DeviceContext* context)
{
    // Keep a sharp HUD without copying an oversized stereo canvas for this mono quad.
    const UINT backWidth = g_backWidth.load();
    const UINT backHeight = g_backHeight.load();
    if (backWidth == 0 || backHeight == 0) {
        return false;
    }
    const UINT width = std::min(backWidth, 3840U);
    const UINT height = std::max(1U, backHeight * width / backWidth);
    if (g_hudTexture && g_hudTextureWidth == width && g_hudTextureHeight == height) {
        return true;
    }
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> view;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &texture)) ||
        FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, &view))) {
        Log("HUD quad: could not create the HUD texture %ux%u", width, height);
        return false;
    }
    g_hudTexture = texture;
    g_hudRtv = view;
    g_hudTextureWidth = width;
    g_hudTextureHeight = height;
    Log("HUD quad: HUD texture %ux%u created", width, height);
    return true;
}

// Arma's bookmarked sprite uses a clip-space UI vertex shader and a BC3 128x128 texture.
bool IsHudMarkerDraw(ID3D11DeviceContext* context, ID3D11VertexShader* vertexShader)
{
    {
        std::scoped_lock lock(g_shaderMutex);
        if (!g_hudMarkerVertexShaders.contains(vertexShader)) {
            return false;
        }
    }
    ComPtr<ID3D11PixelShader> pixelShader;
    context->PSGetShader(&pixelShader, nullptr, nullptr);
    {
        std::scoped_lock lock(g_shaderMutex);
        if (!g_hudMarkerPixelShaders.contains(pixelShader.Get())) {
            return false;
        }
    }
    ComPtr<ID3D11ShaderResourceView> view;
    context->PSGetShaderResources(0, 1, &view);
    if (!view) {
        return false;
    }
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    return desc.Format == DXGI_FORMAT_BC3_TYPELESS && desc.Width == 128 && desc.Height == 128;
}

constexpr char kHudMarkerShader[] = R"(
struct Vertex {
    float4 color : TEXCOORD6;
    float4 uv : TEXCOORD0;
    float4 position : SV_Position;
};
[maxvertexcount(3)]
void Main(triangle Vertex vertices[3], inout TriangleStream<Vertex> stream) {
    const float2 uv0 = vertices[0].uv.xy, uv1 = vertices[1].uv.xy, uv2 = vertices[2].uv.xy;
    const float2 uvMin = min(uv0, min(uv1, uv2));
    const float2 uvMax = max(uv0, max(uv1, uv2));
    const float2 du = uv1 - uv0, dv = uv2 - uv0;
    const float determinant = du.x * dv.y - du.y * dv.x;

    // Both triangles of a full sprite span the whole texture along X and Y.
    // Locate its center by UV so both triangles get the same translation.
    float2 center = 0.0;
    if (all(uvMin >= -0.02) && all(uvMax <= 1.02) &&
        all(uvMin < 0.02) && all(uvMax > 0.98) && abs(determinant) > 0.9 &&
        vertices[0].position.w > 0.0 && vertices[1].position.w > 0.0 &&
        vertices[2].position.w > 0.0) {
        const float2 delta = 0.5 - uv0;
        const float2 bary = float2(delta.x * dv.y - delta.y * dv.x,
                                   du.x * delta.y - du.y * delta.x) / determinant;
        const float2 p0 = vertices[0].position.xy / vertices[0].position.w;
        const float2 p1 = vertices[1].position.xy / vertices[1].position.w;
        const float2 p2 = vertices[2].position.xy / vertices[2].position.w;
        center = p0 + bary.x * (p1 - p0) + bary.y * (p2 - p0);
    }
    [unroll]
    for (uint i = 0; i < 3; ++i) {
        Vertex vertex = vertices[i];
        vertex.position.xy -= center * vertex.position.w;
        stream.Append(vertex);
    }
}
)";

// The engine blends the UI over an opaque picture. On a transparent texture the alpha channel has
// to follow the same "over" rule (premultiplied result), so the alpha blend part is rewritten.
ID3D11BlendState* HudBlendFor(ID3D11DeviceContext* context, ID3D11BlendState* original)
{
    D3D11_BLEND_DESC desc{};
    if (original) {
        original->GetDesc(&desc);
    } else {
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    for (auto& target : desc.RenderTarget) {
        if (target.BlendEnable) {
            target.SrcBlendAlpha = D3D11_BLEND_ONE;
            target.DestBlendAlpha =
                target.DestBlend == D3D11_BLEND_ONE ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA;
            target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        }
    }
    for (const auto& entry : g_hudBlends) {
        if (std::memcmp(&entry.desc, &desc, sizeof(desc)) == 0) {
            return entry.state.Get();
        }
    }
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    HudBlendEntry entry;
    entry.desc = desc;
    if (!device || FAILED(device->CreateBlendState(&desc, &entry.state))) {
        return nullptr;
    }
    g_hudBlends.push_back(entry);
    return g_hudBlends.back().state.Get();
}

}  // namespace

bool StereoDraw::BeginHudQuad()
{
    if (!EnsureHudTexture(m_context)) {
        return false;
    }
    m_context->OMGetRenderTargets(1, &m_savedRtv, &m_savedDsv);
    m_context->OMGetBlendState(&m_savedBlend, m_savedFactor, &m_savedMask);
    ID3D11RenderTargetView* hud = g_hudRtv.Get();
    if (g_hudQuadDraws.fetch_add(1) == 0) {
        const FLOAT clear[4]{};
        m_context->ClearRenderTargetView(hud, clear);
    }
    m_context->OMSetRenderTargets(1, &hud, nullptr);
    if (ID3D11BlendState* blend = HudBlendFor(m_context, m_savedBlend)) {
        m_context->OMSetBlendState(blend, m_savedFactor, m_savedMask);
    }
    const float sx = static_cast<float>(g_hudTextureWidth) / std::max(1.0F, static_cast<float>(m_targetWidth));
    const float sy = static_cast<float>(g_hudTextureHeight) / std::max(1.0F, static_cast<float>(m_targetHeight));
    D3D11_VIEWPORT viewport = m_viewport;
    viewport.TopLeftX *= sx;
    viewport.Width *= sx;
    viewport.TopLeftY *= sy;
    viewport.Height *= sy;
    m_context->RSSetViewports(1, &viewport);
    if (m_hasScissor) {
        D3D11_RECT rect{};
        rect.left = static_cast<LONG>(static_cast<float>(m_scissor.left) * sx);
        rect.right = static_cast<LONG>(static_cast<float>(m_scissor.right) * sx);
        rect.top = static_cast<LONG>(static_cast<float>(m_scissor.top) * sy);
        rect.bottom = static_cast<LONG>(static_cast<float>(m_scissor.bottom) * sy);
        m_context->RSSetScissorRects(1, &rect);
    }
    return true;
}

void StereoDraw::CenterHudMarker(ID3D11VertexShader* vertexShader)
{
    if (g_hudMarkerShaderFailed || !IsHudMarkerDraw(m_context, vertexShader)) {
        return;
    }
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    m_context->IAGetPrimitiveTopology(&topology);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
        topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) {
        return;
    }
    ComPtr<ID3D11GeometryShader> original;
    m_context->GSGetShader(&original, nullptr, nullptr);
    if (original) {
        return;
    }
    if (!g_hudMarkerShader) {
        ComPtr<ID3D11Device> device;
        m_context->GetDevice(&device);
        ComPtr<ID3DBlob> shader;
        if (!device ||
            FAILED(D3DCompile(kHudMarkerShader, sizeof(kHudMarkerShader) - 1, nullptr, nullptr,
                              nullptr, "Main", "gs_5_0", 0, 0, &shader, nullptr)) ||
            FAILED(device->CreateGeometryShader(shader->GetBufferPointer(), shader->GetBufferSize(),
                                                nullptr, &g_hudMarkerShader))) {
            g_hudMarkerShaderFailed = true;
            Log("HUD marked element: could not create centering shader");
            return;
        }
        Log("HUD marked element: centering stock UI 749bbcd4bb855b00 / BC3 128x128");
    }

    m_centeredHudMarker = true;
    m_context->GSSetShader(g_hudMarkerShader.Get(), nullptr, 0);
}

bool StereoDraw::BeginVirtual(ID3D11RenderTargetView* windowTarget)
{
    const UINT backWidth = g_backWidth.load(), backHeight = g_backHeight.load();
    UINT viewportCount = 1;
    m_context->RSGetViewports(&viewportCount, &m_viewport);
    if (viewportCount != 1 || m_viewport.TopLeftX != 0.0F || m_viewport.TopLeftY != 0.0F ||
        m_viewport.Width != static_cast<float>(backWidth) ||
        m_viewport.Height != static_cast<float>(backHeight) || g_frameSceneHeight == 0 ||
        g_frameSceneWidth == 0 || g_frameSceneWidth % 2 != 0) {
        g_virtualSkipReason = "unexpected viewport or scene extent";
        return false;
    }

    // The source of the compositor draw must be the full-resolution 3D scene. A lower
    // resolution intermediate or a UI texture would only produce an enlarged copy.
    ID3D11ShaderResourceView* sources[4]{};
    m_context->PSGetShaderResources(0, 4, sources);
    bool readsFullScene = false;
    for (auto* source : sources) {
        if (!source) {
            continue;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        source->GetDesc(&view);
        if (!readsFullScene && view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D &&
            view.Texture2D.MostDetailedMip == 0 &&
            (view.Format == DXGI_FORMAT_R8G8B8A8_UNORM || view.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
             view.Format == DXGI_FORMAT_B8G8R8A8_UNORM || view.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
             view.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
             view.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || view.Format == DXGI_FORMAT_R11G11B10_FLOAT)) {
            ComPtr<ID3D11Resource> resource;
            ComPtr<ID3D11Texture2D> texture;
            source->GetResource(&resource);
            if (SUCCEEDED(resource.As(&texture))) {
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                readsFullScene = desc.Width == g_frameSceneWidth && desc.Height == g_frameSceneHeight;
            }
        }
        source->Release();
    }
    if (!readsFullScene) {
        g_virtualSkipReason = "final blit has no full-resolution color source in slots 0-3";
        return false;
    }

    D3D11_RENDER_TARGET_VIEW_DESC targetDesc{};
    windowTarget->GetDesc(&targetDesc);
    if (targetDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D ||
        !EnsureVirtualTarget(m_context, targetDesc.Format)) {
        g_virtualSkipReason = "unsupported desktop render target";
        return false;
    }

    ComPtr<ID3D11RasterizerState> rasterizer;
    m_context->RSGetState(&rasterizer);
    D3D11_RASTERIZER_DESC rasterizerDesc{};
    if (rasterizer) {
        rasterizer->GetDesc(&rasterizerDesc);
    }
    if (rasterizerDesc.ScissorEnable) {
        UINT count = 1;
        m_context->RSGetScissorRects(&count, &m_scissor);
        if (count != 1 || m_scissor.left > 0 || m_scissor.top > 0 ||
            m_scissor.right < static_cast<LONG>(backWidth) ||
            m_scissor.bottom < static_cast<LONG>(backHeight)) {
            g_virtualSkipReason = "final blit has a partial scissor rectangle";
            return false;
        }
        m_hasScissor = true;
        const D3D11_RECT rect{0, 0, static_cast<LONG>(g_virtWidth), static_cast<LONG>(g_virtHeight)};
        m_context->RSSetScissorRects(1, &rect);
    }

    m_context->OMGetRenderTargets(1, &m_savedRtv, &m_savedDsv);
    ID3D11RenderTargetView* target = g_virtRtv.Get();
    m_context->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT viewport = m_viewport;
    viewport.Width = static_cast<float>(g_virtWidth);
    viewport.Height = static_cast<float>(g_virtHeight);
    m_context->RSSetViewports(1, &viewport);

    m_virtual = true;
    m_active = true;
    return true;
}

void StereoDraw::BindStereoShadows(ID3D11RenderTargetView* target)
{
    if (!g_frameWorldDepth.valid || !g_patchBuffer || g_stereoShadowShaderFailed ||
        !g_projMode.load() || g_aerNative.load() || !GetConfig().stereoShadows) {
        return;
    }
    D3D11_RENDER_TARGET_VIEW_DESC targetDesc{};
    target->GetDesc(&targetDesc);
    if (targetDesc.Format != DXGI_FORMAT_R8_UNORM ||
        targetDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) {
        return;
    }
    ComPtr<ID3D11PixelShader> original;
    m_context->PSGetShader(&original, nullptr, nullptr);
    {
        std::scoped_lock lock(g_shaderMutex);
        if (!g_shadowResolveShaders.contains(original.Get())) {
            return;
        }
    }
    ComPtr<ID3D11ShaderResourceView> depthView;
    m_context->PSGetShaderResources(0, 1, &depthView);
    if (!depthView) return;
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    depthView->GetDesc(&viewDesc);
    if (viewDesc.Format != DXGI_FORMAT_R32_FLOAT || viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
        viewDesc.Texture2D.MostDetailedMip != 0 || targetDesc.Texture2D.MipSlice != 0) {
        return;
    }
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    depthView->GetResource(&resource);
    if (FAILED(resource.As(&texture))) return;
    D3D11_TEXTURE2D_DESC depthDesc{};
    texture->GetDesc(&depthDesc);
    UINT viewportCount = 1;
    D3D11_VIEWPORT viewport{};
    m_context->RSGetViewports(&viewportCount, &viewport);
    if (viewportCount != 1 || viewport.TopLeftX != 0.0F || viewport.TopLeftY != 0.0F ||
        viewport.Width != static_cast<float>(depthDesc.Width) ||
        viewport.Height != static_cast<float>(depthDesc.Height) || depthDesc.Width % 2 != 0 ||
        depthDesc.Width != g_sceneWidth.load() || depthDesc.Height != g_sceneHeight.load()) {
        return;
    }
    if (!g_stereoShadowShader) {
        ComPtr<ID3DBlob> compiled, errors;
        if (FAILED(D3DCompile(kStereoShadowShader, sizeof(kStereoShadowShader) - 1, "StereoShadow", nullptr,
                              nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                              &compiled, &errors)) ||
            FAILED(g_device->CreatePixelShader(compiled->GetBufferPointer(), compiled->GetBufferSize(),
                                              nullptr, &g_stereoShadowShader))) {
            g_stereoShadowShaderFailed = true;
            Log("Stereo shadow resolve unavailable: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "CreatePixelShader failed");
            return;
        }
        Log("Stereo shadow resolve active: per-eye projection, shared shadow maps (%ux%u)",
            depthDesc.Width, depthDesc.Height);
    }
    m_originalShadowShader = std::move(original);
    m_context->PSGetConstantBuffers(kShadowProjectionSlot, 1, &m_savedShadowConstants);
    ID3D11Buffer* projection = g_patchBuffer.Get();
    m_context->PSSetConstantBuffers(kShadowProjectionSlot, 1, &projection);
    m_context->PSSetShader(g_stereoShadowShader.Get(), nullptr, 0);
    g_stereoShadowThisFrame = true;
}

void StereoDraw::Begin(UINT elementCount, bool instanced)
{
    if (m_context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        return;
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    m_context->OMGetRenderTargets(1, &rtv, &dsv);
    if (!dsv && rtv && !instanced && elementCount == 4) {
        BindStereoShadows(rtv.Get());
    }
    UINT width = 0;
    const TargetClass cls = Classify(rtv.Get(), dsv.Get(), width);
    if (cls == TargetClass::None) {
        return;
    }
    ComPtr<ID3D11VertexShader> vertexShader;
    m_context->VSGetShader(&vertexShader, nullptr, nullptr);
    const bool blit = IsBlitShader(vertexShader.Get());
    const bool smallSceneDraw = cls == TargetClass::Scene && !instanced && elementCount <= 6;
    const bool projectedGeometry = smallSceneDraw && UsesSceneProjection(vertexShader.Get());
    if (cls == TargetClass::BackBuffer && blit && !dsv && !instanced && elementCount <= 6 &&
        g_virtualBack.load() && !g_virtualFrameReady && !g_scopeUi.load() && BeginVirtual(rtv.Get())) {
        ++g_statComposite;
        return;
    }
    if (blit) {
        return;  // a full-screen copy: the image is already side by side
    }
    if (smallSceneDraw && !projectedGeometry) {
        return;  // a screen-space pass; a projected quad can be glass or a cockpit display
    }
    if (cls == TargetClass::BackBuffer && ReadsSceneTexture(m_context)) {
        ++g_statComposite;
        return;  // the composite of the finished 3D image
    }
    UINT viewportCount = 1;
    m_context->RSGetViewports(&viewportCount, &m_viewport);
    if (viewportCount < 1 || width == 0) {
        return;
    }
    m_targetWidth = width;
    m_targetHeight = g_backHeight.load();
    m_ui = cls == TargetClass::BackBuffer;
    m_weapon = cls == TargetClass::Scene && m_viewport.MaxDepth <= 0.0105F;
    g_statWeapon += m_weapon ? 1U : 0U;
    m_mainWorldDepth = cls == TargetClass::Scene && m_viewport.MinDepth >= 0.009F &&
                       m_viewport.MinDepth < 0.02F && m_viewport.MaxDepth >= 0.98F &&
                       m_viewport.TopLeftX == 0.0F && m_viewport.TopLeftY == 0.0F &&
                       m_viewport.Width == static_cast<float>(width);

    bool depthClip = true;  // D3D11 default rasterizer state
    ID3D11RasterizerState* rasterizer = nullptr;
    m_context->RSGetState(&rasterizer);
    if (rasterizer) {
        D3D11_RASTERIZER_DESC description{};
        rasterizer->GetDesc(&description);
        rasterizer->Release();
        depthClip = description.DepthClipEnable != FALSE;
        if (description.ScissorEnable) {
            UINT rectCount = 1;
            m_context->RSGetScissorRects(&rectCount, &m_scissor);
            m_hasScissor = rectCount >= 1;
            ++g_statScissor;
        }
    }

    // Through a scope the reticle and mask are drawn in the picture.
    const bool inventory = g_inventoryHud.load();
    const bool captureHud = inventory ? g_virtualFrameReady : g_hudQuadEnabled.load() && g_hudActive.load();
    if (m_ui && g_projMode.load() && !g_scopeUi.load() &&
        captureHud && BeginHudQuad()) {
        if (!inventory && GetConfig().stabilizeHudMarker && !instanced &&
            (elementCount == 4 || elementCount == 6)) {
            CenterHudMarker(vertexShader.Get());
        }
        m_hudQuad = true;
        m_active = true;
        m_secondDone = true;
        ++g_statUiDoubled;
        return;
    }
    if (m_ui && g_projMode.load() && g_hudActive.load() && g_lensPass.load() == 1) {
        // World frame of the scope lens: the scope overlay and the reticle belong to the lens
        // frame only. A zero-size viewport draws nothing (restored in the destructor).
        D3D11_VIEWPORT none = m_viewport;
        none.Width = 0.0F;
        none.Height = 0.0F;
        m_context->RSSetViewports(1, &none);
        m_active = true;
        m_secondDone = true;
        return;
    }
    if (cls == TargetClass::Scene) {
        m_context->VSGetConstantBuffers(kProjectionSlot, 1, &m_originalBuffer);
        if (m_originalBuffer) {
            D3D11_BUFFER_DESC description{};
            m_originalBuffer->GetDesc(&description);
            m_patchable = description.ByteWidth == kProjectionBytes &&
                          EnsureResources(m_context);
            if (!m_patchable) {
                m_originalBuffer->Release();
                m_originalBuffer = nullptr;
                ++g_statUnpatched;
            }
        } else {
            ++g_statUnpatched;
        }
        if (m_patchable && m_weapon && (g_bodyMaskPrepared || g_firstPersonClipPrepared) &&
            m_viewport.MinDepth >= 0.002F && m_viewport.MinDepth < m_viewport.MaxDepth &&
            m_viewport.TopLeftX == 0.0F && m_viewport.TopLeftY == 0.0F &&
            m_viewport.Width == static_cast<float>(width)) {
            const bool maskBound = g_bodyMaskPrepared && BeginBodyMask(vertexShader.Get());
            if (!maskBound && depthClip && g_firstPersonClipPrepared) {
                std::scoped_lock lock(g_shaderMutex);
                m_firstPersonClip = g_skinnedShaders.contains(vertexShader.Get());
            }
        }
        if (m_firstPersonClip) {
            static bool logged = false;
            if (!logged) {
                Log("VR first-person body clipping active (configured %.1f cm)",
                    GetConfig().firstPersonClipCm);
                logged = true;
            }
        }
        if (m_patchable && m_mainWorldDepth && g_projMode.load()) {
            g_frameWorldDepth = g_preparedWorldDepth;
            if (!g_frameWorldDrawPose.sampled && !g_monoScreen.load(std::memory_order_relaxed)) {
                const auto& config = GetConfig();
                if (config.detailedCapture || (config.headTracking && config.poseBySequence)) {
                    SampleWorldDrawPose(config.detailedCapture);
                }
            }
            // Match the size of the last resolved independent VR scene. Half-size
            // auxiliary passes stay at full rate and cannot churn the primary mask.
            if (rtv && dsv && !g_aerNative.load() && GetConfig().worldVrs && !g_scopeUi.load() &&
                !g_opticsIntent.load() && g_weaponAimingBlend.load() == 0.0F &&
                g_drawZoom.load() <= 1.03F && g_frameWorldDepth.valid && g_virtualBack.load() &&
                width == g_virtWidth && m_viewport.Height == static_cast<float>(g_virtHeight)) {
                D3D11_RENDER_TARGET_VIEW_DESC color{};
                rtv->GetDesc(&color);
                const bool worldColor = color.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                                        color.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
                if (worldColor) {
                    // Keep MSAA, blend and alpha-tested coverage at full rate.
                    // The same tile mask is used only for eligible world materials.
                    if (color.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) {
                        m_vrsWorldTarget = true;
                        m_foveatedWorld = CanShadeOpaqueMaterialCoarsely(m_context);
                    } else {
                        ++g_worldVrsTargetRejected;
                    }
                    if (!m_foveatedWorld) ++g_worldVrsFullRateDraws;
                }
            }
            if (g_virtualBack.load() && width > g_frameSceneWidth) {
                g_frameSceneWidth = width;
                g_frameSceneHeight = static_cast<UINT>(m_viewport.Height);
            }
        }
        const bool depthForDlss =
            (GetConfig().dlssBalancedExperimental || GetConfig().staleEyeWarp) &&
            g_projMode.load() && g_renderEye.load() < 0;
        if (m_patchable && (g_captureDibrDepth.load() || depthForDlss) && dsv &&
            m_viewport.TopLeftX == 0.0F && m_viewport.TopLeftY == 0.0F &&
            m_viewport.Width == static_cast<float>(width)) {
            if (m_mainWorldDepth && (!g_dibrSceneDepth || (depthForDlss && width > g_capturedDepthWidth))) {
                ComPtr<ID3D11Resource> depth;
                dsv->GetResource(&depth);
                if (SUCCEEDED(depth.As(&g_dibrSceneDepth))) {
                    g_dibrDepthView = dsv.Get();
                    g_capturedDepthWidth = width;
                    g_dibrDepthInfo = {};
                    g_dibrDepthInfo.worldMin = m_viewport.MinDepth;
                    g_dibrDepthInfo.worldMax = m_viewport.MaxDepth;
                }
            } else if (dsv.Get() == g_dibrDepthView && m_weapon &&
                       m_viewport.MaxDepth > m_viewport.MinDepth) {
                g_dibrDepthInfo.weaponMin = m_viewport.MinDepth;
                g_dibrDepthInfo.weaponMax = m_viewport.MaxDepth;
            }
        }
        if (g_renderEye.load() < 0 ||
            (m_weapon && m_patchable && g_captureDibrDepth.load())) {
            ++g_statSceneDoubled;
        }
    } else {
        ++g_statUiDoubled;
    }
    // DIBR reconstructs the world only. Render the first-person layer at the
    // current pose of both eyes, including its native depth/occlusion.
    const bool renderBothNear = m_weapon && m_patchable && g_captureDibrDepth.load();
    const int renderEye =
        (cls == TargetClass::Scene || (m_ui && g_aerNative.load())) && !renderBothNear
            ? g_renderEye.load() : -1;
    m_native = g_aerNative.load() && renderEye >= 0;
    m_secondDone = renderEye >= 0;
    m_active = true;
    SetEye(renderEye < 0 ? 0 : renderEye);
    const auto& config = GetConfig();
    if ((config.singlePassDepth || config.singlePassWorldPrepass) && !m_secondDone && m_mainWorldDepth && m_patchable &&
        !m_hasScissor && width == g_virtWidth && m_viewport.Height == static_cast<float>(g_virtHeight) &&
        g_projMode.load() && g_frameWorldDepth.valid && !g_scopeUi.load() && !g_opticsIntent.load() &&
        g_weaponAimingBlend.load() == 0.0F && g_drawZoom.load() <= 1.03F &&
        IsVerifiedSinglePassDraw(m_context, vertexShader.Get(), rtv.Get())) {
        const auto layout = rtv ? SinglePassLayout::WorldPrepass : SinglePassLayout::PositionOnly;
        ID3D11Buffer* rightProjection = rtv ? PrepareEyeProjection(ProjectionVariant(1, true), true) : nullptr;
        m_singlePassStereo = BeginSinglePassDepth(m_context, g_device.Get(), m_viewport,
            g_uploaded, g_uploaded + kProjectionRecordFloats, m_savedDepthGeometryConstants, layout,
            rightProjection);
        if (m_singlePassStereo) {
            m_secondDone = true;
            --g_statSceneDoubled;
            ++(rtv ? g_singlePassWorldDraws : g_singlePassDepthDraws);
            static bool loggedDepth = false;
            static bool loggedWorld = false;
            bool& logged = rtv ? loggedWorld : loggedDepth;
            if (!logged) {
                logged = true;
                Log("Single-pass %s active on verified draws at %ux%u",
                    rtv ? "R32 world prepass (direct right-eye projection)" : "depth", width, g_virtHeight);
            }
        }
    }
    if (!m_secondDone) {
        ++g_statDoubled;
    }
}

bool StereoDraw::BeginBodyMask(ID3D11VertexShader* shader)
{
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    m_context->IAGetPrimitiveTopology(&topology);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
        topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) {
        return false;
    }
    ComPtr<ID3D11GeometryShader> original;
    m_context->GSGetShader(&original, nullptr, nullptr);
    if (original) {
        return false;
    }

    ComPtr<ID3D11GeometryShader> mask;
    {
        std::scoped_lock lock(g_shaderMutex);
        const auto found = g_skinnedShaders.find(shader);
        if (found == g_skinnedShaders.end()) {
            return false;
        }
        auto& variant = found->second;
        if (!variant.vertexCode.empty()) {
            BuildBodyMaskShader(g_device.Get(), variant.vertexCode, &variant.mask);
            variant.vertexCode = {};
        }
        mask = variant.mask;
    }
    if (!mask) {
        return false;
    }

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 5U * 4U * sizeof(float);
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    for (auto& constants : g_bodyMaskConstants) {
        if (!constants && FAILED(g_device->CreateBuffer(&desc, nullptr, &constants))) {
            return false;
        }
    }
    m_context->GSGetConstantBuffers(0, 1, &m_savedGeometryConstants);
    m_bodyMask = std::move(mask);
    m_context->GSSetShader(m_bodyMask.Get(), nullptr, 0);
    static bool logged = false;
    if (!logged) {
        Log("VR torso mask active for the player's skinned first-person geometry");
        logged = true;
    }
    return true;
}

UINT StereoDraw::ProjectionVariant(int eye, bool projected) const
{
    const bool auxiliaryWorld = projected && !m_weapon && !m_mainWorldDepth;
    const UINT layerOffset = auxiliaryWorld ? kAuxWorldOffset :
                             m_firstPersonClip ? kClippedBodyOffset :
                             m_weapon ? 2 * kProjectionRecordFloats : 0;
    return layerOffset / kProjectionRecordFloats + static_cast<UINT>(eye);
}

ID3D11Buffer* StereoDraw::PrepareEyeProjection(UINT variant, bool projected)
{
    const UINT record = variant * kProjectionRecordFloats * sizeof(float);
    const auto build = [&](ID3D11Buffer* target) {
        m_context->CopyResource(target, m_originalBuffer);
        const auto copy = [&](UINT destination, UINT source, UINT bytes) {
            const D3D11_BOX box{record + source, 0, 0, record + source + bytes, 1, 1};
            m_context->CopySubresourceRegion(target, 0, destination, 0, 0, g_patchBuffer.Get(), 0, &box);
        };
        if (projected) {
            copy(0, 0, 4);                // P00   (cb2[0].x)
            copy(20, 4, 4);               // P11   (cb2[1].y)
            copy(kMatrix20Offset, 8, 8);  // M20, M21 (cb2[2].x, cb2[2].y)
            const float clipW = g_uploaded[record / sizeof(float) + 7];
            const bool translateDepth = m_weapon ? (m_firstPersonClip || clipW != 0.0F)
                                                  : (m_mainWorldDepth && clipW != 0.0F);
            // Apply the depth translation to clip.z and clip.w together.
            copy(kMatrix30Offset, 16, translateDepth ? 16U : 8U);
        } else {
            copy(kMatrix20Offset, 8, 4);
            copy(kMatrix30Offset, 16, 4);
        }
    };
    if (GetConfig().cacheStereoProjection) {
        if (auto* cached = EngineProjectionCache().GetOrBuild(m_context, g_device.Get(),
                                                            m_originalBuffer, variant, build)) {
            return cached;
        }
    }
    ID3D11Buffer* target = g_eyeBuffer[variant % 2].Get();
    build(target);
    return target;
}

void StereoDraw::SetEye(int eye)
{
    // x' = ax*x + bx, y' = ay*y + by maps a back buffer pixel to this eye's half.
    const float eyeWidth = static_cast<float>(m_targetWidth) * (m_native ? 1.0F : 0.5F);
    const float eyeOffset = (eye == 0 || m_native) ? 0.0F : eyeWidth;
    float ax = m_native ? 1.0F : 0.5F;
    float bx = eyeOffset;
    float ay = 1.0F;
    float by = 0.0F;
    if (m_ui && g_hudActive.load() && m_targetHeight > 0) {
        // World view: the UI is smaller and nearer the picture centre, with its own depth.
        const float height = static_cast<float>(m_targetHeight);
        const bool lens = g_scopeUi.load() && g_projMode.load();
        float k = lens ? 1.0F : g_hudScale.load();
        const float baseCx = eyeOffset + 0.5F * eyeWidth;
        float forwardX = 0.0F;  // where "straight ahead" lies in this eye image (NDC)
        float forwardY = 0.0F;
        float half = g_flat.load() ? 0.0F
                                   : 0.5F * GetConfig().engineStereoSeparationMeters *
                                         g_separationScale.load();
        float p00 = g_projectionX.load();
        float inverseConvergence = 1.0F / EngineStereoConvergenceMeters();
        if (g_projMode.load()) {
            const float tanLeft = g_eyeTan[eye * 4 + 0].load();
            const float tanRight = g_eyeTan[eye * 4 + 1].load();
            const float tanUp = g_eyeTan[eye * 4 + 2].load();
            const float tanDown = g_eyeTan[eye * 4 + 3].load();
            p00 = 2.0F / std::max(0.2F, tanLeft + tanRight);
            forwardX = -(tanRight - tanLeft) / std::max(0.2F, tanLeft + tanRight);
            forwardY = -(tanUp - tanDown) / std::max(0.2F, tanUp + tanDown);
            half = g_halfIpd.load();
            inverseConvergence = 0.0F;  // the headset frustum converges at infinity
            if (lens && !g_centeredOptics.load()) {
                // Through a scope the reticle belongs to the weapon: it sits where the bore points in
                // the picture (the picture is magnified by the game zoom, so is the offset of the aim
                // axis from the view centre) and follows the mouse, not the head.
                const float aimZ = std::max(0.25F, g_weaponForward[2].load());
                const float magnification = std::max(1.0F, g_drawZoom.load());
                const float p11Scope = 2.0F / std::max(0.2F, tanUp + tanDown);
                forwardX += std::clamp(g_weaponForward[0].load() / aimZ * p00 * magnification, -3.0F, 3.0F);
                forwardY += std::clamp(g_weaponForward[1].load() / aimZ * p11Scope * magnification, -3.0F, 3.0F);
            }
            if (!lens && GetConfig().hudBodyLock && g_hudLock.load()) {
                // Project the body-aligned HUD panel after applying the viewer's head offset.
                const float hudDistance = std::max(0.3F, g_hudDistance.load());
                const float px = hudDistance * g_weaponForward[0].load() - g_lean[0].load();
                const float py = hudDistance * g_weaponForward[1].load() - g_lean[1].load();
                const float pz = std::max(
                    0.3F, hudDistance * std::max(0.25F, g_weaponForward[2].load()) - g_lean[2].load());
                const float p11 = 2.0F / std::max(0.2F, tanUp + tanDown);
                forwardX += std::clamp(px / pz * p00, -3.0F, 3.0F);
                forwardY += std::clamp(py / pz * p11, -3.0F, 3.0F);
                k *= std::clamp(hudDistance / pz, 0.5F, 2.0F);
            }
        }
        const float shiftX = lens ? 0.0F : g_hudShiftX.load();
        const float shiftY = lens ? 0.0F : g_hudShiftY.load();
        const float cx = baseCx + shiftX * eyeWidth + forwardX * eyeWidth * 0.5F;
        const float cy = 0.5F * height - shiftY * height - forwardY * height * 0.5F;
        const float distance = std::max(0.3F, g_hudDistance.load());
        // Same parallax a scene point at `distance` gets: the left eye shifts right.
        // A scope picture sits at infinity (no parallax).
        const float parallaxNdc = lens ? 0.0F : p00 * half * (1.0F / distance - inverseConvergence);
        const float parallaxPixels = (eye == 0 ? 1.0F : -1.0F) * parallaxNdc * eyeWidth * 0.5F;
        ax = (m_native ? 1.0F : 0.5F) * k;
        bx = cx + (eyeOffset - baseCx) * k + parallaxPixels;
        ay = k;
        by = cy - 0.5F * height * k;
    }
    D3D11_VIEWPORT viewport = m_viewport;
    viewport.TopLeftX = ax * m_viewport.TopLeftX + bx;
    viewport.Width = ax * m_viewport.Width;
    viewport.TopLeftY = ay * m_viewport.TopLeftY + by;
    viewport.Height = ay * m_viewport.Height;
    m_context->RSSetViewports(1, &viewport);
    if (m_hasScissor) {
        D3D11_RECT rect{};
        rect.left = static_cast<LONG>(ax * static_cast<float>(m_scissor.left) + bx);
        rect.right = static_cast<LONG>(ax * static_cast<float>(m_scissor.right) + bx);
        rect.top = static_cast<LONG>(ay * static_cast<float>(m_scissor.top) + by);
        rect.bottom = static_cast<LONG>(ay * static_cast<float>(m_scissor.bottom) + by);
        m_context->RSSetScissorRects(1, &rect);
    }
    if (m_patchable) {
        const bool projected = g_projMode.load();
        const UINT variant = ProjectionVariant(eye, projected);
        ID3D11Buffer* target = PrepareEyeProjection(variant, projected);
        m_context->VSSetConstantBuffers(kProjectionSlot, 1, &target);

        if (m_bodyMask) {
            ID3D11Buffer* constants = g_bodyMaskConstants[eye].Get();
            const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
            if (g_bodyMaskFrame[eye] != frame) {
                const float* r = g_uploaded + variant * kProjectionRecordFloats;
                const float yaw = g_bodyYaw.load(), pitch = g_bodyPitch.load();
                const float sy = std::sin(yaw), cy = std::cos(yaw);
                const float sp = std::sin(pitch), cp = std::cos(pitch);
                const float parameters[5][4]{
                    {r[0], r[1], r[2], r[3]}, {r[4], r[5], r[7], 0.0F},
                    {cy, sy * sp, -sy * cp, 0.0F}, {0.0F, cp, sp, 0.0F},
                    {sy, -cy * sp, cy * cp, 0.0F}};
                m_context->UpdateSubresource(constants, 0, nullptr, parameters, 0, 0);
                g_bodyMaskFrame[eye] = frame;
            }
            m_context->GSSetConstantBuffers(0, 1, &constants);
        }
    }
}

bool StereoDraw::NextEye()
{
    if (!m_active) {
        return false;
    }
    // The draw already reached the headset's HUD texture. Skip the duplicate
    // draw into the monitor image when the mirror does not need a HUD.
    if (m_hudQuad && g_virtualFrameReady && !g_desktopHudMirror.load()) {
        ++g_skippedDesktopHudDraws;
        return false;  // The destructor restores the redirected draw's state.
    }
    // Replay to the monitor only when the frame has its own VR scene
    // texture, so the desktop fallback cannot include a duplicate HUD.
    if (m_virtual || (m_hudQuad && g_virtualFrameReady)) {
        m_profileRepeat = PerfGpuRepeatBegin(m_context, PerfRepeatKind::Desktop);
        g_virtualFrameReady |= m_virtual;
        RestoreRedirectedDraw();
        // The hook issues the original draw once more into the desktop back buffer.
        m_active = false;
        return true;
    }
    if (m_secondDone) {
        return false;
    }
    m_secondDone = true;
    const auto kind = m_ui ? PerfRepeatKind::Overlay
                          : m_weapon ? PerfRepeatKind::FirstPerson : PerfRepeatKind::World;
    m_profileRepeat = PerfGpuRepeatBegin(m_context, kind);
    SetEye(1);
    return true;
}

void StereoDraw::RestoreRedirectedDraw()
{
    m_context->RSSetViewports(1, &m_viewport);
    if (m_hasScissor) {
        m_context->RSSetScissorRects(1, &m_scissor);
    }
    m_context->OMSetRenderTargets(1, &m_savedRtv, m_savedDsv);
    if (m_hudQuad) {
        m_context->OMSetBlendState(m_savedBlend, m_savedFactor, m_savedMask);
        if (m_centeredHudMarker) {
            m_context->GSSetShader(nullptr, nullptr, 0);
        }
    }
    if (m_savedRtv) {
        m_savedRtv->Release();
    }
    if (m_savedDsv) {
        m_savedDsv->Release();
    }
    if (m_savedBlend) {
        m_savedBlend->Release();
    }
}

StereoDraw::~StereoDraw()
{
    if (m_originalShadowShader) {
        ID3D11Buffer* constants = m_savedShadowConstants.Get();
        m_context->PSSetConstantBuffers(kShadowProjectionSlot, 1, &constants);
        m_context->PSSetShader(m_originalShadowShader.Get(), nullptr, 0);
    }
    if (m_profileRepeat) {
        PerfGpuRepeatEnd(m_context);
    }
    if (!m_active) {
        return;
    }
    if (m_virtual || m_hudQuad) {
        RestoreRedirectedDraw();
        return;
    }
    if (m_singlePassStereo) {
        EndSinglePassDepth(m_context, m_savedDepthGeometryConstants.Get());
    }
    m_context->RSSetViewports(1, &m_viewport);
    if (m_hasScissor) {
        m_context->RSSetScissorRects(1, &m_scissor);
    }
    if (m_bodyMask) {
        m_context->GSSetShader(nullptr, nullptr, 0);
        ID3D11Buffer* constants = m_savedGeometryConstants.Get();
        m_context->GSSetConstantBuffers(0, 1, &constants);
    }
    if (m_patchable) {
        m_context->VSSetConstantBuffers(kProjectionSlot, 1, &m_originalBuffer);
    }
    if (m_originalBuffer) {
        m_originalBuffer->Release();
    }
}

void EngineStereoNoteVertexShader(ID3D11VertexShader* shader, const void* code, SIZE_T length)
{
    if (!GetConfig().engineStereo || !shader || !code) {
        return;
    }
    // The engine strips the reflection data (D3DReflect reports 0 constant buffers
    // for every shader), so the instruction listing is read instead. No constant
    // buffer declared = no model or projection transform: a pass-through blit of
    // a full-screen texture.
    ComPtr<ID3DBlob> listing;
    if (FAILED(D3DDisassemble(code, length, D3D_DISASM_DISABLE_DEBUG_INFO, nullptr, &listing)) ||
        !listing) {
        return;
    }
    const std::string_view blob(static_cast<const char*>(listing->GetBufferPointer()),
                                listing->GetBufferSize());
    // D3DDisassemble includes the C-string terminator in the blob size.
    const std::string_view text = blob.substr(0, blob.find('\0'));
    {
        std::scoped_lock lock(g_shaderMutex);
        // A new shader can reuse the address of a destroyed one.
        g_blitShaders.erase(shader);
        g_projectedShaders.erase(shader);
        g_skinnedShaders.erase(shader);
        g_hudMarkerVertexShaders.erase(shader);
        g_depthOnlyVertexShaders.erase(shader);
        g_prepassVertexShaders.erase(shader);
        const bool depthCandidate = GetConfig().singlePassDepth &&
            (length == 8956 || length == 1148 || length == 10784 || length == 796);
        const bool prepassCandidate = GetConfig().singlePassWorldPrepass &&
            (length == 1880 || length == 1700 || length == 2608 || length == 5440 || length == 4812);
        if (length == 604 || depthCandidate || prepassCandidate) {
            std::uint64_t hash = 1469598103934665603ULL;
            const auto* bytes = static_cast<const unsigned char*>(code);
            for (SIZE_T i = 0; i < length; ++i) {
                hash = (hash ^ bytes[i]) * 1099511628211ULL;
            }
            if (hash == 0x678df5acc771c4b2ULL) {
                g_hudMarkerVertexShaders.insert(shader);
            }
            // Captured world prepass shaders write only SV_Position with the cb2 projection.
            if (depthCandidate && (hash == 0x98d2b153d475c67fULL || hash == 0xbe9e995a4df8aeb9ULL ||
                hash == 0xb0ad7d667687a1f1ULL || hash == 0xf6133f99a5727677ULL)) {
                g_depthOnlyVertexShaders.insert(shader);
            }
            // In these shaders the patched cb2 projection rows feed SV_Position alone.
            // Their TEXCOORD outputs can be forwarded to both viewports unchanged.
            if (prepassCandidate && (hash == 0xd8c45c8412ed8119ULL || hash == 0x7a034df3ab737852ULL ||
                hash == 0x2c42c71658fd3092ULL || hash == 0xe5be18a571962f9cULL ||
                hash == 0x13070def8bbd1e95ULL || hash == 0xb8ebeb58ba2ba726ULL)) {
                g_prepassVertexShaders.insert(shader);
            }
        }
        if (text.find("dcl_output_siv o0.xyzw, position") != std::string_view::npos &&
            text.find("cb2[0].xyzw") != std::string_view::npos &&
            text.find("cb2[1].xyzw") != std::string_view::npos &&
            text.find("cb2[2].xyzw") != std::string_view::npos &&
            text.find("cb2[3].xyzw") != std::string_view::npos) {
            g_projectedShaders.insert(shader);
            if (text.find("BLENDWEIGHT") != std::string_view::npos &&
                text.find("BLENDINDICES") != std::string_view::npos) {
                const auto* begin = static_cast<const unsigned char*>(code);
                auto& entry = g_skinnedShaders[shader];
                entry.vertexCode.assign(begin, begin + length);
            }
        }
    }
    if (text.find("dcl_constantbuffer") != std::string_view::npos) {
        return;
    }
    // ... and nothing but copies: the 2D UI shader also has no constant buffer,
    // but it converts colours and divides by w (utof, mul, div) and must be doubled.
    std::size_t position = 0;
    while (position < text.size()) {
        std::size_t end = text.find('\n', position);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(position, end - position);
        position = end + 1;
        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string_view::npos) {
            continue;
        }
        line.remove_prefix(first);
        if (line.rfind("//", 0) == 0 || line.rfind("dcl_", 0) == 0 || line.rfind("vs_", 0) == 0) {
            continue;
        }
        const std::size_t space = line.find_first_of(" \t\r");
        const std::string_view opcode = line.substr(0, space);
        if (opcode != "mov" && opcode != "ret") {
            return;
        }
    }
    std::scoped_lock lock(g_shaderMutex);
    g_blitShaders.insert(shader);
}

void EngineStereoNotePixelShader(ID3D11PixelShader* shader, const void* code, SIZE_T size,
                                std::uint64_t bytecodeHash)
{
    ComPtr<ID3DBlob> listing;
    const bool opaque = size >= 1024 &&
        SUCCEEDED(D3DDisassemble(code, size, D3D_DISASM_DISABLE_DEBUG_INFO, nullptr, &listing)) &&
        listing && IsOpaqueVrsMaterial({static_cast<const char*>(listing->GetBufferPointer()),
                                        listing->GetBufferSize()});
    std::scoped_lock lock(g_shaderMutex);
    g_opaqueVrsPixelShaders.erase(shader);
    if (opaque) g_opaqueVrsPixelShaders.insert(shader);
    // These shaders consume the verified interpolants and only write SV_Target0.
    if (GetConfig().singlePassWorldPrepass && (bytecodeHash == 0x70d42f7413931f06ULL ||
        bytecodeHash == 0x9415ff056842a49bULL || bytecodeHash == 0x93b19fefd1f317c0ULL ||
        bytecodeHash == 0x8a76294ec7603869ULL)) {
        g_prepassPixelShaders.insert(shader);
    } else {
        g_prepassPixelShaders.erase(shader);
    }
    if (GetConfig().singlePassDepth && bytecodeHash == 0xb403f48cb35d9739ULL) {
        g_emptyDepthPixelShaders.insert(shader);
    } else {
        g_emptyDepthPixelShaders.erase(shader);
    }
    if (bytecodeHash == 0x48c433fb9f5427e7ULL) {
        g_shadowResolveShaders.insert(shader);
    } else {
        g_shadowResolveShaders.erase(shader);
    }
    if (bytecodeHash == 0x749bbcd4bb855b00ULL) {
        g_hudMarkerPixelShaders.insert(shader);
    } else {
        g_hudMarkerPixelShaders.erase(shader);
    }
}

void EngineStereoSetTuning(bool enabled, bool flat, float separationScale, float convergenceFactor)
{
    g_tuneEnabled.store(enabled);
    g_flat.store(flat);
    g_separationScale.store(std::clamp(separationScale, 0.0F, 4.0F));
    g_convergenceFactor.store(std::clamp(convergenceFactor, 0.05F, 40.0F));
}

void EngineStereoSetHeadset(bool active, float halfIpdMeters, const float tangents[8],
                            float aimBias, bool centeredOptics)
{
    g_aimBias.store(std::clamp(aimBias, -1.0F, 1.0F));
    g_centeredOptics.store(centeredOptics);
    for (int i = 0; i < 8; ++i) {
        g_eyeTan[i].store(std::clamp(tangents[i], 0.1F, 6.0F));
    }
    g_halfIpd.store(std::clamp(halfIpdMeters, 0.0F, 0.1F));
    g_projMode.store(active);
}

void EngineStereoSetLean(float right, float up, float forward)
{
    g_lean[0].store(std::clamp(right, -0.5F, 0.5F));
    g_lean[1].store(std::clamp(up, -0.5F, 0.5F));
    g_lean[2].store(std::clamp(forward, -0.5F, 0.5F));
}

void EngineStereoSetPhysicalEyes(bool active, float right, float up, float forward)
{
    g_rightEyeOffset[0].store(right);
    g_rightEyeOffset[1].store(up);
    g_rightEyeOffset[2].store(forward);
    g_physicalEyes.store(active);
}

void EngineStereoSetVehicle(bool vehicle)
{
    g_vehicleView.store(vehicle);
}

void EngineStereoSetHeadOffset(float right, float up, float forward)
{
    g_headOffset[0].store(std::clamp(right, -0.6F, 0.6F));
    g_headOffset[1].store(std::clamp(up, -0.6F, 0.6F));
    g_headOffset[2].store(std::clamp(forward, -0.6F, 0.6F));
}

void EngineStereoSetHudQuad(bool enabled)
{
    g_hudQuadEnabled.store(enabled);
}

void EngineStereoSetDesktopHudMirror(bool enabled)
{
    g_desktopHudMirror.store(enabled);
}

ID3D11Texture2D* EngineStereoHudTexture(UINT& width, UINT& height)
{
    width = g_hudTextureWidth;
    height = g_hudTextureHeight;
    return g_hudLastReady ? g_hudTexture.Get() : nullptr;
}

void EngineStereoSetHudLock(bool enabled)
{
    g_hudLock.store(enabled);
}

void EngineStereoSetBodyAngles(float yaw, float pitch)
{
    g_bodyYaw.store(yaw);
    g_bodyPitch.store(pitch);
}

void EngineStereoSetWeaponForward(float right, float up, float forward, bool bodyLock,
                                  float aimingBlend)
{
    g_weaponAimingBlend.store(aimingBlend);
    g_weaponForward[0].store(right);
    g_weaponForward[1].store(up);
    g_weaponForward[2].store(forward);
    g_weaponBodyLock.store(bodyLock);
}

std::uint32_t EngineStereoLastWeaponDraws()
{
    return g_lastWeapon;
}

void EngineStereoSetOpticsIntent(bool enabled)
{
    g_opticsIntent.store(enabled);
}

float EngineStereoDrawZoom()
{
    return g_drawZoom.load();
}

bool EngineStereoScopeUi()
{
    return g_scopeUi.load() && g_projMode.load();
}

bool EngineStereoScopeLens()
{
    return g_scopeLens.load() && g_projMode.load();
}

EngineZoomTrace EngineStereoLastZoomTrace()
{
    return g_zoomTrace;
}

void EngineStereoSetMonoScreen(bool mono)
{
    g_monoScreen.store(mono);
}

void EngineStereoSetInventoryHud(bool active)
{
    g_inventoryHud.store(active);
}

std::uint64_t EngineStereoFrameId()
{
    return g_frame.load(std::memory_order_relaxed);
}

void EngineStereoNoteInventoryDraw()
{
    g_inventoryDraw = true;
}

bool EngineStereoLastInventoryDraw()
{
    return g_lastInventoryDraw;
}

void EngineStereoSetVirtualBack(bool enabled)
{
    g_virtualBack.store(enabled);
}

ID3D11Texture2D* EngineStereoVirtualTexture()
{
    return g_virtualLastReady ? g_virtTexture.Get() : nullptr;
}

bool EngineStereoTargetIsBackBuffer(ID3D11DeviceContext* context)
{
    ID3D11RenderTargetView* rtv = nullptr;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    bool result = false;
    if (rtv) {
        ComPtr<ID3D11Resource> resource;
        rtv->GetResource(&resource);
        result = resource.Get() == g_backBuffer.load();
        rtv->Release();
    }
    return result;
}

void EngineStereoSetAerNative(bool native)
{
    g_aerNative.store(native);
}

void EngineStereoSetLensPass(int pass)
{
    g_lensPassSet.store(pass);
}

int EngineStereoLensPass()
{
    return g_projMode.load() ? g_lensPass.load() : 0;
}

float EngineStereoZoom()
{
    return g_projMode.load() ? g_zoom.load() : 1.0F;
}

void EngineStereoResetZoomReference()
{
    // Preserve the widest known game FOV so recentering while scoped cannot reset the zoom baseline.
}

void EngineStereoSetWeapon(float separationScale, float convergenceMeters, float sizeScale)
{
    g_weaponSize.store(std::clamp(sizeScale, 0.2F, 1.5F));
    g_weaponScale.store(std::clamp(separationScale, 0.0F, 2.0F));
    g_weaponConvergence.store(std::clamp(convergenceMeters, 0.2F, 5.0F));
}

void EngineStereoSetHud(bool active, float scale, float shiftX, float shiftY, float distanceMeters)
{
    g_hudActive.store(active);
    g_hudScale.store(std::clamp(scale, 0.2F, 1.2F));
    g_hudShiftX.store(std::clamp(shiftX, -0.6F, 0.6F));
    g_hudShiftY.store(std::clamp(shiftY, -0.6F, 0.6F));
    g_hudDistance.store(std::clamp(distanceMeters, 0.4F, 10.0F));
}

void EngineStereoSceneSize(unsigned& width, unsigned& height)
{
    width = g_sceneWidth.load();
    height = g_sceneHeight.load();
}

bool EngineStereoActive()
{
    return GetConfig().engineStereo && g_tuneEnabled.load();
}

bool EngineStereoPerfWorld()
{
    return EngineStereoActive() && g_projMode.load() && !g_monoScreen.load() && !g_scopeUi.load();
}

float EngineStereoConvergenceMeters()
{
    return std::max(0.1F, GetConfig().engineStereoConvergenceMeters * g_convergenceFactor.load());
}

void EngineStereoSetRenderEye(int eye, bool captureDibrDepth)
{
    g_renderEye.store(eye);
    g_captureDibrDepth.store(eye >= 0 && captureDibrDepth);
}

WorldDepthProjection EngineStereoLastWorldDepthProjection()
{
    return g_lastWorldDepth;
}

void EngineStereoSetPoseFeed(const FreeTrackFeed* feed)
{
    g_poseFeed.store(feed, std::memory_order_release);
}

WorldDrawPoseTrace EngineStereoLastWorldDrawPoseTrace()
{
    return g_lastWorldDrawPose;
}

void EngineStereoReadWorldEyeRecords(float (&eyes)[2][8])
{
    std::memcpy(eyes, g_uploaded, sizeof(eyes));
}

DibrSceneDepth EngineStereoLastDibrDepth()
{
    return g_lastDibrDepthInfo;
}

bool EngineStereoLastWorldVrs()
{
    return g_worldVrsLastFrame;
}

bool EngineStereoLastWorldVrsCoarse()
{
    return g_worldVrsCoarseLastFrame;
}

bool EngineStereoLastStereoShadow()
{
    return g_stereoShadowLastFrame;
}

void EngineStereoOnPresent(IDXGISwapChain* swapChain)
{
    if (!swapChain) {
        return;
    }
    // Overlays may present their own swapchains: follow the game's one.
    static IDXGISwapChain* mainSwapChain = nullptr;
    static int foreignPresents = 0;
    if (!mainSwapChain) {
        mainSwapChain = swapChain;
    } else if (swapChain != mainSwapChain) {
        if (++foreignPresents < 30) {
            return;
        }
        mainSwapChain = swapChain;
    }
    foreignPresents = 0;
    ++g_presents;
    if ((GetConfig().singlePassDepth || GetConfig().singlePassWorldPrepass) && ++g_singlePassDepthFrames == 240) {
        Log("Single-pass depth issued %u draws in the last 240 presents", g_singlePassDepthDraws);
        Log("Single-pass R32 world prepass issued %u draws in the last 240 presents", g_singlePassWorldDraws);
        g_singlePassDepthFrames = g_singlePassDepthDraws = g_singlePassWorldDraws = 0;
    }
    g_frame.fetch_add(1);
    if (GetConfig().cacheStereoProjection) {
        EngineProjectionCache().Invalidate();
    }
    g_virtualLastReady = g_virtualFrameReady;
    g_virtualFrameReady = false;
    if (g_virtualBack.load() && !g_virtualLastReady && !g_monoScreen.load()) {
        const char* reason = g_scopeUi.load() ? "scope overlay"
            : (g_virtualSkipReason ? g_virtualSkipReason : "no recognized desktop blit");
        static const char* previousReason = nullptr;
        static unsigned messages = 0;
        if (messages < 12 && previousReason != reason) {
            ++messages;
            previousReason = reason;
            Log("VR scene output unavailable: %s (scene %ux%u, desktop %ux%u)", reason,
                g_frameSceneWidth, g_frameSceneHeight, g_backWidth.load(), g_backHeight.load());
        }
    }
    g_virtualSkipReason = nullptr;
    g_frameSceneWidth = g_frameSceneHeight = 0;
    // The next draw starts on a clear HUD even if OpenXR skipped the completed frame.
    g_hudLastReady = g_hudQuadDraws.exchange(0) > 0;
    g_lastInventoryDraw = g_inventoryDraw;
    g_inventoryDraw = false;
    g_lastWorldDepth = g_frameWorldDepth;
    g_lastWorldDrawPose = g_frameWorldDrawPose;
    ClearWorldShadingRate();
    g_worldVrsLastFrame = g_worldVrsThisFrame;
    g_worldVrsCoarseLastFrame = g_worldVrsCoarseThisFrame;
    g_worldVrsCoarseThisFrame = false;
    g_stereoShadowLastFrame = g_stereoShadowThisFrame;
    g_worldVrsThisFrame = g_stereoShadowThisFrame = false;
    g_frameWorldDepth = {};
    g_frameWorldDrawPose = {};
    g_lastDibrSceneDepth = std::move(g_dibrSceneDepth);
    g_lastDibrDepthInfo = g_dibrDepthInfo;
    g_lastDibrDepthInfo.texture = g_lastDibrSceneDepth.Get();
    g_dibrDepthInfo = {};
    g_dibrDepthView = nullptr;
    g_capturedDepthWidth = 0;
    g_lastDoubled = g_statDoubled;
    g_lastScene = g_statSceneDoubled;
    g_lastUi = g_statUiDoubled;
    g_lastUnpatched = g_statUnpatched;
    g_lastScissor = g_statScissor;
    g_lastComposite = g_statComposite;
    g_statComposite = 0;
    g_lastWeapon = g_statWeapon;
    g_statWeapon = 0;
    g_statDoubled = g_statSceneDoubled = g_statUiDoubled = g_statUnpatched = g_statScissor = 0;

    ComPtr<ID3D11Texture2D> backBuffer;
    if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) && backBuffer) {
        D3D11_TEXTURE2D_DESC description{};
        backBuffer->GetDesc(&description);
        g_backBuffer.store(backBuffer.Get());
        g_backWidth.store(description.Width);
        g_backHeight.store(description.Height);
    }
    if (!GetConfig().engineStereo) {
        return;
    }
    float matrices[32]{};
    const bool traceZoom = GetConfig().detailedCapture;
    const bool haveProjection = ReadRenderProjectionView(matrices);
    if (traceZoom) {
        g_zoomTrace = {};
        g_zoomTrace.nativeP00 = matrices[0];
        g_zoomTrace.nativeP11 = matrices[5];
        g_zoomTrace.reference = g_baseP00.load();
        g_zoomTrace.previousReference = g_zoomTrace.reference;
        if (haveProjection && g_zoomTrace.reference > 0.0F) {
            g_zoomTrace.nativeRatio = matrices[0] / g_zoomTrace.reference;
        }
        g_zoomTrace.opticsIntent = g_opticsIntent.load();
        g_zoomTrace.decision = haveProjection ? "p00_outside_limits" : "no_native_projection";
    }
    static bool confirmedOptics = false;
    if (haveProjection) {
        if (ValidZoomProjection(matrices[0], matrices[5], matrices[11])) {
            if (matrices[0] < 8.0F) {
                g_projectionX.store(matrices[0]);
            }
            if (g_projMode.load() && g_hudActive.load()) {
                float base = g_baseP00.load();
                const bool explicitOptics = GetConfig().adsRightMouseToggle;
                const bool optics = g_opticsIntent.load();
                static float candidate = 0.0F;
                static int stableFrames = 0;
                bool updateBase = base <= 0.0F && matrices[0] < 1.25F;
                if (explicitOptics) {
                    // Seat and camera switches change the native lens even at normal
                    // VR scale. Track a settled non-optics lens for the next sight.
                    const bool trackReference = NativeZoomMayUpdateReference(matrices[0], base, optics);
                    updateBase &= trackReference;
                    if (trackReference) {
                        if (std::fabs(matrices[0] - candidate) > 0.0005F) {
                            candidate = matrices[0];
                            stableFrames = 0;
                        }
                        stableFrames = std::min(stableFrames + 1, 6);
                        updateBase |= stableFrames == 6 && std::fabs(candidate - base) > 0.0005F;
                    } else {
                        stableFrames = 0;
                    }
                } else {
                    updateBase |= matrices[0] < base;
                }
                if (updateBase) {
                    base = explicitOptics && !optics ? candidate : matrices[0];
                    g_baseP00.store(base);
                    static int referenceLogs = 0;
                    if (referenceLogs++ < 24) {
                        Log("STEREO zoom reference P00=%.4f", base);
                    }
                }
                confirmedOptics = explicitOptics &&
                    KeepNativeOptics(matrices[0], base, optics, confirmedOptics);
                const float zoom = NativeOpticsZoom(matrices[0], base, explicitOptics,
                                                    optics || confirmedOptics);
                if (traceZoom) {
                    g_zoomTrace.reference = base;
                    g_zoomTrace.nativeRatio = base > 0.0F ? matrices[0] / base : 0.0F;
                    g_zoomTrace.opticsIntent = optics;
                    g_zoomTrace.referenceUpdated = updateBase;
                    g_zoomTrace.decision = base <= 0.0F ? "waiting_for_world_reference" :
                        explicitOptics && zoom == 1.0F ? "below_zoom_threshold" : "accepted";
                }
                g_zoom.store(zoom);
                static float loggedZoom = 1.0F;
                static int zoomLogs = 0;
                if (std::fabs(g_zoom.load() - loggedZoom) > 0.05F && zoomLogs++ < 80) {
                    loggedZoom = g_zoom.load();
                    Log("STEREO game zoom %.2f (P00=%.4f, reference %.4f)", loggedZoom,
                        matrices[0], base);
                }
            } else {
                confirmedOptics = false;
                g_zoom.store(1.0F);
                if (traceZoom) g_zoomTrace.decision = "world_projection_inactive";
            }
        }
    }
    if (!g_loggedFirst && g_lastDoubled > 0) {
        g_loggedFirst = true;
        std::size_t blits = 0;
        {
            std::scoped_lock lock(g_shaderMutex);
            blits = g_blitShaders.size();
        }
        Log("STEREO %zu blit vertex shaders known", blits);
        Log("STEREO first doubled frame: %u draws (%u scene, %u ui), %u without projection "
            "buffer, %u with scissor; back buffer %ux%u, projection x=%.4f, separation %.3f m, "
            "convergence %.2f m",
            g_lastDoubled, g_lastScene, g_lastUi, g_lastUnpatched, g_lastScissor,
            g_backWidth.load(), g_backHeight.load(), g_projectionX.load(),
            GetConfig().engineStereoSeparationMeters * g_separationScale.load(),
            GetConfig().engineStereoConvergenceMeters * g_convergenceFactor.load());
    }
    const int every = GetConfig().statsEveryPresents;
    if (every > 0 && g_presents % static_cast<std::uint64_t>(every) == 0) {
        if (GetConfig().worldVrs || g_worldVrsAppliedDraws || g_worldVrsFullRateDraws) {
            Log("World VRS over %d presents: %u eligible material draws shaded by rings, "
                "%u color draws protected at full rate",
                every, g_worldVrsAppliedDraws, g_worldVrsFullRateDraws);
            if (g_worldVrsFullRateDraws || g_worldVrsBindFailed) {
                Log("World VRS rejection: multisampled or other target %u, shader %u, "
                    "blend or alpha-to-coverage %u, rate binding failed %u",
                    g_worldVrsTargetRejected, g_worldVrsShaderRejected,
                    g_worldVrsBlendRejected, g_worldVrsBindFailed);
            }
            g_worldVrsAppliedDraws = g_worldVrsFullRateDraws = 0;
            g_worldVrsTargetRejected = g_worldVrsShaderRejected = 0;
            g_worldVrsBlendRejected = g_worldVrsBindFailed = 0;
        }
        if (!g_desktopHudMirror.load()) {
            Log("SteamVR desktop HUD: %llu duplicate draws skipped over %d presents",
                static_cast<unsigned long long>(g_skippedDesktopHudDraws), every);
        }
        g_skippedDesktopHudDraws = 0;
        if (GetConfig().cacheStereoProjection) {
            const auto stats = EngineProjectionCache().TakeStats();
            Log("STEREO projection cache: %u reused, %u built over %d presents", stats.reused, stats.built, every);
        }
        Log("STEREO scene target %ux%u, back buffer %ux%u, HUD %d scale=%.2f, weapon draws %u "
            "(scale %.2f, distance %.2f m)",
            g_sceneWidth.load(), g_sceneHeight.load(), g_backWidth.load(), g_backHeight.load(),
            g_hudActive.load() ? 1 : 0, g_hudScale.load(), g_lastWeapon, g_weaponScale.load(),
            g_weaponConvergence.load());
        Log("STEREO draws doubled last frame: %u (scene %u, ui %u, no projection buffer %u, "
            "scissor %u, composite draws left single %u), projection x=%.4f, on=%d flat=%d "
            "scale=%.2f factor=%.2f",
            g_lastDoubled, g_lastScene, g_lastUi, g_lastUnpatched, g_lastScissor, g_lastComposite,
            g_projectionX.load(), g_tuneEnabled.load() ? 1 : 0, g_flat.load() ? 1 : 0,
            g_separationScale.load(), g_convergenceFactor.load());
    }
    const int dumpSeconds = GetConfig().frameDumpSeconds;
    // World view only, a ring of 30 files (the newest overwrites the oldest).
    if (dumpSeconds > 0 && g_hudActive.load()) {
        if (g_firstPresentTick == 0) {
            g_firstPresentTick = GetTickCount64();
        }
        if (GetTickCount64() - g_firstPresentTick >= static_cast<ULONGLONG>(dumpSeconds) * 1000ULL) {
            g_firstPresentTick = GetTickCount64();
            ++g_dumps;
            const std::filesystem::path path =
                ModuleDirectory() /
                (L"arma3_native_vr.dump" + std::to_wstring((g_dumps - 1) % 30 + 1) + L".jpg");
            const bool ok = DumpBackBufferJpeg(swapChain, path.c_str());
            Log("STEREO back buffer dump %d %s (%ls); head look yaw=%.1f pitch=%.1f deg (positive "
                "left/up), lean right/up/forward = %.3f %.3f %.3f m",
                g_dumps, ok ? "written" : "FAILED", path.c_str(),
                std::asin(std::clamp(g_weaponForward[0].load(), -1.0F, 1.0F)) * 57.29578F,
                std::asin(std::clamp(-g_weaponForward[1].load(), -1.0F, 1.0F)) * 57.29578F,
                g_lean[0].load(), g_lean[1].load(), g_lean[2].load());
        }
    }
}

bool DumpBackBufferJpeg(IDXGISwapChain* swapChain, const wchar_t* path)
{
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || !backBuffer) {
        return false;
    }
    return DumpTextureJpeg(backBuffer.Get(), path);
}

bool DumpTextureJpeg(ID3D11Texture2D* texture, const wchar_t* path, const D3D11_BOX* region)
{
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.SampleDesc.Count != 1 || description.ArraySize != 1 ||
        description.MipLevels != 1) {
        return false;
    }
    if (region) {
        if (region->right > description.Width || region->bottom > description.Height ||
            region->left >= region->right || region->top >= region->bottom) {
            return false;
        }
        description.Width = region->right - region->left;
        description.Height = region->bottom - region->top;
    }
    if (description.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS &&
        description.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        description.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        description.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        return false;
    }
    ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC stagingDescription = description;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0;
    stagingDescription.SampleDesc = {1, 0};
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDescription, nullptr, &staging))) {
        return false;
    }
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, 0, region);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(description.Width) *
                                     description.Height * 4);
    for (UINT row = 0; row < description.Height; ++row) {
        std::memcpy(pixels.data() + static_cast<std::size_t>(row) * description.Width * 4,
                    static_cast<const std::uint8_t*>(mapped.pData) +
                        static_cast<std::size_t>(row) * mapped.RowPitch,
                    static_cast<std::size_t>(description.Width) * 4);
    }
    context->Unmap(staging.Get(), 0);

    const bool bgra = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // already initialised is fine
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        return false;
    }
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromMemory(
            description.Width, description.Height,
            bgra ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat32bppRGBA,
            description.Width * 4, static_cast<UINT>(pixels.size()), pixels.data(), &bitmap))) {
        return false;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(bitmap.Get(), GUID_WICPixelFormat24bppBGR,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, &properties))) {
        return false;
    }
    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT quality;
    VariantInit(&quality);
    quality.vt = VT_R4;
    quality.fltVal = 0.85F;
    properties->Write(1, &option, &quality);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    const bool ok = SUCCEEDED(frame->Initialize(properties.Get())) &&
                    SUCCEEDED(frame->SetSize(description.Width, description.Height)) &&
                    SUCCEEDED(frame->SetPixelFormat(&format)) &&
                    SUCCEEDED(frame->WriteSource(converter.Get(), nullptr)) &&
                    SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    return ok;
}

}  // namespace a3vr
