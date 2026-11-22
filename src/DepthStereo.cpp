#include "DepthStereo.h"

#include "Log.h"

#include <MinHook.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace a3vr {
namespace {

using Microsoft::WRL::ComPtr;

const char kShader[] = R"(
Texture2D<float4> g_color : register(t0);
#ifdef MSAA
Texture2DMS<float> g_depth : register(t1);
#else
Texture2D<float> g_depth : register(t1);
#endif
SamplerState g_linear : register(s0);

cbuffer Params : register(b0) {
  float eye;          // -1 left, +1 right
  float strength;     // parallax at infinity, fraction of image width
  float convergence;  // 1 / (near / distance) at the screen plane
  float reversedZ;
  float2 depthSize;
  float depthView;
  float flatMode;
};

struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
  VSOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.uv = uv;
  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}

// near / distance: 1 - depth for a normal projection, depth itself for reversed Z.
float InvDist(float2 uv) {
  int2 p = int2(saturate(uv) * (depthSize - 1));
#ifdef MSAA
  float d = g_depth.Load(p, 0);
#else
  float d = g_depth.Load(int3(p, 0));
#endif
  return saturate(reversedZ > 0.5 ? d : 1 - d);
}

float Parallax(float2 uv) {
  if (flatMode > 0.5) return 0;
  return clamp(strength * (1 - convergence * InvDist(uv)), -strength, strength);
}

float4 PSMain(VSOut i) : SV_Target {
  float2 src = i.uv;
  // Backward search for the source pixel that lands here: dest = src + eye * p(src) / 2.
  [unroll] for (int k = 0; k < 8; ++k) src.x = i.uv.x - eye * 0.5 * Parallax(src);
  float3 col = g_color.Sample(g_linear, src).rgb;
  if (depthView > 0.5) col = 0.5 + 0.5 * Parallax(i.uv) / max(strength, 1e-6);
  return float4(col, 1);
}
)";

struct Params {
    float eye, strength, convergence, reversedZ;
    float depthWidth, depthHeight, depthView, flatMode;
};
static_assert(sizeof(Params) == 32);

using ClearDepthFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT,
                                              FLOAT, UINT8);
ClearDepthFn g_originalClearDepth = nullptr;

void STDMETHODCALLTYPE HookClearDepth(ID3D11DeviceContext* context, ID3D11DepthStencilView* view,
                                      UINT flags, FLOAT depth, UINT8 stencil)
{
    DepthStereo::Instance().OnClearDepth(view, flags, depth);
    g_originalClearDepth(context, view, flags, depth, stencil);
}

DXGI_FORMAT ColorCopyFormat(DXGI_FORMAT format, DXGI_FORMAT& view)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        view = DXGI_FORMAT_R8G8B8A8_UNORM;
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        view = DXGI_FORMAT_B8G8R8A8_UNORM;
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        view = DXGI_FORMAT_B8G8R8X8_UNORM;
        return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    default:
        view = format;
        return format;
    }
}

bool DepthCopyFormat(DXGI_FORMAT format, DXGI_FORMAT& texture, DXGI_FORMAT& view)
{
    switch (format) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        texture = DXGI_FORMAT_R24G8_TYPELESS;
        view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        return true;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        texture = DXGI_FORMAT_R32_TYPELESS;
        view = DXGI_FORMAT_R32_FLOAT;
        return true;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        texture = DXGI_FORMAT_R32G8X24_TYPELESS;
        view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        return true;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        texture = DXGI_FORMAT_R16_TYPELESS;
        view = DXGI_FORMAT_R16_UNORM;
        return true;
    default:
        return false;
    }
}

// Saves and restores exactly the pipeline state RenderEye touches, so the
// game's next frame sees what it left behind.
struct SavedState {
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* depth = nullptr;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11RasterizerState* rasterizer = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0;
    ID3D11DepthStencilState* depthState = nullptr;
    UINT stencilRef = 0;
    ID3D11InputLayout* layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11GeometryShader* geometryShader = nullptr;
    ID3D11HullShader* hullShader = nullptr;
    ID3D11DomainShader* domainShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    ID3D11ShaderResourceView* views[2] = {};
    ID3D11SamplerState* sampler = nullptr;
    ID3D11Buffer* constants = nullptr;

