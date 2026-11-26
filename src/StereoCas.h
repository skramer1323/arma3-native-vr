#pragma once

#include "WorldShadingMask.h"

#include <d3d11_1.h>
#include <wrl/client.h>

namespace a3vr {

class StereoCas final {
public:
    // Returns nullptr when processing is unavailable, leaving the source usable.
    ID3D11Texture2D* Render(ID3D11DeviceContext* context, ID3D11Texture2D* stereo, int percent,
                            const WorldShadingGuide* guide, bool showRadius);

private:
    bool Initialize(ID3D11Device* device);
    bool Resize(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& source);
    bool PrepareInput(ID3D11Device* device, ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& desc);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> m_state;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_shader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_source, m_copyInput, m_output;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_sourceView, m_copyInputView;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_outputView;
    UINT m_width = 0, m_height = 0;
    bool m_unavailable = false;
};

}  // namespace a3vr
