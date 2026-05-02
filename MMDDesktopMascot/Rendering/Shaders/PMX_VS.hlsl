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
    float4 tangent : TANGENT;
};

#include "SkinningCommon.hlsli"

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

PSIn VSMain(VSIn i)
{
    PSIn o;

    float3 skinnedPos = i.pos;
    float3 skinnedNrm = i.nrm;
    float3 skinnedTan = i.tangent.xyz;

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
                ApplySdefSkin(i, skinnedPos, skinnedNrm, skinnedTan);
            }
            else if (i.weightType == 4)
            {
                ApplyQdefSkin(i, skinnedPos, skinnedNrm, skinnedTan);
            }
            else
            {
                ApplyLinearSkin(i, skinnedPos, skinnedNrm, skinnedTan);
            }
        }
    }

    float3x3 normalMatrix = float3x3(
        g_normalMatrixRow0.xyz,
        g_normalMatrixRow1.xyz,
        g_normalMatrixRow2.xyz
    );

    float4 worldPos = mul(float4(skinnedPos, 1.0), g_model);
    o.pos = mul(float4(skinnedPos, 1.0), g_mvp);
    o.worldPos = worldPos.xyz;
    o.worldNormal = normalize(mul(normalMatrix, skinnedNrm));
    o.viewDir = normalize(g_cameraPos - worldPos.xyz);
    o.uv = i.uv;
    o.addUv1 = i.addUv1;

    float3 worldTan = normalize(mul(normalMatrix, skinnedTan));
    // Re-orthogonalize tangent against normal
    worldTan = normalize(worldTan - o.worldNormal * dot(o.worldNormal, worldTan));
    o.worldTangent = float4(worldTan, i.tangent.w);

    return o;
}