    void Save(ID3D11DeviceContext* c)
    {
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth);
        c->RSGetViewports(&viewportCount, viewports);
        c->RSGetState(&rasterizer);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->OMGetDepthStencilState(&depthState, &stencilRef);
        c->IAGetInputLayout(&layout);
        c->IAGetPrimitiveTopology(&topology);
        c->VSGetShader(&vertexShader, nullptr, nullptr);
        c->GSGetShader(&geometryShader, nullptr, nullptr);
        c->HSGetShader(&hullShader, nullptr, nullptr);
        c->DSGetShader(&domainShader, nullptr, nullptr);
        c->PSGetShader(&pixelShader, nullptr, nullptr);
        c->PSGetShaderResources(0, 2, views);
        c->PSGetSamplers(0, 1, &sampler);
        c->PSGetConstantBuffers(0, 1, &constants);
    }

    template <typename T> static void Drop(T*& pointer)
    {
        if (pointer) {
            pointer->Release();
            pointer = nullptr;
        }
    }

    void Restore(ID3D11DeviceContext* c)
    {
        ID3D11ShaderResourceView* none[2] = {};
        c->PSSetShaderResources(0, 2, none);
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth);
        c->RSSetViewports(viewportCount, viewports);
        c->RSSetState(rasterizer);
        c->OMSetBlendState(blend, blendFactor, sampleMask);
        c->OMSetDepthStencilState(depthState, stencilRef);
        c->IASetInputLayout(layout);
        c->IASetPrimitiveTopology(topology);
        c->VSSetShader(vertexShader, nullptr, 0);
        c->GSSetShader(geometryShader, nullptr, 0);
        c->HSSetShader(hullShader, nullptr, 0);
        c->DSSetShader(domainShader, nullptr, 0);
        c->PSSetShader(pixelShader, nullptr, 0);
        c->PSSetShaderResources(0, 2, views);
        c->PSSetSamplers(0, 1, &sampler);
        c->PSSetConstantBuffers(0, 1, &constants);
        for (auto& target : targets) {
            Drop(target);
        }
        Drop(depth);
        Drop(rasterizer);
        Drop(blend);
        Drop(depthState);
        Drop(layout);
        Drop(vertexShader);
        Drop(geometryShader);
        Drop(hullShader);
        Drop(domainShader);
        Drop(pixelShader);
        for (auto& view : views) {
            Drop(view);
        }
        Drop(sampler);
        Drop(constants);
    }
};

}  // namespace

DepthStereo& DepthStereo::Instance()
{
    static DepthStereo instance;
    return instance;
}

