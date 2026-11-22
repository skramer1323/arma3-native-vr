#include "DlssBalanced.h"
#ifdef ARMA_HAS_DLSS
#include "Log.h"

#include <nvsdk_ngx_helpers.h>

namespace a3vr {
namespace {

constexpr auto kQuality = NVSDK_NGX_PerfQuality_Value_Balanced;

struct InputRange {
    DlssBalanced::Size optimal;
    DlssBalanced::Size minimum;
    DlssBalanced::Size maximum;
};

InputRange GetInputRange(NVSDK_NGX_Parameter* params, DlssBalanced::Size output)
{
    InputRange sizes{};
    float sharpness = 0.0F;
    const auto result = NGX_DLSS_GET_OPTIMAL_SETTINGS(params, output.width, output.height, kQuality,
        &sizes.optimal.width, &sizes.optimal.height, &sizes.maximum.width, &sizes.maximum.height,
        &sizes.minimum.width, &sizes.minimum.height, &sharpness);
    return NVSDK_NGX_SUCCEED(result) ? sizes : InputRange{};
}

}  // namespace

bool DlssBalanced::Initialize(ID3D11Device* device, const std::filesystem::path& directory)
{
    if (m_initializationAttempted || !device) {
        return m_available;
    }
    m_initializationAttempted = true;
    const auto directoryName = directory.wstring();
    const auto result = NVSDK_NGX_D3D11_Init_with_ProjectID("93cc48cf-a3f4-4a3f-91ae-8be04f01cd94",
        NVSDK_NGX_ENGINE_TYPE_CUSTOM, "Arma3NativeVR", directoryName.c_str(), device);
    if (NVSDK_NGX_FAILED(result)) {
        Log("DLSS Balanced: NGX init failed (0x%08x)", static_cast<unsigned>(result));
        return false;
    }
    m_initialized = true;
    const auto capability = NVSDK_NGX_D3D11_GetCapabilityParameters(&m_parameters);
    int available = 0;
    if (NVSDK_NGX_SUCCEED(capability) && m_parameters) {
        m_parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    }
    m_available = available != 0;
    Log("DLSS Balanced: NGX Super Resolution available=%d (capability 0x%08x)",
        available, static_cast<unsigned>(capability));
    return m_available;
}

DlssBalanced::Size DlssBalanced::OptimalInput(Size output)
{
    return m_available ? GetInputRange(m_parameters, output).optimal : Size{};
}

bool DlssBalanced::Prepare(ID3D11DeviceContext* context, Size input, Size output)
{
    if (!context || !m_available || input.width == 0 || input.height == 0 ||
        output.width == 0 || output.height == 0 ||
        (input == m_rejectedInput && output == m_rejectedOutput)) {
        return false;
    }
    if (m_features[0] && m_features[1] && input == m_input && output == m_output) {
        return true;
    }
    ReleaseFeatures();
    const InputRange range = GetInputRange(m_parameters, output);
    if (range.optimal.width == 0 || input.width < range.minimum.width ||
        input.height < range.minimum.height || input.width > range.maximum.width ||
        input.height > range.maximum.height) {
        Log("DLSS Balanced: input %ux%u outside NGX range %ux%u..%ux%u (output %ux%u)",
            input.width, input.height, range.minimum.width, range.minimum.height,
            range.maximum.width, range.maximum.height, output.width, output.height);
        m_rejectedInput = input;
        m_rejectedOutput = output;
        return false;
    }

    for (auto*& feature : m_features) {
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = input.width;
        create.Feature.InHeight = input.height;
        create.Feature.InTargetWidth = output.width;
        create.Feature.InTargetHeight = output.height;
        create.Feature.InPerfQualityValue = kQuality;
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                      NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        const auto result = NGX_D3D11_CREATE_DLSS_EXT(context, &feature, m_parameters, &create);
        if (NVSDK_NGX_FAILED(result) || !feature) {
            Log("DLSS Balanced: feature creation failed (0x%08x)", static_cast<unsigned>(result));
            ReleaseFeatures();
            m_rejectedInput = input;
            m_rejectedOutput = output;
            return false;
        }
    }
    m_input = input;
    m_output = output;
    Log("DLSS Balanced: two independent eyes, %ux%u -> %ux%u each",
        input.width, input.height, output.width, output.height);
    return true;
}

bool DlssBalanced::EvaluatePair(ID3D11DeviceContext* context, const std::array<EyeFrame, 2>& eyes,
                                float jitterX, float jitterY, bool reset)
{
    if (!context || !m_features[0] || !m_features[1]) {
        return false;
    }
    for (std::size_t eye = 0; eye < eyes.size(); ++eye) {
        const auto& resources = eyes[eye];
        if (!resources.color || !resources.depth || !resources.motionPixels || !resources.output) {
            return false;
        }
        NVSDK_NGX_D3D11_DLSS_Eval_Params params{};
        params.Feature.pInColor = resources.color;
        params.Feature.pInOutput = resources.output;
        params.pInDepth = resources.depth;
        params.pInMotionVectors = resources.motionPixels;
        params.InJitterOffsetX = jitterX;
        params.InJitterOffsetY = jitterY;
        params.InRenderSubrectDimensions = {m_input.width, m_input.height};
        params.InMVScaleX = params.InMVScaleY = 1.0F;
        params.InPreExposure = params.InExposureScale = 1.0F;
        params.InReset = reset ? 1 : 0;
        const auto result = NGX_D3D11_EVALUATE_DLSS_EXT(context, m_features[eye], m_parameters, &params);
        if (NVSDK_NGX_FAILED(result)) {
            Log("DLSS Balanced: evaluation failed for eye %zu (0x%08x)",
                eye, static_cast<unsigned>(result));
            const Size rejectedInput = m_input;
            const Size rejectedOutput = m_output;
            ReleaseFeatures();
            m_rejectedInput = rejectedInput;
            m_rejectedOutput = rejectedOutput;
            return false;
        }
    }
    return true;
}

void DlssBalanced::ReleaseFeatures()
{
    for (auto*& feature : m_features) {
        if (feature) {
            NVSDK_NGX_D3D11_ReleaseFeature(feature);
            feature = nullptr;
        }
    }
    m_input = m_output = {};
}

void DlssBalanced::Shutdown(ID3D11Device* device)
{
    ReleaseFeatures();
    if (m_parameters) {
        NVSDK_NGX_D3D11_DestroyParameters(m_parameters);
        m_parameters = nullptr;
    }
    if (m_initialized) {
        NVSDK_NGX_D3D11_Shutdown1(device);
    }
    m_initializationAttempted = m_initialized = m_available = false;
    m_rejectedInput = m_rejectedOutput = {};
}

}  // namespace a3vr
#else
namespace a3vr {

bool DlssBalanced::Initialize(ID3D11Device*, const std::filesystem::path&) { return false; }
DlssBalanced::Size DlssBalanced::OptimalInput(Size) { return {}; }
bool DlssBalanced::Prepare(ID3D11DeviceContext*, Size, Size) { return false; }
bool DlssBalanced::EvaluatePair(ID3D11DeviceContext*, const std::array<EyeFrame, 2>&, float, float, bool)
{
    return false;
}
void DlssBalanced::Shutdown(ID3D11Device*) {}

}  // namespace a3vr
#endif

