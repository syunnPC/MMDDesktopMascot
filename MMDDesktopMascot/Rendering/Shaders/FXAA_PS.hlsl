Texture2D g_src : register(t0);
SamplerState g_samp : register(s0);

cbuffer FxaaCB : register(b0)
{
    float2 g_invScreenSize; // (1/width, 1/height)
    float g_sharpenStrength; // 0..1 (usually <= 0.35)
    float g_enableFxaa;
    float4 _pad0;
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

// FXAA 3.11 quality settings
#define FXAA_QUALITY__PRESET 12
#define FXAA_QUALITY__PS 5
#define FXAA_QUALITY__P0 1.0
#define FXAA_QUALITY__P1 1.5
#define FXAA_QUALITY__P2 2.0
#define FXAA_QUALITY__P3 2.0
#define FXAA_QUALITY__P4 2.0
#define FXAA_QUALITY__P5 4.0
#define FXAA_QUALITY__P6 8.0

// Tuning parameters
#define FXAA_EDGE_THRESHOLD      0.125
#define FXAA_EDGE_THRESHOLD_MIN  0.0312

float EdgeSignal(float4 rgba)
{
    float luma = dot(rgba.rgb, float3(0.299, 0.587, 0.114));
    return max(luma, rgba.a);
}

float3 LinearToSrgb(float3 c)
{
    c = saturate(c);
    float3 low = c * 12.92;
    float3 high = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return lerp(high, low, step(c, 0.0031308.xxx));
}

float4 EncodeForPresent(float4 rgba)
{
    float alpha = saturate(rgba.a);
    float3 straight = (alpha > 1e-5) ? (rgba.rgb / alpha) : 0.0.xxx;
    float3 encoded = LinearToSrgb(straight) * alpha;
    return float4(encoded, alpha);
}

float4 PSMain(PSIn i) : SV_TARGET
{
    float2 rcpFrame = g_invScreenSize;
    float2 uv = i.uv;
    float4 rgbaM = g_src.SampleLevel(g_samp, uv, 0);

    if (g_enableFxaa < 0.5)
    {
        return EncodeForPresent(rgbaM);
    }

    float4 rgbaN = g_src.SampleLevel(g_samp, uv + float2(0, -rcpFrame.y), 0);
    float4 rgbaW = g_src.SampleLevel(g_samp, uv + float2(-rcpFrame.x, 0), 0);
    float4 rgbaE = g_src.SampleLevel(g_samp, uv + float2(rcpFrame.x, 0), 0);
    float4 rgbaS = g_src.SampleLevel(g_samp, uv + float2(0, rcpFrame.y), 0);

    float lumaN = EdgeSignal(rgbaN);
    float lumaW = EdgeSignal(rgbaW);
    float lumaM = EdgeSignal(rgbaM);
    float lumaE = EdgeSignal(rgbaE);
    float lumaS = EdgeSignal(rgbaS);

    float rangeMin = min(lumaM, min(min(lumaN, lumaW), min(lumaS, lumaE)));
    float rangeMax = max(lumaM, max(max(lumaN, lumaW), max(lumaS, lumaE)));
    float range = rangeMax - rangeMin;

    if (range < max(FXAA_EDGE_THRESHOLD_MIN, rangeMax * FXAA_EDGE_THRESHOLD))
    {
        return EncodeForPresent(rgbaM);
    }

    float lumaL = (lumaN + lumaW + lumaE + lumaS) * 0.25;
    float rangeL = abs(lumaL - lumaM);
    float blendL = max(0.0, (rangeL / range) - 0.5);

    float lumaNW = EdgeSignal(g_src.SampleLevel(g_samp, uv + float2(-rcpFrame.x, -rcpFrame.y), 0));
    float lumaNE = EdgeSignal(g_src.SampleLevel(g_samp, uv + float2(rcpFrame.x, -rcpFrame.y), 0));
    float lumaSW = EdgeSignal(g_src.SampleLevel(g_samp, uv + float2(-rcpFrame.x, rcpFrame.y), 0));
    float lumaSE = EdgeSignal(g_src.SampleLevel(g_samp, uv + float2(rcpFrame.x, rcpFrame.y), 0));

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
    if (!horzSpan) lumaN = lumaW;
    if (!horzSpan) lumaS = lumaE;

    float gradientN = abs(lumaN - lumaM);
    float gradientS = abs(lumaS - lumaM);
    lumaN = (lumaN + lumaM) * 0.5;
    lumaS = (lumaS + lumaM) * 0.5;

    bool pairN = gradientN >= gradientS;
    if (!pairN) lumaN = lumaS;
    if (!pairN) gradientN = gradientS;
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

    for (int j = 0; j < FXAA_QUALITY__PS; ++j)
    {
        if (!doneN) lumaEndN = EdgeSignal(g_src.SampleLevel(g_samp, posN, 0)) - lumaN;
        if (!doneP) lumaEndP = EdgeSignal(g_src.SampleLevel(g_samp, posP, 0)) - lumaN;

        doneN = abs(lumaEndN) >= gradientN;
        doneP = abs(lumaEndP) >= gradientN;
        if (doneN && doneP) break;

        if (!doneN)
        {
            posN -= offNP * (j == 0 ? FXAA_QUALITY__P0 : (j == 1 ? FXAA_QUALITY__P1 : FXAA_QUALITY__P2));
        }
        if (!doneP)
        {
            posP += offNP * (j == 0 ? FXAA_QUALITY__P0 : (j == 1 ? FXAA_QUALITY__P1 : FXAA_QUALITY__P2));
        }
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
    float4 rgbaF = g_src.SampleLevel(g_samp, finalUv, 0);

    // Preserve some sub-pixel alias reduction from the original path.
    rgbaF.rgb = lerp(rgbaF.rgb, rgbaM.rgb, saturate(blendL * 0.10));

    float sharpen = saturate(g_sharpenStrength);
    if (sharpen <= 0.0001)
    {
        return EncodeForPresent(rgbaF);
    }

    // Lightweight unsharp mask to counter scale-dependent softening.
    float3 n = g_src.SampleLevel(g_samp, finalUv + float2(0, -rcpFrame.y), 0).rgb;
    float3 s = g_src.SampleLevel(g_samp, finalUv + float2(0,  rcpFrame.y), 0).rgb;
    float3 w = g_src.SampleLevel(g_samp, finalUv + float2(-rcpFrame.x, 0), 0).rgb;
    float3 e = g_src.SampleLevel(g_samp, finalUv + float2( rcpFrame.x, 0), 0).rgb;
    float3 blur = (n + s + w + e + rgbaF.rgb) * 0.2;

    float3 sharpRgb = max(rgbaF.rgb + (rgbaF.rgb - blur) * sharpen, 0.0);
    return EncodeForPresent(float4(sharpRgb, rgbaF.a));
}
