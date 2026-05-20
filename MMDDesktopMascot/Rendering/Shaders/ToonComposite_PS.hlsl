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
    float g_outlineSilhouetteModeEnabled;
    float g_outlineWidthScale;
    float g_outlineSilhouetteAngleTolerance;

    float g_outlineNonSilhouetteWidthScale;
    float g_outlineNonSilhouetteOpacityScale;
    float g_outlineNonSilhouetteAngleTolerance;
    float g_outlineNonSilhouetteEnabled;

    float4 _pad1;
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
    float proxyDepth = 1.0 - g_auxOutlineTex.SampleLevel(g_pointClamp, uv, 0).a;
    return lerp(proxyDepth, hardwareDepth, step(0.5, g_useDepthTexture));
}

float3 NormalAt(float2 uv)
{
    float3 n = g_auxNormalTex.SampleLevel(g_pointClamp, uv, 0).rgb * 2.0 - 1.0;
    float lenSq = dot(n, n);
    return (lenSq > 1.0e-5) ? n * rsqrt(lenSq) : float3(0.0, 0.0, 1.0);
}

float AlphaAt(float2 uv)
{
    return g_sceneTex.SampleLevel(g_pointClamp, uv, 0).a;
}

float MaterialClassAt(float2 uv)
{
    return g_auxNormalTex.SampleLevel(g_pointClamp, uv, 0).a * 255.0;
}

float MaterialAt(float2 uv)
{
    return g_auxOutlineTex.SampleLevel(g_pointClamp, uv, 0).g;
}

