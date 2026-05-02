struct PSIn
{
    float4 pos : SV_POSITION;
    float3 worldPos : TEXCOORD1;
    float3 worldNormal : TEXCOORD2;
    float3 viewDir : TEXCOORD3;
    float2 uv : TEXCOORD0;
    float4 addUv1 : TEXCOORD4;
    float4 worldTangent : TEXCOORD5;
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
    float2 _pad2;
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
    float3 _pad3;
};

Texture2D g_base : register(t0);
Texture2D g_toon : register(t1);
Texture2D g_sphere : register(t2);
Texture2D g_normalMap : register(t3);
Texture2D g_shadowMap : register(t4);
SamplerState g_samp : register(s0);
SamplerState g_toonSamp : register(s1);
SamplerComparisonState g_shadowSamp : register(s2);

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

float3 MakeShadowColor(float3 baseLinear)
{
    float3 hsv = RgbToHsv(baseLinear);
    hsv.x = frac(hsv.x + g_shadowHueShift * 0.15915494);
    hsv.y = saturate(hsv.y + g_shadowSaturation);
    hsv.z = hsv.z * 0.8;
    return HsvToRgb(hsv);
}

float ScreenStableNoise(float2 pixel)
{
    float2 p = floor(pixel);
    return frac(52.9829189 * frac(0.06711056 * p.x + 0.00583715 * p.y));
}

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
    float depthBias = g_shadowBias * lerp(2.2, 0.8, ndotl);
    float2 texel = float2(g_shadowMapInvSize, g_shadowMapInvSize);

    float visibility = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            visibility += g_shadowMap.SampleCmpLevelZero(
                g_shadowSamp,
                shadowUv + float2(x, y) * texel,
                shadowDepth - depthBias);
        }
    }

    return visibility / 9.0;
}

float StylizeSelfShadow(float visibility, float2 pixel)
{
    if (g_enableToon == 0 || g_enableSelfShadow == 0)
    {
        return visibility;
    }

    float threshold = saturate(0.5 + g_shadowRampShift * 0.18);
    float softness = max(0.02, g_shadowDeepSoftness * 0.9 + 0.015);
    float jitter = (ScreenStableNoise(pixel) - 0.5) * softness * 0.5;
    return smoothstep(
        threshold - softness + jitter,
        threshold + softness + jitter,
        visibility);
}

