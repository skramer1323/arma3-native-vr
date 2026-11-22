#pragma once

#include <d3d11.h>
#include <array>
#include <filesystem>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace a3vr {

class DlssBalanced final {
public:
    struct Size {
        UINT width = 0;
        UINT height = 0;
        bool operator==(const Size&) const = default;
    };

    struct EyeFrame {
        ID3D11Texture2D* color = nullptr;
        ID3D11Texture2D* depth = nullptr;
        ID3D11Texture2D* motionPixels = nullptr;
        ID3D11Texture2D* output = nullptr;
    };

    bool Initialize(ID3D11Device* device, const std::filesystem::path& moduleDirectory);
    Size OptimalInput(Size output);
    bool Prepare(ID3D11DeviceContext* context, Size input, Size output);
    bool EvaluatePair(ID3D11DeviceContext* context, const std::array<EyeFrame, 2>& eyes,
                      float jitterX, float jitterY, bool reset);
    void Shutdown(ID3D11Device* device);

private:
    void ReleaseFeatures();

    bool m_initializationAttempted = false;
    bool m_initialized = false;
    bool m_available = false;
    NVSDK_NGX_Parameter* m_parameters = nullptr;
    std::array<NVSDK_NGX_Handle*, 2> m_features{};
    Size m_input{}, m_output{};
    Size m_rejectedInput{}, m_rejectedOutput{};
};

}  // namespace a3vr

