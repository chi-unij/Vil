cbuffer SceneCB : register(b0)
{
    float4x4 gWorldViewProj;
};

struct VSIn
{
    float3 pos   : POSITION;
    float4 color : COLOR;
};

struct PSIn
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
};

PSIn VSMain(VSIn v)
{
    PSIn o;
    o.pos = mul(float4(v.pos, 1.0f), gWorldViewProj);
    o.color = v.color;
    return o;
}

float4 PSMain(PSIn i) : SV_TARGET
{
    return i.color;
}

