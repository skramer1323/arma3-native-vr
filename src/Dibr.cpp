#include "Dibr.h"

#include "Log.h"

#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace a3vr {
namespace {

using Microsoft::WRL::ComPtr;
using namespace DirectX;

XMMATRIX EyeProjection(const DlssWorld::Camera& camera, int eye)
{
    const auto& p = camera.eye[eye];
    return XMMatrixSet(p[0], 0, 0, 0, 0, p[1], 0, 0, p[2], p[3], camera.depthScale,
                       1, p[4], p[5], p[6], p[7]);
}

void SetHistoryTransform(const DlssWorld::Camera& old, const DlssWorld::Camera& now,
                         int eye, Dibr::Settings& settings)
{
    if (old.worldMax - old.worldMin <= 0.001F || now.worldMax - now.worldMin <= 0.001F ||
        std::fabs(old.view[15] - 1.0F) > 0.01F || std::fabs(now.view[15] - 1.0F) > 0.01F) {
        return;
    }
    XMFLOAT4X4 oldValues, nowValues;
    std::memcpy(&oldValues, old.view.data(), sizeof(oldValues));
    std::memcpy(&nowValues, now.view.data(), sizeof(nowValues));
    const XMMATRIX oldView = XMLoadFloat4x4(&oldValues);
    const XMMATRIX nowView = XMLoadFloat4x4(&nowValues);
    XMVECTOR viewDet{}, projectionDet{};
    const XMMATRIX oldToNow = XMMatrixInverse(&viewDet, oldView) * nowView;
    const XMMATRIX oldUnproject = XMMatrixInverse(&projectionDet, EyeProjection(old, eye));
    const auto validDet = [](XMVECTOR value) {
        const float determinant = XMVectorGetX(value);
        return std::isfinite(determinant) && std::fabs(determinant) > 1e-6F;
    };
    if (!validDet(viewDet) || !validDet(projectionDet) ||
        !(XMVectorGetX(XMVector3Length(oldToNow.r[3])) < 2.0F) ||
        !(XMVectorGetZ(oldToNow.r[2]) > 0.75F)) {
        return;
    }
    XMFLOAT4X4 transform;
    XMStoreFloat4x4(&transform, oldUnproject * oldToNow * EyeProjection(now, eye));
    std::memcpy(settings.historyClipToCurrent, &transform, sizeof(transform));
    for (const auto& row : settings.historyClipToCurrent) {
        if (!std::all_of(std::begin(row), std::end(row), [](float f) { return std::isfinite(f); })) {
            return;
        }
    }
    settings.historyDepth[0] = old.worldMin;
    settings.historyDepth[1] = old.worldMax;
    settings.historyDepth[2] = 1.0F / (old.worldMax - old.worldMin);
    settings.historyDepth[3] = 1.0F;
}

constexpr char kDibrShader[] = R"(
cbuffer Parameters : register(b0) {
    float4 sourceProjection;
    float4 targetProjection;
    float4 staleProjection;
    float4 depthTerms;
    float4 separation;
    float4 nearDepth;
    float4 staleRotation[3];
    row_major float4x4 historyClipToCurrent;
    float4 historyDepth;
};

Texture2D<float4> fresh : register(t0);
Texture2D<float4> stale : register(t1);
Texture2D<float> depth : register(t2);
Texture2D<uint> coverage : register(t3);
Texture2D<float4> currentNear : register(t4);
Texture2D<float> staleDepth : register(t5);
Texture2D<uint> historyWinner : register(t6);
RWTexture2D<uint> scatter : register(u0);
RWTexture2D<float4> outputImage : register(u1);
RWTexture2D<uint> historyKey : register(u2);
RWTexture2D<uint> historyIndex : register(u3);

float2 ToNdc(float2 uv) { return uv * float2(2, -2) + float2(-1, 1); }
float2 ToUv(float2 ndc) { return ndc * float2(0.5, -0.5) + 0.5; }

float ReadDepth(Texture2D<float> image, uint2 pixel, bool rightEye) {
    uint width, height;
    image.GetDimensions(width, height);
    const uint eyeWidth = width / 2;
    const float2 uv = (pixel + 0.5) * separation.zw;
    uint2 position = min(uint2(uv * float2(eyeWidth, height)),
                         uint2(eyeWidth - 1, height - 1));
    position.x += rightEye ? eyeWidth : 0;
    return image.Load(int3(position, 0));
}

bool IsNear(float d, float2 bounds) {
    return bounds.y > bounds.x && d >= bounds.x - 1e-6 && d <= bounds.y + 1e-6;
}

bool ProjectHistory(uint2 source, uint2 extent, out uint2 dest, out uint depthKey) {
    dest = 0;
    depthKey = 0;
    const float d = ReadDepth(staleDepth, source, separation.x > 0);
    if (IsNear(d, nearDepth.zw) || d < historyDepth.x || d >= historyDepth.y - 1e-6)
        return false;
    const float z = (d - historyDepth.x) * historyDepth.z;
    const float2 uv = (source + 0.5) * separation.zw;
    const float4 clip = mul(float4(ToNdc(uv), z, 1), historyClipToCurrent);
    if (!(clip.w > 1e-6) || clip.z < 0 || clip.z > clip.w) return false;
    const float2 target = ToUv(clip.xy / clip.w);
    if (any(target < 0) || any(target >= 1)) return false;
    dest = uint2(target * extent);
    if (coverage.Load(int3(dest, 0)) != 0) return false;
    depthKey = asuint(clip.z / clip.w);
    return true;
}

[numthreads(8, 8, 1)]
void ScatterHistoryDepth(uint3 id : SV_DispatchThreadID) {
    uint2 extent;
    stale.GetDimensions(extent.x, extent.y);
    if (any(id.xy >= extent)) return;
    uint2 dest;
    uint depthKey;
    if (ProjectHistory(id.xy, extent, dest, depthKey)) {
        InterlockedMin(historyKey[dest], depthKey);
    }
}

[numthreads(8, 8, 1)]
void ScatterHistoryIndex(uint3 id : SV_DispatchThreadID) {
    uint2 extent;
    stale.GetDimensions(extent.x, extent.y);
    if (any(id.xy >= extent)) return;
    uint2 dest;
    uint depthKey;
    if (ProjectHistory(id.xy, extent, dest, depthKey) && historyKey[dest] == depthKey) {
        InterlockedMin(historyIndex[dest], id.y * extent.x + id.x);
    }
}

// Invert the history projection only at holes. Accept a retained pixel if
// its depth reprojects onto the output pixel. This keeps stale foreground
// from bleeding over newly exposed background.
bool LocateHistory(float2 initialUv, float2 targetUv, uint2 extent, bool rightEye,
                   out uint2 pixel) {
    pixel = 0;
    float2 uv = initialUv;
    [loop] for (int attempt = 0; attempt < 3; ++attempt) {
        if (any(uv < 0) || any(uv >= 1)) return false;
        pixel = uint2(uv * extent);
        const float d = ReadDepth(staleDepth, pixel, rightEye);
        if (IsNear(d, nearDepth.zw) || d < historyDepth.x || d >= historyDepth.y - 1e-6)
            return false;
        const float z = (d - historyDepth.x) * historyDepth.z;
        const float2 center = (pixel + 0.5) * separation.zw;
        const float4 clip = mul(float4(ToNdc(center), z, 1), historyClipToCurrent);
        if (!(clip.w > 1e-6) || clip.z < 0 || clip.z > clip.w) return false;
        const float2 error = targetUv - ToUv(clip.xy / clip.w);
        if (all(abs(error) * extent <= 0.75)) return true;
        uv = center + error;
    }
    return false;
}

float ProjectX(uint x, float d, uint width) {
    const float inverseZ = max(0, (d - depthTerms.x) * depthTerms.y);
    const float sourceX = ((x + 0.5) * separation.z) * 2.0 - 1.0;
    const float targetX = (sourceX - sourceProjection.z) * targetProjection.x /
                         sourceProjection.x + targetProjection.z -
                         targetProjection.x * separation.x * inverseZ;
    return (targetX * 0.5 + 0.5) * width;
}

[numthreads(8, 8, 1)]
void Scatter(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    fresh.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;

    const bool sourceRight = separation.x < 0;
    const float d = ReadDepth(depth, id.xy, sourceRight);
    if (IsNear(d, nearDepth.xy)) return;

    const float destination = ProjectX(id.x, d, width);
    const int first = int(floor(destination));
    int last = first;
    // Scatter across the source footprint on a continuous surface. A depth
    // discontinuity leaves a genuine hole behind an occluder.
    if (id.x + 1 < width) {
        const float nextZ = ReadDepth(depth, id.xy + uint2(1, 0), sourceRight);
        if (!IsNear(nextZ, nearDepth.xy)) {
            const float next = ProjectX(id.x + 1, nextZ, width);
            const float pixelScale = targetProjection.x / sourceProjection.x;
            if (abs(next - destination - pixelScale) <= 1.25) {
                last = clamp(int(floor(next)) - 1, first, first + 2);
            }
        }
    }
    const uint priority = 65535u - uint(saturate(d) * 65534.0);
    const uint packed = (priority << 16u) | id.x;
    [loop] for (int x = max(first, 0); x <= min(last, int(width) - 1); ++x) {
        InterlockedMax(scatter[uint2(x, id.y)], packed);
    }
}

[numthreads(8, 8, 1)]
void Resolve(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    fresh.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;

    const bool targetRight = separation.x > 0;
    // This eye's first-person draw is from the same simulation update as
    // the current world render. Keep its color and silhouette as rendered.
    if (IsNear(ReadDepth(depth, id.xy, targetRight), nearDepth.xy)) {
        outputImage[id.xy] = currentNear.Load(int3(id.xy, 0));
        return;
    }

    const uint winner = coverage.Load(int3(id.xy, 0));
    if (winner != 0) {
        outputImage[id.xy] = fresh.Load(int3(winner & 65535u, id.y, 0));
        return;
    }

    // Fill mode (settings menu): 0 = magenta holes (diagnostic), 1 = the other eye's stale
    // render then nearby background (default), 2 = nearby background only.
    const int fillMode = int(separation.y + 0.5);
    if (fillMode == 0) {
        outputImage[id.xy] = float4(1, 0, 1, 1);
        return;
    }
    if (historyDepth.w > 1.5) {
        const uint index = historyWinner.Load(int3(id.xy, 0));
        if (index != 0xffffffffu) {
            outputImage[id.xy] = stale.Load(int3(index % width, index / width, 0));
            return;
        }
    }
    // Use the other eye's previous real background to fill true disocclusions.
    const float2 uv = (id.xy + 0.5) * separation.zw;
    const float3 ray = float3((ToNdc(uv) - targetProjection.zw) /
                              targetProjection.xy, 1);
    const float3 oldRay = float3(dot(staleRotation[0].xyz, ray),
                                 dot(staleRotation[1].xyz, ray),
                                 dot(staleRotation[2].xyz, ray));
    const float2 oldUv = ToUv(staleProjection.xy * oldRay.xy /
                              max(oldRay.z, 1e-5) + staleProjection.zw);
    if (fillMode == 1 && oldRay.z > 0 && all(oldUv >= 0) && all(oldUv < 1)) {
        uint2 oldPixel = uint2(oldUv * float2(width, height));
        const bool usable = historyDepth.w > 0
            ? LocateHistory(oldUv, uv, uint2(width, height), targetRight, oldPixel)
            : !IsNear(ReadDepth(staleDepth, oldPixel, targetRight), nearDepth.zw);
        if (usable) {
            outputImage[id.xy] = stale.Load(int3(oldPixel, 0));
            return;
        }
    }

    // A stale foreground sample would duplicate the weapon along its old
    // edge. Prefer a nearby projected background sample for those pixels.
    uint background = 0;
    [unroll] for (int offset = 1; offset <= 6; ++offset) {
        const int2 row = int2(id.xy);
        const uint left = coverage.Load(int3(max(0, row.x - offset), row.y, 0));
        const uint right = coverage.Load(int3(min(int(width) - 1, row.x + offset), row.y, 0));
        if (left != 0 && (background == 0 || left < background)) background = left;
        if (right != 0 && (background == 0 || right < background)) background = right;
    }
    if (background != 0) {
        outputImage[id.xy] = fresh.Load(int3(background & 65535u, id.y, 0));
    } else if (!IsNear(ReadDepth(depth, id.xy, !targetRight), nearDepth.xy)) {
        outputImage[id.xy] = fresh.Load(int3(id.xy, 0));
    } else {
        outputImage[id.xy] = float4(0, 0, 0, 1);
    }
}
)";

