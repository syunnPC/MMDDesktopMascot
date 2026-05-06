Texture2D<float4> g_sceneTex : register(t0);
Texture2D<float> g_depthTex : register(t3);
Texture2D<float4> g_auxNormalTex : register(t4);
Texture2D<float4> g_auxToonTex : register(t5);
Texture2D<float4> g_auxOutlineTex : register(t6);
Texture2D<float4> g_auxEdgeColorTex : register(t7);

SamplerState g_linearClamp : register(s0);
SamplerState g_pointClamp : register(s1);

cbuffer ToonPostCB : register(b0)
{
    float2 g_invScreenSize;
    float g_outlineOpacity;
    float g_outlineBaseWidth;

    float g_depthEdgeThreshold;
    float g_normalEdgeThreshold;
    float g_materialEdgeThreshold;
    float g_rimIntensity;

    float g_rimThreshold;
    float g_rimSoftness;
    float g_enableOutline;
    float g_enableRim;

    float g_useDepthTexture;
    float3 g_rimColor;

    float g_debugView;
    float3 _pad0;
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float DepthAt(float2 uv)
{
    float hardwareDepth = g_depthTex.SampleLevel(g_pointClamp, uv, 0);
    float proxyDepth = g_auxOutlineTex.SampleLevel(g_pointClamp, uv, 0).a;
    return lerp(proxyDepth, hardwareDepth, step(0.5, g_useDepthTexture));
}

float3 NormalAt(float2 uv)
{
    float3 n = g_auxNormalTex.SampleLevel(g_pointClamp, uv, 0).rgb * 2.0 - 1.0;
    float lenSq = dot(n, n);
    return (lenSq > 1.0e-5) ? n * rsqrt(lenSq) : float3(0.0, 0.0, 1.0);
}

float MaterialAt(float2 uv)
{
    return g_auxOutlineTex.SampleLevel(g_pointClamp, uv, 0).g;
}

float AlphaAt(float2 uv)
{
    return g_sceneTex.SampleLevel(g_pointClamp, uv, 0).a;
}

float3 StraightColorAt(float2 uv)
{
    float4 c = g_sceneTex.SampleLevel(g_linearClamp, uv, 0);
    return (c.a > 1.0e-5) ? c.rgb / c.a : 0.0;
}

void GetNeighborhoodCoverage(float2 uv, float2 texel, out float minAlpha, out float maxAlpha)
{
    minAlpha = 1.0;
    maxAlpha = 0.0;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float a = AlphaAt(uv + texel * float2((float)x, (float)y));
            minAlpha = min(minAlpha, a);
            maxAlpha = max(maxAlpha, a);
        }
    }
}

void GetNeighborhoodOutlineData(float2 uv, float2 texel, out float edgeMask, out float outlineWidth, out float4 edgeColor)
{
    edgeMask = 0.0;
    outlineWidth = 0.0;
    edgeColor = 0.0;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float4 outlineData = g_auxOutlineTex.SampleLevel(g_pointClamp, uv + texel * float2((float)x, (float)y), 0);
            edgeMask = max(edgeMask, outlineData.b);
            if (outlineData.r >= outlineWidth)
            {
                outlineWidth = outlineData.r;
                edgeColor = g_auxEdgeColorTex.SampleLevel(g_pointClamp, uv + texel * float2((float)x, (float)y), 0);
            }
        }
    }
}

float SobelDepth(float2 uv, float2 texel)
{
    float d00 = DepthAt(uv + texel * float2(-1.0, -1.0));
    float d10 = DepthAt(uv + texel * float2( 0.0, -1.0));
    float d20 = DepthAt(uv + texel * float2( 1.0, -1.0));
    float d01 = DepthAt(uv + texel * float2(-1.0,  0.0));
    float d21 = DepthAt(uv + texel * float2( 1.0,  0.0));
    float d02 = DepthAt(uv + texel * float2(-1.0,  1.0));
    float d12 = DepthAt(uv + texel * float2( 0.0,  1.0));
    float d22 = DepthAt(uv + texel * float2( 1.0,  1.0));

    float gx = (d20 + 2.0 * d21 + d22) - (d00 + 2.0 * d01 + d02);
    float gy = (d02 + 2.0 * d12 + d22) - (d00 + 2.0 * d10 + d20);
    return sqrt(gx * gx + gy * gy);
}

float SobelNormal(float2 uv, float2 texel)
{
    float3 n = NormalAt(uv);
    float edge = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
            {
                continue;
            }
            float3 s = NormalAt(uv + texel * float2((float)x, (float)y));
            edge = max(edge, 1.0 - saturate(dot(n, s)));
        }
    }
    return edge;
}

