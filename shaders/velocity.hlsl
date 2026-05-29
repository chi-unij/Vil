// ======================================
// File: velocity.hlsl
// Purpose: Velocity generation pass (Phase 10.5 — Motion Blur).
//          Reconstructs world position from depth, then computes per-pixel
//          screen-space velocity using current and previous ViewProjection.
// ======================================

cbuffer VelocityCB : register(b0)
{
    float4x4 gInvViewProj;
    float4x4 gPrevViewProj;
};

Texture2D<float> gDepth : register(t0);

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

float2 PSMain(VSOut i) : SV_Target
{
    // Load exact depth value (no filtering).
    float depth = gDepth.Load(int3(i.pos.xy, 0));

    // Reconstruct clip-space position (DX left-handed NDC).
    float2 ndc = float2(i.uv.x * 2.0 - 1.0, (1.0 - i.uv.y) * 2.0 - 1.0);
    float4 clipPos = float4(ndc, depth, 1.0);

    // World position via inverse ViewProjection.
    float4 worldPos = mul(clipPos, gInvViewProj);
    worldPos /= worldPos.w;

    // Project into previous frame's clip space.
    float4 prevClip = mul(worldPos, gPrevViewProj);
    float2 prevNdc = prevClip.xy / prevClip.w;

    // Velocity in UV space (NDC [-1,1] -> UV [0,1] = * 0.5).
    float2 velocity = (ndc - prevNdc) * 0.5;

    return velocity;
}
