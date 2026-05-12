Texture2D<float4> g_sceneTex : register(t0);

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

float Luma(float3 color)
{
    return dot(color, float3(0.299f, 0.587f, 0.114f));
}

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

float3 StraightDisplayColor(int2 pixel)
{
    const float4 c = SceneAt(pixel);
    const float alpha = saturate(c.a);
    float3 color = (alpha > 1.0e-5f) ? (c.rgb / alpha) : float3(0.0f, 0.0f, 0.0f);
    color = max(color, float3(0.0f, 0.0f, 0.0f));

    const float peak = max(color.r, max(color.g, color.b));
    return color / (1.0f + peak);
}

float AlphaAt(int2 pixel)
{
    return saturate(SceneAt(pixel).a);
}

float EdgeMetric(int2 a, int2 b)
{
    const float3 ca = StraightDisplayColor(a);
    const float3 cb = StraightDisplayColor(b);
    const float lumaDelta = abs(Luma(ca) - Luma(cb));
    const float3 colorDelta = abs(ca - cb);
    const float channelDelta = max(colorDelta.r, max(colorDelta.g, colorDelta.b));
    const float alphaDelta = abs(AlphaAt(a) - AlphaAt(b));
    return max(max(lumaDelta, channelDelta * 0.45f), alphaDelta * 0.65f);
}

float4 PSMain(PSIn i) : SV_TARGET
{
    const int2 pixel = int2(i.pos.xy);

    float4 delta = 0.0f;
    delta.x = EdgeMetric(pixel, pixel + int2(-1, 0));
    delta.y = EdgeMetric(pixel, pixel + int2(0, -1));
    float2 edges = step(float2(g_edgeThreshold, g_edgeThreshold), delta.xy);

    if (dot(edges, float2(1.0f, 1.0f)) <= 0.0f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    delta.z = EdgeMetric(pixel, pixel + int2(1, 0));
    delta.w = EdgeMetric(pixel, pixel + int2(0, 1));
    float2 maxDelta = max(delta.xy, delta.zw);

    const float leftLeft = EdgeMetric(pixel + int2(-1, 0), pixel + int2(-2, 0));
    const float topTop = EdgeMetric(pixel + int2(0, -1), pixel + int2(0, -2));
    maxDelta = max(maxDelta, float2(leftLeft, topTop));

    const float finalDelta = max(maxDelta.x, maxDelta.y);
    edges *= step(finalDelta, max(g_localContrast, 1.0f) * delta.xy);

    return float4(edges, 0.0f, 0.0f);
}