bool DepthStereo::CompileShaders()
{
    const auto compile = [](const char* entry, const char* target, const D3D_SHADER_MACRO* macros,
                            ComPtr<ID3DBlob>& blob) {
        ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(kShader, sizeof(kShader) - 1, "DepthStereo", macros,
                                          nullptr, entry, target, 0, 0, &blob, &errors);
        if (FAILED(result)) {
            Log("DEPTH shader %s failed 0x%08lX: %s", entry, static_cast<unsigned long>(result),
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            return false;
        }
        return true;
    };
    const D3D_SHADER_MACRO msaa[] = {{"MSAA", "1"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> vertex;
    ComPtr<ID3DBlob> pixel;
    ComPtr<ID3DBlob> pixelMsaa;
    if (!compile("VSMain", "vs_5_0", nullptr, vertex) ||
        !compile("PSMain", "ps_5_0", nullptr, pixel) ||
        !compile("PSMain", "ps_5_0", msaa, pixelMsaa)) {
        return false;
    }
    return SUCCEEDED(m_device->CreateVertexShader(vertex->GetBufferPointer(),
                                                  vertex->GetBufferSize(), nullptr,
                                                  &m_vertexShader)) &&
           SUCCEEDED(m_device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(),
                                                 nullptr, &m_pixelShader)) &&
           SUCCEEDED(m_device->CreatePixelShader(pixelMsaa->GetBufferPointer(),
                                                 pixelMsaa->GetBufferSize(), nullptr,
                                                 &m_pixelShaderMsaa));
}

bool DepthStereo::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (m_initializeTried) {
        return m_ready;
    }
    m_initializeTried = true;
    m_device = device;
    if (!CompileShaders()) {
        return false;
    }
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Params);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC depthState{};
    depthState.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &m_constants)) ||
        FAILED(device->CreateSamplerState(&sampler, &m_sampler)) ||
        FAILED(device->CreateRasterizerState(&rasterizer, &m_rasterizer)) ||
        FAILED(device->CreateBlendState(&blend, &m_blend)) ||
        FAILED(device->CreateDepthStencilState(&depthState, &m_depthState))) {
        Log("DEPTH renderer objects could not be created");
        return false;
    }
    // ClearDepthStencilView is slot 53 of ID3D11DeviceContext. The code address
    // is hooked, so every immediate context of d3d11.dll is covered.
    void** vtable = *reinterpret_cast<void***>(context);
    const MH_STATUS created = MH_CreateHook(vtable[53], reinterpret_cast<void*>(&HookClearDepth),
                                            reinterpret_cast<void**>(&g_originalClearDepth));
    const MH_STATUS enabled =
        created == MH_OK || created == MH_ERROR_ALREADY_CREATED ? MH_EnableHook(vtable[53])
                                                                : created;
    Log("DEPTH ClearDepthStencilView hook at %p: %s", vtable[53], MH_StatusToString(enabled));
    m_ready = true;
    return true;
}

void DepthStereo::OnClearDepth(ID3D11DepthStencilView* view, UINT flags, float depth)
{
    if (!view || (flags & D3D11_CLEAR_DEPTH) == 0) {
        return;
    }
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture))) {
        return;
    }
    // Other D3D11 devices live in this process too (OpenXR runtime, overlays).
    ComPtr<ID3D11Device> owner;
    texture->GetDevice(&owner);
    if (owner.Get() != m_device.Get()) {
        return;
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    std::scoped_lock lock(m_mutex);
    ++m_clearsThisFrame;
    if (m_clearLogs < 60) {
        ++m_clearLogs;
        Log("DEPTH clear #%u in frame: tex=%p %ux%u format=%d samples=%u value=%.3f thread=%lu",
            m_clearsThisFrame, static_cast<void*>(texture.Get()), description.Width,
            description.Height, static_cast<int>(description.Format),
            description.SampleDesc.Count, depth, GetCurrentThreadId());
    }
    if (description.ArraySize != 1 || description.Width < 256 || m_targetWidth == 0) {
        return;
    }
    // Same aspect as the back buffer (the 3D resolution may be scaled).
    const double aspectError =
        static_cast<double>(description.Width) * m_targetHeight /
            (static_cast<double>(description.Height) * m_targetWidth) -
        1.0;
    if (aspectError > 0.03 || aspectError < -0.03) {
        return;
    }
    const std::uint64_t area = static_cast<std::uint64_t>(description.Width) * description.Height;
    if (area >= m_frameBestArea) {
        m_frameBestArea = area;
        m_frameBest = texture;
        m_frameBestClear = depth;
    }
}

