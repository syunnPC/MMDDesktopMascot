// ToneMap_PS.hlsl
// Final composite: Tone map (ACES) + SSAO + Bloom + FXAA + sRGB encode.

Texture2D g_sceneTex : register(t0);
Texture2D g_ssaoTex : register(t1);
Texture2D g_bloomTex : register(t2);

SamplerState g_linearClamp : register(s0);
SamplerState g_pointClamp : register(s1);

cbuffer PostProcessCB : register(b0)
{
    float2 g_invScreenSize;
    float g_exposure;
    float g_ssaoIntensity;
    float g_bloomIntensity;
    float g_enableFxaa;
    float g_enableToneMap;
    float g_enableSsao;
    float g_enableBloom;
    float3 _pad0;
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

// ACES filmic tonemapping (approximation)
float3 ACESFilm(float3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 LinearToSrgb(float3 c)
{
    c = saturate(c);
    float3 low = c * 12.92;
    float3 high = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return lerp(high, low, step(c, 0.0031308.xxx));
}

float Luma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

// FXAA 3.11 quality settings
#define FXAA_EDGE_THRESHOLD      0.125
#define FXAA_EDGE_THRESHOLD_MIN  0.0312

float4 SampleScene(float2 uv)
{
    return g_sceneTex.SampleLevel(g_linearClamp, uv, 0);
}

float4 FXAA(float2 uv)
{
    float2 rcpFrame = g_invScreenSize;

    float4 rgbaM = SampleScene(uv);
    float4 rgbaN = SampleScene(uv + float2(0, -rcpFrame.y));
    float4 rgbaW = SampleScene(uv + float2(-rcpFrame.x, 0));
    float4 rgbaE = SampleScene(uv + float2(rcpFrame.x, 0));
    float4 rgbaS = SampleScene(uv + float2(0, rcpFrame.y));

    float lumaM = Luma(rgbaM.rgb);
    float lumaN = Luma(rgbaN.rgb);
    float lumaW = Luma(rgbaW.rgb);
    float lumaE = Luma(rgbaE.rgb);
    float lumaS = Luma(rgbaS.rgb);

    float rangeMin = min(lumaM, min(min(lumaN, lumaW), min(lumaS, lumaE)));
    float rangeMax = max(lumaM, max(max(lumaN, lumaW), max(lumaS, lumaE)));
    float range = rangeMax - rangeMin;

    if (range < max(FXAA_EDGE_THRESHOLD_MIN, rangeMax * FXAA_EDGE_THRESHOLD))
    {
        return rgbaM;
    }

    float lumaL = (lumaN + lumaW + lumaE + lumaS) * 0.25;
    float rangeL = abs(lumaL - lumaM);
    float blendL = max(0.0, (rangeL / range) - 0.5);

    float lumaNW = Luma(SampleScene(uv + float2(-rcpFrame.x, -rcpFrame.y)).rgb);
    float lumaNE = Luma(SampleScene(uv + float2(rcpFrame.x, -rcpFrame.y)).rgb);
    float lumaSW = Luma(SampleScene(uv + float2(-rcpFrame.x, rcpFrame.y)).rgb);
    float lumaSE = Luma(SampleScene(uv + float2(rcpFrame.x, rcpFrame.y)).rgb);

    float edgeVert =
        abs((0.25 * lumaNW) + (-0.5 * lumaN) + (0.25 * lumaNE)) +
        abs((0.50 * lumaW) + (-1.0 * lumaM) + (0.50 * lumaE)) +
        abs((0.25 * lumaSW) + (-0.5 * lumaS) + (0.25 * lumaSE));
    float edgeHorz =
        abs((0.25 * lumaNW) + (-0.5 * lumaW) + (0.25 * lumaSW)) +
        abs((0.50 * lumaN) + (-1.0 * lumaM) + (0.50 * lumaS)) +
        abs((0.25 * lumaNE) + (-0.5 * lumaE) + (0.25 * lumaSE));

    bool horzSpan = edgeHorz >= edgeVert;
    float lengthSign = horzSpan ? -rcpFrame.y : -rcpFrame.x;
    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }

    float gradientN = abs(lumaN - lumaM);
    float gradientS = abs(lumaS - lumaM);
    lumaN = (lumaN + lumaM) * 0.5;
    lumaS = (lumaS + lumaM) * 0.5;

    bool pairN = gradientN >= gradientS;
    if (!pairN) { lumaN = lumaS; gradientN = gradientS; }
    if (!pairN) lengthSign *= -1.0;

    float2 posN;
    posN.x = uv.x + (horzSpan ? 0.0 : lengthSign * 0.5);
    posN.y = uv.y + (horzSpan ? lengthSign * 0.5 : 0.0);

    gradientN *= 0.25;

    float2 posP = posN;
    float2 offNP = horzSpan ? float2(rcpFrame.x, 0.0) : float2(0.0, rcpFrame.y);

    float lumaEndN = lumaN;
    float lumaEndP = lumaN;
    bool doneN = false;
    bool doneP = false;

    posN -= offNP * 1.0;
    posP += offNP * 1.0;

    [loop]
    for (int j = 0; j < 6; ++j)
    {
        if (!doneN) lumaEndN = Luma(SampleScene(posN).rgb) - lumaN;
        if (!doneP) lumaEndP = Luma(SampleScene(posP).rgb) - lumaN;

        doneN = abs(lumaEndN) >= gradientN;
        doneP = abs(lumaEndP) >= gradientN;
        if (doneN && doneP) break;

        if (!doneN) posN -= offNP;
        if (!doneP) posP += offNP;
    }

    float dstN = horzSpan ? uv.x - posN.x : uv.y - posN.y;
    float dstP = horzSpan ? posP.x - uv.x : posP.y - uv.y;

    bool directionN = dstN < dstP;
    lumaEndN = directionN ? lumaEndN : lumaEndP;
    if (((lumaM - lumaN) < 0.0) == (lumaEndN < 0.0))
    {
        lengthSign = 0.0;
    }

    float spanLength = max(dstP + dstN, 1e-4);
    dstN = directionN ? dstN : dstP;
    float subPixelOffset = (0.5 + (dstN * (-1.0 / spanLength))) * lengthSign;

    float2 finalUv = float2(
        uv.x + (horzSpan ? 0.0 : subPixelOffset),
        uv.y + (horzSpan ? subPixelOffset : 0.0));

    float4 rgbaF = SampleScene(finalUv);
    rgbaF.rgb = lerp(rgbaF.rgb, rgbaM.rgb, saturate(blendL * 0.10));
    return rgbaF;
}

float4 PSMain(PSIn i) : SV_TARGET
{
    float2 uv = i.uv;

    // Read scene (HDR) and preserve alpha for DirectComposition transparency
    float4 sceneColor = g_sceneTex.SampleLevel(g_linearClamp, uv, 0);
    float alpha = sceneColor.a;

    // FXAA operates on the raw HDR scene before tone mapping / SSAO / bloom,
    // so that edge-detection luma is computed from the original pixel values.
    // This is simpler than running FXAA on the final composite (which would
    // require an extra full-resolution target).
    if (g_enableFxaa > 0.5f)
    {
        float4 fxaaResult = FXAA(uv);
        alpha = fxaaResult.a;
        // Keep premultiplied; common un-premultiply below handles both paths
        sceneColor = fxaaResult;
    }

    // Un-premultiply to get straight RGB for HDR processing
    float3 color = (alpha > 1e-5f) ? (sceneColor.rgb / alpha) : 0.0f;
    color *= g_exposure;

    // Tone map
    if (g_enableToneMap > 0.5f)
    {
        color = ACESFilm(color);
    }

    // SSAO
    if (g_enableSsao > 0.5f)
    {
        float ao = g_ssaoTex.SampleLevel(g_pointClamp, uv, 0).r;
        float aoMul = lerp(1.0f, ao, g_ssaoIntensity);
        color *= aoMul;
    }

    // Bloom
    if (g_enableBloom > 0.5f)
    {
        float3 bloom = g_bloomTex.SampleLevel(g_linearClamp, uv, 0).rgb;
        color += bloom * g_bloomIntensity;
    }

    // sRGB encode (on straight color)
    color = LinearToSrgb(color);

    // Re-premultiply for DirectComposition DXGI_ALPHA_MODE_PREMULTIPLIED
    color *= alpha;

    return float4(color, alpha);
}
