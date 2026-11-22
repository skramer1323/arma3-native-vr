#pragma once

#include "DlssWorld.h"

#include <array>

namespace a3vr {

// Retain only real renders. The synthetic eye is never fed back into history.
class StaleEyeWarp final {
public:
    struct Pair {
        ID3D11Texture2D* color = nullptr;
        ID3D11Texture2D* depth = nullptr;
    };

    void ResetHistory() { m_history = {}; }
    bool Seed(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* color,
              ID3D11Texture2D* depth, const DlssWorld::Camera& camera, std::uint64_t frame, double timestamp);
    Pair Reproject(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* color,
                   ID3D11Texture2D* depth, const DlssWorld::Camera& camera, int freshEye,
                   std::uint64_t frame, double timestamp);

private:
    using Texture = Microsoft::WRL::ComPtr<ID3D11Texture2D>;
    using Srv = Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>;
    using Uav = Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>;
    struct History {
        DlssWorld::Camera camera{};
        std::uint64_t frame = 0;
        double timestamp = 0;
    };

    bool EnsureDevice(ID3D11Device* device);
    bool EnsureImages(ID3D11Device* device, ID3D11Texture2D* color, ID3D11Texture2D* depth);
    void Retain(ID3D11DeviceContext* context, ID3D11Texture2D* color, ID3D11Texture2D* depth,
                const History& history, UINT eye);
    void CopyEye(ID3D11DeviceContext* context, ID3D11Texture2D* dest, ID3D11Texture2D* packed, UINT eye) const;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> m_isolated;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    std::array<Microsoft::WRL::ComPtr<ID3D11ComputeShader>, 3> m_shaders;
    bool m_attempted = false;
    UINT m_width = 0, m_height = 0;
    DXGI_FORMAT m_depthFormat = DXGI_FORMAT_UNKNOWN;
    unsigned m_inputRejects = 0;
    std::array<History, 2> m_history;
    std::array<Texture, 2> m_color, m_depth;
    std::array<Srv, 2> m_colorView, m_depthView;
    Texture m_nearColor, m_depthKey, m_sourceIndex, m_pairColor, m_pairDepth;
    Srv m_nearView;
    Uav m_keyUav, m_indexUav, m_colorUav, m_depthUav;
};

}  // namespace a3vr