bool DepthStereo::EnsureColor(const D3D11_TEXTURE2D_DESC& source)
{
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    const DXGI_FORMAT textureFormat = ColorCopyFormat(source.Format, viewFormat);
    if (m_colorTexture && m_colorDesc.Width == source.Width &&
        m_colorDesc.Height == source.Height && m_colorDesc.Format == textureFormat) {
        return true;
    }
    m_colorView.Reset();
    m_colorTexture.Reset();
    D3D11_TEXTURE2D_DESC description{};
    description.Width = source.Width;
    description.Height = source.Height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = textureFormat;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = viewFormat;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    const bool ok =
        SUCCEEDED(m_device->CreateTexture2D(&description, nullptr, &m_colorTexture)) &&
        SUCCEEDED(m_device->CreateShaderResourceView(m_colorTexture.Get(), &view, &m_colorView));
    Log("DEPTH colour copy %ux%u format=%d view=%d: %s", source.Width, source.Height,
        static_cast<int>(textureFormat), static_cast<int>(viewFormat), ok ? "ok" : "FAILED");
    if (!ok) {
        m_colorView.Reset();
        m_colorTexture.Reset();
        return false;
    }
    m_colorDesc = description;
    m_colorViewFormat = viewFormat;
    return true;
}

bool DepthStereo::EnsureDepth(const D3D11_TEXTURE2D_DESC& source)
{
    DXGI_FORMAT textureFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    if (!DepthCopyFormat(source.Format, textureFormat, viewFormat)) {
        return false;
    }
    if (m_depthTexture && m_depthDesc.Width == source.Width &&
        m_depthDesc.Height == source.Height && m_depthDesc.Format == textureFormat &&
        m_depthDesc.SampleDesc.Count == source.SampleDesc.Count) {
        return true;
    }
    m_depthView.Reset();
    m_depthTexture.Reset();
    D3D11_TEXTURE2D_DESC description{};
    description.Width = source.Width;
    description.Height = source.Height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = textureFormat;
    description.SampleDesc = source.SampleDesc;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = viewFormat;
    if (source.SampleDesc.Count > 1) {
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    } else {
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
    }
    const bool ok =
        SUCCEEDED(m_device->CreateTexture2D(&description, nullptr, &m_depthTexture)) &&
        SUCCEEDED(m_device->CreateShaderResourceView(m_depthTexture.Get(), &view, &m_depthView));
    Log("DEPTH depth copy %ux%u format=%d view=%d samples=%u: %s", source.Width, source.Height,
        static_cast<int>(textureFormat), static_cast<int>(viewFormat), source.SampleDesc.Count,
        ok ? "ok" : "FAILED");
    if (!ok) {
        m_depthView.Reset();
        m_depthTexture.Reset();
        return false;
    }
    m_depthDesc = description;
    return true;
}

bool DepthStereo::Capture(ID3D11DeviceContext* context, ID3D11Texture2D* backBuffer)
{
    if (!m_ready || !backBuffer) {
        return false;
    }
    D3D11_TEXTURE2D_DESC color{};
    backBuffer->GetDesc(&color);
    if (!EnsureColor(color)) {
        return false;
    }
    if (color.SampleDesc.Count > 1) {
        context->ResolveSubresource(m_colorTexture.Get(), 0, backBuffer, 0, m_colorViewFormat);
    } else {
        context->CopyResource(m_colorTexture.Get(), backBuffer);
    }

    ComPtr<ID3D11Texture2D> depth;
    float clearValue = 1.0F;
    {
        std::scoped_lock lock(m_mutex);
        m_targetWidth = color.Width;
        m_targetHeight = color.Height;
        if (m_frameBest) {
            m_lastBest = m_frameBest;
            m_lastBestClear = m_frameBestClear;
        }
        depth = m_lastBest;
        clearValue = m_lastBestClear;
        m_frameBest.Reset();
        m_frameBestArea = 0;
        m_clearsLastFrame = m_clearsThisFrame;
        m_clearsThisFrame = 0;
    }
    m_depthSource = "cleared this frame";
    if (!depth) {
        // No clear seen (deferred contexts?): take what is bound at Present.
        ComPtr<ID3D11DepthStencilView> bound;
        context->OMGetRenderTargets(0, nullptr, &bound);
        if (bound) {
            ComPtr<ID3D11Resource> resource;
            bound->GetResource(&resource);
            if (resource) {
                resource.As(&depth);
            }
            m_depthSource = "bound at Present";
        }
    }
    m_hasDepth = false;
    if (depth) {
        D3D11_TEXTURE2D_DESC description{};
        depth->GetDesc(&description);
        if (EnsureDepth(description)) {
            context->CopyResource(m_depthTexture.Get(), depth.Get());
            m_hasDepth = true;
            m_reversed = clearValue < 0.5F;
            MeasureDepth(context);
        }
    }
    if (!m_hasDepth && m_missLogs < 10) {
        ++m_missLogs;
        Log("DEPTH no usable depth buffer this frame (clears seen=%u)", m_clearsLastFrame);
    }
    return true;
}

