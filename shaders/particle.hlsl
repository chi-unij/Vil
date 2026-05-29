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
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD;
    float4 color    : COLOR;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0), viewProj);
    output.uv       = input.uv;
    output.color    = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    // Procedural soft circle: map UV [0,1] -> [-1,1], compute radial falloff.
    float2 center = input.uv * 2.0 - 1.0;
    float dist2 = dot(center, center);

    // Smooth falloff from center to edge
    float glow = saturate(1.0 - dist2);
    glow *= glow; // sharpen the falloff for a nice glow shape

    float alpha = glow * input.color.a;

    // Discard fully transparent pixels
    clip(alpha - 0.001);

    // Output with premultiplied alpha for additive blending
    return float4(input.color.rgb * alpha, alpha);
}
