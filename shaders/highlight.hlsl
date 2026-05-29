// ======================================
// File: highlight.hlsl
// Purpose: Simple wireframe highlight for selected entities.
//          VS transforms position, PS outputs a solid color.
// ======================================

cbuffer HighlightCB : register(b0)
{
    float4x4 gWorldViewProj;
    float4   gColor; // highlight color (e.g. bright yellow)
};

struct VSIn
{
    float3 pos : POSITION;
};

struct PSIn
{
    float4 pos : SV_POSITION;
};

PSIn VSMain(VSIn v)
{
    PSIn o;
    o.pos = mul(float4(v.pos, 1.0f), gWorldViewProj);
    return o;
}

float4 PSMain(PSIn i) : SV_TARGET
{
    return gColor;
}
