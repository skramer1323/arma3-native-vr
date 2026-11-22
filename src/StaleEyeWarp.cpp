#include "StaleEyeWarp.h"

#include "Log.h"
#include "SceneDepthFormat.h"

#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace a3vr {
namespace {
using namespace DirectX;
using Microsoft::WRL::ComPtr;

// Same-eye depth reprojection, in three passes as in SnowRunner VR's reproject.cpp.
// A D3D11 atomic orders depth first, and another resolves ties by source pixel.
constexpr char kShader[] = R"(
cbuffer Parameters : register(b0) {
    row_major float4x4 oldClipToNew;
    row_major float4x4 newViewToOld;
    float4 oldFrustum;
    float4 newFrustum;
    float4 freshFrustum;
    float4 worldRange;
    float4 nearRange;
    uint4 extent;  // eye width, height, stale eye, fresh eye
};
Texture2D<float4> OldColor : register(t0);
Texture2D<float> OldDepth : register(t1);
Texture2D<float4> FreshColor : register(t2);
Texture2D<float> CurrentDepth : register(t3);
Texture2D<float4> CurrentNear : register(t4);
RWTexture2D<uint> Key : register(u0);
RWTexture2D<uint> Index : register(u1);
RWTexture2D<float4> Color : register(u2);
RWTexture2D<float> Depth : register(u3);

float2 ToNdc(uint2 p) { return (float2(p) + 0.5) / extent.xy * float2(2, -2) + float2(-1, 1); }
float2 ToUv(float2 ndc) { return ndc * float2(0.5, -0.5) + 0.5; }
uint2 Packed(uint2 p, uint eye) { return p + uint2(extent.x * eye, 0); }
float ReadOld(uint2 p) { return OldDepth.Load(int3(Packed(p, extent.z), 0)); }
float ReadCurrent(uint2 p, uint eye) { return CurrentDepth.Load(int3(Packed(p, eye), 0)); }
bool IsNear(float v, float2 r) { return r.y > r.x && v >= r.x - 1e-6 && v <= r.y + 1e-6; }

bool Project(uint2 src, out uint2 dest, out uint key) {
    dest = 0;
    key = 0;
    const float raw = ReadOld(src);
    if (IsNear(raw, nearRange.xy) || raw < worldRange.x || raw >= worldRange.y - 1e-7) return false;
    const float z = (raw - worldRange.x) / (worldRange.y - worldRange.x);
    const float4 clip = mul(float4(ToNdc(src), z, 1), oldClipToNew);
    if (!(clip.w > 1e-6)) return false;
    const float3 ndc = clip.xyz / clip.w;
    const float2 uv = ToUv(ndc.xy);
    if (any(uv < 0) || any(uv >= 1) || !(ndc.z >= 0 && ndc.z <= 1)) return false;
    dest = uint2(uv * extent.xy);
    key = asuint(max(0.0, ndc.z));
    return true;
}

[numthreads(8, 8, 1)]
void ScatterDepth(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= extent.xy)) return;
    uint2 dst;
    uint key;
    if (Project(id.xy, dst, key)) InterlockedMin(Key[dst], key);
}

[numthreads(8, 8, 1)]
void ScatterIndex(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= extent.xy)) return;
    uint2 dst;
    uint key;
    if (Project(id.xy, dst, key) && Key[dst] == key) {
        InterlockedMin(Index[dst], id.y * extent.x + id.x);
    }
}

bool Locate(float3 ray, float4 projection, out uint2 pixel) {
    const float2 uv = ToUv(ray.xy / max(ray.z, 1e-6) * projection.xy + projection.zw);
    pixel = min(uint2(saturate(uv) * extent.xy), extent.xy - 1);
    return ray.z > 0 && all(uv >= 0) && all(uv < 1);
}

float4 FillHole(uint2 pixel) {
    const float3 ray = float3((ToNdc(pixel) - newFrustum.zw) / newFrustum.xy, 1);
    const float3 oldRay = mul(float4(ray, 0), newViewToOld).xyz;
    uint2 src;
    if (Locate(oldRay, oldFrustum, src) && !IsNear(ReadOld(src), nearRange.xy)) {
        return OldColor.Load(int3(src, 0));
    }

    // The edge exposed by a head turn can exist in the current other eye.
    // Its angular map fills only holes. Keep the current eye's weapon out of it.
    if (Locate(ray, freshFrustum, src) && !IsNear(ReadCurrent(src, extent.w), nearRange.zw)) {
        return FreshColor.Load(int3(src, 0));
    }

    // Never resurrect last frame's weapon along its old silhouette.
    [unroll] for (int offset = 1; offset <= 3; ++offset) {
        const uint left = Index[uint2(max(0, int(pixel.x) - offset), pixel.y)];
        const uint right = Index[uint2(min(extent.x - 1, pixel.x + offset), pixel.y)];
        const uint neighbor = min(left, right);
        if (neighbor != 0xffffffffu) {
            return OldColor.Load(int3(neighbor % extent.x, neighbor / extent.x, 0));
        }
    }
    return float4(0, 0, 0, 1);
}

