Texture2D<float4> g_edgesTex : register(t0);

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

bool IsInside(int2 pixel)
{
    const int2 size = ScreenSize();
    return all(pixel >= int2(0, 0)) && all(pixel < size);
}

int2 ClampPixel(int2 pixel)
{
    const int2 maxPixel = ScreenSize() - 1;
    return min(max(pixel, int2(0, 0)), maxPixel);
}

float EdgeH(int2 pixel)
{
    return g_edgesTex.Load(int3(ClampPixel(pixel), 0)).r;
}

float EdgeV(int2 pixel)
{
    return g_edgesTex.Load(int3(ClampPixel(pixel), 0)).g;
}

int SearchHorizontal(int2 pixel, int direction, out float crossing)
{
    crossing = 0.0f;
    int distance = 0;
    const int maxSteps = min(32, max(1, (int)(g_maxSearchSteps + 0.5f)));

    [loop]
    for (int stepIndex = 1; stepIndex <= 32; ++stepIndex)
    {
        if (stepIndex > maxSteps)
        {
            break;
        }

        const int2 samplePixel = pixel + int2(direction * stepIndex, 0);
        if (!IsInside(samplePixel))
        {
            break;
        }

        const float continuation = EdgeH(samplePixel);
        const float corner = max(EdgeV(samplePixel), EdgeV(samplePixel + int2(0, 1)));
        crossing = max(crossing, corner);

        if (continuation < 0.5f)
        {
            break;
        }

        distance = stepIndex;

        if (corner > 0.5f)
        {
            break;
        }
    }

    return distance;
}

int SearchVertical(int2 pixel, int direction, out float crossing)
{
    crossing = 0.0f;
    int distance = 0;
    const int maxSteps = min(32, max(1, (int)(g_maxSearchSteps + 0.5f)));

    [loop]
    for (int stepIndex = 1; stepIndex <= 32; ++stepIndex)
    {
        if (stepIndex > maxSteps)
        {
            break;
        }

        const int2 samplePixel = pixel + int2(0, direction * stepIndex);
        if (!IsInside(samplePixel))
        {
            break;
        }

        const float continuation = EdgeV(samplePixel);
        const float corner = max(EdgeH(samplePixel), EdgeH(samplePixel + int2(1, 0)));
        crossing = max(crossing, corner);

        if (continuation < 0.5f)
        {
            break;
        }

        distance = stepIndex;

        if (corner > 0.5f)
        {
            break;
        }
    }

    return distance;
}

float AreaApprox(int negativeDistance, int positiveDistance, float negativeCrossing, float positiveCrossing)
{
    const float span = (float)(negativeDistance + positiveDistance + 1);
    const float maxCrossing = max(negativeCrossing, positiveCrossing);
    const float bothCrossing = min(negativeCrossing, positiveCrossing);
    const float endpointPattern = saturate(maxCrossing * 0.45f + bothCrossing * 0.55f);
    const float shortRunPattern = 1.0f - smoothstep(1.0f, 5.0f, span);
    const float pattern = max(endpointPattern, shortRunPattern * 0.42f);

    if (pattern <= 0.001f)
    {
        return 0.0f;
    }

    const float balance = 1.0f - abs((float)(negativeDistance - positiveDistance)) / max(span, 1.0f);
    const float lengthFactor = saturate(span / 10.0f);
    const float centerFactor = lerp(0.62f, 1.0f, balance);
    const float lengthWeight = lerp(0.72f, 1.0f, lengthFactor);
    const float baseWeight = lerp(0.22f, 0.48f, pattern);
    return saturate(baseWeight * centerFactor * lengthWeight);
}

float DiagonalCornerMain(int2 pixel)
{
    return min(EdgeH(pixel), EdgeV(pixel));
}

float DiagonalCornerAnti(int2 pixel)
{
    return min(EdgeH(pixel), EdgeV(pixel + int2(1, 0)));
}

int SearchDiagonalMain(int2 pixel, int2 direction)
{
    int distance = 0;
    const int maxSteps = min(12, max(1, (int)(g_maxSearchSteps * 0.5f + 0.5f)));

    [loop]
    for (int stepIndex = 1; stepIndex <= 12; ++stepIndex)
    {
        if (stepIndex > maxSteps)
        {
            break;
        }

        const int2 samplePixel = pixel + direction * stepIndex;
        if (!IsInside(samplePixel) || DiagonalCornerMain(samplePixel) < 0.5f)
        {
            break;
        }

        distance = stepIndex;
    }

    return distance;
}

