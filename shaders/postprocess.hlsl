// Tonemapping pass: HDR scene + bloom + AO -> LDR output.
// ACES filmic tonemapping (Narkowicz fit) + gamma.
// Stores luminance in alpha for FXAA.

cbuffer TonemapCB : register(b0)
{
    float gExposure;
    float gBloomIntensity;
    float gAOStrength;
    float gPad;
};

Texture2D<float4> gHdrScene : register(t0);
Texture2D<float4> gBloom    : register(t1);
Texture2D<float>  gAOTex    : register(t2);
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

// ACES filmic tonemapping (Narkowicz 2015 fit).
float3 ACESFilm(float3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 hdr = gHdrScene.SampleLevel(gSamp, i.uv, 0).rgb;
    float3 bloom = gBloom.SampleLevel(gSamp, i.uv, 0).rgb;

    // Composite bloom.
    hdr += bloom * gBloomIntensity;

    // Apply SSAO: lerp(1, ao, strength) so strength=0 means no AO effect.
    float ao = gAOTex.SampleLevel(gSamp, i.uv, 0);
    hdr *= lerp(1.0, ao, gAOStrength);

    // Apply exposure.
    hdr *= gExposure;

    // ACES tonemap.
    float3 ldr = ACESFilm(hdr);

    // Gamma encode.
    ldr = pow(ldr, 1.0 / 2.2);

    // Store luminance in alpha for FXAA.
    float luma = Luminance(ldr);
    return float4(ldr, luma);
}