[numthreads(8, 8, 1)]
void Resolve(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= extent.xy)) return;
    const uint2 dest = Packed(id.xy, extent.z);
    const float nativeNear = ReadCurrent(id.xy, extent.z);
    Depth[Packed(id.xy, extent.w)] = ReadCurrent(id.xy, extent.w);
    if (IsNear(nativeNear, nearRange.zw)) {
        Color[dest] = CurrentNear.Load(int3(id.xy, 0));
        Depth[dest] = nativeNear;
        return;
    }
    const uint source = Index[id.xy];
    if (source != 0xffffffffu) {
        Color[dest] = OldColor.Load(int3(source % extent.x, source / extent.x, 0));
        Depth[dest] = lerp(worldRange.z, worldRange.w, asfloat(Key[id.xy]));
        return;
    }
    Color[dest] = FillHole(id.xy);
    Depth[dest] = 1.0;
}
)";

struct Parameters {
    XMFLOAT4X4 oldClipToNew, newViewToOld;
    XMFLOAT4 oldFrustum, newFrustum, freshFrustum, worldRange, nearRange;
    UINT extent[4];
};
static_assert(sizeof(Parameters) == 224);

XMMATRIX Projection(const DlssWorld::Camera& camera, UINT eye)
{
    const auto& p = camera.eye[eye];
    return XMMatrixSet(p[0], 0, 0, 0, 0, p[1], 0, 0,
                       p[2], p[3], camera.depthScale, 1, p[4], p[5], p[6], p[7]);
}

bool ValidCamera(const DlssWorld::Camera& camera)
{
    const auto finite = [](float f) { return std::isfinite(f); };
    return camera.worldMax > camera.worldMin && std::isfinite(camera.depthScale) &&
        camera.depthBias < -0.001F && std::fabs(camera.view[15] - 1.0F) <= 0.01F &&
        std::all_of(camera.view.begin(), camera.view.end(), finite) &&
        std::all_of(camera.eye.begin(), camera.eye.end(), [&](const auto& p) {
            return p[0] > 0.1F && p[1] > 0.1F && std::all_of(p.begin(), p.end(), finite);
        });
}

XMMATRIX View(const DlssWorld::Camera& camera)
{
    XMFLOAT4X4 view;
    std::memcpy(&view, camera.view.data(), sizeof(view));
    return XMLoadFloat4x4(&view);
}

bool MapViews(const DlssWorld::Camera& old, const DlssWorld::Camera& now, UINT stale, Parameters& out)
{
    const XMMATRIX oldView = View(old), newView = View(now);
    XMVECTOR oldDet{}, newDet{}, projectionDet{};
    const auto oldToNewView = XMMatrixInverse(&oldDet, oldView) * newView;
    const auto newToOldView = XMMatrixInverse(&newDet, newView) * oldView;
    const auto oldClipToView = XMMatrixInverse(&projectionDet, Projection(old, stale));
    const auto validDet = [](XMVECTOR det) {
        const float d = XMVectorGetX(det);
        return std::isfinite(d) && std::fabs(d) > 1e-6F;
    };
    if (!validDet(oldDet) || !validDet(newDet) || !validDet(projectionDet) ||
        XMVectorGetX(XMVector3Length(oldToNewView.r[3])) > 2.0F || XMVectorGetZ(oldToNewView.r[2]) < 0.75F) {
        return false;
    }
    XMStoreFloat4x4(&out.oldClipToNew, oldClipToView * oldToNewView * Projection(now, stale));
    XMStoreFloat4x4(&out.newViewToOld, newToOldView);
    std::memcpy(&out.oldFrustum, old.eye[stale].data(), sizeof(XMFLOAT4));
    std::memcpy(&out.newFrustum, now.eye[stale].data(), sizeof(XMFLOAT4));
    std::memcpy(&out.freshFrustum, now.eye[1 - stale].data(), sizeof(XMFLOAT4));
    out.worldRange = {old.worldMin, old.worldMax, now.worldMin, now.worldMax};
    out.nearRange = {old.weaponMin, old.weaponMax, now.weaponMin, now.weaponMax};
    return true;
}