struct SavedCompute {
    explicit SavedCompute(ID3D11DeviceContext* context) : ctx(context)
    {
        ctx->CSGetShader(&shader, instances, &instanceCount);
        ctx->CSGetShaderResources(0, 7, views);
        ctx->CSGetUnorderedAccessViews(0, 4, uavs);
        ctx->CSGetConstantBuffers(0, 1, &constants);
        ctx->GetPredication(&predicate, &predicateValue);
        ctx->SetPredication(nullptr, FALSE);
    }

    ~SavedCompute()
    {
        ID3D11ShaderResourceView* noViews[7]{};
        ID3D11UnorderedAccessView* noUavs[4]{};
        ctx->CSSetShaderResources(0, 7, noViews);
        ctx->CSSetUnorderedAccessViews(0, 4, noUavs, nullptr);
        ctx->CSSetShader(shader, instances, instanceCount);
        ctx->CSSetShaderResources(0, 7, views);
        ctx->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &constants);
        ctx->SetPredication(predicate, predicateValue);
        if (shader) shader->Release();
        if (constants) constants->Release();
        if (predicate) predicate->Release();
        for (UINT i = 0; i < instanceCount; ++i) instances[i]->Release();
        for (auto* view : views) if (view) view->Release();
        for (auto* uav : uavs) if (uav) uav->Release();
    }

    ID3D11DeviceContext* ctx;
    ID3D11ComputeShader* shader = nullptr;
    ID3D11ClassInstance* instances[D3D11_SHADER_MAX_INTERFACES]{};
    UINT instanceCount = D3D11_SHADER_MAX_INTERFACES;
    ID3D11ShaderResourceView* views[7]{};
    ID3D11UnorderedAccessView* uavs[4]{};
    ID3D11Buffer* constants = nullptr;
    ID3D11Predicate* predicate = nullptr;
    BOOL predicateValue = FALSE;
};

}  // namespace

