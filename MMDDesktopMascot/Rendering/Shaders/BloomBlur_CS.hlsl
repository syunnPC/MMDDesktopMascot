// BloomBlur_CS.hlsl
// Separable Gaussian blur via compute. Direction selected via constant.

Texture2D<float4> g_src : register(t0);
RWTexture2D<float4> g_dst : register(u0);

SamplerState g_linearClamp : register(s0);

cbuffer BlurCB : register(b0)
{
    float4 g_params; // x=invW, y=invH, z=horizontal(1.0)/vertical(0.0), w=unused
};

static const float kWeights[9] = {
    0.0162162162f,
    0.0540540541f,
    0.1216216216f,
    0.1945945946f,
    0.2270270270f,
    0.1945945946f,
    0.1216216216f,
    0.0540540541f,
    0.0162162162f
};

static const float kOffsets[9] = {
    -4.0f, -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f, 4.0f
};

[numthreads(8, 8, 1)]
void MainCS(uint2 tid : SV_DispatchThreadID)
{
    uint2 dstSize;
    g_dst.GetDimensions(dstSize.x, dstSize.y);
    if (any(tid >= dstSize))
        return;

    float2 uv = (tid + 0.5f) / float2(dstSize);
    float2 texel = float2(g_params.x, g_params.y);
    bool horizontal = (g_params.z > 0.5f);

    float4 sum = 0.0;
    [unroll]
    for (int i = 0; i < 9; ++i)
    {
        float2 offset = horizontal ? float2(kOffsets[i] * texel.x, 0.0f) : float2(0.0f, kOffsets[i] * texel.y);
        sum += g_src.SampleLevel(g_linearClamp, uv + offset, 0) * kWeights[i];
    }

    g_dst[tid] = sum;
}