template <typename T>
bool MakeTexture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format,
                 ComPtr<ID3D11Texture2D>& texture, ComPtr<T>& view, DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN)
{
    constexpr bool srv = std::is_same_v<T, ID3D11ShaderResourceView>;
    D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT,
                             srv ? D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_UNORDERED_ACCESS, 0, 0};
    if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.ReleaseAndGetAddressOf()))) return false;
    if constexpr (srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = viewFormat == DXGI_FORMAT_UNKNOWN ? format : viewFormat;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        return SUCCEEDED(device->CreateShaderResourceView(texture.Get(), &sd, view.ReleaseAndGetAddressOf()));
    } else {
        return SUCCEEDED(device->CreateUnorderedAccessView(texture.Get(), nullptr, view.ReleaseAndGetAddressOf()));
    }
}

class ScopedComputeState {
public:
    ScopedComputeState(ID3D11DeviceContext1* context, ID3DDeviceContextState* isolated) : m_context(context)
    {
        context->SwapDeviceContextState(isolated, m_saved.GetAddressOf());
    }
    ~ScopedComputeState() { m_context->SwapDeviceContextState(m_saved.Get(), nullptr); }
private:
    ID3D11DeviceContext1* m_context;
    ComPtr<ID3DDeviceContextState> m_saved;
};

}  // namespace

bool StaleEyeWarp::EnsureDevice(ID3D11Device* device)
{
    if (device != m_device.Get()) {
        *this = {};
        m_device = device;
    }
    if (m_attempted) return m_constants && m_isolated;
    m_attempted = true;
    const char* entries[]{"ScatterDepth", "ScatterIndex", "Resolve"};
    for (std::size_t i = 0; i < m_shaders.size(); ++i) {
        ComPtr<ID3DBlob> code, errors;
        if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "Arma stale-eye warp", nullptr, nullptr,
                             entries[i], "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)) ||
            FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m_shaders[i]))) {
            Log("AER 6DoF could not create %s: %s", entries[i],
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "D3D11 shader creation failed");
            return false;
        }
    }
    const D3D11_BUFFER_DESC constants{sizeof(Parameters), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    ComPtr<ID3D11Device1> device1;
    const auto level = device->GetFeatureLevel();
    const bool ready = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device1))) &&
        SUCCEEDED(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                                   nullptr, m_isolated.GetAddressOf())) &&
        SUCCEEDED(device->CreateBuffer(&constants, nullptr, m_constants.GetAddressOf()));
    if (!ready) {
        Log("AER 6DoF could not create the isolated D3D11 compute state");
    }
    return ready;
}

bool StaleEyeWarp::EnsureImages(ID3D11Device* device, ID3D11Texture2D* color, ID3D11Texture2D* depth)
{
    if (!color || !depth) return false;
    D3D11_TEXTURE2D_DESC c{}, d{};
    color->GetDesc(&c);
    depth->GetDesc(&d);
    const auto copyFormat = DepthCopyFormat(d.Format);
    if (c.Width <= 1 || c.Width % 2 || c.Width != d.Width || c.Height != d.Height ||
        c.SampleDesc.Count != 1 || d.SampleDesc.Count != 1 || c.ArraySize != 1 || d.ArraySize != 1 ||
        c.MipLevels != 1 || d.MipLevels != 1 || copyFormat == DXGI_FORMAT_UNKNOWN ||
        (c.Format != DXGI_FORMAT_R8G8B8A8_UNORM && c.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         c.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS)) {
        if (m_inputRejects++ < 4) {
            Log("AER 6DoF incompatible inputs: color %ux%u fmt=%u msaa=%u mips=%u array=%u; "
                "depth %ux%u fmt=%u msaa=%u mips=%u array=%u",
                c.Width, c.Height, static_cast<unsigned>(c.Format), c.SampleDesc.Count, c.MipLevels, c.ArraySize,
                d.Width, d.Height, static_cast<unsigned>(d.Format), d.SampleDesc.Count, d.MipLevels, d.ArraySize);
        }
        return false;
    }
    if (!EnsureDevice(device)) return false;

    const UINT width = c.Width / 2, height = c.Height;
    if (m_width == width && m_height == height && m_depthFormat == copyFormat) return true;
    m_width = m_height = 0;
    ResetHistory();
    for (UINT eye = 0; eye < 2; ++eye) {
        if (!MakeTexture(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, m_color[eye], m_colorView[eye]) ||
            !MakeTexture(device, c.Width, height, copyFormat, m_depth[eye], m_depthView[eye],
                         DepthViewFormat(copyFormat))) return false;
    }
    if (!MakeTexture(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, m_nearColor, m_nearView) ||
        !MakeTexture(device, width, height, DXGI_FORMAT_R32_UINT, m_depthKey, m_keyUav) ||
        !MakeTexture(device, width, height, DXGI_FORMAT_R32_UINT, m_sourceIndex, m_indexUav) ||
        !MakeTexture(device, c.Width, height, DXGI_FORMAT_R8G8B8A8_UNORM, m_pairColor, m_colorUav) ||
        !MakeTexture(device, c.Width, height, DXGI_FORMAT_R32_FLOAT, m_pairDepth, m_depthUav)) return false;
    m_width = width;
    m_height = height;
    m_depthFormat = copyFormat;
    Log("AER 6DoF input ready: %ux%u color fmt=%u, depth fmt=%u", c.Width, c.Height,
        static_cast<unsigned>(c.Format), static_cast<unsigned>(d.Format));
    return true;
}

