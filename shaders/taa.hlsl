// ======================================
// File: taa.hlsl
// Purpose: Temporal Anti-Aliasing resolve (Phase 10.4).
//          Reprojects previous frame via velocity, uses variance clipping
//          in YCoCg color space for robust neighborhood rejection.
// ======================================

cbuffer TaaCB : register(b0)
{
    float2 gTexelSize;    // 1.0 / resolution
    float  gBlendFactor;  // base alpha (~0.05 for static pixels)
    float  gFirstFrame;   // 1.0 on first frame (no history)
};

Texture2D<float4> gCurrent  : register(t0); // current frame HDR (jittered)
Texture2D<float4> gHistory  : register(t1); // previous frame TAA output
Texture2D<float2> gVelocity : register(t2); // screen-space motion vectors

SamplerState gSampler : register(s0); // bilinear clamp

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

// --- Color space helpers ---
float3 RGBToYCoCg(float3 rgb)
{
    float Y  = dot(rgb, float3(0.25, 0.5, 0.25));
    float Co = dot(rgb, float3(0.5, 0.0, -0.5));
    float Cg = dot(rgb, float3(-0.25, 0.5, -0.25));
    return float3(Y, Co, Cg);
}

float3 YCoCgToRGB(float3 ycocg)
{
    float Y  = ycocg.x;
    float Co = ycocg.y;
    float Cg = ycocg.z;
    return float3(Y + Co - Cg, Y + Cg, Y - Co - Cg);
}

float Luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// --- Variance clipping (Salvi 2016 / INSIDE TAA) ---
// Computes mean and stddev of 3x3 neighborhood in YCoCg, then clips
// history towards the mean along the AABB defined by mean ± gamma*stddev.
float3 ClipToAABB(float3 history, float3 aabbMin, float3 aabbMax, float3 avg)
{
    float3 center = 0.5 * (aabbMin + aabbMax);
    float3 extent = 0.5 * (aabbMax - aabbMin) + 0.0001;

    float3 offset = history - center;
    float3 ts = abs(extent / max(abs(offset), 0.0001));
    float t = saturate(min(ts.x, min(ts.y, ts.z)));
    return center + offset * t;
}

float4 PSMain(VSOut i) : SV_Target
{
    int2 pixelCoord = int2(i.pos.xy);
    float3 current = gCurrent.Load(int3(pixelCoord, 0)).rgb;

    // First frame: no history, pass through.
    if (gFirstFrame > 0.5)
        return float4(current, 1.0);

    // Read velocity and reproject.
    float2 velocity = gVelocity.Load(int3(pixelCoord, 0));
    float2 historyUV = i.uv - velocity;

    // Reject out-of-screen reprojection.
    if (any(historyUV < 0.0) || any(historyUV > 1.0))
        return float4(current, 1.0);

    // Sample history with bilinear filtering (sub-pixel accuracy).
    float3 history = gHistory.SampleLevel(gSampler, historyUV, 0).rgb;

    // --- Variance clipping in YCoCg ---
    // Gather 3x3 neighborhood, compute mean and variance.
    float3 m1 = float3(0.0, 0.0, 0.0); // sum
    float3 m2 = float3(0.0, 0.0, 0.0); // sum of squares

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            float3 c = gCurrent.Load(int3(pixelCoord + int2(dx, dy), 0)).rgb;
            float3 ycocg = RGBToYCoCg(c);
            m1 += ycocg;
            m2 += ycocg * ycocg;
        }
    }

    float3 mean = m1 / 9.0;
    float3 variance = abs(m2 / 9.0 - mean * mean);
    float3 stddev = sqrt(variance);

    // Gamma controls clipping tightness: lower = more stable but more ghosting.
    // 1.0 is tight (stable), 1.25 is the sweet spot for most scenes.
    float gamma = 1.0;

    float3 aabbMin = mean - gamma * stddev;
    float3 aabbMax = mean + gamma * stddev;

    // Clip history in YCoCg space.
    float3 historyYCoCg = RGBToYCoCg(history);
    float3 clippedYCoCg = ClipToAABB(historyYCoCg, aabbMin, aabbMax, mean);
    history = YCoCgToRGB(clippedYCoCg);

    // Ensure no negative values after color space conversion.
    history = max(history, 0.0);

    // Adaptive blend: more current frame contribution when pixels are moving fast.
    float velocityPixels = length(velocity / gTexelSize);
    float alpha = lerp(gBlendFactor, 0.5, saturate(velocityPixels / 5.0));

    // Luminance-weighted blending (Karis 2014) to reduce flickering on HDR pixels.
    float lumCurrent = Luminance(current);
    float lumHistory = Luminance(history);
    float wCurrent = alpha / (1.0 + lumCurrent);
    float wHistory = (1.0 - alpha) / (1.0 + lumHistory);
    float3 result = (current * wCurrent + history * wHistory) /
                    max(wCurrent + wHistory, 0.00001);

    return float4(result, 1.0);
}
