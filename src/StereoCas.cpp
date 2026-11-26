#include "StereoCas.h"

#include "Log.h"
#include "StereoCasShader.h"

namespace a3vr {
namespace {

using Microsoft::WRL::ComPtr;

struct CasConstants {
    UINT eyeWidth;
    UINT height;
    float strength;
    UINT showRadius;
    WorldShadingGuide guide;
    UINT limitSharpening;
};
static_assert(sizeof(CasConstants) == 64);

}  // namespace

bool StereoCas::Initialize(ID3D11Device* device)
{
    if (m_shader) return true;
    ComPtr<ID3D11Device1> device1;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) return false;
    const auto level = device->GetFeatureLevel();
    const D3D11_BUFFER_DESC constants{sizeof(CasConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER,
                                      0, 0, 0};
    return SUCCEEDED(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
                                                       __uuidof(ID3D11Device), nullptr, &m_state)) &&
           SUCCEEDED(device->CreateBuffer(&constants, nullptr, &m_constants)) &&
           SUCCEEDED(device->CreateComputeShader(kStereoCas, sizeof(kStereoCas), nullptr, &m_shader));
}

bool StereoCas::Resize(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& source)
{
    if (source.Width == m_width && source.Height == m_height) return true;
    auto desc = source;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = desc.MiscFlags = 0;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11UnorderedAccessView> uav;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &output)) ||
        FAILED(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav))) return false;
    m_copyInputView.Reset();
    m_copyInput.Reset();
    m_output = output;
    m_outputView = uav;
    m_width = source.Width;
    m_height = source.Height;
    Log("Stereo CAS/ring overlay scene: %ux%u per eye", m_width / 2, m_height);
    return true;
}

bool StereoCas::PrepareInput(ID3D11Device* device, ID3D11Texture2D* source,
                             const D3D11_TEXTURE2D_DESC& desc)
{
    if (m_source.Get() != source) {
        m_sourceView.Reset();
        m_source = source;
        if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
            device->CreateShaderResourceView(source, nullptr, &m_sourceView);
        }
    }
    if (m_sourceView || m_copyInputView) return true;

    auto copy = desc;
    copy.Usage = D3D11_USAGE_DEFAULT;
    copy.CPUAccessFlags = copy.MiscFlags = 0;
    copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(device->CreateTexture2D(&copy, nullptr, &m_copyInput)) &&
           SUCCEEDED(device->CreateShaderResourceView(m_copyInput.Get(), nullptr, &m_copyInputView));
}

ID3D11Texture2D* StereoCas::Render(ID3D11DeviceContext* context, ID3D11Texture2D* stereo, int percent,
                                  const WorldShadingGuide* guide, bool showRadius)
{
    if (!context || !stereo || (percent <= 0 && !showRadius)) return nullptr;
    D3D11_TEXTURE2D_DESC source{};
    stereo->GetDesc(&source);
    if (source.Width < 4 || source.Width % 2 || source.Height < 2 ||
        source.Format != DXGI_FORMAT_R8G8B8A8_UNORM || source.MipLevels != 1 ||
        source.ArraySize != 1 || source.SampleDesc.Count != 1) return nullptr;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (m_device.Get() != device.Get()) {
        *this = StereoCas{};
        m_device = device;
    }
    if (m_unavailable) return nullptr;
    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))) ||
        !Initialize(device.Get()) || !Resize(device.Get(), source) ||
        !PrepareInput(device.Get(), stereo, source)) {
        m_unavailable = true;
        Log("Stereo CAS/ring overlay unavailable; submitting the original scene");
        return nullptr;
    }

    const CasConstants setup{m_width / 2, m_height, percent / 100.0F,
                             showRadius && guide ? 1U : 0U,
                             guide ? *guide : WorldShadingGuide{}, guide ? 1U : 0U};
    ComPtr<ID3DDeviceContextState> previous;
    context1->SwapDeviceContextState(m_state.Get(), &previous);
    // The private state unbinds the game's RTV before the scene is read as an SRV.
    if (!m_sourceView) context->CopyResource(m_copyInput.Get(), stereo);
    context->UpdateSubresource(m_constants.Get(), 0, nullptr, &setup, 0, 0);
    auto* constants = m_constants.Get();
    auto* srv = m_sourceView ? m_sourceView.Get() : m_copyInputView.Get();
    auto* uav = m_outputView.Get();
    context->CSSetShader(m_shader.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &constants);
    context->CSSetShaderResources(0, 1, &srv);
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context->Dispatch((setup.eyeWidth + 7) / 8, (setup.height + 7) / 8, 2);
    srv = nullptr;
    uav = nullptr;
    context->CSSetShaderResources(0, 1, &srv);
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context1->SwapDeviceContextState(previous.Get(), nullptr);
    return m_output.Get();
}

}  // namespace a3vr
