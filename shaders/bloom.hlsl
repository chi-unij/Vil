// Bloom pass shaders: downsample (13-tap) + upsample (9-tap tent).
// Uses fullscreen triangle (SV_VertexID, no vertex buffer).

cbuffer BloomCB : register(b0)
{
    float2 gTexelSize;   // 1/width, 1/height of source texture
    float  gThreshold;   // brightness threshold (only used on first downsample)
    float  gPad;
};

Texture2D<float4> gInput : register(t0);
SamplerState gSamp : register(s0);

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
    o.uv = float2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);
    return o;
}

// 13-tap box downsample (from Call of Duty: Advanced Warfare presentation).
// Samples a 4x4 region with overlapping bilinear taps for quality.
float4 PSDownsample(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float2 ts = gTexelSize;

    // Center + 4 corner groups (13 taps total)
    float3 a = gInput.SampleLevel(gSamp, uv + float2(-2, -2) * ts, 0).rgb;
    float3 b = gInput.SampleLevel(gSamp, uv + float2( 0, -2) * ts, 0).rgb;
    float3 c = gInput.SampleLevel(gSamp, uv + float2( 2, -2) * ts, 0).rgb;
    float3 d = gInput.SampleLevel(gSamp, uv + float2(-2,  0) * ts, 0).rgb;
    float3 e = gInput.SampleLevel(gSamp, uv + float2( 0,  0) * ts, 0).rgb;
    float3 f = gInput.SampleLevel(gSamp, uv + float2( 2,  0) * ts, 0).rgb;
    float3 g = gInput.SampleLevel(gSamp, uv + float2(-2,  2) * ts, 0).rgb;
    float3 h = gInput.SampleLevel(gSamp, uv + float2( 0,  2) * ts, 0).rgb;
    float3 ii = gInput.SampleLevel(gSamp, uv + float2( 2,  2) * ts, 0).rgb;
    float3 j = gInput.SampleLevel(gSamp, uv + float2(-1, -1) * ts, 0).rgb;
    float3 k = gInput.SampleLevel(gSamp, uv + float2( 1, -1) * ts, 0).rgb;
    float3 l = gInput.SampleLevel(gSamp, uv + float2(-1,  1) * ts, 0).rgb;
    float3 m = gInput.SampleLevel(gSamp, uv + float2( 1,  1) * ts, 0).rgb;

    // Weighted combination
    float3 color = e * 0.125;
    color += (j + k + l + m) * 0.125;
    color += (a + c + g + ii) * 0.03125;
    color += (b + d + f + h) * 0.0625;

    // Apply brightness threshold (soft knee) on first downsample pass only.
    if (gThreshold > 0.0)
    {
        float brightness = max(color.r, max(color.g, color.b));
        float knee = gThreshold * 0.5;
        float soft = brightness - gThreshold + knee;
        soft = clamp(soft, 0.0, 2.0 * knee);
        soft = soft * soft / (4.0 * knee + 1e-5);
        float contribution = max(soft, brightness - gThreshold) / max(brightness, 1e-5);
        color *= contribution;
    }

    return float4(color, 1.0);
}

// 9-tap tent upsample filter. Rendered with additive blend PSO.
float4 PSUpsample(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float2 ts = gTexelSize;

    float3 color = 0;
    color += gInput.SampleLevel(gSamp, uv + float2(-1, -1) * ts, 0).rgb * 1.0;
    color += gInput.SampleLevel(gSamp, uv + float2( 0, -1) * ts, 0).rgb * 2.0;
    color += gInput.SampleLevel(gSamp, uv + float2( 1, -1) * ts, 0).rgb * 1.0;
    color += gInput.SampleLevel(gSamp, uv + float2(-1,  0) * ts, 0).rgb * 2.0;
    color += gInput.SampleLevel(gSamp, uv + float2( 0,  0) * ts, 0).rgb * 4.0;
    color += gInput.SampleLevel(gSamp, uv + float2( 1,  0) * ts, 0).rgb * 2.0;
    color += gInput.SampleLevel(gSamp, uv + float2(-1,  1) * ts, 0).rgb * 1.0;
    color += gInput.SampleLevel(gSamp, uv + float2( 0,  1) * ts, 0).rgb * 2.0;
    color += gInput.SampleLevel(gSamp, uv + float2( 1,  1) * ts, 0).rgb * 1.0;
    color /= 16.0;

    return float4(color, 1.0);
}
