struct PSIn
{
    float4 pos : SV_POSITION;
    float3 worldPos : TEXCOORD1;
    float3 worldNormal : TEXCOORD2;
    float3 viewDir : TEXCOORD3;
    float2 uv : TEXCOORD0;
    float4 addUv1 : TEXCOORD4;
    float4 worldTangent : TEXCOORD5;
    float edgeScale : TEXCOORD6;
};

struct PSOut
{
    float4 sceneColor : SV_Target0;
    float4 auxNormal : SV_Target1;
    float4 auxToon : SV_Target2;
    float4 auxOutline : SV_Target3;
    float4 auxEdgeColor : SV_Target4;
};

cbuffer SceneCB : register(b0)
{
    float4x4 g_model;
    float4x4 g_view;
    float4x4 g_proj;
    float4x4 g_mvp;

    float3 g_lightDir0;
    float g_ambient;
    float3 g_lightColor0;
    float g_lightInt0;

    float3 g_lightDir1;
    float g_lightInt1;
    float3 g_lightColor1;
    float _pad1;

    float3 g_cameraPos;
    float g_specPower;
    float3 g_specColor;
    float g_specStrength;

    float4 g_normalMatrixRow0;
    float4 g_normalMatrixRow1;
    float4 g_normalMatrixRow2;
    float4x4 g_shadowMatrix;

    float g_brightness;
    uint g_enableSkinning;
    float g_toonContrast;
    float g_shadowHueShift;

    float g_shadowSaturation;
    float g_rimWidth;
    float g_rimIntensity;
    float g_specularStep;

    uint g_enableToon;
    float g_outlineRefDistance;
    float g_outlineDistanceScale;
    float g_outlineDistancePower;

    float g_shadowRampShift;
    float g_shadowDeepThreshold;
    float g_shadowDeepSoftness;
    float g_shadowDeepMul;
    float g_globalSaturation;

    float2 g_invScreenSize;
    float g_shadowMapInvSize;
    float g_shadowStrength;

    uint g_enableSelfShadow;
    float g_shadowBias;
    float g_outlineWidthScale;
    float g_outlineOpacityScale;

    float g_toonShadowThreshold;
    float g_toonShadowSoftness;
    float g_ambientNormalInfluence;
    float g_skinLightInfluence;

    float g_specThreshold;
    float g_hairSpecCenter;
    float g_hairSpecWidth;
    float g_hairSpecIntensity;

    float g_outlineBaseWidth;
    float g_depthEdgeThreshold;
    float g_normalEdgeThreshold;
    float g_rimThreshold;

    float g_rimSoftness;
    float3 g_rimColor;

    uint g_toonDebugView;
    float g_selfShadowSmoothing;
    float2 _pad4;
};

cbuffer Material : register(b1)
{
    float4 g_diffuse;
    float3 g_ambientMat;
    float _pad0;
    float3 g_specularMat;
    float g_specPowerMat;

    uint g_sphereMode;
    float g_edgeSize;
    float g_rimMul;
    float g_specMul;

    float4 g_edgeColor;

    uint g_materialType;
    float g_shadowMul;
    float g_toonContrastMul;
    float g_alphaCutout;

    float4 g_textureFactor;
    float4 g_sphereFactor;
    float4 g_toonFactor;
    float4 g_normalFactor;
    float g_normalMapIntensity;
    float g_materialIdNormalized;
    float g_edgeEnabled;
    float _pad3;
    float4 g_shadowColorOverride;
    float4 g_toonParams0;
    float4 g_toonParams1;
    float4 g_toonParams2;
};

Texture2D g_base : register(t0);
Texture2D g_toon : register(t1);
Texture2D g_sphere : register(t2);
Texture2D g_normalMap : register(t3);
Texture2D g_hairSpecMask : register(t4);
Texture2D g_shadowMap : register(t5);
SamplerState g_samp : register(s0);
SamplerState g_toonSamp : register(s1);
SamplerComparisonState g_shadowSamp : register(s2);