void Dibr::ResetHistory()
{
    m_valid[0] = m_valid[1] = false;
    m_depthValid[0] = m_depthValid[1] = false;
    m_historyCamera = {};
    m_historyWarpActive = false;
    m_forwardHistoryActive = false;
}

bool Dibr::EnsureShaders()
{
    if (m_shaderAttempted) {
        return m_scatter && m_resolve && m_constants;
    }
    m_shaderAttempted = true;
    const auto compile = [&](const char* entry, ID3D11ComputeShader** shader) {
        ComPtr<ID3DBlob> bytecode, diagnostics;
        if (FAILED(D3DCompile(kDibrShader, sizeof(kDibrShader) - 1, "Arma DIBR", nullptr, nullptr,
                              entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                              &bytecode, &diagnostics))) {
            Log("DIBR shader %s: %s", entry,
                diagnostics ? static_cast<const char*>(diagnostics->GetBufferPointer()) : "compile failed");
            return false;
        }
        return SUCCEEDED(m_device->CreateComputeShader(bytecode->GetBufferPointer(),
                                                        bytecode->GetBufferSize(), nullptr, shader));
    };
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Settings);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const bool ready = compile("Scatter", &m_scatter) && compile("Resolve", &m_resolve) &&
        SUCCEEDED(m_device->CreateBuffer(&buffer, nullptr, &m_constants));
    if (ready) {
        compile("ScatterHistoryDepth", &m_scatterHistoryDepth);
        compile("ScatterHistoryIndex", &m_scatterHistoryIndex);
    }
    return ready;
}

