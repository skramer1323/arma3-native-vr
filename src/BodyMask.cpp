#include "BodyMask.h"

#include "Log.h"

#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <bit>
#include <cstring>
#include <string>
#include <vector>

namespace a3vr {
namespace {
using Microsoft::WRL::ComPtr;

bool SameParameter(const D3D11_SIGNATURE_PARAMETER_DESC& left,
                   const D3D11_SIGNATURE_PARAMETER_DESC& right)
{
    return _stricmp(left.SemanticName, right.SemanticName) == 0 &&
           left.SemanticIndex == right.SemanticIndex && left.Register == right.Register &&
           left.Mask == right.Mask && left.ComponentType == right.ComponentType &&
           left.SystemValueType == right.SystemValueType && left.Stream == right.Stream;
}

bool ValidGeometrySignature(ID3DBlob* code, const std::vector<D3D11_SIGNATURE_PARAMETER_DESC>& fields)
{
    ComPtr<ID3D11ShaderReflection> geometry;
    D3D11_SHADER_DESC desc{};
    if (FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_ID3D11ShaderReflection,
                         reinterpret_cast<void**>(geometry.GetAddressOf()))) ||
        FAILED(geometry->GetDesc(&desc)) || desc.InputParameters != fields.size() ||
        desc.OutputParameters != fields.size() + 1) {
        return false;
    }
    for (UINT i = 0; i < fields.size(); ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC input{}, output{};
        if (FAILED(geometry->GetInputParameterDesc(i, &input)) ||
            FAILED(geometry->GetOutputParameterDesc(i, &output)) ||
            !SameParameter(fields[i], input) || !SameParameter(fields[i], output)) {
            return false;
        }
    }
    D3D11_SIGNATURE_PARAMETER_DESC clip{};
    return SUCCEEDED(geometry->GetOutputParameterDesc(static_cast<UINT>(fields.size()), &clip)) &&
           clip.SystemValueType == D3D_NAME_CLIP_DISTANCE;
}

constexpr char kClipFunction[] = R"(
cbuffer Body : register(b0) {
    float4 projection; // p00, p11, p20, p21
    float4 eyeOffset;  // p30, p31, p33
    float4 bodyRight;
    float4 bodyUp;
    float4 bodyForward;
};
float BodyClip(float4 p) {
    float z = p.w - eyeOffset.z;
    float2 xy = (p.xy - z * projection.zw - eyeOffset.xy) / projection.xy;
    float3 view = float3(xy, z);
    float x = dot(bodyRight.xyz, view);
    float y = dot(bodyUp.xyz, view);
    float f = dot(bodyForward.xyz, view);
    // Keep the sides of the upper body, everything below the waist, and arms
    // that are extended in front of the chest.
    return max(max(abs(x) - 0.21f, -y - 0.59f), f - 0.21f);
}
)";

}  // namespace

bool BuildBodyMaskShader(ID3D11Device* device, std::span<const unsigned char> vertexCode,
                         ID3D11GeometryShader** shader)
{
    ComPtr<ID3D11ShaderReflection> vertex;
    D3D11_SHADER_DESC desc{};
    if (FAILED(D3DReflect(vertexCode.data(), vertexCode.size(), IID_ID3D11ShaderReflection,
                         reinterpret_cast<void**>(vertex.GetAddressOf()))) ||
        FAILED(vertex->GetDesc(&desc)) || desc.OutputParameters == 0) {
        return false;
    }

    std::vector<D3D11_SIGNATURE_PARAMETER_DESC> fields(desc.OutputParameters);
    std::string members, copy, position;
    for (UINT i = 0; i < desc.OutputParameters; ++i) {
        auto& field = fields[i];
        if (FAILED(vertex->GetOutputParameterDesc(i, &field)) || field.Mask == 0 ||
            field.Stream != 0 || (field.SystemValueType != D3D_NAME_UNDEFINED &&
                                  field.SystemValueType != D3D_NAME_POSITION)) {
            return false;
        }

        const char* type = nullptr;
        switch (field.ComponentType) {
        case D3D_REGISTER_COMPONENT_FLOAT32: type = "float"; break;
        case D3D_REGISTER_COMPONENT_UINT32: type = "nointerpolation uint"; break;
        case D3D_REGISTER_COMPONENT_SINT32: type = "nointerpolation int"; break;
        default: return false;
        }
        const int components = std::popcount(static_cast<unsigned>(field.Mask));
        const std::string name = "v" + std::to_string(i);
        members += type + (components == 1 ? std::string{} : std::to_string(components)) +
            " " + name + " : " + field.SemanticName + std::to_string(field.SemanticIndex) + ";\n";
        copy += "v." + name + " = vertices[i]." + name + ";\n";
        if (field.SystemValueType == D3D_NAME_POSITION && components == 4) {
            position = name;
        }
    }
    if (position.empty()) {
        return false;
    }

    const std::string source = std::string(kClipFunction) +
        "struct In {" + members + "};\n"
        "struct Out {" + members + "float clip : SV_ClipDistance0; };\n"
        "[maxvertexcount(3)] void GS(triangle In vertices[3], inout TriangleStream<Out> stream) {\n"
        "[unroll] for (uint i = 0; i < 3; ++i) { Out v;\n" + copy +
        "v.clip = BodyClip(v." + position + "); stream.Append(v); } stream.RestartStrip(); }";

    ComPtr<ID3DBlob> compiled, errors;
    if (FAILED(D3DCompile(source.data(), source.size(), "Arma VR body mask", nullptr, nullptr, "GS",
                          "gs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &compiled, &errors))) {
        Log("Body mask shader compile failed: %s",
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no diagnostics");
        return false;
    }
    return ValidGeometrySignature(compiled.Get(), fields) &&
           SUCCEEDED(device->CreateGeometryShader(compiled->GetBufferPointer(), compiled->GetBufferSize(),
                                                  nullptr, shader));
}

}  // namespace a3vr