#define MATERIAL_CLOTH 0u
#define MATERIAL_SKIN 1u
#define MATERIAL_HAIR 2u
#define MATERIAL_FACE 3u
#define MATERIAL_EYE 4u
#define MATERIAL_TRANSPARENT 5u

float3 LinearizeSrgb(float3 c)
{
    c = saturate(c);
    float3 low = c / 12.92;
    float3 high = pow((c + 0.055) / 1.055, 2.4);
    return lerp(high, low, step(c, 0.04045.xxx));
}

float Luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float3 ApplySaturation(float3 c, float s)
{
    float l = Luminance(c);
    return lerp(l.xxx, c, s);
}

float MaterialSaturationBoost(uint materialType)
{
    if (materialType == MATERIAL_HAIR)
    {
        return 1.08;
    }
    if (materialType == MATERIAL_EYE)
    {
        return 1.10;
    }
    if (materialType == MATERIAL_SKIN || materialType == MATERIAL_FACE)
    {
        return 1.02;
    }
    return 1.04;
}

float3 RgbToHsv(float3 c)
{
    float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    float4 p = (c.g < c.b) ? float4(c.bg, K.wz) : float4(c.gb, K.xy);
    float4 q = (c.r < p.x) ? float4(p.xyw, c.r) : float4(c.r, p.yzx);

    float d = q.x - min(q.w, q.y);
    float e = 1e-10;
    float h = abs(q.z + (q.w - q.y) / (6.0 * d + e));
    float s = d / (q.x + e);
    float v = q.x;
    return float3(h, s, v);
}

float3 HsvToRgb(float3 c)
{
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * lerp(K.xxx, saturate(p - K.xxx), c.y);
}

bool IsSkinMaterial(uint materialType)
{
    return materialType == MATERIAL_SKIN || materialType == MATERIAL_FACE;
}

void GetShadowRule(uint materialType, out float hueShift, out float saturationScale, out float valueScale)
{
    hueShift = -0.02;
    saturationScale = 1.00;
    valueScale = 0.62;

    if (IsSkinMaterial(materialType))
    {
        hueShift = 0.02;
        saturationScale = 1.05;
        valueScale = 0.72;
    }
    else if (materialType == MATERIAL_HAIR)
    {
        hueShift = 0.00;
        saturationScale = 1.10;
        valueScale = 0.55;
    }
    else if (materialType == MATERIAL_EYE)
    {
        hueShift = 0.00;
        saturationScale = 1.00;
        valueScale = 0.75;
    }
    else if (materialType == MATERIAL_TRANSPARENT)
    {
        hueShift = -0.01;
        saturationScale = 1.00;
        valueScale = 0.70;
    }
}

float GetRimMask(uint materialType)
{
    if (materialType == MATERIAL_HAIR)
    {
        return 1.0;
    }
    if (IsSkinMaterial(materialType))
    {
        return 0.3;
    }
    if (materialType == MATERIAL_EYE)
    {
        return 0.0;
    }
    return 0.6;
}

float3 MakeShadowColor(float3 baseLinear, uint materialType)
{
    float hueShift;
    float saturationScale;
    float valueScale;
    GetShadowRule(materialType, hueShift, saturationScale, valueScale);
    hueShift += g_toonParams1.z;
    saturationScale = (g_toonParams1.w > 0.0) ? g_toonParams1.w : saturationScale;
    valueScale = (g_toonParams2.x > 0.0) ? g_toonParams2.x : valueScale;

    float3 hsv = RgbToHsv(baseLinear);
    hsv.x = frac(hsv.x + hueShift + g_shadowHueShift * 0.15915494);
    hsv.y = saturate(hsv.y * saturationScale + g_shadowSaturation);
    hsv.z = saturate(hsv.z * valueScale * lerp(1.0, g_shadowDeepMul, saturate(g_shadowMul)));
    float shadowSaturation = lerp(1.0, MaterialSaturationBoost(materialType), 0.65);
    float3 generated = ApplySaturation(HsvToRgb(hsv), shadowSaturation);

    float ambientLum = Luminance(g_ambientMat);
    float3 ambientFallback = ApplySaturation(saturate(g_ambientMat * max(0.35, Luminance(baseLinear) * 1.8)), 1.05);
    float ambientWeight = saturate((ambientLum - 0.015) * 12.0) * 0.18;
    float3 shadowColor = lerp(generated, ambientFallback, ambientWeight);
    return lerp(shadowColor, saturate(g_shadowColorOverride.rgb), saturate(g_shadowColorOverride.a));
}