bool Dibr::EnsureColor(ID3D11Device* device, UINT width, UINT height)
{
    if (m_device.Get() == device && m_width == width && m_height == height && m_outputUav) {
        return true;
    }
    if (m_device.Get() != device) {
        *this = Dibr{};
        m_device = device;
    }
    ResetHistory();
    m_width = width;
    m_height = height;
    m_outputUav.Reset();
    m_historyKeyUav.Reset();
    m_historyIndexUav.Reset();
    m_historyIndexView.Reset();
    m_forwardHistoryAttempted = false;
    if (!EnsureShaders()) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    for (int eye = 0; eye < 2; ++eye) {
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, m_color[eye].ReleaseAndGetAddressOf())) ||
            FAILED(m_device->CreateShaderResourceView(m_color[eye].Get(), nullptr,
                                                     m_colorView[eye].ReleaseAndGetAddressOf()))) {
            return false;
        }
    }
    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, m_currentNear.ReleaseAndGetAddressOf())) ||
        FAILED(m_device->CreateShaderResourceView(m_currentNear.Get(), nullptr,
                                                 m_currentNearView.ReleaseAndGetAddressOf()))) {
        return false;
    }
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, m_output.ReleaseAndGetAddressOf()))) {
        return false;
    }
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(m_device->CreateTexture2D(&desc, nullptr, m_coverage.ReleaseAndGetAddressOf())) &&
           SUCCEEDED(m_device->CreateShaderResourceView(m_coverage.Get(), nullptr,
                                                        m_coverageView.ReleaseAndGetAddressOf())) &&
           SUCCEEDED(m_device->CreateUnorderedAccessView(m_coverage.Get(), nullptr,
                                                         m_coverageUav.ReleaseAndGetAddressOf())) &&
           SUCCEEDED(m_device->CreateUnorderedAccessView(m_output.Get(), nullptr, &m_outputUav));
}

