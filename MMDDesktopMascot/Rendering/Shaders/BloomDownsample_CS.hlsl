// BloomDownsample_CS.hlsl
// HDR threshold + 13-tap bilinear downsample in one compute dispatch.

Texture2D<float4> g_src : register(t0);
RWTexture2D<float4> g_dst : register(u0);

SamplerState g_linearClamp : register(s0);

cbuffer BloomCB : register(b0)
{
    float3 g_params; // x=threshold, y=invW, z=invH
};

[numthreads(8, 8, 1)]
void MainCS(uint2 tid : SV_DispatchThreadID)
{
    uint2 dstSize;
    g_dst.GetDimensions(dstSize.x, dstSize.y);
    if (any(tid >= dstSize))
        return;

    float2 uv = (tid + 0.5f) * float2(g_params.y, g_params.z);

    // 4x4 bicubic-like downsample using bilinear taps
    float4 sum = 0.0;
    float2 texel = float2(g_params.y, g_params.z);

    // 13-tap pattern optimized for 4:1 downsample
    float w0 = 1.0f / 4.0f;
    float w1 = 1.0f / 8.0f;
    float w2 = 1.0f / 16.0f;

    sum += g_src.SampleLevel(g_linearClamp, uv, 0) * w0;
    sum += g_src.SampleLevel(g_linearClamp, uv + float2(-texel.x, -texel.y), 0) * w2;
    sum += g_src.SampleLevel(g_linearClamp, uv + float2( texel.x, -texel.y), 0) * w2;
    sum += g_src.SampleLevel(g_linearClamp, uv + float2(-texel.x,  texel.y), 0) * w2;
    sum += g_src.SampleLevel(g_linearClamp, uv + float2( texel.x,  texel.y), 0) * w2;
    sum += g_src.SampleLevel(g_linearClamp, uv + float2(-texel.x * 2.0f, 0), 0) * w1;
    sum += g_src.SampleLevel(g_linearClamp, uv + float2( texel.x * 2.0f, 0), 0) * w1;
    sum += g_src.SampleLevel(g_linearClamp, uv + float2(0, -texel.y * 2.0f), 0) * w1;
    sum += g_src.SampleLevel(g_linearClamp, uv + float2(0,  texel.y * 2.0f), 0) * w1;

    // Threshold bright pixels
    float lum = dot(sum.rgb, float3(0.2126, 0.7152, 0.0722));
    float threshold = g_params.x;
    float knee = threshold * 0.5f;
    float soft = lum - threshold + knee;
    soft = clamp(soft, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * knee + 1e-5f);
    float contribution = max(soft, lum - threshold) / max(lum, 1e-5f);

    g_dst[tid] = float4(sum.rgb * contribution, 1.0f);
}
