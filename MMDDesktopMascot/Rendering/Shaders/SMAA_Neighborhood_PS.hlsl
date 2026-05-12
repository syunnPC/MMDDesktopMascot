Texture2D<float4> g_sceneTex : register(t0);
Texture2D<float4> g_blendTex : register(t1);

SamplerState g_linearClamp : register(s0);
SamplerState g_pointClamp : register(s1);

cbuffer SmaaCB : register(b0)
{
    float2 g_invScreenSize;
    float g_edgeThreshold;
    float g_localContrast;
    float g_maxSearchSteps;
    float g_cornerRounding;
    float2 _pad0;
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

int2 ScreenSize()
{
    return max(int2(1, 1), int2((1.0f / g_invScreenSize) + 0.5f));
}

int2 ClampPixel(int2 pixel)
{
    const int2 maxPixel = ScreenSize() - 1;
    return min(max(pixel, int2(0, 0)), maxPixel);
}

float4 SceneAt(int2 pixel)
{
    return g_sceneTex.Load(int3(ClampPixel(pixel), 0));
}

float4 WeightsAt(int2 pixel)
{
    return g_blendTex.Load(int3(ClampPixel(pixel), 0));
}

float4 PSMain(PSIn i) : SV_TARGET
{
    const int2 pixel = int2(i.pos.xy);
    const float4 center = SceneAt(pixel);
    const float4 weights = WeightsAt(pixel);
    const float horizontalWeight = weights.r + weights.b;
    const float verticalWeight = weights.g + weights.a;

    if (max(horizontalWeight, verticalWeight) <= 0.001f)
    {
        return center;
    }

    if (horizontalWeight >= verticalWeight)
    {
        const float amount = saturate(horizontalWeight);
        const float2 blend = weights.rb / max(horizontalWeight, 1.0e-4f);
        const float4 left = SceneAt(pixel + int2(-1, 0));
        const float4 right = SceneAt(pixel + int2(1, 0));
        const float4 neighbor = left * blend.x + right * blend.y;
        return lerp(center, neighbor, amount);
    }

    const float amount = saturate(verticalWeight);
    const float2 blend = weights.ga / max(verticalWeight, 1.0e-4f);
    const float4 up = SceneAt(pixel + int2(0, -1));
    const float4 down = SceneAt(pixel + int2(0, 1));
    const float4 neighbor = up * blend.x + down * blend.y;
    return lerp(center, neighbor, amount);
}
