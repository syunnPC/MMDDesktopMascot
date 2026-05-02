// SSAO_CS.hlsl
// Screen-space ambient occlusion using depth derivatives for normals.
// Reconstructed from CS_ScreenSpaceAO_Resolve_Reconstructed.hlsl patterns.

Texture2D<float> g_sceneDepth : register(t0);
RWTexture2D<float> g_outAO : register(u0);
SamplerState g_pointClamp : register(s1);

cbuffer SSAOCB : register(b0)
{
    row_major float4x4 g_invProj;
};

#define AO_RADIUS 0.5f
#define AO_INTENSITY 1.2f
#define NUM_SAMPLES 8

static const float2 g_sampleOffsets[NUM_SAMPLES] = {
    float2(0.355512, -0.709318),
    float2(0.534336, 0.283099),
    float2(-0.876571, 0.656315),
    float2(-0.074451, -0.916279),
    float2(0.101453, 0.687098),
    float2(-0.588007, -0.356506),
    float2(0.365531, 0.155641),
    float2(-0.259657, 0.741338)
};

float3 ViewSpacePositionFromDepth(float2 uv, float rawDepth)
{
    // Map UV [0,1] to clip space [-1,1] (flip Y because UV v=0 is top, clip Y=1 is top)
    float2 clipPos = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clipH = float4(clipPos, rawDepth, 1.0f);
    float4 viewPos = mul(clipH, g_invProj);
    viewPos.xyz /= viewPos.w;
    return viewPos.xyz;
}

float3 ReconstructNormalFromDepth(float2 uv, float2 texelSize)
{
    float d0 = g_sceneDepth.SampleLevel(g_pointClamp, uv, 0);
    float d1 = g_sceneDepth.SampleLevel(g_pointClamp, uv + float2(texelSize.x, 0), 0);
    float d2 = g_sceneDepth.SampleLevel(g_pointClamp, uv + float2(0, texelSize.y), 0);

    float3 v0 = float3(0, 0, d0);
    float3 v1 = float3(texelSize.x, 0, d1);
    float3 v2 = float3(0, texelSize.y, d2);

    float3 n = cross(v1 - v0, v2 - v0);
    return normalize(n);
}

[numthreads(8, 8, 1)]
void MainCS(uint2 tid : SV_DispatchThreadID)
{
    uint2 outputSize;
    g_outAO.GetDimensions(outputSize.x, outputSize.y);
    if (any(tid >= outputSize))
        return;

    float2 uv = (tid + 0.5f) / float2(outputSize);
    float2 texelSize = 1.0f / float2(outputSize);

    float depth = g_sceneDepth.SampleLevel(g_pointClamp, uv, 0);
    if (depth >= 1.0f || depth <= 0.0f)
    {
        g_outAO[tid] = 1.0f;
        return;
    }

    float3 centerPos = ViewSpacePositionFromDepth(uv, depth);
    float3 centerNormal = ReconstructNormalFromDepth(uv, texelSize);

    float occlusion = 0.0f;
    float radius = AO_RADIUS;

    [unroll]
    for (int i = 0; i < NUM_SAMPLES; ++i)
    {
        float2 sampleOffset = g_sampleOffsets[i] * radius * texelSize;
        float2 sampleUV = uv + sampleOffset;
        float sampleDepth = g_sceneDepth.SampleLevel(g_pointClamp, sampleUV, 0);

        float3 samplePos = ViewSpacePositionFromDepth(sampleUV, sampleDepth);
        float3 diff = samplePos - centerPos;
        float dist = length(diff);
        float3 diffNorm = diff / max(dist, 1e-5f);

        float dotNL = saturate(dot(centerNormal, diffNorm));
        float rangeCheck = saturate(1.0f - dist * dist / (radius * radius));
        occlusion += dotNL * rangeCheck;
    }

    float ao = 1.0f - (occlusion / NUM_SAMPLES) * AO_INTENSITY;
    g_outAO[tid] = saturate(ao);
}