float SilhouetteFacingAt(float2 uv)
{
    float center = AlphaAt(uv);
    float right  = AlphaAt(uv + float2(g_invScreenSize.x, 0));
    float left   = AlphaAt(uv - float2(g_invScreenSize.x, 0));
    float top    = AlphaAt(uv + float2(0, g_invScreenSize.y));
    float bottom = AlphaAt(uv - float2(0, g_invScreenSize.y));
    float gradient = max(abs(center - right),
                     max(abs(center - left),
                     max(abs(center - top), abs(center - bottom))));
    return smoothstep(0.005, 0.28, gradient);
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

void GetWideNeighborhoodCoverage(float2 uv, float2 texel, float radiusPixels, out float minAlpha, out float maxAlpha)
{
    minAlpha = 1.0;
    maxAlpha = 0.0;

    float radius = clamp(radiusPixels, 1.0, 24.0);

    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            float2 offset = float2((float)x, (float)y) * (radius * 0.5);
            float a = AlphaAt(uv + texel * offset);
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

void GetCapturedEdgeData(float2 uv, float2 texel, float dilationRadius, out float edgeMask, out float4 edgeColor)
{
    edgeMask = 0.0;
    edgeColor = 0.0;

    float radius = clamp(dilationRadius, 0.0, 4.0);
    float radiusSq = (radius + 0.35) * (radius + 0.35);

    [unroll]
    for (int y = -4; y <= 4; ++y)
    {
        [unroll]
        for (int x = -4; x <= 4; ++x)
        {
            float2 offset = float2((float)x, (float)y);
            if (dot(offset, offset) > radiusSq)
            {
                continue;
            }

            float4 captured = g_auxEdgeColorTex.SampleLevel(g_pointClamp, uv + texel * offset, 0);
            float a = saturate(captured.a);
            if (a >= edgeMask)
            {
                edgeMask = a;
                float3 straight = (a > 1.0e-5) ? captured.rgb / a : 0.0;
                edgeColor = float4(straight, step(1.0e-5, a));
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

float MaxSilhouetteFacing(float2 uv, float2 texel)
{
    float facing = 0.0;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            facing = max(facing, SilhouetteFacingAt(uv + texel * float2((float)x, (float)y)));
        }
    }

    return facing;
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

    float silhouetteMode = step(0.5, g_outlineSilhouetteModeEnabled);
    float capturedEdgeMask = 0.0;
    float4 capturedEdgeColorData = 0.0;
    if (silhouetteMode > 0.5)
    {
        GetCapturedEdgeData(uv, g_invScreenSize, 0.0, capturedEdgeMask, capturedEdgeColorData);
    }

    float outlineWidth = max(1.0, outlineControl * 8.0 * max(g_outlineBaseWidth, 0.0));
    float silhouetteSearchWidth = lerp(
        outlineWidth,
        max(outlineWidth, 2.0 + max(g_outlineWidthScale, 0.0) * 2.5),
        silhouetteMode);
    float2 texel = g_invScreenSize * silhouetteSearchWidth;

    float minAlpha;
    float maxAlpha;
    GetNeighborhoodCoverage(uv, texel, minAlpha, maxAlpha);
    float coverage = max(smoothstep(0.005, 0.08, maxAlpha), capturedEdgeMask * silhouetteMode);
    if (coverage <= 1.0e-5)
    {
        return scene;
    }

    float silhouetteAngleAllowance = 0.0;
    if (g_outlineSilhouetteAngleTolerance > 0.001)
    {
        float tolerance = saturate(g_outlineSilhouetteAngleTolerance);
        float threshold = 1.0 - tolerance;
        float softness = max(0.015, tolerance * 0.12);
        silhouetteAngleAllowance = smoothstep(threshold, min(1.0, threshold + softness), MaxSilhouetteFacing(uv, texel));
    }

    float depthEdge = SobelDepth(uv, texel);
    float normalEdge = SobelNormal(uv, texel);
    float materialEdge = SobelMaterial(uv, texel);
    float alphaEdge = saturate(maxAlpha - minAlpha);
    float wideMinAlpha = minAlpha;
    float wideMaxAlpha = maxAlpha;
    if (silhouetteMode > 0.5)
    {
        GetWideNeighborhoodCoverage(
            uv,
            g_invScreenSize,
            5.0 + max(g_outlineWidthScale, 0.0) * 6.0,
            wideMinAlpha,
            wideMaxAlpha);
    }
    float wideAlphaEdge = saturate(wideMaxAlpha - wideMinAlpha);
    float wideSilhouetteMask = smoothstep(0.01, 0.28, wideAlphaEdge) * smoothstep(0.005, 0.12, wideMaxAlpha);

    float depthMask = smoothstep(g_depthEdgeThreshold, g_depthEdgeThreshold * 4.0, depthEdge);
    float normalMask = smoothstep(g_normalEdgeThreshold, g_normalEdgeThreshold * 1.75, normalEdge);
    float materialMask = smoothstep(g_materialEdgeThreshold, g_materialEdgeThreshold + 0.35, materialEdge);
    float alphaSilhouetteMask = lerp(
        smoothstep(0.02, 0.55, alphaEdge),
        max(smoothstep(0.006, 0.32, alphaEdge), wideSilhouetteMask),
        silhouetteMode);

    float outlineEnable = step(0.5, g_enableOutline);
    float baseSilhouetteMask = max(depthMask, alphaSilhouetteMask);
    float baseInteriorGate = step(0.04, alpha) * smoothstep(0.20, 0.95, minAlpha);
    float baseNormalMask = normalMask * saturate(baseSilhouetteMask * 1.15 + materialMask * 0.25) * baseInteriorGate;
    float baseMaterialMask = materialMask * saturate(baseSilhouetteMask * 1.05 + baseNormalMask * 0.35) * baseInteriorGate;
    float baseOutlineMask = max(baseSilhouetteMask, max(baseNormalMask, baseMaterialMask));
    float baseOutlineCandidate = baseOutlineMask * edgeMask;
    float baseOpacity = saturate(g_outlineOpacity);
    float baseOutline = baseOutlineCandidate * baseOpacity * outlineEnable;

    float nearAlphaBoundary = 1.0 - smoothstep(0.72, 0.98, minAlpha);
    float silhouetteMask = alphaSilhouetteMask;

    float silhouetteInteriorGate = step(0.04, alpha) * smoothstep(0.20, 0.95, minAlpha);
    float silhouetteBoundaryGate = alphaSilhouetteMask * max(nearAlphaBoundary, alphaSilhouetteMask);
    float silhouetteNormalMask = normalMask * silhouetteAngleAllowance * silhouetteBoundaryGate * silhouetteInteriorGate;
    float silhouetteDepthMask = depthMask * silhouetteBoundaryGate;

    if (g_outlineSilhouetteAngleTolerance > 0.001)
    {
        silhouetteMask = max(silhouetteMask, max(silhouetteDepthMask, silhouetteNormalMask));
    }

    float silhouetteRegion = saturate(max(max(silhouetteMask, max(silhouetteDepthMask, silhouetteNormalMask)), wideSilhouetteMask * silhouetteMode));
    float capturedSilhouetteCandidate = saturate(capturedEdgeMask * 1.85);
    float silhouetteCandidate = lerp(baseOutlineCandidate, capturedSilhouetteCandidate, silhouetteMode);
    float silhouetteOpacity = lerp(baseOpacity, 1.0, silhouetteMode);
    float capturedSilhouetteGate = smoothstep(0.01, 0.08, capturedEdgeMask) * silhouetteMode;
    float silhouetteVisibility = lerp(
        silhouetteRegion,
        max(silhouetteRegion, capturedSilhouetteGate),
        silhouetteMode);
    float silhouetteOutline = silhouetteCandidate * silhouetteVisibility * silhouetteOpacity * outlineEnable;

    float nonSilhouetteBase = 0.0;
    float nonSilhouetteOutline = 0.0;
    float nonSilhouetteWidthScale = max(g_outlineNonSilhouetteWidthScale, 0.0);
    float capturedNonSilhouetteMask = capturedEdgeMask;
    float4 capturedNonSilhouetteColorData = capturedEdgeColorData;
    if (g_outlineNonSilhouetteEnabled > 0.5 && nonSilhouetteWidthScale > 0.001 && g_outlineNonSilhouetteOpacityScale > 0.001)
    {
        if (silhouetteMode > 0.5)
        {
            GetCapturedEdgeData(
                uv,
                g_invScreenSize,
                max(nonSilhouetteWidthScale - 1.0, 0.0),
                capturedNonSilhouetteMask,
                capturedNonSilhouetteColorData);
        }
        float tolerance = saturate(g_outlineNonSilhouetteAngleTolerance);
        float nonSilhouetteCandidate = lerp(baseOutlineCandidate, capturedNonSilhouetteMask, silhouetteMode);
        float excludedBySilhouette = nonSilhouetteCandidate * saturate(1.0 - silhouetteRegion);
        nonSilhouetteBase = excludedBySilhouette * tolerance;
        nonSilhouetteOutline = nonSilhouetteBase *
            max(g_outlineNonSilhouetteOpacityScale, 0.0) *
            min(nonSilhouetteWidthScale, 1.0) *
            outlineEnable;
    }
    float silhouetteModeOutline = max(silhouetteOutline, nonSilhouetteOutline);
    float outline = lerp(baseOutline, silhouetteModeOutline, silhouetteMode);

    float rimEdge = lerp(max(baseSilhouetteMask, baseNormalMask), max(silhouetteMask, silhouetteNormalMask), silhouetteMode);
    float rim = smoothstep(g_rimThreshold, g_rimThreshold + max(g_rimSoftness, 0.001), rimEdge);
    rim *= auxToon.b * saturate(g_rimIntensity) * step(0.5, g_enableRim);
    rim *= (1.0 - saturate(outline * 0.65));

    if (g_debugView >= 6.5)
    {
        float debugValue = outline;
        if (abs(g_debugView - 7.0) < 0.5) debugValue = lerp(baseOutlineMask, max(silhouetteRegion * silhouetteCandidate, nonSilhouetteBase), silhouetteMode);
        else if (abs(g_debugView - 8.0) < 0.5) debugValue = lerp(depthMask, silhouetteDepthMask, silhouetteMode);
        else if (abs(g_debugView - 9.0) < 0.5) debugValue = lerp(saturate(normalEdge / max(g_normalEdgeThreshold * 1.75, 1.0e-4)), silhouetteNormalMask, silhouetteMode);
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
    float useNonSilhouetteColor = silhouetteMode * step(silhouetteOutline, nonSilhouetteOutline);
    float4 silhouetteEdgeColorData = lerp(edgeColorData, capturedEdgeColorData, silhouetteMode);
    float4 nonSilhouetteEdgeColorData = lerp(edgeColorData, capturedNonSilhouetteColorData, silhouetteMode);
    float4 selectedEdgeColorData = lerp(silhouetteEdgeColorData, nonSilhouetteEdgeColorData, useNonSilhouetteColor);
    float edgeColorWeight = saturate(selectedEdgeColorData.a);
    float3 outlineColor = lerp(autoOutlineColor, saturate(selectedEdgeColorData.rgb), edgeColorWeight);

    straight += g_rimColor * rim;
    straight = lerp(straight, outlineColor, saturate(outline));

    float outAlpha = max(alpha, saturate(outline * 0.95));
    return float4(straight * outAlpha, outAlpha);
}