float StylizeSpecular(float raw, float stepControl)
{
    float edge = saturate(stepControl);
    float width = max(0.05, 0.35 - edge * 0.25);
    float start = saturate(1.0 - width * 2.0);
    float endv = saturate(1.0 - width);
    return smoothstep(start * edge, endv, raw);
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

float4 PSMain(PSIn i) : SV_TARGET
{
    float3 N_base = normalize(i.worldNormal);
    float3 V = normalize(i.viewDir);

    // Normal mapping
    float3 worldTangent = normalize(i.worldTangent.xyz);
    float3 worldBitangent = cross(N_base, worldTangent) * i.worldTangent.w;
    float3x3 TBN = float3x3(worldTangent, worldBitangent, N_base);

    float3 normalTS = g_normalMap.Sample(g_samp, i.uv).rgb;
    normalTS = normalTS * 2.0f - 1.0f;
    normalTS.xy *= g_normalMapIntensity * g_normalFactor.xy;
    normalTS = normalize(normalTS);
    float3 N = normalize(mul(normalTS, TBN));

    float4 texSample = g_base.Sample(g_samp, i.uv);
    float baseAlpha = texSample.a * g_textureFactor.a * g_diffuse.a;
    if (baseAlpha < 0.01)
        discard;

    float3 albedo = LinearizeSrgb(texSample.rgb) * g_textureFactor.rgb * g_diffuse.rgb;
    float3 shadowColor = MakeShadowColor(albedo);

    float3 L0 = normalize(g_lightDir0);
    float3 L1 = normalize(g_lightDir1);
    float shadowVisibility = SampleSelfShadow(i.worldPos, N, L0);
    shadowVisibility = StylizeSelfShadow(shadowVisibility, i.pos.xy);
    float keyShadow = lerp(1.0 - g_shadowStrength, 1.0, shadowVisibility);

    float NdotL0 = saturate(dot(N, L0));
    float NdotL1 = saturate(dot(N, L1));

    float toonContrast = g_toonContrast * g_toonContrastMul;

    float shade0 = pow(NdotL0 * 0.5 + 0.5, toonContrast) * keyShadow;
    float shade1 = pow(NdotL1 * 0.5 + 0.5, toonContrast * 0.7);

    float keyIntensity = g_lightInt0 * keyShadow;
    float lightWeight = max(keyIntensity + g_lightInt1 * 0.5, 1e-4);
    float shade = saturate((shade0 * keyIntensity + shade1 * g_lightInt1 * 0.5) / lightWeight);
    float toonCoord = saturate(shade);
    float rnd = ScreenStableNoise(i.pos.xy);

    float ditherStrength = 0.015;
    float toonLookup = saturate(toonCoord + (rnd - 0.5) * ditherStrength);
    float3 toonColor =
        (g_enableToon != 0)
        ? LinearizeSrgb(g_toon.Sample(g_toonSamp, float2(toonLookup, 0.5)).rgb) * g_toonFactor.rgb
        : toonLookup.xxx;
    float toonRamp = saturate(Luminance(toonColor) - g_shadowRampShift);

    float3 midLit = lerp(shadowColor, albedo * toonColor, toonRamp);

    float deepT = saturate(g_shadowDeepThreshold);
    float deepSoft = max(0.001, g_shadowDeepSoftness);
    float deepMul = saturate(g_shadowDeepMul);

    float deepMask = 1.0 - smoothstep(deepT - deepSoft, deepT + deepSoft, toonCoord);
    deepMask *= g_shadowMul;
    float3 deepShadow = shadowColor * deepMul;

    float3 litAlbedo = lerp(midLit, deepShadow, deepMask);

    float3 H0 = normalize(L0 + V);
    float3 H1 = normalize(L1 + V);
    float specPowerScale = max(g_specPower, 1.0) / 48.0;
    float sp = max(g_specPowerMat * specPowerScale, 1.0);
    float rawSpec0 = pow(saturate(dot(N_base, H0)), sp);
    float rawSpec1 = pow(saturate(dot(N_base, H1)), sp);
    float specStep = g_specularStep * g_specMul;
    float specBand0 = (g_enableToon != 0) ? StylizeSpecular(rawSpec0, specStep) : rawSpec0;
    float specBand1 = (g_enableToon != 0) ? StylizeSpecular(rawSpec1, specStep) : rawSpec1;
    float3 specColor = g_specularMat * g_specColor;
    float3 spec = specColor * g_specStrength *
        (specBand0 * g_lightColor0 * keyIntensity +
         specBand1 * g_lightColor1 * g_lightInt1 * 0.5);

    float3 diff = litAlbedo *
        (g_lightColor0 * keyIntensity + g_lightColor1 * g_lightInt1 * 0.5);

    float3 ambient = shadowColor * g_ambientMat * (g_ambient + 0.05);

    // Rim
    float rim = pow(1.0 - saturate(dot(N, V)), 1.5 + g_rimWidth * 2.0);
    float rimBand = smoothstep(g_rimWidth * 0.35, g_rimWidth, rim);
    float3 rimCol = litAlbedo * g_rimIntensity * rimBand;

    // Sphere
    float3x3 V3 = (float3x3)g_view;
    float3 Nview = normalize(mul(N, V3));
    float2 sphereUV = (g_sphereMode == 3) ? i.addUv1.xy : (Nview.xy * 0.5 + 0.5);
    float3 sphereTex = LinearizeSrgb(g_sphere.Sample(g_samp, sphereUV).rgb) * g_sphereFactor.rgb;

    float3 color = diff + ambient + spec + rimCol;
    color = ApplySphere(color, sphereTex, g_sphereMode);

    color *= g_brightness;

    color = ApplySaturation(color, g_globalSaturation);
    color = max(color, 0.0);

    float3 outColor = (g_alphaCutout > 0.5) ? color : (color * baseAlpha);
    return float4(outColor, baseAlpha);
}