bool Dibr::EnsureForwardHistory()
{
    if (m_forwardHistoryAttempted) {
        return m_historyIndexView != nullptr;
    }
    m_forwardHistoryAttempted = true;
    if (!m_scatterHistoryDepth || !m_scatterHistoryIndex) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    Texture key, index;
    return SUCCEEDED(m_device->CreateTexture2D(&desc, nullptr, &key)) &&
           SUCCEEDED(m_device->CreateTexture2D(&desc, nullptr, &index)) &&
           SUCCEEDED(m_device->CreateUnorderedAccessView(key.Get(), nullptr, &m_historyKeyUav)) &&
           SUCCEEDED(m_device->CreateUnorderedAccessView(index.Get(), nullptr, &m_historyIndexUav)) &&
           SUCCEEDED(m_device->CreateShaderResourceView(index.Get(), nullptr, &m_historyIndexView));
}

bool Dibr::EnsureDepth(const D3D11_TEXTURE2D_DESC& description)
{
    if (description.Format != DXGI_FORMAT_R24G8_TYPELESS || description.SampleDesc.Count != 1 ||
        description.ArraySize != 1 || description.Width < 2 || description.Width % 2 != 0) {
        return false;
    }
    if (m_depthView[0] && m_depthView[1] && m_depthWidth == description.Width &&
        m_depthHeight == description.Height) {
        return true;
    }
    auto copy = description;
    copy.MipLevels = 1;
    copy.Usage = D3D11_USAGE_DEFAULT;
    copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy.MiscFlags = copy.CPUAccessFlags = 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    m_depthWidth = description.Width;
    m_depthHeight = description.Height;
    m_depthValid[0] = m_depthValid[1] = false;
    for (int eye = 0; eye < 2; ++eye) {
        m_depthView[eye].Reset();
        if (FAILED(m_device->CreateTexture2D(&copy, nullptr, m_depth[eye].ReleaseAndGetAddressOf())) ||
            FAILED(m_device->CreateShaderResourceView(m_depth[eye].Get(), &view, &m_depthView[eye]))) {
            return false;
        }
    }
    return true;
}

bool Dibr::Retain(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame,
                  int eye)
{
    m_valid[eye] = false;
    m_depthValid[eye] = false;
    D3D11_TEXTURE2D_DESC desc{};
    frame->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1 || (desc.Width % 2) != 0 ||
        (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
         desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         desc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS) ||
        !EnsureColor(device, desc.Width / 2, desc.Height)) {
        return false;
    }
    const UINT x = eye == 0 ? 0 : m_width;
    const D3D11_BOX box{x, 0, 0, x + m_width, m_height, 1};
    context->CopySubresourceRegion(m_color[eye].Get(), 0, 0, 0, 0, frame, 0, &box);
    m_valid[eye] = true;
    m_depthValid[eye] = false;
    return true;
}