float ScreenStableNoise(float2 pixel)
{
    float2 p = floor(pixel);
    return frac(52.9829189 * frac(0.06711056 * p.x + 0.00583715 * p.y));
}

static const float2 g_shadowPoisson[16] = {
    float2(-0.326, -0.406),
    float2(-0.840, -0.074),
    float2(-0.696,  0.457),
    float2(-0.203,  0.621),
    float2( 0.962, -0.195),
    float2( 0.473, -0.480),
    float2( 0.519,  0.767),
    float2( 0.185, -0.893),
    float2( 0.507,  0.064),
    float2( 0.896,  0.412),
    float2(-0.322, -0.933),
    float2(-0.792, -0.598),
    float2( 0.125,  0.298),
    float2(-0.114,  0.176),
    float2( 0.302, -0.251),
    float2(-0.188, -0.214)
};

float SampleSelfShadow(float3 worldPos, float3 normal, float3 lightDir)
{
    if (g_enableSelfShadow == 0)
    {
        return 1.0;
    }

    float4 shadowClip = mul(float4(worldPos, 1.0), g_shadowMatrix);
    float invW = rcp(max(shadowClip.w, 1e-5));
    float2 shadowUv = shadowClip.xy * invW * float2(0.5, -0.5) + 0.5;
    if (any(shadowUv < 0.0) || any(shadowUv > 1.0))
    {
        return 1.0;
    }

    float shadowDepth = shadowClip.z * invW;
    if (shadowDepth <= 0.0 || shadowDepth >= 1.0)
    {
        return 1.0;
    }

    float ndotl = saturate(dot(normal, lightDir));
    float depthBias = g_shadowBias * lerp(3.9, 1.2, ndotl);
    float receiverSoftness = saturate(1.0 - ndotl);
    float2 texel = float2(g_shadowMapInvSize, g_shadowMapInvSize);
    float smoothing = saturate(g_selfShadowSmoothing);
    float filterRadius = g_shadowMapInvSize * lerp(3.2, 7.0, receiverSoftness) * lerp(1.0, 2.05, smoothing);

    float visibility = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (int tap = 0; tap < 16; ++tap)
    {
        float2 offset = g_shadowPoisson[tap];
        float distanceWeight = saturate(1.0 - dot(offset, offset) * 0.48);
        float2 uvOffset = offset * filterRadius;
        float sample = g_shadowMap.SampleCmpLevelZero(
            g_shadowSamp,
            shadowUv + uvOffset,
            shadowDepth - depthBias);
        visibility += sample * distanceWeight;
        totalWeight += distanceWeight;
    }

    float center = g_shadowMap.SampleCmpLevelZero(
        g_shadowSamp,
        shadowUv,
        shadowDepth - depthBias * 0.85);
    float centerWeight = lerp(0.95, 0.22, smoothing);
    visibility += center * centerWeight;
    totalWeight += centerWeight;

    return visibility / max(totalWeight, 1.0e-4);
}

