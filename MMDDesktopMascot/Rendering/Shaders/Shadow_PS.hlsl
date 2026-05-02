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

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

Texture2D g_base : register(t0);
SamplerState g_samp : register(s0);

void PSMain(PSIn i)
{
    float alpha = (g_base.Sample(g_samp, i.uv) * g_textureFactor).a * g_diffuse.a;
    float alphaThreshold = (g_alphaCutout > 0.5f) ? 0.333f : 0.01f;
    clip(alpha - alphaThreshold);
}