float SobelMaterial(float2 uv, float2 texel)
{
    float center = MaterialAt(uv);
    float edge = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
            {
                continue;
            }
            float m = MaterialAt(uv + texel * float2((float)x, (float)y));
            edge = max(edge, abs(m - center) * 255.0);
        }
    }
    return edge;
}

float4 PSMain(PSIn i) : SV_TARGET
{
    float2 uv = i.uv;
    float4 scene = g_sceneTex.SampleLevel(g_linearClamp, uv, 0);
    float alpha = scene.a;

    float4 auxToon = g_auxToonTex.SampleLevel(g_pointClamp, uv, 0);
    if (g_debugView > 0.5 && g_debugView < 6.5)
    {
        return scene;
    }

    float edgeMask;
    float outlineControl;
    float4 edgeColorData;
    GetNeighborhoodOutlineData(uv, g_invScreenSize, edgeMask, outlineControl, edgeColorData);

    float outlineWidth = max(1.0, outlineControl * 8.0 * max(g_outlineBaseWidth, 0.0));
    float2 texel = g_invScreenSize * outlineWidth;

    float minAlpha;
    float maxAlpha;
    GetNeighborhoodCoverage(uv, texel, minAlpha, maxAlpha);
    float coverage = smoothstep(0.005, 0.08, maxAlpha);
    if (coverage <= 1.0e-5)
    {
        return scene;
    }

    float depthEdge = SobelDepth(uv, texel);
    float normalEdge = SobelNormal(uv, texel);
    float materialEdge = SobelMaterial(uv, texel);
    float alphaEdge = saturate(maxAlpha - minAlpha);

    float depthMask = smoothstep(g_depthEdgeThreshold, g_depthEdgeThreshold * 4.0, depthEdge);
    float normalMask = smoothstep(g_normalEdgeThreshold, g_normalEdgeThreshold * 1.75, normalEdge);
    float materialMask = smoothstep(g_materialEdgeThreshold, g_materialEdgeThreshold + 0.35, materialEdge);
    float silhouetteMask = max(depthMask, smoothstep(0.02, 0.55, alphaEdge));

    float interiorGate = step(0.04, alpha) * smoothstep(0.20, 0.95, minAlpha);
    normalMask *= saturate(silhouetteMask * 1.15 + materialMask * 0.25) * interiorGate;
    materialMask *= saturate(silhouetteMask * 1.05 + normalMask * 0.35) * interiorGate;

    float outline = max(silhouetteMask, max(normalMask, materialMask));
    outline *= edgeMask * saturate(g_outlineOpacity) * step(0.5, g_enableOutline);

    float rimEdge = max(silhouetteMask, normalMask);
    float rim = smoothstep(g_rimThreshold, g_rimThreshold + max(g_rimSoftness, 0.001), rimEdge);
    rim *= auxToon.b * saturate(g_rimIntensity) * step(0.5, g_enableRim);
    rim *= (1.0 - saturate(outline * 0.65));

    if (g_debugView >= 6.5)
    {
        float debugValue = outline;
        if (abs(g_debugView - 7.0) < 0.5) debugValue = max(silhouetteMask, max(normalMask, materialMask));
        else if (abs(g_debugView - 8.0) < 0.5) debugValue = depthMask;
        else if (abs(g_debugView - 9.0) < 0.5) debugValue = saturate(normalEdge / max(g_normalEdgeThreshold * 1.75, 1.0e-4));
        else if (abs(g_debugView - 10.0) < 0.5) debugValue = saturate(materialEdge / max(g_materialEdgeThreshold + 0.35, 1.0e-4));
        else if (abs(g_debugView - 11.0) < 0.5) debugValue = outline;
        else if (abs(g_debugView - 12.0) < 0.5) debugValue = rim;

        float debugAlpha = max(alpha, coverage);
        return float4(debugValue.xxx * debugAlpha, debugAlpha);
    }

    float3 straight = (alpha > 1.0e-5) ? scene.rgb / alpha : StraightColorAt(uv + g_invScreenSize);
    float shadowFactor = auxToon.a;
    float outlineDarkness = lerp(0.10, 0.28, saturate(Luma(straight) + shadowFactor * 0.35));
    float3 autoOutlineColor = saturate(straight * outlineDarkness);
    float edgeColorWeight = saturate(edgeColorData.a);
    float3 outlineColor = lerp(autoOutlineColor, saturate(edgeColorData.rgb), edgeColorWeight);

    straight += g_rimColor * rim;
    straight = lerp(straight, outlineColor, saturate(outline));

    float outAlpha = max(alpha, saturate(outline * 0.95));
    return float4(straight * outAlpha, outAlpha);
}