float StylizeSelfShadow(float visibility, float2 pixel)
{
    if (g_enableToon == 0 || g_enableSelfShadow == 0)
    {
        return visibility;
    }

    float threshold = saturate(0.5 + g_shadowRampShift * 0.18);
    float smoothing = saturate(g_selfShadowSmoothing);
    float softness = max(0.06, g_toonShadowSoftness * 2.9) + smoothing * 0.08;
    float jitter = (ScreenStableNoise(pixel) - 0.5) * softness * 0.12;
    return smoothstep(
        threshold - softness + jitter,
        threshold + softness + jitter,
        visibility);
}

float ComputeNdotL01(float3 normalWS, float3 lightDirWS)
{
    float ndotl = dot(normalize(normalWS), normalize(lightDirWS));
    return saturate(ndotl * 0.5 + 0.5);
}

float ApplySkinLightRelaxation(float ndotl01, uint materialType)
{
    if (IsSkinMaterial(materialType))
    {
        float influence = (g_toonParams0.y >= 0.0) ? g_toonParams0.y : g_skinLightInfluence;
        ndotl01 = lerp(0.5, ndotl01, saturate(influence));
    }
    return ndotl01;
}

float ComputeToonLit(float ndotl01, float selfShadow, float toonRamp)
{
    float directional = saturate(ndotl01 + g_shadowRampShift - (1.0 - selfShadow) * g_shadowStrength);
    if (g_enableToon == 0)
    {
        return directional;
    }

    float softness = max(0.001, g_toonShadowSoftness);
    float toonLit = smoothstep(
        g_toonShadowThreshold - softness,
        g_toonShadowThreshold + softness,
        directional);
    toonLit *= lerp(0.70, 1.10, toonRamp);
    return saturate(toonLit);
}

float ComputeToonSpecular(float3 normalWS, float3 viewDirWS, float3 lightDirWS, float specPower)
{
    float3 h = normalize(viewDirWS + normalize(lightDirWS));
    float s = pow(saturate(dot(normalize(normalWS), h)), specPower);
    if (g_enableToon == 0)
    {
        return s;
    }
    return smoothstep(g_specThreshold - 0.025, g_specThreshold + 0.025, s);
}

float ComputeHairSpecular(float3 normalWS, float3 viewDirWS, float3 lightDirWS)
{
    float3 h = normalize(viewDirWS + normalize(lightDirWS));
    float center = g_toonParams0.z;
    float width = max(g_toonParams0.w, 0.001);
    float intensity = g_toonParams1.x;
    float band = abs(dot(normalize(normalWS), h) - center);
    float spec = 1.0 - smoothstep(width, width + 0.05, band);
    return spec * intensity;
}

float3 ApplySphere(float3 base, float3 sphere, uint mode)
{
    if (mode == 1)
    {
        return base * sphere;
    }
    else if (mode == 2)
    {
        return base + sphere;
    }
    else if (mode == 3)
    {
        return base * sphere;
    }
    return base;
}

float3 DebugMaterialColor(float materialId)
{
    float h = frac(materialId * 37.0 + 0.17);
    return saturate(0.35 + 0.65 * float3(
        frac(h + 0.00),
        frac(h + 0.37),
        frac(h + 0.71)));
}

