#pragma once

#include "DlssWorld.h"

#include <d3d11.h>
#include <wrl/client.h>

namespace a3vr {

class Dibr final {
public:
    struct Settings {
        float sourceProjection[4];  // P00, P11, M20, M21
        float targetProjection[4];
        float staleProjection[4];
        float depth[4];             // world A and reciprocal B after viewport remapping
        float separation[4];        // signed eye delta, reserved, 1/width, 1/height
        float nearDepth[4];         // current and retained first-person viewport bounds
        float staleRotation[3][4];  // target rays to stale camera axes
        float historyClipToCurrent[4][4];
        float historyDepth[4];      // old viewport min, max, reciprocal range, warp mode
    };

    void ResetHistory();
    ID3D11Texture2D* OutputTexture() const { return m_output.Get(); }
    bool HistoryWarpActive() const { return m_historyWarpActive; }
    bool ForwardHistoryActive() const { return m_forwardHistoryActive; }
    bool Retain(ID3D11Device* device, ID3D11DeviceContext* context,
                ID3D11Texture2D* frame, int eye);
    ID3D11Texture2D* Render(ID3D11DeviceContext* context, ID3D11Texture2D* frame,
                           ID3D11Texture2D* depth, int renderedEye, Settings settings,
                           const DlssWorld::Camera* camera = nullptr, bool forwardHistory = false);

private:
    bool EnsureColor(ID3D11Device* device, UINT width, UINT height);
    bool EnsureDepth(const D3D11_TEXTURE2D_DESC& description);
    bool EnsureShaders();
    bool EnsureForwardHistory();

    using Texture = Microsoft::WRL::ComPtr<ID3D11Texture2D>;
    using View = Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>;
    using Uav = Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_scatter, m_resolve;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_scatterHistoryDepth, m_scatterHistoryIndex;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    bool m_shaderAttempted = false;
    bool m_historyWarpActive = false;
    bool m_forwardHistoryActive = false;
    bool m_forwardHistoryAttempted = false;
    bool m_valid[2]{};
    bool m_depthValid[2]{};
    float m_nearDepth[2][2]{};
    std::array<DlssWorld::Camera, 2> m_historyCamera{};
    UINT m_width = 0, m_height = 0;
    UINT m_depthWidth = 0, m_depthHeight = 0;
    Texture m_color[2], m_depth[2], m_currentNear, m_coverage, m_output;
    View m_colorView[2], m_depthView[2], m_currentNearView, m_coverageView;
    Uav m_coverageUav, m_outputUav;
    Uav m_historyKeyUav, m_historyIndexUav;
    View m_historyIndexView;
};

}  // namespace a3vr
