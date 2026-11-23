#include "SinglePassDepth.h"
#include "SinglePassDepthMath.h"
#include "Log.h"

#include <d3dcompiler.h>

namespace a3vr {
namespace {
using Microsoft::WRL::ComPtr;

// NVAPI D3D11 ABI from NVIDIA's SDK reference.
struct CustomSemantic {
    UINT version, type;
    char name[256];
    BOOL registerSpecified;
    UINT reg, mask, reserved;
};
struct GeometryShaderOptions {
    UINT version;
    BOOL useViewportMask, offsetTarget, forceFast, ignoreViewportOrder, skipAttributes, swizzle;
    void* coordinateSwizzle;
    UINT semanticCount;
    CustomSemantic* semantics;
    BOOL convertToFast, specificExtensions;
};
static_assert(sizeof(CustomSemantic) == 280 && sizeof(GeometryShaderOptions) == 64);
using SetStereoMode = int (__cdecl*)(IUnknown*, UINT, UINT, unsigned char);
using CreateGeometryEx = int (__cdecl*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                       const GeometryShaderOptions*, ID3D11GeometryShader**);
struct StereoSupport { UINT version, flags; };
using QueryStereoSupport = int (__cdecl*)(IUnknown*, StereoSupport*);

struct DepthState {
    // Hold the device so a recreated device cannot reuse a failed identity.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11GeometryShader> depthShader;
    ComPtr<ID3D11GeometryShader> prepassShader;
    ComPtr<ID3D11Buffer> constants;
    SinglePassDepthConstants uploaded{};
    SetStereoMode setMode = nullptr;
    CreateGeometryEx createShader = nullptr;
    bool unavailable = false;
    bool prepassUnavailable = false;
};
DepthState g_depth;

constexpr char kDepthShader[] = R"(
cbuffer StereoProjection : register(b0) {
#if ARMA_PREPASS
    float4 projectionX : packoffset(c0);
    float4 projectionY : packoffset(c1);
    float4 projectionZ : packoffset(c2);
    float4 projectionOffset : packoffset(c3);
#else
    float4 scaleAndZ;
    float4 offset;
#endif
};
struct InputVertex {
    float4 position : SV_Position;
#if ARMA_PREPASS
    float4 camera : TEXCOORD7;
    float4 uv : TEXCOORD0;
    float4 color : TEXCOORD6;
    float opacity : TEXCOORD11;
#endif
};
struct StereoVertex {
    float4 position : SV_Position;
#if ARMA_PREPASS
    float4 camera : TEXCOORD7;
    float4 uv : TEXCOORD0;
    float4 color : TEXCOORD6;
    float opacity : TEXCOORD11;
#endif
    float4 right : NV_XYZW_RIGHT;
    nointerpolation uint viewportMask : NV_VIEWPORT_MASK;
};
[maxvertexcount(3)]
void main(triangle InputVertex left[3], inout TriangleStream<StereoVertex> stream) {
    [unroll] for (uint i = 0; i < 3; ++i) {
        StereoVertex v;
        v.position = left[i].position;
#if ARMA_PREPASS
        v.camera = left[i].camera;
        v.uv = left[i].uv;
        v.color = left[i].color;
        v.opacity = left[i].opacity;
        // The approved VS produces this same projection from TEXCOORD7.xyz.
        // Keep the multiply, two MADs and add in the same order as its bytecode.
        float4 right = v.camera.y * projectionY;
        right = mad(v.camera.x, projectionX, right);
        right = mad(v.camera.z, projectionZ, right);
        v.right = right + projectionOffset;
#else
        precise float2 rightXY = v.position.xy * scaleAndZ.xy + v.position.w * scaleAndZ.zw + offset.xy;
        v.right = float4(rightXY, v.position.zw + offset.zw);
#endif
        v.viewportMask = 0x00020001u;
        stream.Append(v);
    }
    stream.RestartStrip();
}
)";

bool InitializeDepthStereo(ID3D11Device* device)
{
    static const HMODULE library = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) return false;
    using Query = void* (__cdecl*)(UINT);
    using Initialize = int (__cdecl*)();
    const auto query = reinterpret_cast<Query>(GetProcAddress(library, "nvapi_QueryInterface"));
    if (!query) return false;
    const auto initialize = reinterpret_cast<Initialize>(query(0x0150e828));
    const auto support = reinterpret_cast<QueryStereoSupport>(query(0x6f5f0a6d));
    g_depth.createShader = reinterpret_cast<CreateGeometryEx>(query(0x99ed5c1c));
    g_depth.setMode = reinterpret_cast<SetStereoMode>(query(0xa39e6e6e));
    StereoSupport capabilities{sizeof(StereoSupport) | (2U << 16), 0};
    if (!initialize || !support || !g_depth.createShader || !g_depth.setMode || initialize() != 0 ||
        support(device, &capabilities) != 0 || (capabilities.flags & 3U) != 3U) {
        return false;
    }
    const D3D11_BUFFER_DESC buffer{sizeof(SinglePassDepthConstants), D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    return SUCCEEDED(device->CreateBuffer(&buffer, nullptr, &g_depth.constants));
}

bool CreateStereoShader(ID3D11Device* device, bool prepass, ComPtr<ID3D11GeometryShader>& result)
{
    const D3D_SHADER_MACRO defines[]{{"ARMA_PREPASS", prepass ? "1" : "0"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> shader, errors;
    if (FAILED(D3DCompile(kDepthShader, sizeof(kDepthShader) - 1, "Arma single-pass stereo", defines, nullptr,
                          "main", "gs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors))) {
        Log("Single-pass stereo shader failed (prepass=%d): %s", prepass, errors ?
            static_cast<const char*>(errors->GetBufferPointer()) : "no diagnostics");
        return false;
    }
    CustomSemantic semantics[]{{sizeof(CustomSemantic) | (1U << 16), 3, "NV_XYZW_RIGHT"},
                                {sizeof(CustomSemantic) | (1U << 16), 2, "NV_VIEWPORT_MASK"}};
    GeometryShaderOptions options{};
    options.version = sizeof(options) | (5U << 16);
    options.useViewportMask = TRUE;
    options.semanticCount = 2;
    options.semantics = semantics;
    const int status = g_depth.createShader(device, shader->GetBufferPointer(), shader->GetBufferSize(), nullptr,
                                          &options, &result);
    if (status == 0 && result) return true;
    Log("Single-pass geometry shader unavailable (prepass=%d, NVAPI %d)", prepass, status);
    return false;
}
} // namespace

bool BeginSinglePassDepth(ID3D11DeviceContext* context, ID3D11Device* device,
                          const D3D11_VIEWPORT& fullViewport, const float* left, const float* right,
                          ComPtr<ID3D11Buffer>& previousGeometryConstants, SinglePassLayout layout,
                          ID3D11Buffer* rightEyeProjection)
{
    if (g_depth.device.Get() != device) {
        g_depth = {};
        g_depth.device = device;
    }
    const bool prepass = layout == SinglePassLayout::WorldPrepass;
    if (g_depth.unavailable || (prepass && g_depth.prepassUnavailable)) return false;

    D3D11_PRIMITIVE_TOPOLOGY topology{};
    context->IAGetPrimitiveTopology(&topology);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
        topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) return false;
    ComPtr<ID3D11GeometryShader> originalGeometry;
    context->GSGetShader(&originalGeometry, nullptr, nullptr);
    if (originalGeometry) return false;

    SinglePassDepthConstants parameters{};
    if (prepass) {
        if (!rightEyeProjection) return false;
    } else if (!BuildSinglePassDepthConstants(left, right, parameters)) {
        return false;
    }
    if (!g_depth.constants && !InitializeDepthStereo(device)) {
        g_depth.unavailable = true;
        Log("Single-pass depth unavailable; using two stereo draws");
        return false;
    }
    auto& shader = prepass ? g_depth.prepassShader : g_depth.depthShader;
    if (!shader && !CreateStereoShader(device, prepass, shader)) {
        if (prepass) g_depth.prepassUnavailable = true;
        else g_depth.unavailable = true;
        return false;
    }
    const int status = g_depth.setMode(context, 2, 0, TRUE);
    if (status != 0) {
        g_depth.unavailable = true;
        Log("Single-pass depth could not enable stereo (NVAPI %d)", status);
        return false;
    }

    if (!prepass && g_depth.uploaded != parameters) {
        context->UpdateSubresource(g_depth.constants.Get(), 0, nullptr, parameters.data(), 0, 0);
        g_depth.uploaded = parameters;
    }
    context->GSGetConstantBuffers(0, 1, &previousGeometryConstants);
    auto* constants = prepass ? rightEyeProjection : g_depth.constants.Get();
    context->GSSetConstantBuffers(0, 1, &constants);
    context->GSSetShader(shader.Get(), nullptr, 0);
    D3D11_VIEWPORT viewports[]{fullViewport, fullViewport};
    viewports[0].Width *= 0.5F;
    viewports[1].Width *= 0.5F;
    viewports[1].TopLeftX += viewports[0].Width;
    context->RSSetViewports(2, viewports);
    return true;
}

void EndSinglePassDepth(ID3D11DeviceContext* context, ID3D11Buffer* previousGeometryConstants)
{
    int status = g_depth.setMode(context, 1, 0, FALSE);
    if (status != 0) status = g_depth.setMode(context, 1, 0, FALSE);
    if (status != 0) {
        g_depth.unavailable = true;
        Log("Single-pass depth could not restore NVIDIA stereo mode (NVAPI %d)", status);
    }
    context->GSSetShader(nullptr, nullptr, 0);
    context->GSSetConstantBuffers(0, 1, &previousGeometryConstants);
}

} // namespace a3vr
