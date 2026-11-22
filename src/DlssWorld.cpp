#include "DlssWorld.h"
#include "Log.h"
#include "SceneDepthFormat.h"

#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace a3vr {
namespace {

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct alignas(16) Constants {
    XMFLOAT4X4 currentToPrevious;
    float current[8], previous[8];
    float depth[4];  // world A/B in hardware depth, first-person viewport bounds
    float jitterNdc[4];
    UINT extents[4]; // eye input W/H and output W/H
    UINT eyeAndReset[4];
};

constexpr char kWorldShader[] = R"(
cbuffer State : register(b0) {
    row_major float4x4 currentToPrevious;
    float4 currentLens, currentTranslation, previousLens, previousTranslation;
    float4 depthInfo;
    float4 jitterNdc;
    uint4 extents;
    uint4 eyeAndReset;
};
Texture2D<float> packedDepth : register(t0);
RWTexture2D<float> eyeDepth : register(u0);
RWTexture2D<float2> eyeMotion : register(u1);

bool IsNear(float depth) {
    return depthInfo.w > depthInfo.z && depth >= depthInfo.z && depth <= depthInfo.w;
}

[numthreads(8, 8, 1)]
void Motion(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= extents.xy)) return;
    float rawDepth = packedDepth.Load(int3(id.xy + uint2(eyeAndReset.x * extents.x, 0), 0));
    float2 mv = 0;
    if (!eyeAndReset.y && !IsNear(rawDepth) && rawDepth < 0.999999) {
        float2 ndc = (float2(id.xy) + 0.5) / extents.xy * float2(2, -2) + float2(-1, 1);
        float eyeZ = depthInfo.y / min(-1e-7, rawDepth - depthInfo.x);
        float viewZ = eyeZ - currentTranslation.w;
        float2 xy = (ndc * eyeZ - currentLens.zw * viewZ - currentTranslation.xy) / currentLens.xy;
        float4 prev = mul(float4(xy, viewZ, 1), currentToPrevious);
        float prevEyeZ = prev.z + previousTranslation.w;
        if (eyeZ > 0 && prevEyeZ > 0.01) {
            float2 oldNdc = (previousLens.xy * prev.xy + previousLens.zw * prev.z +
                             previousTranslation.xy) / prevEyeZ;
            mv = (oldNdc - (ndc - jitterNdc.xy)) * extents.xy * float2(0.5, -0.5);
        }
    }
    eyeDepth[id.xy] = rawDepth;
    eyeMotion[id.xy] = clamp(mv, -4096.0, 4096.0);
}

Texture2D<float4> reconstructed : register(t1);
Texture2D<float4> nativeColor : register(t2);
Texture2D<float> currentDepth : register(t3);
SamplerState linearSampler : register(s0);
RWTexture2D<float4> packedOutput : register(u0);

[numthreads(8, 8, 1)]
void Pack(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= extents.zw)) return;
    float2 uv = (float2(id.xy) + 0.5) / extents.zw;
    int2 source = int2(uv * extents.xy);
    bool nearPixel = false;
    // Use the current, genuinely rendered viewmodel at silhouettes as well.
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            int2 coord = clamp(source + int2(x,y), int2(0,0), int2(extents.xy) - 1);
            nearPixel = nearPixel || IsNear(currentDepth.Load(int3(coord, 0)));
        }
    }
    float2 nativeUv = uv + jitterNdc.xy * float2(0.5, -0.5);
    float4 pixel = nearPixel ? nativeColor.SampleLevel(linearSampler, nativeUv, 0)
                             : reconstructed.Load(int3(id.xy, 0));
    packedOutput[id.xy + uint2(eyeAndReset.x * extents.z, 0)] = pixel;
}
)";

bool Shader(ID3D11Device* device, const char* entry, ID3D11ComputeShader** shader)
{
    ComPtr<ID3DBlob> bytecode, errors;
    if (FAILED(D3DCompile(kWorldShader, sizeof(kWorldShader) - 1, nullptr, nullptr, nullptr,
                          entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &errors))) {
        if (errors) {
            Log("DLSS Balanced: world shader %s failed: %s", entry,
                static_cast<const char*>(errors->GetBufferPointer()));
        }
        return false;
    }
    return SUCCEEDED(device->CreateComputeShader(bytecode->GetBufferPointer(),
                                                  bytecode->GetBufferSize(), nullptr, shader));
}