ID3D11Texture2D* Dibr::Render(ID3D11DeviceContext* context, ID3D11Texture2D* frame,
                             ID3D11Texture2D* depth, int renderedEye, Settings settings,
                             const DlssWorld::Camera* camera, bool forwardHistory)
{
    m_historyWarpActive = false;
    m_forwardHistoryActive = false;
    if (!depth || !m_valid[renderedEye]) {
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC desc{};
    depth->GetDesc(&desc);
    const float colorAspect = static_cast<float>(m_width * 2) / m_height;
    const float depthAspect = static_cast<float>(desc.Width) / desc.Height;
    if (depthAspect < 0.97F * colorAspect || depthAspect > 1.03F * colorAspect ||
        !EnsureDepth(desc)) {
        return nullptr;
    }

    context->CopySubresourceRegion(m_depth[renderedEye].Get(), 0, 0, 0, 0, depth, 0, nullptr);
    m_nearDepth[renderedEye][0] = settings.nearDepth[0];
    m_nearDepth[renderedEye][1] = settings.nearDepth[1];
    m_depthValid[renderedEye] = true;
    m_historyCamera[renderedEye] = camera ? *camera : DlssWorld::Camera{};
    const int staleEye = 1 - renderedEye;
    if (!m_valid[staleEye] || !m_depthValid[staleEye]) {
        return nullptr;
    }
    settings.nearDepth[2] = m_nearDepth[staleEye][0];
    settings.nearDepth[3] = m_nearDepth[staleEye][1];
    if (camera && settings.separation[1] == 1.0F) {
        SetHistoryTransform(m_historyCamera[staleEye], *camera, staleEye, settings);
        m_historyWarpActive = settings.historyDepth[3] > 0.0F;
    }
    m_forwardHistoryActive = forwardHistory && m_historyWarpActive && EnsureForwardHistory();
    if (m_forwardHistoryActive) {
        settings.historyDepth[3] = 2.0F;
    }
    const UINT nearX = staleEye == 0 ? 0 : m_width;
    const D3D11_BOX nearBox{nearX, 0, 0, nearX + m_width, m_height, 1};
    context->CopySubresourceRegion(m_currentNear.Get(), 0, 0, 0, 0, frame, 0, &nearBox);

    SavedCompute saved(context);
    const UINT clear[4]{};
    context->ClearUnorderedAccessViewUint(m_coverageUav.Get(), clear);
    context->UpdateSubresource(m_constants.Get(), 0, nullptr, &settings, 0, 0);
    ID3D11Buffer* constants = m_constants.Get();
    context->CSSetConstantBuffers(0, 1, &constants);
    ID3D11ShaderResourceView* views[7]{m_colorView[renderedEye].Get(), m_colorView[staleEye].Get(),
                                      m_depthView[renderedEye].Get(), nullptr,
                                      m_currentNearView.Get(), m_depthView[staleEye].Get(), nullptr};
    context->CSSetShaderResources(0, 7, views);
    ID3D11UnorderedAccessView* uavs[4]{m_coverageUav.Get(), nullptr, nullptr, nullptr};
    context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
    context->CSSetShader(m_scatter.Get(), nullptr, 0);
    const UINT groupsX = (m_width + 7) / 8, groupsY = (m_height + 7) / 8;
    context->Dispatch(groupsX, groupsY, 1);

    uavs[0] = nullptr;
    context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
    views[3] = m_coverageView.Get();
    context->CSSetShaderResources(0, 7, views);
    if (m_forwardHistoryActive) {
        const UINT empty[4]{0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
        context->ClearUnorderedAccessViewUint(m_historyKeyUav.Get(), empty);
        context->ClearUnorderedAccessViewUint(m_historyIndexUav.Get(), empty);
        uavs[2] = m_historyKeyUav.Get();
        uavs[3] = m_historyIndexUav.Get();
        context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
        context->CSSetShader(m_scatterHistoryDepth.Get(), nullptr, 0);
        context->Dispatch(groupsX, groupsY, 1);
        context->CSSetShader(m_scatterHistoryIndex.Get(), nullptr, 0);
        context->Dispatch(groupsX, groupsY, 1);
        uavs[2] = uavs[3] = nullptr;
        views[6] = m_historyIndexView.Get();
    }
    uavs[1] = m_outputUav.Get();
    context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
    context->CSSetShaderResources(0, 7, views);
    context->CSSetShader(m_resolve.Get(), nullptr, 0);
    context->Dispatch(groupsX, groupsY, 1);
    return m_output.Get();
}

}  // namespace a3vr
