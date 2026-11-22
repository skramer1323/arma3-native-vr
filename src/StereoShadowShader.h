#pragma once

namespace a3vr {

inline constexpr char kStereoShadowShader[] = R"hlsl(
Texture2D<float> sceneDepth : register(t0);
Texture2D<float> shadowMap : register(t15);
SamplerComparisonState shadowSampler : register(s15);

cbuffer Scene : register(b0) {
    float4 shadowTexel : packoffset(c6);
};
cbuffer Cascade : register(b13) {
    float4 lightX : packoffset(c45);
    float4 lightY : packoffset(c46);
    float4 lightZ : packoffset(c47);
    float4 cascadeDepth : packoffset(c49);
};
cbuffer Stereo : register(b12) {
    float4 projection[4]; // P00, P11, M20, M21; M30, M31, M32, M33 per eye
};

float4 main(float4 pixel : SV_Position) : SV_Target {
    uint width, height;
    sceneDepth.GetDimensions(width, height);
    uint eye = pixel.x >= width * 0.5 ? 1 : 0;
    float z = sceneDepth.Load(int3(int2(pixel.xy), 0));
    if (z < cascadeDepth.x || z >= cascadeDepth.y)
        discard;

    // The R32 buffer stores the unshifted view-space Z. Undo this eye's projection
    // to return to the engine's view space, which the cascade matrices expect.
    float2 eyeUv = float2(pixel.x * 2.0 / width - eye, pixel.y / height);
    float2 ndc = eyeUv * float2(2, -2) + float2(-1, 1);
    float4 p = projection[eye * 2];
    float4 q = projection[eye * 2 + 1];
    float4 viewPosition = float4((ndc * (z + q.w) - p.zw * z - q.xy) / p.xy, z, 1);
    float3 shadow = float3(dot(viewPosition, lightX), dot(viewPosition, lightY), dot(viewPosition, lightZ));

    // Seed the filter in light space so both eyes use the same pattern on a surface.
    float2 lightTexel = floor(shadow.xy / max(shadowTexel.y, 1e-7));
    float angle = frac(sin(dot(lightTexel, float2(24.37, 61.19))) * 17315.73) * 6.2831853;
    float s, c;
    sincos(angle, s, c);
    float2 u = float2(c, s) * (1.3 * shadowTexel.y);
    float2 v = float2(-u.y, u.x);
    float coverage = shadowMap.SampleCmpLevelZero(shadowSampler, shadow.xy + u + v, shadow.z)
                   + shadowMap.SampleCmpLevelZero(shadowSampler, shadow.xy + u - v, shadow.z)
                   + shadowMap.SampleCmpLevelZero(shadowSampler, shadow.xy - u + v, shadow.z)
                   + shadowMap.SampleCmpLevelZero(shadowSampler, shadow.xy - u - v, shadow.z);
    if (coverage > 0.001 && coverage < 3.999) {
        [unroll] for (int y = -1; y <= 1; ++y) {
            [unroll] for (int x = -1; x <= 1; ++x) {
                coverage += shadowMap.SampleCmpLevelZero(shadowSampler,
                    shadow.xy + (x * u + y * v) * 0.5, shadow.z);
            }
        }
        coverage *= 1.0 / 13.0;
    } else {
        coverage *= 0.25;
    }
    return float4(coverage, 0, 0, 1);
}
)hlsl";

}  // namespace a3vr
