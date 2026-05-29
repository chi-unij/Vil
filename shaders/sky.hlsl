// Fullscreen sky pass using an equirectangular HDR texture (lat-long).

cbuffer SkyCB : register(b0)
{
    float4x4 gInvViewProj; // inverse(view * proj)
    float3   gCameraPos;
    float    gExposure;
};

Texture2D<float4> gEnvLatLong : register(t0);
SamplerState gSamp : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    // Fullscreen triangle (no vertex buffer).
    float2 p;
    p.x = (vid == 2) ? 3.0 : -1.0;
    p.y = (vid == 1) ? -3.0 : 1.0;

    VSOut o;
    o.pos = float4(p, 0.0, 1.0);
    o.uv = float2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);
    return o;
}

static const float PI = 3.14159265359;
static const float TWO_PI = 6.28318530718;

float2 DirToLatLongUV(float3 d)
{
    d = normalize(d);
    // Left-handed: +Z forward, +X right, +Y up.
    // Map direction to equirectangular (u: 0..1, v: 0..1).
    float phi = atan2(d.z, d.x);          // -pi..pi
    // NOTE: d.y is in [-1, 1]. Using saturate() breaks the lower hemisphere.
    float theta = acos(clamp(d.y, -1.0, 1.0));    // 0..pi
    float u = phi / TWO_PI + 0.5;
    float v = theta / PI;
    return float2(u, v);
}

float4 PSMain(VSOut i) : SV_Target
{
    // Reconstruct a world position on the far plane from screen uv.
    float2 ndc = float2(i.uv.x * 2.0 - 1.0, (1.0 - i.uv.y) * 2.0 - 1.0);
    float4 clip = float4(ndc, 1.0, 1.0);
    float4 world = mul(clip, gInvViewProj);
    world.xyz /= world.w;

    float3 dir = normalize(world.xyz - gCameraPos);
    float2 envUV = DirToLatLongUV(dir);

    float3 hdr = gEnvLatLong.SampleLevel(gSamp, envUV, 0).rgb;
    hdr *= gExposure;

    // Output linear HDR — tonemapping happens in post-process.
    return float4(hdr, 1.0);
}

