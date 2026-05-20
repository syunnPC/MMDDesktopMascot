// SSAO_MSAA_CS.hlsl
// Full-resolution SSAO from MSAA hardware depth.

Texture2DMS<float> g_sceneDepthMsaa : register(t8);
RWTexture2D<float> g_outAO : register(u0);

cbuffer SSAOCB : register(b0)
{
    row_major float4x4 g_invProj;
    float2 g_invScreenSize;
    float g_radius;
    float g_bias;
    float g_intensity;
    float g_power;
    float g_useProxyDepth;
    float _pad0;
};

#define NUM_SAMPLES 16

static const float2 g_sampleOffsets[NUM_SAMPLES] = {
    float2( 0.5381,  0.1856),
    float2(-0.4319,  0.2523),
    float2( 0.1807, -0.4521),
    float2(-0.0720, -0.7819),
    float2( 0.6145, -0.3988),
    float2(-0.7712, -0.2414),
    float2( 0.3724,  0.6869),
    float2(-0.3177,  0.7028),
    float2( 0.8664,  0.1326),
    float2(-0.9090,  0.1513),
    float2( 0.0905,  0.9530),
    float2(-0.1100, -0.9850),
    float2( 0.6462,  0.5811),
    float2(-0.5431, -0.6714),
    float2( 0.2827, -0.8912),
    float2(-0.7631,  0.5299)
};

float ReadDepthAtPixel(int2 pixel, int2 size)
{
    pixel = clamp(pixel, int2(0, 0), size - 1);

    uint width;
    uint height;
    uint sampleCount;
    g_sceneDepthMsaa.GetDimensions(width, height, sampleCount);

    float depth = 0.0f;
    [loop]
    for (uint sample = 0; sample < sampleCount; ++sample)
    {
        depth += g_sceneDepthMsaa.Load(pixel, sample);
    }
    return depth / max((float)sampleCount, 1.0f);
}

bool IsValidDepth(float depth)
{
    return depth > 1.0e-5f && depth < 0.99999f;
}

float3 ViewSpacePositionFromDepth(float2 uv, float rawDepth)
{
    float2 clipPos = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clipH = float4(clipPos, rawDepth, 1.0f);
    float4 viewPos = mul(clipH, g_invProj);
    viewPos.xyz /= viewPos.w;
    return viewPos.xyz;
}

float3 SafeViewSpacePosition(int2 pixel, int2 size, float3 fallbackPos)
{
    float depth = ReadDepthAtPixel(pixel, size);
    if (!IsValidDepth(depth))
    {
        return fallbackPos;
    }

    float2 uv = (float2(pixel) + 0.5f) / float2(size);
    return ViewSpacePositionFromDepth(uv, depth);
}

float3 ReconstructNormalFromDepth(int2 pixel, int2 size, float3 centerPos)
{
    float3 left = SafeViewSpacePosition(pixel + int2(-1, 0), size, centerPos);
    float3 right = SafeViewSpacePosition(pixel + int2(1, 0), size, centerPos);
    float3 up = SafeViewSpacePosition(pixel + int2(0, -1), size, centerPos);
    float3 down = SafeViewSpacePosition(pixel + int2(0, 1), size, centerPos);

    float3 dx = (abs(right.z - centerPos.z) < abs(centerPos.z - left.z))
        ? (right - centerPos)
        : (centerPos - left);
    float3 dy = (abs(down.z - centerPos.z) < abs(centerPos.z - up.z))
        ? (down - centerPos)
        : (centerPos - up);

    float3 normal = cross(dx, dy);
    float lenSq = dot(normal, normal);
    if (lenSq <= 1.0e-8f)
    {
        return normalize(-centerPos);
    }

    normal *= rsqrt(lenSq);
    if (dot(normal, -centerPos) < 0.0f)
    {
        normal = -normal;
    }
    return normal;
}

float InterleavedGradientNoise(uint2 p)
{
    return frac(52.9829189f * frac(dot(float2(p), float2(0.06711056f, 0.00583715f))));
}

float2 Rotate(float2 v, float angle)
{
    float s;
    float c;
    sincos(angle, s, c);
    return float2(v.x * c - v.y * s, v.x * s + v.y * c);
}

[numthreads(8, 8, 1)]
void MainCS(uint2 tid : SV_DispatchThreadID)
{
    uint2 outputSize;
    g_outAO.GetDimensions(outputSize.x, outputSize.y);
    if (any(tid >= outputSize))
    {
        return;
    }

    int2 pixel = int2(tid);
    int2 size = int2(outputSize);
    float2 uv = (float2(tid) + 0.5f) / float2(outputSize);

    float depth = ReadDepthAtPixel(pixel, size);
    if (!IsValidDepth(depth))
    {
        g_outAO[tid] = 1.0f;
        return;
    }

    float3 centerPos = ViewSpacePositionFromDepth(uv, depth);
    float3 centerNormal = ReconstructNormalFromDepth(pixel, size, centerPos);
    float viewZ = max(centerPos.z, 0.05f);
    float radius = max(g_radius, 1.0e-4f);
    float radiusSq = radius * radius;

    float invProjX = max(abs(g_invProj._11), 1.0e-4f);
    float invProjY = max(abs(g_invProj._22), 1.0e-4f);
    float2 projScale = float2(1.0f / invProjX, 1.0f / invProjY);
    float2 radiusUv = 0.5f * radius * projScale / viewZ;
    radiusUv = clamp(radiusUv, g_invScreenSize * 1.5f, g_invScreenSize * 48.0f);

    float rotation = InterleavedGradientNoise(tid) * 6.2831853f;
    float occlusion = 0.0f;
    float weight = 0.0f;

    [unroll]
    for (int i = 0; i < NUM_SAMPLES; ++i)
    {
        float sampleScale = (float(i) + 1.0f) / float(NUM_SAMPLES);
        sampleScale = lerp(0.18f, 1.0f, sampleScale * sampleScale);
        float2 sampleUv = uv + Rotate(g_sampleOffsets[i], rotation) * radiusUv * sampleScale;
        int2 samplePixel = int2(sampleUv * float2(outputSize));
        float sampleDepth = ReadDepthAtPixel(samplePixel, size);
        if (!IsValidDepth(sampleDepth))
        {
            continue;
        }

        samplePixel = clamp(samplePixel, int2(0, 0), size - 1);
        float2 sampleUvCenter = (float2(samplePixel) + 0.5f) / float2(size);
        float3 samplePos = ViewSpacePositionFromDepth(sampleUvCenter, sampleDepth);
        float3 v = samplePos - centerPos;
        float distSq = dot(v, v);
        if (distSq <= 1.0e-8f || distSq >= radiusSq)
        {
            continue;
        }

        float invDist = rsqrt(distSq);
        float nDotV = saturate(dot(centerNormal, v) * invDist - g_bias);
        float range = saturate(1.0f - distSq / radiusSq);
        range = range * range * (3.0f - 2.0f * range);
        occlusion += nDotV * range;
        weight += 1.0f;
    }

    float ao = 1.0f - (occlusion / max(weight, 1.0f)) * max(g_intensity, 0.0f);
    ao = pow(saturate(ao), max(g_power, 0.25f));
    g_outAO[tid] = ao;
}
