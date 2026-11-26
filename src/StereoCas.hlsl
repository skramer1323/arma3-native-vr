#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

cbuffer Parameters : register(b0) {
    uint eyeWidth;
    uint height;
    float strength;
    uint showRadius;
    float4 projection[2];
    float3 radii;
    uint limitSharpening;
};

Texture2D<float4> inputImage : register(t0);
RWTexture2D<float4> outputImage : register(u0);
static uint eyeIndex;

AF3 CasLoad(ASU2 p)
{
    const int2 bounds = int2(eyeWidth - 1, height - 1);
    const int2 eyePixel = clamp(int2(p), int2(0, 0), bounds);
    return inputImage.Load(int3(eyePixel + int2(eyeIndex * eyeWidth, 0), 0)).rgb;
}

void CasInput(inout AF1 r, inout AF1 g, inout AF1 b)
{
    // Gamma 2 approximation for the UNORM game image, as described in ffx_cas.h.
    r *= r;
    g *= g;
    b *= b;
}

#include "ffx_cas.h"

[numthreads(8, 8, 1)]
void Sharpen(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= eyeWidth || thread.y >= height) return;
    eyeIndex = thread.z;
    const uint2 pixel = uint2(eyeIndex * eyeWidth + thread.x, thread.y);
    const float4 center = inputImage.Load(int3(pixel, 0));
    float3 color = center.rgb;
    float radial = 0.0;
    float thickness = 0.0;
    float sharpness = strength;
    [branch] if (limitSharpening) {
        const float4 eye = projection[eyeIndex];
        const float2 tangentStep = 2.0 / float2(eyeWidth, height) / eye.xy;
        const float2 ndc = ((float2(thread.xy) + 0.5) * (2.0 / float2(eyeWidth, height)) - 1.0) *
                           float2(1.0, -1.0);
        radial = length((ndc - eye.zw) / eye.xy);
        thickness = 2.0 * max(tangentStep.x, tangentStep.y);
        sharpness *= 1.0 - smoothstep(radii.x, radii.y, radial);
    }
    [branch] if (sharpness > 0.0) {
        uint4 const0, const1;
        CasSetup(const0, const1, strength, eyeWidth, height, eyeWidth, height);
        float3 sharpened;
        CasFilter(sharpened.r, sharpened.g, sharpened.b, thread.xy, const0, const1, true);
        color = lerp(color, sqrt(saturate(sharpened)), sharpness);
    }
    [branch] if (showRadius) {
        const float ringWeight = 1.0 - smoothstep(thickness * 0.4, thickness, abs(radial - radii.x));
        color = lerp(color, float3(0.1, 1.0, 0.25), ringWeight);
    }
    outputImage[pixel] = float4(color, center.a);
}
