#pragma once

#include "DlssBalanced.h"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>

namespace a3vr {

class DlssWorld final {
public:
    struct Camera {
        std::array<float, 16> view{};
        std::array<std::array<float, 8>, 2> eye{};
        float depthScale = 0;
        float depthBias = 0;
        float worldMin = 0;
        float worldMax = 1;
        float weaponMin = 0;
        float weaponMax = 0;
        float jitterX = 0;
        float jitterY = 0;
        bool synthesized = false;
    };

    ID3D11Texture2D* Render(ID3D11Device* device, ID3D11DeviceContext* context,
                           ID3D11Texture2D* color, ID3D11Texture2D* depth,
                           const Camera& camera, DlssBalanced::Size output, std::uint64_t frame);
    void ResetHistory() { m_lastFrame = 0; }

private:
    using Texture = Microsoft::WRL::ComPtr<ID3D11Texture2D>;
    using Srv = Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>;
    using Uav = Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>;

    bool EnsureShaders(ID3D11Device* device);
    bool EnsureTextures(ID3D11Device* device, DlssBalanced::Size source, DlssBalanced::Size target,
                        DXGI_FORMAT depthFormat);

    DlssBalanced m_balanced;
    DlssBalanced::Size m_input{}, m_target{};
    DXGI_FORMAT m_depthFormat = DXGI_FORMAT_UNKNOWN;
    std::array<Texture, 2> m_color, m_depth, m_motion, m_output;
    std::array<Srv, 2> m_colorView, m_depthView, m_outputView;
    std::array<Uav, 2> m_depthUav, m_motionUav;
    Texture m_depthCopy, m_packed;
    Srv m_depthCopyView;
    Uav m_packedUav;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_buildMotion, m_packEyes;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_linear;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> m_isolated;
    bool m_attemptedShaders = false;
    bool m_reported = false;
    std::uint64_t m_lastFrame = 0;
    Camera m_previous{};
};

}  // namespace a3vr

