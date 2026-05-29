// FXAA 3.11 quality implementation (simplified).
// Reads luminance from alpha channel (stored by tonemap pass).

cbuffer FxaaCB : register(b0)
{
    float2 gRcpFrame;  // 1/width, 1/height
    float2 gPad;
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

// Quality settings.
static const float FXAA_EDGE_THRESHOLD     = 0.0625;  // 1/16
static const float FXAA_EDGE_THRESHOLD_MIN = 0.0312;  // 1/32
static const float FXAA_SUBPIX_QUALITY     = 0.75;
static const int   FXAA_SEARCH_STEPS       = 8;

float4 PSMain(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float2 rcp = gRcpFrame;

    // Sample center + 4 neighbors (luminance from alpha).
    float lumaM  = gInput.SampleLevel(gSamp, uv, 0).a;
    float lumaN  = gInput.SampleLevel(gSamp, uv + float2( 0, -1) * rcp, 0).a;
    float lumaS  = gInput.SampleLevel(gSamp, uv + float2( 0,  1) * rcp, 0).a;
    float lumaW  = gInput.SampleLevel(gSamp, uv + float2(-1,  0) * rcp, 0).a;
    float lumaE  = gInput.SampleLevel(gSamp, uv + float2( 1,  0) * rcp, 0).a;

    float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    float lumaRange = lumaMax - lumaMin;

    // Early exit if contrast is low.
    if (lumaRange < max(FXAA_EDGE_THRESHOLD_MIN, lumaMax * FXAA_EDGE_THRESHOLD))
        return gInput.SampleLevel(gSamp, uv, 0);

    // Compute sub-pixel aliasing test.
    float lumaNW = gInput.SampleLevel(gSamp, uv + float2(-1, -1) * rcp, 0).a;
    float lumaNE = gInput.SampleLevel(gSamp, uv + float2( 1, -1) * rcp, 0).a;
    float lumaSW = gInput.SampleLevel(gSamp, uv + float2(-1,  1) * rcp, 0).a;
    float lumaSE = gInput.SampleLevel(gSamp, uv + float2( 1,  1) * rcp, 0).a;

    float lumaL = (lumaN + lumaS + lumaW + lumaE) * 0.25;
    float rangeL = abs(lumaL - lumaM);
    float blendL = saturate(rangeL / lumaRange);
    blendL = smoothstep(0, 1, blendL);
    blendL = blendL * blendL * FXAA_SUBPIX_QUALITY;

    // Determine edge direction (horizontal vs vertical).
    float edgeH = abs(lumaNW + lumaNE - 2.0 * lumaN) +
                  2.0 * abs(lumaW + lumaE - 2.0 * lumaM) +
                  abs(lumaSW + lumaSE - 2.0 * lumaS);
    float edgeV = abs(lumaNW + lumaSW - 2.0 * lumaW) +
                  2.0 * abs(lumaN + lumaS - 2.0 * lumaM) +
                  abs(lumaNE + lumaSE - 2.0 * lumaE);
    bool isHorizontal = (edgeH >= edgeV);

    // Choose step direction perpendicular to edge.
    float stepLength = isHorizontal ? rcp.y : rcp.x;
    float luma1 = isHorizontal ? lumaN : lumaW;
    float luma2 = isHorizontal ? lumaS : lumaE;
    float gradient1 = abs(luma1 - lumaM);
    float gradient2 = abs(luma2 - lumaM);

    bool is1Steeper = gradient1 >= gradient2;
    float gradientScaled = 0.25 * max(gradient1, gradient2);
    float lumaLocalAvg;

    if (is1Steeper) {
        stepLength = -stepLength;
        lumaLocalAvg = 0.5 * (luma1 + lumaM);
    } else {
        lumaLocalAvg = 0.5 * (luma2 + lumaM);
    }

    // Shift UV to edge.
    float2 currentUv = uv;
    if (isHorizontal) {
        currentUv.y += stepLength * 0.5;
    } else {
        currentUv.x += stepLength * 0.5;
    }

    // Search along edge in both directions.
    float2 offset = isHorizontal ? float2(rcp.x, 0.0) : float2(0.0, rcp.y);
    float2 uv1 = currentUv - offset;
    float2 uv2 = currentUv + offset;

    float lumaEnd1 = gInput.SampleLevel(gSamp, uv1, 0).a - lumaLocalAvg;
    float lumaEnd2 = gInput.SampleLevel(gSamp, uv2, 0).a - lumaLocalAvg;

    bool reached1 = abs(lumaEnd1) >= gradientScaled;
    bool reached2 = abs(lumaEnd2) >= gradientScaled;
    bool reachedBoth = reached1 && reached2;

    if (!reached1) uv1 -= offset;
    if (!reached2) uv2 += offset;

    if (!reachedBoth) {
        for (int s = 2; s < FXAA_SEARCH_STEPS; s++) {
            if (!reached1) {
                lumaEnd1 = gInput.SampleLevel(gSamp, uv1, 0).a - lumaLocalAvg;
                reached1 = abs(lumaEnd1) >= gradientScaled;
            }
            if (!reached2) {
                lumaEnd2 = gInput.SampleLevel(gSamp, uv2, 0).a - lumaLocalAvg;
                reached2 = abs(lumaEnd2) >= gradientScaled;
            }
            reachedBoth = reached1 && reached2;
            if (!reached1) uv1 -= offset;
            if (!reached2) uv2 += offset;
            if (reachedBoth) break;
        }
    }

    // Estimate sub-pixel offset.
    float dist1 = isHorizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float dist2 = isHorizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);

    bool isDirection1 = dist1 < dist2;
    float distFinal = min(dist1, dist2);
    float edgeLength = dist1 + dist2;
    float pixelOffset = -distFinal / edgeLength + 0.5;

    bool isLumaMSmaller = lumaM < lumaLocalAvg;
    bool correctVariation = ((isDirection1 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaMSmaller;
    float finalOffset = correctVariation ? pixelOffset : 0.0;
    finalOffset = max(finalOffset, blendL);

    // Apply offset.
    float2 finalUv = uv;
    if (isHorizontal) {
        finalUv.y += finalOffset * stepLength;
    } else {
        finalUv.x += finalOffset * stepLength;
    }

    float3 color = gInput.SampleLevel(gSamp, finalUv, 0).rgb;
    return float4(color, 1.0);
}
