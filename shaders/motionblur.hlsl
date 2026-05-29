// ======================================
// File: motionblur.hlsl
// Purpose: Motion blur post-process pass (Phase 10.5).
//          Samples along per-pixel velocity direction to produce directional blur.
// ======================================

cbuffer MotionBlurCB : register(b0)
{
    float gStrength;        // Blur intensity multiplier
    float gInvSampleCount;  // 1.0 / numSamples
    float2 gPad;
};

Texture2D<float4> gColor    : register(t0);  // LDR input (from tonemap)
Texture2D<float2> gVelocity : register(t1);  // Velocity buffer
SamplerState      gSamp     : register(s0);  // Linear clamp

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSFullscreen(uint vid : SV_VertexID)
{
    float2 p;
    p.x = (vid == 2) ? 3.0 : -1.0;
    p.y = (vid == 1) ? -3.0 : 1.0;

    VSOut o;
    o.pos = float4(p, 0.0, 1.0);
    o.uv  = float2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 velocity = gVelocity.SampleLevel(gSamp, i.uv, 0) * gStrength;

    // Clamp max blur length to 5% of screen to prevent extreme streaking.
    float len = length(velocity);
    float maxLen = 0.05;
    if (len > maxLen)
        velocity *= (maxLen / len);

    // Center sample.
    float4 color = gColor.SampleLevel(gSamp, i.uv, 0);
    float totalWeight = 1.0;

    // Sample along velocity direction, centered on current pixel.
    int numSamples = max(int(1.0 / gInvSampleCount), 1);
    for (int s = 1; s < numSamples; ++s)
    {
        float t = (float(s) / float(numSamples)) - 0.5;  // range [-0.5, ~0.5)
        float2 sampleUV = i.uv + velocity * t;
        color += gColor.SampleLevel(gSamp, sampleUV, 0);
        totalWeight += 1.0;
    }

    color /= totalWeight;

    // Preserve luminance in alpha for FXAA downstream.
    float luma = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
    return float4(color.rgb, luma);
}
