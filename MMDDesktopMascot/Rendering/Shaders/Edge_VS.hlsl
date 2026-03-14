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
};

cbuffer BoneCB : register(b2)
{
    float4x4 g_boneMatrices[1024];
};

struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv : TEXCOORD0;
    int4 boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
    float3 sdefC : TEXCOORD1;
    float3 sdefR0 : TEXCOORD2;
    float3 sdefR1 : TEXCOORD3;
    uint weightType : TEXCOORD4;
    float edgeScale : TEXCOORD5;
    float4 addUv1 : TEXCOORD6;
    float4 addUv2 : TEXCOORD7;
    float4 addUv3 : TEXCOORD8;
    float4 addUv4 : TEXCOORD9;
};

#include "SkinningCommon.hlsli"

struct PSIn
{
    float4 pos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD0;
};

PSIn VSMain(VSIn i)
{
    PSIn o;
    float3 skinnedPos = i.pos;
    float3 skinnedNrm = i.nrm;

    if (g_enableSkinning != 0)
    {
        float totalWeight = 0.0;
        if (i.boneIndices[0] >= 0)
            totalWeight += i.boneWeights[0];
        if (i.boneIndices[1] >= 0)
            totalWeight += i.boneWeights[1];
        if (i.boneIndices[2] >= 0)
            totalWeight += i.boneWeights[2];
        if (i.boneIndices[3] >= 0)
            totalWeight += i.boneWeights[3];

        if (totalWeight > 0.001)
        {
            if (i.weightType == 3)
            {
                ApplySdefSkin(i, skinnedPos, skinnedNrm);
            }
            else if (i.weightType == 4)
            {
                ApplyQdefSkin(i, skinnedPos, skinnedNrm);
            }
            else
            {
                ApplyLinearSkin(i, skinnedPos, skinnedNrm);
            }
        }
    }

    float3x3 normalMatrix = float3x3(
        g_normalMatrixRow0.xyz,
        g_normalMatrixRow1.xyz,
        g_normalMatrixRow2.xyz
    );

    float3 worldPos = mul(float4(skinnedPos, 1.0), g_model).xyz;
    float3 worldNrm = normalize(mul(normalMatrix, skinnedNrm));
    float3 viewDir = normalize(g_cameraPos - worldPos);

    float ndv = saturate(abs(dot(worldNrm, viewDir)));
    float rim = 1.0 - ndv;

    float rimWeight = smoothstep(0.05, 0.35, rim);

    float widthFactor = lerp(0.15, 1.0, rimWeight);
    float dist = max(distance(g_cameraPos, worldPos), 1e-3);
    float distanceRatio = max(g_outlineRefDistance / dist, 1e-3);
    float distanceFactor = pow(distanceRatio, max(g_outlineDistancePower, 0.01));
    float outlineScale = max(0.05, 1.0 + (distanceFactor - 1.0) * g_outlineDistanceScale);

    float outlineWidthScale = max(g_outlineWidthScale, 0.0);
    float baseEdgePixels = g_edgeSize * i.edgeScale * 1.2 * widthFactor * outlineScale * outlineWidthScale;
    float minEdgePixels = 0.5 * saturate(outlineWidthScale);
    float edgePixels = max(minEdgePixels, baseEdgePixels);
    float4 clipPos = mul(float4(skinnedPos, 1.0), g_mvp);
    float4 clipNrmPos = mul(float4(skinnedPos + skinnedNrm, 1.0), g_mvp);

    float2 ndcBase = clipPos.xy / max(clipPos.w, 1e-5);
    float2 ndcNrm = clipNrmPos.xy / max(clipNrmPos.w, 1e-5);
    float2 ndcDir = ndcNrm - ndcBase;
    float dirLenSq = dot(ndcDir, ndcDir);
    if (dirLenSq < 1e-8)
    {
        ndcDir = float2(0.0, 1.0);
    }
    else
    {
        ndcDir *= rsqrt(dirLenSq);
    }

    float2 clipOffset = ndcDir * (edgePixels * 2.0) * g_invScreenSize * clipPos.w;
    o.pos = clipPos;
    o.pos.xy += clipOffset;

    float4 col = g_edgeColor;
    col.a = saturate(col.a * lerp(0.2, 1.0, rimWeight) * max(g_outlineOpacityScale, 0.0));
    o.color = col;
    o.uv = i.uv;
    return o;
}