bool Texture2D(ID3D11Device* device, DlssBalanced::Size dimensions, DXGI_FORMAT format,
               ID3D11Texture2D** texture, ID3D11ShaderResourceView** view,
               ID3D11UnorderedAccessView** uav = nullptr, DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = dimensions.width;
    desc.Height = dimensions.height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = ((view || uav) ? D3D11_BIND_SHADER_RESOURCE : 0U) |
                     (uav ? D3D11_BIND_UNORDERED_ACCESS : 0U);
    if (FAILED(device->CreateTexture2D(&desc, nullptr, texture))) {
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = viewFormat == DXGI_FORMAT_UNKNOWN ? format : viewFormat;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    return (!view || SUCCEEDED(device->CreateShaderResourceView(*texture, &srv, view))) &&
           (!uav || SUCCEEDED(device->CreateUnorderedAccessView(*texture, nullptr, uav)));
}

class ContextScope {
public:
    ContextScope(ID3D11DeviceContext1* context, ID3DDeviceContextState* isolated) : m_context(context)
    {
        m_context->SwapDeviceContextState(isolated, &m_previous);
    }
    ~ContextScope() { m_context->SwapDeviceContextState(m_previous.Get(), nullptr); }
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;

private:
    ID3D11DeviceContext1* m_context;
    ComPtr<ID3DDeviceContextState> m_previous;
};

}  // namespace

bool DlssWorld::EnsureShaders(ID3D11Device* device)
{
    if (m_attemptedShaders) {
        return m_isolated != nullptr;
    }
    m_attemptedShaders = true;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(Constants);
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11Device1> device1;
    const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    return SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device1))) &&
           Shader(device, "Motion", &m_buildMotion) && Shader(device, "Pack", &m_packEyes) &&
           SUCCEEDED(device->CreateBuffer(&desc, nullptr, &m_constants)) &&
           SUCCEEDED(device->CreateSamplerState(&sampler, &m_linear)) &&
           SUCCEEDED(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
                                                       __uuidof(ID3D11Device), nullptr, &m_isolated));
}

bool DlssWorld::EnsureTextures(ID3D11Device* device, DlssBalanced::Size input, DlssBalanced::Size target,
                                DXGI_FORMAT depthFormat)
{
    if (m_input == input && m_target == target && m_depthFormat == depthFormat) {
        return true;
    }
    m_input = m_target = {};
    m_lastFrame = 0;
    constexpr auto rgba = DXGI_FORMAT_R8G8B8A8_UNORM;
    for (std::size_t eye = 0; eye < 2; ++eye) {
        ComPtr<ID3D11UnorderedAccessView> motionUav, outputUav;
        if (!Texture2D(device, input, rgba, m_color[eye].ReleaseAndGetAddressOf(),
                       m_colorView[eye].ReleaseAndGetAddressOf()) ||
            !Texture2D(device, input, DXGI_FORMAT_R32_FLOAT, m_depth[eye].ReleaseAndGetAddressOf(),
                       m_depthView[eye].ReleaseAndGetAddressOf(), m_depthUav[eye].ReleaseAndGetAddressOf()) ||
            !Texture2D(device, input, DXGI_FORMAT_R16G16_FLOAT, m_motion[eye].ReleaseAndGetAddressOf(),
                       nullptr, &motionUav) ||
            !Texture2D(device, target, rgba, m_output[eye].ReleaseAndGetAddressOf(),
                       m_outputView[eye].ReleaseAndGetAddressOf(), &outputUav)) {
            return false;
        }
        m_motionUav[eye] = std::move(motionUav);
    }
    if (!Texture2D(device, {2 * input.width, input.height}, depthFormat,
                   m_depthCopy.ReleaseAndGetAddressOf(), m_depthCopyView.ReleaseAndGetAddressOf(),
                   nullptr, DepthViewFormat(depthFormat)) ||
        !Texture2D(device, {2 * target.width, target.height}, rgba, m_packed.ReleaseAndGetAddressOf(),
                   nullptr, m_packedUav.ReleaseAndGetAddressOf())) {
        return false;
    }
    m_input = input;
    m_target = target;
    m_depthFormat = depthFormat;
    return true;
}

