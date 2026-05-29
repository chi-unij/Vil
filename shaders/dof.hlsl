// ======================================
// File: dof.hlsl
// Purpose: Depth of Field post-process (Phase 10.6)
//          Single-pass gather blur using Poisson disc sampling.
//          Computes Circle of Confusion (CoC) from depth, then blurs
//          proportionally. Supports both near and far field blur.
// ======================================

// Root constants (b0): DOF parameters
cbuffer DofCB : register(b0)
{
    float gFocalDistance;   // world-space distance to focal plane
    float gFocalRange;     // world-space transition zone width
    float gMaxBlur;        // max blur radius in pixels
    float gNearZ;          // camera near plane
    float gFarZ;           // camera far plane
    float gTexelSizeX;     // 1.0 / screen width
    float gTexelSizeY;     // 1.0 / screen height
    float gPad;            // padding to 8 constants
};

Texture2D<float4> gColor : register(t0);   // LDR scene color
Texture2D<float>  gDepth : register(t1);   // depth buffer (R32_FLOAT, 0..1 NDC)
SamplerState      gSamp  : register(s0);   // linear clamp

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// Fullscreen triangle (same pattern as all post-process shaders)
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

// Convert NDC depth [0,1] to linear eye-space distance.
float LinearizeDepth(float ndcDepth)
{
    // Reverse the perspective projection: z_ndc = (far * near) / (far - depth * (far - near))
    // For a standard DX12 projection where depth 0 = near, 1 = far:
    return (gNearZ * gFarZ) / (gFarZ - ndcDepth * (gFarZ - gNearZ));
}

// Compute Circle of Confusion.
// Returns signed CoC: negative = near field, positive = far field.
// Magnitude represents blur strength (0 = in focus, 1 = max blur).
float ComputeCoC(float linearDepth)
{
    float diff = linearDepth - gFocalDistance;
    // Normalize by focal range — objects within range are in focus
    float coc = diff / max(gFocalRange, 0.001);
    // Clamp to [-1, 1]
    return clamp(coc, -1.0, 1.0);
}

// 16-sample Poisson disc (unit circle, pre-computed).
static const float2 kPoissonDisc[16] = {
    float2(-0.9465, -0.1028),
    float2(-0.8132,  0.4780),
    float2(-0.5048, -0.7658),
    float2(-0.3856,  0.0498),
    float2(-0.2682,  0.5667),
    float2(-0.1340, -0.3981),
    float2(-0.0620,  0.9270),
    float2( 0.0550, -0.8544),
    float2( 0.1292,  0.2007),
    float2( 0.2851, -0.1580),
    float2( 0.3632,  0.6104),
    float2( 0.4628, -0.5671),
    float2( 0.6062,  0.1019),
    float2( 0.6932, -0.3018),
    float2( 0.7793,  0.4554),
    float2( 0.9326, -0.0867),
};

float4 PSDof(VSOut pin) : SV_TARGET
{
    // Sample center pixel
    float centerDepthRaw = gDepth.SampleLevel(gSamp, pin.uv, 0).r;
    float centerDepth = LinearizeDepth(centerDepthRaw);
    float centerCoC = ComputeCoC(centerDepth);
    float absCoC = abs(centerCoC);

    // If CoC is negligible, return sharp color (skip blur)
    if (absCoC < 0.01)
    {
        return gColor.SampleLevel(gSamp, pin.uv, 0);
    }

    // Blur radius in UV space
    float radiusX = absCoC * gMaxBlur * gTexelSizeX;
    float radiusY = absCoC * gMaxBlur * gTexelSizeY;

    float3 colorSum = float3(0, 0, 0);
    float weightSum = 0.0;

    [unroll]
    for (int i = 0; i < 16; ++i)
    {
        float2 offset = float2(kPoissonDisc[i].x * radiusX,
                               kPoissonDisc[i].y * radiusY);
        float2 sampleUV = pin.uv + offset;

        // Read sample color and depth
        float3 sampleColor = gColor.SampleLevel(gSamp, sampleUV, 0).rgb;
        float sampleDepthRaw = gDepth.SampleLevel(gSamp, sampleUV, 0).r;
        float sampleDepth = LinearizeDepth(sampleDepthRaw);
        float sampleCoC = ComputeCoC(sampleDepth);

        // Weight: allow blur contribution if the sample is also out of focus.
        // For far field: sample must be at same depth or further to contribute
        //   (prevents sharp foreground from bleeding into blurry background).
        // For near field: near-field samples always contribute (near blur overlaps everything).
        float sampleAbsCoC = abs(sampleCoC);
        float w = 1.0;

        if (centerCoC > 0.0)
        {
            // Far field: only accept samples that are also far-blurred
            // or closer (which would have their own near blur)
            w = smoothstep(0.0, 0.3, sampleAbsCoC);
        }
        else
        {
            // Near field: accept all samples but weight by their near CoC
            w = smoothstep(0.0, 0.3, max(sampleAbsCoC, absCoC));
        }

        colorSum += sampleColor * w;
        weightSum += w;
    }

    // Normalize
    float3 blurred = (weightSum > 0.001) ? colorSum / weightSum
                                          : gColor.SampleLevel(gSamp, pin.uv, 0).rgb;

    // Lerp between sharp and blurred based on CoC magnitude
    float3 sharp = gColor.SampleLevel(gSamp, pin.uv, 0).rgb;
    float blendFactor = smoothstep(0.0, 1.0, absCoC);
    float3 result = lerp(sharp, blurred, blendFactor);

    return float4(result, 1.0);
}