PSOut PSMain(PSIn i)
{
    PSOut o;

    float3 N_base = normalize(i.worldNormal);
    float3 V = normalize(i.viewDir);

    float3 worldTangent = normalize(i.worldTangent.xyz);
    float3 worldBitangent = cross(N_base, worldTangent) * i.worldTangent.w;
    float3x3 TBN = float3x3(worldTangent, worldBitangent, N_base);

    float3 normalTS = g_normalMap.Sample(g_samp, i.uv).rgb * 2.0f - 1.0f;
    normalTS.xy *= g_normalMapIntensity * g_normalFactor.xy;
    normalTS = normalize(normalTS);
    float3 N = normalize(mul(normalTS, TBN));
    N = normalize(lerp(N_base, N, saturate(g_normalMapIntensity)));

    float4 texSample = g_base.Sample(g_samp, i.uv);
    float baseAlpha = texSample.a * g_textureFactor.a * g_diffuse.a;
    float alphaThreshold = (g_alphaCutout > 0.5f) ? 0.333f : 0.01f;
    clip(baseAlpha - alphaThreshold);

    uint materialType = g_materialType;
    float3 authoredBaseColor = LinearizeSrgb(texSample.rgb) * g_textureFactor.rgb * g_diffuse.rgb;
    float3 baseColor = authoredBaseColor;
    baseColor = ApplySaturation(baseColor, MaterialSaturationBoost(materialType));
    float3 shadowColor = MakeShadowColor(baseColor, materialType);

    float3 L = normalize(g_lightDir0);
    float3 fillDir = (dot(g_lightDir1, g_lightDir1) > 1.0e-5f) ? g_lightDir1 : float3(-0.5f, 0.5f, -0.25f);
    float3 Lfill = normalize(fillDir);
    float selfShadow = StylizeSelfShadow(SampleSelfShadow(i.worldPos, N, L), i.pos.xy);

    float ndotl01 = ComputeNdotL01(N, L);
    ndotl01 = ApplySkinLightRelaxation(ndotl01, materialType);

    float toonCoord = saturate(ndotl01 + g_shadowRampShift);
    float3 toonColor =
        (g_enableToon != 0)
        ? LinearizeSrgb(g_toon.Sample(g_toonSamp, float2(toonCoord, 0.5)).rgb) * g_toonFactor.rgb
        : toonCoord.xxx;
    float toonRamp = saturate(Luminance(toonColor));

    float toonLit = ComputeToonLit(ndotl01, selfShadow, toonRamp);
    float3 diffuse = lerp(shadowColor, baseColor, toonLit);
    diffuse = ApplySaturation(diffuse, lerp(1.0, MaterialSaturationBoost(materialType), 0.35));

    float fillNdotL01 = ComputeNdotL01(N, Lfill);
    fillNdotL01 = ApplySkinLightRelaxation(fillNdotL01, materialType);
    float fillLit =
        (g_enableToon != 0)
        ? smoothstep(0.12f, 0.88f, fillNdotL01 + 0.08f)
        : fillNdotL01;
    float fillShadowRelief = lerp(0.55f, 1.0f, selfShadow);
    float fillIntensity = max(g_lightInt1, 0.0f) * fillLit * fillShadowRelief;
    float3 fillDiffuse = lerp(shadowColor, baseColor, saturate(fillLit * 0.75f + 0.20f));

    float upFactor = 0.5 + 0.5 * saturate(N.y);
    float ambientInfluence = lerp(1.0, upFactor, saturate(g_ambientNormalInfluence));
    float3 ambient = shadowColor * g_ambientMat * (g_ambient + 0.05) * ambientInfluence;

    float keyIntensity = g_lightInt0 * lerp(1.0 - g_shadowStrength, 1.0, selfShadow);
    float3 color = diffuse * g_lightColor0 * keyIntensity + fillDiffuse * g_lightColor1 * fillIntensity + ambient;

    float specPower = lerp(8.0, 128.0, saturate(g_specPowerMat * 5.0));
    specPower *= max(g_specPower, 1.0) / 48.0;
    float specMask = ComputeToonSpecular(N_base, V, L, max(specPower, 1.0));
    float specMul = (g_toonParams1.y >= 0.0) ? g_toonParams1.y : g_specMul;
    specMask *= saturate(specMul) * saturate(toonLit + 0.35);
    float fillSpecMask = ComputeToonSpecular(N_base, V, Lfill, max(specPower, 1.0));
    fillSpecMask *= saturate(specMul) * saturate(fillLit + 0.25f) * fillShadowRelief * 0.35f * max(g_lightInt1, 0.0f);
    float specMaterialLum = Luminance(saturate(g_specularMat));
    float3 specularMat = (specMaterialLum > 0.001f)
        ? g_specularMat
        : lerp(baseColor, float3(1.0f, 1.0f, 1.0f), 0.35f) * 0.08f;
    float3 spec = specularMat * g_specColor * g_specStrength * (specMask * g_lightColor0 + fillSpecMask * g_lightColor1);
    color += spec;

    if (materialType == MATERIAL_HAIR)
    {
        float hairSpecMask = g_hairSpecMask.Sample(g_samp, i.uv).r;
        float globalHairSpec = saturate(g_specStrength / 0.45f);
        float hairSpec = ComputeHairSpecular(N_base, V, L) * hairSpecMask * saturate(toonLit + 0.25) * globalHairSpec;
        float fillHairSpec = ComputeHairSpecular(N_base, V, Lfill) * hairSpecMask * saturate(fillLit + 0.20f) * fillShadowRelief * 0.35f * max(g_lightInt1, 0.0f) * globalHairSpec;
        color += max(g_specularMat, float3(0.15, 0.15, 0.15)) * (hairSpec * g_lightColor0 + fillHairSpec * g_lightColor1);
    }

    float3x3 V3 = (float3x3)g_view;
    float3 Nview = normalize(mul(N, V3));
    float2 sphereUV = (g_sphereMode == 3) ? i.addUv1.xy : (Nview.xy * 0.5 + 0.5);
    float3 sphereTex = LinearizeSrgb(g_sphere.Sample(g_samp, sphereUV).rgb) * g_sphereFactor.rgb;
    color = ApplySphere(color, sphereTex, g_sphereMode);

    color *= g_brightness;
    color = ApplySaturation(color, g_globalSaturation);
    color = max(color, 0.0);

    if (g_toonDebugView == 1u)
    {
        color = authoredBaseColor;
    }
    else if (g_toonDebugView == 2u)
    {
        color = shadowColor;
    }
    else if (g_toonDebugView == 3u)
    {
        color = ndotl01.xxx;
    }
    else if (g_toonDebugView == 4u)
    {
        color = specMask.xxx;
    }
    else if (g_toonDebugView == 5u)
    {
        color = normalize(N) * 0.5 + 0.5;
    }
    else if (g_toonDebugView == 6u)
    {
        color = DebugMaterialColor(g_materialIdNormalized);
    }

    float3 outColor = (g_alphaCutout > 0.5f) ? color : color * baseAlpha;
    o.sceneColor = float4(outColor, baseAlpha);

    float rimClassMask = GetRimMask(materialType);
    rimClassMask = (g_toonParams0.x >= 0.0) ? g_toonParams0.x : rimClassMask;
    float rimMask = rimClassMask * g_rimMul;
    float edgeEnabled = saturate(g_edgeEnabled) * step(0.001, g_edgeSize) * step(0.001, g_edgeColor.a);
    float distanceToCamera = max(distance(g_cameraPos, i.worldPos), 1e-3);
    float distanceRatio = max(g_outlineRefDistance / distanceToCamera, 1e-3);
    float distanceFactor = pow(distanceRatio, max(g_outlineDistancePower, 0.01));
    float outlineScale = max(0.05, 1.0 + (distanceFactor - 1.0) * g_outlineDistanceScale);
    float outlinePixels = g_outlineBaseWidth * g_edgeSize * i.edgeScale * outlineScale * g_outlineWidthScale;
    float outlineWidthNorm = saturate(outlinePixels / 8.0);

    o.auxNormal = float4(normalize(N) * 0.5 + 0.5, float(materialType) / 255.0);
    o.auxToon = float4(ndotl01, specMask, saturate(rimMask), 1.0 - toonLit);
    o.auxOutline = float4(outlineWidthNorm, saturate(g_materialIdNormalized), edgeEnabled, saturate(1.0f - i.pos.z));
    o.auxEdgeColor = saturate(g_edgeColor);

    return o;
}
