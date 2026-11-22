#include "LensMask.h"

#include "Log.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstring>

using a3vr::Log;

namespace {

using Microsoft::WRL::ComPtr;

constexpr char kShader[] = R"(
cbuffer Ellipse : register(b0) { float4 e; };
float4 VS(uint id : SV_VertexID) : SV_Position
{
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 pos : SV_Position) : SV_Target
{
    float2 d = (pos.xy - e.xy) / e.zw;
    float a = saturate((1.0 - length(d)) * min(e.z, e.w) / 4.0);
    return float4(0, 0, 0, a);
}
)";

struct Resources {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11DepthStencilState> depth;
    bool failed = false;
};
Resources g_res;

bool Ensure(ID3D11Device* device)
{
    if (g_res.device.Get() == device && g_res.vs) {
        return true;
    }
    if (g_res.failed && g_res.device.Get() == device) {
        return false;
    }
    g_res = {};
    g_res.device = device;
    ComPtr<ID3DBlob> vsCode, psCode, errors;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "LensMask", nullptr, nullptr, "VS", "vs_5_0",
                          0, 0, &vsCode, &errors)) ||
        FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "LensMask", nullptr, nullptr, "PS", "ps_5_0",
                          0, 0, &psCode, &errors)) ||
        FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
                                          nullptr, &g_res.vs)) ||
        FAILED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(),
                                         nullptr, &g_res.ps))) {
        Log("LENS mask shader failed to build: %s",
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
        g_res.failed = true;
        return false;
    }
    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = 16;
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    if (FAILED(device->CreateBuffer(&bufferDesc, nullptr, &g_res.constants)) ||
        FAILED(device->CreateBlendState(&blendDesc, &g_res.blend)) ||
        FAILED(device->CreateRasterizerState(&rasterDesc, &g_res.rasterizer)) ||
        FAILED(device->CreateDepthStencilState(&depthDesc, &g_res.depth))) {
        Log("LENS mask states failed to build");
        g_res.failed = true;
        return false;
    }
    Log("LENS mask shader ready");
    return true;
}

template <typename T> void Drop(T*& pointer)
{
    if (pointer) {
        pointer->Release();
        pointer = nullptr;
    }
}

}  // namespace

void LensMaskWriteAlpha(ID3D11Device* device, ID3D11DeviceContext* c,
                        ID3D11RenderTargetView* target, const float rect[4])
{
    if (!device || !c || !target || !Ensure(device)) {
        return;
    }
    // Save what is touched.
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
    ID3D11Buffer* constants = nullptr;
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
    c->PSGetConstantBuffers(0, 1, &constants);

    const float ellipse[4] = {rect[0] + 0.5F * rect[2], rect[1] + 0.5F * rect[3], 0.5F * rect[2],
                              0.5F * rect[3]};
    c->UpdateSubresource(g_res.constants.Get(), 0, nullptr, ellipse, 0, 0);
    ID3D11RenderTargetView* one = target;
    c->OMSetRenderTargets(1, &one, nullptr);
    D3D11_VIEWPORT viewport{rect[0], rect[1], rect[2], rect[3], 0.0F, 1.0F};
    c->RSSetViewports(1, &viewport);
    c->RSSetState(g_res.rasterizer.Get());
    const FLOAT factor[4] = {};
    c->OMSetBlendState(g_res.blend.Get(), factor, 0xFFFFFFFFU);
    c->OMSetDepthStencilState(g_res.depth.Get(), 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(g_res.vs.Get(), nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0);
    c->HSSetShader(nullptr, nullptr, 0);
    c->DSSetShader(nullptr, nullptr, 0);
    c->PSSetShader(g_res.ps.Get(), nullptr, 0);
    ID3D11Buffer* cb = g_res.constants.Get();
    c->PSSetConstantBuffers(0, 1, &cb);
    c->Draw(3, 0);

    // Restore.
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
    c->PSSetConstantBuffers(0, 1, &constants);
    for (auto& t : targets) {
        Drop(t);
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
    Drop(constants);
}
