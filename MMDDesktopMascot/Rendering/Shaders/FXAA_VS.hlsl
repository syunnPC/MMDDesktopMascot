struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    // 3頂点のフルスクリーントライアングル
    float2 pos;
    if (vid == 0)
        pos = float2(-1.0, -1.0);
    else if (vid == 1)
        pos = float2(-1.0, 3.0);
    else
        pos = float2(3.0, -1.0);

    VSOut o;
    o.pos = float4(pos, 0.0, 1.0);

    // NDC [-1,1] → UV [0,1]
    o.uv = float2(0.5 * pos.x + 0.5, -0.5 * pos.y + 0.5);
    return o;
}
