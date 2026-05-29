// ======================================
// File: ssao.hlsl
// Purpose: Screen-Space Ambient Occlusion (Phase 10.3).
//          Hemisphere kernel sampling + bilateral blur.
// ======================================

// ---- SSAO Generation Constants (b0) ----
cbuffer SSAOConstants : register(b0) {
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gView;      // Phase 12.1: world-space → view-space normal transform
    float4   gParams;     // (radius, bias, power, kernelSize)
    float4   gScreenSize; // (ssaoW, ssaoH, fullW, fullH)
    float4   gKernel[64];
};

Texture2D<float>  gDepthTex   : register(t0);
Texture2D<float4> gNormalTex  : register(t1);
Texture2D<float4> gNoiseTex   : register(t2);

SamplerState gSampPoint  : register(s0); // point clamp
SamplerState gSampNoise  : register(s1); // point wrap

// ---- Blur Constants (b0) ----
cbuffer BlurConstants : register(b0) {
    float gTexelX;
    float gTexelY;
    float gPad0;
    float gPad1;
};

Texture2D<float> gAOInput    : register(t0);
Texture2D<float> gDepthBlur  : register(t1);

// ============================================================================
// Fullscreen triangle vertex shader (shared by both passes).
// ============================================================================
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSFullscreen(uint vid : SV_VertexID) {
    VSOut o;
    // Generate fullscreen triangle from vertex ID.
    o.uv  = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

// ============================================================================
// Reconstruct view-space position from depth + UV.
// ============================================================================
float3 ReconstructViewPos(float2 uv, float depth) {
    // NDC: x [-1,1], y [-1,1], z [0,1] (DX convention)
    float4 ndc = float4(uv * 2.0f - 1.0f, depth, 1.0f);
    ndc.y = -ndc.y; // flip Y for DX
    float4 viewPos = mul(ndc, gInvProj);
    return viewPos.xyz / viewPos.w;
}

// ============================================================================
// PSGenerateSSAO — hemisphere kernel sampling.
// ============================================================================
float4 PSGenerateSSAO(VSOut pin) : SV_TARGET {
    float radius     = gParams.x;
    float bias       = gParams.y;
    float power      = gParams.z;
    int   kernelSize = (int)gParams.w;

    // Sample depth at full-res UV.
    float depth = gDepthTex.Sample(gSampPoint, pin.uv);
    if (depth >= 1.0f)
        return 1.0f; // sky — no occlusion

    // Reconstruct view-space position.
    float3 fragPos = ReconstructViewPos(pin.uv, depth);

    // Sample world-space normal from G-buffer, transform to view space.
    float3 normalWorld = gNormalTex.Sample(gSampPoint, pin.uv).xyz;
    float3 normal = normalize(mul(float4(normalWorld, 0.0f), gView).xyz);

    // Tile noise texture across screen.
    float2 noiseScale = float2(gScreenSize.x / 4.0f, gScreenSize.y / 4.0f);
    float3 randomVec = gNoiseTex.Sample(gSampNoise, pin.uv * noiseScale).xyz;

    // Build TBN (tangent-bitangent-normal) from normal + random rotation.
    float3 tangent   = normalize(randomVec - normal * dot(randomVec, normal));
    float3 bitangent = cross(normal, tangent);
    float3x3 TBN     = float3x3(tangent, bitangent, normal);

    // Accumulate occlusion.
    float occlusion = 0.0f;
    for (int i = 0; i < kernelSize; ++i) {
        // Transform kernel sample to view space.
        float3 sampleDir = mul(gKernel[i].xyz, TBN);
        float3 samplePos = fragPos + sampleDir * radius;

        // Project sample to screen space.
        float4 offset = float4(samplePos, 1.0f);
        offset = mul(offset, gProj);
        offset.xy /= offset.w;
        // NDC to UV.
        float2 sampleUV = offset.xy * float2(0.5f, -0.5f) + 0.5f;

        // Sample depth at projected position.
        float sampleDepth = gDepthTex.Sample(gSampPoint, sampleUV);
        float3 sampleViewPos = ReconstructViewPos(sampleUV, sampleDepth);

        // Range check: fade out samples that are too far from fragment.
        float rangeCheck = smoothstep(0.0f, 1.0f,
            radius / (abs(fragPos.z - sampleViewPos.z) + 0.0001f));

        // Occlusion test: surface closer than sample → sample is behind geometry.
        // In LH view space (Z positive into screen), closer = smaller Z.
        occlusion += (sampleViewPos.z <= samplePos.z - bias ? 1.0f : 0.0f) * rangeCheck;
    }

    float ao = 1.0f - (occlusion / (float)kernelSize);
    ao = pow(saturate(ao), power);
    return ao;
}

// ============================================================================
// PSBilateralBlur — 5x5 depth-aware blur to smooth SSAO noise.
// ============================================================================
float4 PSBilateralBlur(VSOut pin) : SV_TARGET {
    float2 texelSize = float2(gTexelX, gTexelY);
    float  centerDepth = gDepthBlur.Sample(gSampPoint, pin.uv);
    float  result = 0.0f;
    float  totalWeight = 0.0f;

    [unroll]
    for (int x = -2; x <= 2; ++x) {
        [unroll]
        for (int y = -2; y <= 2; ++y) {
            float2 offset = float2((float)x, (float)y) * texelSize;
            float2 sampleUV = pin.uv + offset;

            float aoSample    = gAOInput.Sample(gSampPoint, sampleUV);
            float depthSample = gDepthBlur.Sample(gSampPoint, sampleUV);

            // Bilateral weight: reduce contribution if depth differs significantly.
            float depthDiff = abs(centerDepth - depthSample);
            float weight = exp(-depthDiff * 1000.0f); // sharp falloff at edges

            // Spatial Gaussian weight (approximate).
            float dist2 = (float)(x * x + y * y);
            weight *= exp(-dist2 / 4.5f);

            result += aoSample * weight;
            totalWeight += weight;
        }
    }

    return result / (totalWeight + 0.0001f);
}
