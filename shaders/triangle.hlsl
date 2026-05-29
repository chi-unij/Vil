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
    o.pos = float4(v.pos, 1.0);
    o.color = v.color;
    return o;
}

float4 PSMain(PSIn i) : SV_TARGET
{
    return i.color;
}