void DepthStereo::RenderEye(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
                            UINT width, UINT height, const EyeSettings& settings)
{
    if (!m_colorView || !target) {
        return;
    }
    const bool reversed =
        settings.reversedOverride >= 0 ? settings.reversedOverride != 0 : m_reversed;
    Params params{};
    params.eye = settings.eye;
    params.strength = settings.strength;
    // Screen plane relative to the nearest surfaces measured in the depth buffer.
    params.convergence = settings.convergence / (m_reference > 1e-7F ? m_reference : 0.1F);
    params.reversedZ = reversed ? 1.0F : 0.0F;
    params.depthWidth = static_cast<float>(m_depthDesc.Width);
    params.depthHeight = static_cast<float>(m_depthDesc.Height);
    params.depthView = settings.depthView ? 1.0F : 0.0F;
    params.flatMode = settings.flat || !m_hasDepth ? 1.0F : 0.0F;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    std::memcpy(mapped.pData, &params, sizeof(params));
    context->Unmap(m_constants.Get(), 0);

    SavedState saved;
    saved.Save(context);
    const D3D11_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(width),
                                  static_cast<float>(height), 0.0F, 1.0F};
    context->OMSetRenderTargets(1, &target, nullptr);
    context->RSSetViewports(1, &viewport);
    context->RSSetState(m_rasterizer.Get());
    const FLOAT zero[4] = {};
    context->OMSetBlendState(m_blend.Get(), zero, 0xFFFFFFFF);
    context->OMSetDepthStencilState(m_depthState.Get(), 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    const bool msaa = m_hasDepth && m_depthDesc.SampleDesc.Count > 1;
    context->PSSetShader(msaa ? m_pixelShaderMsaa.Get() : m_pixelShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* views[2] = {m_colorView.Get(),
                                          m_hasDepth ? m_depthView.Get() : nullptr};
    context->PSSetShaderResources(0, 2, views);
    ID3D11SamplerState* sampler = m_sampler.Get();
    context->PSSetSamplers(0, 1, &sampler);
    ID3D11Buffer* constants = m_constants.Get();
    context->PSSetConstantBuffers(0, 1, &constants);
    context->Draw(3, 0);
    saved.Restore(context);
}

// Reads the depth copy back every few frames (sparse grid) and finds how near
// the nearest surfaces are, in the buffer's own units.
void DepthStereo::MeasureDepth(ID3D11DeviceContext* context)
{
    ++m_measureCounter;
    if (m_depthDesc.SampleDesc.Count > 1) {
        return;
    }
    if (!m_staging || m_stagingDesc.Width != m_depthDesc.Width ||
        m_stagingDesc.Height != m_depthDesc.Height || m_stagingDesc.Format != m_depthDesc.Format) {
        m_staging.Reset();
        D3D11_TEXTURE2D_DESC description = m_depthDesc;
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m_device->CreateTexture2D(&description, nullptr, &m_staging))) {
            return;
        }
        m_stagingDesc = description;
    }
    context->CopyResource(m_staging.Get(), m_depthTexture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return;
    }
    const UINT width = m_depthDesc.Width;
    const UINT height = m_depthDesc.Height;
    const DXGI_FORMAT format = m_depthDesc.Format;
    const bool reversed = m_reversed;
    const auto read = [&](UINT x, UINT y) {
        const std::uint8_t* row = static_cast<const std::uint8_t*>(mapped.pData) +
                                  static_cast<std::size_t>(y) * mapped.RowPitch;
        float depth = 1.0F;
        if (format == DXGI_FORMAT_R24G8_TYPELESS) {
            std::uint32_t value = 0;
            std::memcpy(&value, row + static_cast<std::size_t>(x) * 4, 4);
            depth = static_cast<float>(value & 0xFFFFFFU) / 16777215.0F;
        } else if (format == DXGI_FORMAT_R32_TYPELESS) {
            std::memcpy(&depth, row + static_cast<std::size_t>(x) * 4, 4);
        } else if (format == DXGI_FORMAT_R32G8X24_TYPELESS) {
            std::memcpy(&depth, row + static_cast<std::size_t>(x) * 8, 4);
        } else {
            std::uint16_t value = 0;
            std::memcpy(&value, row + static_cast<std::size_t>(x) * 2, 2);
            depth = static_cast<float>(value) / 65535.0F;
        }
        return std::clamp(reversed ? depth : 1.0F - depth, 0.0F, 1.0F);
    };
    std::vector<float> all;
    std::vector<float> weapon;
    all.reserve(static_cast<std::size_t>(width / 8 + 1) * (height / 8 + 1));
    for (UINT y = 0; y < height; y += 8) {
        for (UINT x = 0; x < width; x += 8) {
            const float value = read(x, y);
            all.push_back(value);
            // Lower middle of the picture: where a first-person weapon is drawn.
            if (y > height * 6 / 10 && x > width * 4 / 10 && x < width * 75 / 100) {
                weapon.push_back(value);
            }
        }
    }
    context->Unmap(m_staging.Get(), 0);
    const auto percentile = [](std::vector<float>& values, double part) {
        if (values.empty()) {
            return 0.0F;
        }
        const std::size_t index =
            std::min(values.size() - 1, static_cast<std::size_t>(part * values.size()));
        std::nth_element(values.begin(), values.begin() + index, values.end());
        return values[index];
    };
    m_lastNearest = percentile(all, 0.99);
    m_lastMedian = percentile(all, 0.5);
    m_lastWeaponNearest = percentile(weapon, 0.99);
    m_lastWeaponMedian = percentile(weapon, 0.5);
    if (m_lastNearest > 1e-7F) {
        // The engine changes its near plane from frame to frame, which rescales the
        // whole buffer at once: a big change is taken immediately (same frame, so
        // the picture does not jump), a small one is smoothed against flicker.
        const float ratio = m_reference > 0.0F ? m_lastNearest / m_reference : 0.0F;
        m_reference = ratio > 0.8F && ratio < 1.25F
                          ? m_reference + 0.3F * (m_lastNearest - m_reference)
                          : m_lastNearest;
    }
}

void DepthStereo::RearmLogs()
{
    std::scoped_lock lock(m_mutex);
    m_clearLogs = 0;
    m_missLogs = 0;
}

void DepthStereo::LogStats()
{
    if (!m_ready) {
        return;
    }
    Log("DEPTH hasDepth=%d source=\"%s\" %ux%u format=%d samples=%u reversedZ=%d "
        "clearsPerFrame=%u",
        m_hasDepth ? 1 : 0, m_depthSource, m_depthDesc.Width, m_depthDesc.Height,
        static_cast<int>(m_depthDesc.Format), m_depthDesc.SampleDesc.Count, m_reversed ? 1 : 0,
        m_clearsLastFrame);
    // Units: near/distance as stored in the buffer (bigger = nearer). "weapon" is
    // the lower middle of the picture. If the weapon is nearer than the scene these
    // numbers show it; if it is not, the weapon is drawn with another projection.
    Log("DEPTH scale: reference=%.6f frame nearest=%.6f median=%.6f | weapon area nearest=%.6f "
        "median=%.6f",
        m_reference, m_lastNearest, m_lastMedian, m_lastWeaponNearest, m_lastWeaponMedian);
}

}  // namespace a3vr