ID3D11Texture2D* DlssWorld::Render(ID3D11Device* device, ID3D11DeviceContext* context,
                                  ID3D11Texture2D* color, ID3D11Texture2D* depth,
                                  const Camera& camera, DlssBalanced::Size target, std::uint64_t frame)
{
    if (!device || !context || !color || !depth || !target.width || !target.height ||
        2 * target.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || camera.depthBias >= -0.001F ||
        camera.worldMax <= camera.worldMin || !std::isfinite(camera.depthBias)) {
        static bool reported = false;
        if (!reported) {
            Log("DLSS Balanced bypass: missing resource or invalid depth (range=%.6f..%.6f A=%.6f B=%.6f)",
                camera.worldMin, camera.worldMax, camera.depthScale, camera.depthBias);
            reported = true;
        }
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC colorDesc{}, depthDesc{};
    color->GetDesc(&colorDesc);
    depth->GetDesc(&depthDesc);
    const auto depthCopyFormat = DepthCopyFormat(depthDesc.Format);
    const bool compatibleColor = colorDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        colorDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || colorDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if (colorDesc.Width % 2 || colorDesc.Width != depthDesc.Width || colorDesc.Height != depthDesc.Height ||
        colorDesc.SampleDesc.Count != 1 || depthDesc.SampleDesc.Count != 1 ||
        colorDesc.ArraySize != 1 || depthDesc.ArraySize != 1 || depthDesc.MipLevels != 1 ||
        !compatibleColor || depthCopyFormat == DXGI_FORMAT_UNKNOWN) {
        static bool reported = false;
        if (!reported) {
            Log("DLSS Balanced bypass: color %ux%u fmt=%u samples=%u array=%u; depth %ux%u fmt=%u samples=%u array=%u mips=%u",
                colorDesc.Width, colorDesc.Height, static_cast<unsigned>(colorDesc.Format),
                colorDesc.SampleDesc.Count, colorDesc.ArraySize, depthDesc.Width, depthDesc.Height,
                static_cast<unsigned>(depthDesc.Format), depthDesc.SampleDesc.Count,
                depthDesc.ArraySize, depthDesc.MipLevels);
            reported = true;
        }
        return nullptr;
    }
    const auto validFloat = [](float f) { return std::isfinite(f); };
    if (!std::all_of(camera.view.begin(), camera.view.end(), validFloat) ||
        std::fabs(camera.view[15] - 1.0F) > 0.01F ||
        !std::all_of(camera.eye[0].begin(), camera.eye[0].end(), validFloat) ||
        !std::all_of(camera.eye[1].begin(), camera.eye[1].end(), validFloat) ||
        camera.eye[0][0] < 0.1F || camera.eye[0][1] < 0.1F ||
        camera.eye[1][0] < 0.1F || camera.eye[1][1] < 0.1F) {
        static bool reported = false;
        if (!reported) {
            Log("DLSS Balanced bypass: invalid camera (view.w=%.6f, left=%.6f/%.6f, right=%.6f/%.6f)",
                camera.view[15], camera.eye[0][0], camera.eye[0][1], camera.eye[1][0], camera.eye[1][1]);
            reported = true;
        }
        return nullptr;
    }
    const DlssBalanced::Size input{colorDesc.Width / 2, colorDesc.Height};
    ComPtr<ID3D11DeviceContext1> context1;
    const HRESULT contextResult = context->QueryInterface(IID_PPV_ARGS(&context1));
    if (FAILED(contextResult) || !EnsureShaders(device)) {
        static bool reported = false;
        if (!reported) {
            Log("DLSS Balanced bypass: cannot isolate D3D11 context (0x%08X, state=%d)",
                static_cast<unsigned>(contextResult), m_isolated ? 1 : 0);
            reported = true;
        }
        return nullptr;
    }
    ContextScope isolated(context1.Get(), m_isolated.Get());
    if (!m_balanced.Initialize(device, ModuleDirectory()) || !m_balanced.Prepare(context, input, target)) {
        return nullptr;
    }
    if (!EnsureTextures(device, input, target, depthCopyFormat)) {
        static bool reported = false;
        if (!reported) {
            Log("DLSS Balanced bypass: working texture creation failed (%ux%u each, depth format %u)",
                input.width, input.height, static_cast<unsigned>(depthCopyFormat));
            reported = true;
        }
        return nullptr;
    }

    XMFLOAT4X4 view{}, previousView{};
    std::memcpy(&view, camera.view.data(), sizeof(view));
    std::memcpy(&previousView, m_previous.view.data(), sizeof(previousView));
    const XMMATRIX current = XMLoadFloat4x4(&view);
    XMVECTOR determinant{};
    const XMMATRIX inverse = XMMatrixInverse(&determinant, current);
    if (!std::isfinite(XMVectorGetX(determinant)) || std::fabs(XMVectorGetX(determinant)) < 0.01F) {
        return nullptr;
    }
    const XMMATRIX relative = inverse * XMLoadFloat4x4(&previousView);
    const bool reset = m_lastFrame == 0 || m_lastFrame + 1 != frame || m_previous.synthesized != camera.synthesized ||
        XMVectorGetX(XMVector3Length(relative.r[3])) > 2.0F || XMVectorGetZ(relative.r[2]) < 0.5F;
    Constants params{};
    XMStoreFloat4x4(&params.currentToPrevious, reset ? XMMatrixIdentity() : relative);
    const float range = camera.worldMax - camera.worldMin;
    params.depth[0] = camera.worldMin + range * camera.depthScale;
    params.depth[1] = range * camera.depthBias;
    params.depth[2] = camera.weaponMin;
    params.depth[3] = camera.weaponMax;
    params.jitterNdc[0] = 2 * camera.jitterX / static_cast<float>(input.width);
    params.jitterNdc[1] = -2 * camera.jitterY / static_cast<float>(input.height);
    params.extents[0] = input.width;
    params.extents[1] = input.height;
    params.extents[2] = target.width;
    params.extents[3] = target.height;
    params.eyeAndReset[1] = reset ? 1U : 0U;
    context->CSSetConstantBuffers(0, 1, m_constants.GetAddressOf());
    context->CopyResource(m_depthCopy.Get(), depth);
    std::array<DlssBalanced::EyeFrame, 2> eyes{};
    for (UINT eye = 0; eye < 2; ++eye) {
        const D3D11_BOX rect{eye * input.width, 0, 0, (eye + 1) * input.width, input.height, 1};
        context->CopySubresourceRegion(m_color[eye].Get(), 0, 0, 0, 0, color, 0, &rect);
        std::memcpy(params.current, camera.eye[eye].data(), sizeof(params.current));
        std::memcpy(params.previous, m_previous.eye[eye].data(), sizeof(params.previous));
        // Evaluate motion in unjittered image coordinates while reconstructing
        // the sampled current depth with the actual, jittered projection.
        const float prevX = 2 * m_previous.jitterX / static_cast<float>(input.width);
        const float prevY = -2 * m_previous.jitterY / static_cast<float>(input.height);
        params.previous[2] -= prevX;
        params.previous[3] -= prevY;
        params.previous[4] -= prevX * params.previous[7];
        params.previous[5] -= prevY * params.previous[7];
        params.eyeAndReset[0] = eye;
        context->UpdateSubresource(m_constants.Get(), 0, nullptr, &params, 0, 0);
        context->CSSetShader(m_buildMotion.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 1, m_depthCopyView.GetAddressOf());
        ID3D11UnorderedAccessView* uavs[]{m_depthUav[eye].Get(), m_motionUav[eye].Get()};
        context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        context->Dispatch((input.width + 7) / 8, (input.height + 7) / 8, 1);
        ID3D11UnorderedAccessView* none[2]{};
        context->CSSetUnorderedAccessViews(0, 2, none, nullptr);
        eyes[eye] = {m_color[eye].Get(), m_depth[eye].Get(), m_motion[eye].Get(), m_output[eye].Get()};
    }
    if (!m_balanced.EvaluatePair(context, eyes, camera.jitterX, camera.jitterY, reset)) {
        return nullptr;
    }

    context->CSSetShader(m_packEyes.Get(), nullptr, 0);
    context->CSSetSamplers(0, 1, m_linear.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, 1, m_packedUav.GetAddressOf(), nullptr);
    for (UINT eye = 0; eye < 2; ++eye) {
        params.eyeAndReset[0] = eye;
        context->UpdateSubresource(m_constants.Get(), 0, nullptr, &params, 0, 0);
        ID3D11ShaderResourceView* views[]{m_outputView[eye].Get(), m_colorView[eye].Get(), m_depthView[eye].Get()};
        context->CSSetShaderResources(1, 3, views);
        context->Dispatch((target.width + 7) / 8, (target.height + 7) / 8, 1);
    }
    if (!m_reported) {
        Log("DLSS Balanced active: stereo output %ux%u, world camera motion and current-frame weapon",
            target.width * 2, target.height);
        m_reported = true;
    }
    m_previous = camera;
    m_lastFrame = frame;
    return m_packed.Get();
}

}  // namespace a3vr

