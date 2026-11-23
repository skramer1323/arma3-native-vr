#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>

namespace a3vr {

// Depth-based stereo for the screen in the headset (DepthStereo=1).
// One finished game frame + the game's scene depth buffer -> a left and a right
// image: every pixel is moved sideways by a parallax that depends on its
// distance. No engine camera is touched, both eyes come from the same frame.
// Method and shader come from the lab's goh-vr-cinema mod.
//
// The scene depth buffer is found generically: ClearDepthStencilView calls on
// the game's device context are observed, the largest depth texture with the
// back buffer's aspect is used, the clear value tells normal from reversed Z.
class DepthStereo final {
public:
    struct EyeSettings {
        float eye = 0.0F;          // -1 left, +1 right
        float strength = 0.027F;   // parallax at infinity, fraction of image width
        // 1.0 = the nearest thing in view (normally the weapon) lies on the screen surface,
        // above 1 it comes out of the screen, below 1 everything sinks behind it.
        float convergence = 1.0F;
        bool flat = false;         // menus: no parallax
        bool depthView = false;    // debug: show parallax as grey
        int reversedOverride = -1; // -1 auto, 0 normal, 1 reversed
    };

    static DepthStereo& Instance();

    // Once, when the game's device is known. Safe to call again.
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    // Called from the ClearDepthStencilView hook.
    void OnClearDepth(ID3D11DepthStencilView* view, UINT flags, float depth);
    // At Present: copies the frame and the chosen depth buffer.
    bool Capture(ID3D11DeviceContext* context, ID3D11Texture2D* backBuffer);
    // Draws one eye into a render target of the same size as the frame.
    void RenderEye(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, UINT width,
                   UINT height, const EyeSettings& settings);
    void LogStats();
    // Log the next depth clears again (used when the debug view is switched on).
    void RearmLogs();
    bool HasDepth() const { return m_hasDepth; }
    bool Ready() const { return m_ready; }

private:
    bool CompileShaders();
    bool EnsureColor(const D3D11_TEXTURE2D_DESC& source);
    bool EnsureDepth(const D3D11_TEXTURE2D_DESC& source);
    void MeasureDepth(ID3D11DeviceContext* context);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    bool m_ready = false;
    bool m_initializeTried = false;

    // Depth candidates (written by the hook, read at Present).
    std::mutex m_mutex;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_frameBest;
    std::uint64_t m_frameBestArea = 0;
    float m_frameBestClear = 1.0F;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_lastBest;
    float m_lastBestClear = 1.0F;
    std::uint32_t m_clearsThisFrame = 0;
    std::uint32_t m_clearsLastFrame = 0;
    std::uint32_t m_clearLogs = 0;
    UINT m_targetWidth = 0;
    UINT m_targetHeight = 0;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShaderMsaa;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blend;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthState;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_colorTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_colorView;
    D3D11_TEXTURE2D_DESC m_colorDesc{};
    DXGI_FORMAT m_colorViewFormat = DXGI_FORMAT_UNKNOWN;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_depthView;
    D3D11_TEXTURE2D_DESC m_depthDesc{};
    bool m_hasDepth = false;
    bool m_reversed = false;
    const char* m_depthSource = "none";

    // Depth scale measured from the buffer itself. The engine moves its near
    // plane all the time, so "1 - depth" has no fixed meaning in metres; the
    // nearest surfaces in view are used as the reference instead.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
    D3D11_TEXTURE2D_DESC m_stagingDesc{};
    std::uint32_t m_measureCounter = 0;
    float m_reference = 0.0F;       // smoothed near/distance of the nearest surfaces
    float m_lastNearest = 0.0F;     // 99th percentile, whole frame
    float m_lastMedian = 0.0F;
    float m_lastWeaponNearest = 0.0F;  // 99th percentile, lower middle (weapon area)
    float m_lastWeaponMedian = 0.0F;
    std::uint32_t m_missLogs = 0;
};

}  // namespace a3vr