void StaleEyeWarp::CopyEye(ID3D11DeviceContext* context, ID3D11Texture2D* dest, ID3D11Texture2D* packed, UINT eye) const
{
    const D3D11_BOX region{eye * m_width, 0, 0, (eye + 1) * m_width, m_height, 1};
    context->CopySubresourceRegion(dest, 0, 0, 0, 0, packed, 0, &region);
}

void StaleEyeWarp::Retain(ID3D11DeviceContext* context, ID3D11Texture2D* color, ID3D11Texture2D* depth,
                         const History& history, UINT eye)
{
    CopyEye(context, m_color[eye].Get(), color, eye);
    context->CopyResource(m_depth[eye].Get(), depth);
    m_history[eye] = history;
}

bool StaleEyeWarp::Seed(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* color,
                       ID3D11Texture2D* depth, const DlssWorld::Camera& camera, std::uint64_t frame, double timestamp)
{
    if (!ValidCamera(camera)) {
        if (m_inputRejects++ < 4) {
            Log("AER 6DoF invalid camera: view.w=%.5f world=%.6f..%.6f bias=%.6f focal=(%.4f %.4f; %.4f %.4f)",
                camera.view[15], camera.worldMin, camera.worldMax, camera.depthBias,
                camera.eye[0][0], camera.eye[0][1], camera.eye[1][0], camera.eye[1][1]);
        }
        return false;
    }
    if (!EnsureImages(device, color, depth)) return false;
    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1)))) return false;
    ScopedComputeState isolated(context1.Get(), m_isolated.Get());
    const History history{camera, frame, timestamp};
    Retain(context, color, depth, history, 0);
    Retain(context, color, depth, history, 1);
    return true;
}

StaleEyeWarp::Pair StaleEyeWarp::Reproject(ID3D11Device* device, ID3D11DeviceContext* context,
                                         ID3D11Texture2D* color, ID3D11Texture2D* depth,
                                         const DlssWorld::Camera& camera, int freshEye,
                                         std::uint64_t frame, double timestamp)
{
    if (freshEye < 0 || freshEye > 1 || !ValidCamera(camera) || !EnsureImages(device, color, depth)) return {};
    const UINT fresh = static_cast<UINT>(freshEye), stale = 1 - fresh;
    const auto& old = m_history[stale];
    if (old.frame == 0 || old.frame + 1 != frame || timestamp - old.timestamp > 100.0) return {};
    Parameters params{};
    ComPtr<ID3D11DeviceContext1> context1;
    if (!MapViews(old.camera, camera, stale, params) || FAILED(context->QueryInterface(IID_PPV_ARGS(&context1)))) return {};
    params.extent[0] = m_width;
    params.extent[1] = m_height;
    params.extent[2] = stale;
    params.extent[3] = fresh;

    ScopedComputeState isolated(context1.Get(), m_isolated.Get());
    Retain(context, color, depth, {camera, frame, timestamp}, fresh);
    CopyEye(context, m_nearColor.Get(), color, stale);
    context->CopyResource(m_pairColor.Get(), color);
    const UINT empty[4]{UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX};
    context->ClearUnorderedAccessViewUint(m_keyUav.Get(), empty);
    context->ClearUnorderedAccessViewUint(m_indexUav.Get(), empty);
    context->UpdateSubresource(m_constants.Get(), 0, nullptr, &params, 0, 0);
    context->CSSetConstantBuffers(0, 1, m_constants.GetAddressOf());
    ID3D11ShaderResourceView* input[]{m_colorView[stale].Get(), m_depthView[stale].Get(),
                                    m_colorView[fresh].Get(), m_depthView[fresh].Get(), m_nearView.Get()};
    ID3D11UnorderedAccessView* output[]{m_keyUav.Get(), m_indexUav.Get(), m_colorUav.Get(), m_depthUav.Get()};
    context->CSSetShaderResources(0, 5, input);
    context->CSSetUnorderedAccessViews(0, 4, output, nullptr);
    for (const auto& shader : m_shaders) {
        context->CSSetShader(shader.Get(), nullptr, 0);
        context->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);
    }
    ID3D11ShaderResourceView* noInputs[5]{};
    ID3D11UnorderedAccessView* noOutputs[4]{};
    context->CSSetShaderResources(0, 5, noInputs);
    context->CSSetUnorderedAccessViews(0, 4, noOutputs, nullptr);
    return {m_pairColor.Get(), m_pairDepth.Get()};
}

}  // namespace a3vr
