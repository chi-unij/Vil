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

    if (input.params.x > 3.5)
    {
        float fracture = sin(input.uv.y * 18.0 + input.params.y * 8.0) * 0.052
                       + sin(input.uv.y * 47.0 - input.params.y * 13.0) * 0.021;
        float center = 0.5 + (input.uv.y - 0.5) * input.params.y * 0.30
                     + fracture;
        float halfWidth = lerp(0.16, 0.025, input.uv.y);
        float body = 1.0 - smoothstep(halfWidth, halfWidth + 0.055,
                                     abs(input.uv.x - center));
        float core = 1.0 - smoothstep(0.008, 0.030,
                                     abs(input.uv.x - center));
        float broken = 0.72 + 0.28 * smoothstep(-0.25, 0.35,
            sin(input.uv.y * 38.0 + input.params.y * 6.0));
        float baseFade = smoothstep(0.00, 0.10, input.uv.y);
        float tipFade = 1.0 - smoothstep(0.84, 1.00, input.uv.y);
        alpha = (body * 0.52 + core) * broken * baseFade * tipFade
              * input.color.a;
    }
    else if (input.params.x > 2.5)
    {
        float sway = sin(input.uv.y * 10.0 + input.params.y) * 0.075
                   + sin(input.uv.y * 23.0 - input.params.y * 1.4) * 0.028;
        float center = 0.5 + sway * (0.35 + input.uv.y * 0.65);
        float halfWidth = lerp(0.42, 0.045, input.uv.y);
        float body = 1.0 - smoothstep(halfWidth, halfWidth + 0.09,
                                     abs(input.uv.x - center));
        float core = 1.0 - smoothstep(halfWidth * 0.18,
                                     halfWidth * 0.52 + 0.018,
                                     abs(input.uv.x - center));
        float baseFade = smoothstep(0.00, 0.08, input.uv.y);
        float tipFade = 1.0 - smoothstep(0.76, 1.00, input.uv.y);
        alpha = (body * 0.58 + core * 0.72) * baseFade * tipFade
              * input.color.a;
    }
    else if (input.params.x > 1.5)
    {
        float phase = input.params.y * 9.0;
        float jagged = sin(input.uv.y * 31.0 + phase) * 0.080
                     + sin(input.uv.y * 73.0 - phase * 1.7) * 0.032;
        float lineCenter = 0.5
                         + (input.uv.y - 0.5) * input.params.y * 0.42
                         + jagged;
        float lineDist = abs(input.uv.x - lineCenter);
        float outer = 1.0 - smoothstep(0.025, 0.115, lineDist);
        float core = 1.0 - smoothstep(0.008, 0.038, lineDist);
        float headFade = smoothstep(0.00, 0.12, input.uv.y);
        float tailFade = 1.0 - smoothstep(0.88, 1.00, input.uv.y);
        alpha = (outer * 0.48 + core) * headFade * tailFade * input.color.a;
    }
    else if (input.params.x > 0.5)
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
