// ======================================
// File: particle.hlsl
// Purpose: Billboard particle rendering with procedural soft-circle glow.
//          Vertices are already billboard-expanded on CPU.
// ======================================

cbuffer ParticleCB : register(b0)
{
    float4x4 viewProj;
};

struct VSInput
{
    float3 position : POSITION;  // world-space (billboard-expanded)
    float2 uv       : TEXCOORD;  // 0..1 quad UVs
    float4 color    : COLOR;     // per-particle RGBA
    float4 params   : PARAMS;    // x=shape, y=slant
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD;
    float4 color    : COLOR;
    float4 params   : PARAMS;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0), viewProj);
    output.uv       = input.uv;
    output.color    = input.color;
    output.params   = input.params;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float alpha = 0.0;

    if (input.params.x > 0.5)
    {
        float lineCenter = 0.5 + (input.uv.y - 0.5) * input.params.y;
        float lineDist = abs(input.uv.x - lineCenter);
        float core = 1.0 - smoothstep(0.015, 0.13, lineDist);
        float headFade = smoothstep(0.00, 0.18, input.uv.y);
        float tailFade = 1.0 - smoothstep(0.82, 1.00, input.uv.y);
        alpha = core * headFade * tailFade * input.color.a;
    }
    else
    {
        float2 center = input.uv * 2.0 - 1.0;
        float dist2 = dot(center, center);
        float glow = saturate(1.0 - dist2);
        glow *= glow;
        alpha = glow * input.color.a;
    }

    clip(alpha - 0.001);
    return float4(input.color.rgb * alpha, alpha);
}