int SearchDiagonalAnti(int2 pixel, int2 direction)
{
    int distance = 0;
    const int maxSteps = min(12, max(1, (int)(g_maxSearchSteps * 0.5f + 0.5f)));

    [loop]
    for (int stepIndex = 1; stepIndex <= 12; ++stepIndex)
    {
        if (stepIndex > maxSteps)
        {
            break;
        }

        const int2 samplePixel = pixel + direction * stepIndex;
        if (!IsInside(samplePixel) || DiagonalCornerAnti(samplePixel) < 0.5f)
        {
            break;
        }

        distance = stepIndex;
    }

    return distance;
}

float DiagonalWeightMain(int2 pixel)
{
    if (DiagonalCornerMain(pixel) < 0.5f)
    {
        return 0.0f;
    }

    const int negativeDistance = SearchDiagonalMain(pixel, int2(-1, -1));
    const int positiveDistance = SearchDiagonalMain(pixel, int2(1, 1));
    const float span = (float)(negativeDistance + positiveDistance + 1);
    if (span < 2.0f)
    {
        return 0.0f;
    }

    const float balance = 1.0f - abs((float)(negativeDistance - positiveDistance)) / max(span, 1.0f);
    const float fade = 1.0f - smoothstep(8.0f, 13.0f, span);
    return saturate(0.18f * lerp(0.65f, 1.0f, balance) * fade);
}

float DiagonalWeightAnti(int2 pixel)
{
    if (DiagonalCornerAnti(pixel) < 0.5f)
    {
        return 0.0f;
    }

    const int negativeDistance = SearchDiagonalAnti(pixel, int2(-1, 1));
    const int positiveDistance = SearchDiagonalAnti(pixel, int2(1, -1));
    const float span = (float)(negativeDistance + positiveDistance + 1);
    if (span < 2.0f)
    {
        return 0.0f;
    }

    const float balance = 1.0f - abs((float)(negativeDistance - positiveDistance)) / max(span, 1.0f);
    const float fade = 1.0f - smoothstep(8.0f, 13.0f, span);
    return saturate(0.18f * lerp(0.65f, 1.0f, balance) * fade);
}

float HorizontalBlendAt(int2 pixel)
{
    if (!IsInside(pixel) || EdgeH(pixel) < 0.5f)
    {
        return 0.0f;
    }

    float leftCrossing = 0.0f;
    float rightCrossing = 0.0f;
    const int leftDistance = SearchHorizontal(pixel, -1, leftCrossing);
    const int rightDistance = SearchHorizontal(pixel, 1, rightCrossing);
    float weight = AreaApprox(leftDistance, rightDistance, leftCrossing, rightCrossing);

    const float immediateCorner = max(max(EdgeV(pixel), EdgeV(pixel + int2(1, 0))),
                                      max(EdgeV(pixel + int2(0, -1)), EdgeV(pixel + int2(1, -1))));
    weight *= lerp(1.0f, 0.65f, saturate(g_cornerRounding) * immediateCorner);
    return weight;
}

float VerticalBlendAt(int2 pixel)
{
    if (!IsInside(pixel) || EdgeV(pixel) < 0.5f)
    {
        return 0.0f;
    }

    float topCrossing = 0.0f;
    float bottomCrossing = 0.0f;
    const int topDistance = SearchVertical(pixel, -1, topCrossing);
    const int bottomDistance = SearchVertical(pixel, 1, bottomCrossing);
    float weight = AreaApprox(topDistance, bottomDistance, topCrossing, bottomCrossing);

    const float immediateCorner = max(max(EdgeH(pixel), EdgeH(pixel + int2(0, 1))),
                                      max(EdgeH(pixel + int2(-1, 0)), EdgeH(pixel + int2(-1, 1))));
    weight *= lerp(1.0f, 0.65f, saturate(g_cornerRounding) * immediateCorner);
    return weight;
}

float4 PSMain(PSIn i) : SV_TARGET
{
    const int2 pixel = int2(i.pos.xy);
    float4 weights;
    weights.r = VerticalBlendAt(pixel);
    weights.b = VerticalBlendAt(pixel + int2(1, 0));
    weights.g = HorizontalBlendAt(pixel);
    weights.a = HorizontalBlendAt(pixel + int2(0, 1));

    const float diagonalMain = DiagonalWeightMain(pixel);
    if (diagonalMain > 0.0f)
    {
        weights.r = max(weights.r, diagonalMain);
        weights.g = max(weights.g, diagonalMain);
    }

    const float diagonalAnti = DiagonalWeightAnti(pixel);
    if (diagonalAnti > 0.0f)
    {
        weights.b = max(weights.b, diagonalAnti);
        weights.g = max(weights.g, diagonalAnti);
    }

    return saturate(weights);
}
