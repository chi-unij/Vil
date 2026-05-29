// ======================================
// File: ibl.hlsl
// Purpose: IBL precomputation shaders.
//   - EquirectToCubePS: convert equirectangular HDRI to cubemap
//   - IrradiancePS:     cosine-weighted hemisphere convolution
//   - PrefilteredPS:    GGX importance-sampled specular convolution
//   - BrdfLutPS:        split-sum BRDF integration LUT
// ======================================

cbuffer IBLConvolveCB : register(b0)
{
    float4x4 gFaceInvViewProj;
    float    gRoughness;
    float    gEnvCubeSize;   // resolution of source env cubemap (for mip bias)
    float2   gPad;
};

// Equirectangular source (Pass 1 only).
Texture2D<float4> gEquirectMap : register(t0);

// Cubemap source (Pass 2, 3).
TextureCube<float4> gEnvCube : register(t0);

SamplerState gLinearSamp : register(s0);

// ------- Common constants / utilities -------

static const float PI     = 3.14159265359f;
static const float TWO_PI = 6.28318530718f;

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// Fullscreen triangle (no vertex buffer, same pattern as sky.hlsl).
VSOut VSMain(uint vid : SV_VertexID)
{
    float2 p;
    p.x = (vid == 2) ? 3.0f : -1.0f;
    p.y = (vid == 1) ? -3.0f : 1.0f;

    VSOut o;
    o.pos = float4(p, 0.0f, 1.0f);
    o.uv  = float2((p.x + 1.0f) * 0.5f, (1.0f - p.y) * 0.5f);
    return o;
}

float3 ReconstructWorldDir(float2 uv, float4x4 invViewProj)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f);
    float4 clip = float4(ndc, 1.0f, 1.0f);
    float4 world = mul(clip, invViewProj);
    return normalize(world.xyz / world.w);
}

float2 DirToLatLongUV(float3 d)
{
    d = normalize(d);
    float phi   = atan2(d.z, d.x);
    float theta = acos(clamp(d.y, -1.0f, 1.0f));
    float u = phi / TWO_PI + 0.5f;
    float v = theta / PI;
    return float2(u, v);
}

// Van der Corput radical inverse (base 2).
float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f; // / 0x100000000
}

float2 Hammersley(uint i, uint N)
{
    return float2(float(i) / float(N), RadicalInverse_VdC(i));
}

float DistributionGGX(float NdotH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float denom = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * denom * denom, 1e-6f);
}

float GeometrySchlickGGX_IBL(float NdotV, float roughness)
{
    // IBL uses k = roughness^2 / 2 (not the direct-lighting remap).
    float a = roughness;
    float k = (a * a) / 2.0f;
    return NdotV / max(NdotV * (1.0f - k) + k, 1e-6f);
}

float GeometrySmith_IBL(float NdotV, float NdotL, float roughness)
{
    return GeometrySchlickGGX_IBL(NdotV, roughness) *
           GeometrySchlickGGX_IBL(NdotL, roughness);
}

float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness)
{
    float a = roughness * roughness;

    float phi      = TWO_PI * Xi.x;
    float cosTheta = sqrt((1.0f - Xi.y) / (1.0f + (a * a - 1.0f) * Xi.y));
    float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

    // Spherical to Cartesian (tangent space).
    float3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;

    // Tangent-space to world-space.
    float3 up    = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 right = normalize(cross(up, N));
    up = cross(N, right);

    return normalize(right * H.x + up * H.y + N * H.z);
}

// ======================================
// Pass 1: Equirectangular -> Cubemap
// ======================================

float4 EquirectToCubePS(VSOut i) : SV_Target
{
    float3 dir = ReconstructWorldDir(i.uv, gFaceInvViewProj);
    float2 uv  = DirToLatLongUV(dir);
    return gEquirectMap.SampleLevel(gLinearSamp, uv, 0);
}

// ======================================
// Pass 2: Irradiance Convolution
// ======================================

float4 IrradiancePS(VSOut i) : SV_Target
{
    float3 N = normalize(ReconstructWorldDir(i.uv, gFaceInvViewProj));

    float3 up    = abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 right = normalize(cross(up, N));
    up = cross(N, right);

    float3 irradiance = float3(0.0f, 0.0f, 0.0f);
    float  sampleDelta = 0.025f;
    uint   nrSamples = 0u;

    for (float phi = 0.0f; phi < TWO_PI; phi += sampleDelta)
    {
        for (float theta = 0.0f; theta < 0.5f * PI; theta += sampleDelta)
        {
            // Tangent-space direction to world-space.
            float3 tangent  = cos(phi) * right + sin(phi) * up;
            float3 sampleVec = cos(theta) * N + sin(theta) * tangent;

            irradiance += gEnvCube.SampleLevel(gLinearSamp, sampleVec, 0).rgb
                        * cos(theta) * sin(theta);
            nrSamples++;
        }
    }
    irradiance = PI * irradiance / float(nrSamples);
    return float4(irradiance, 1.0f);
}

// ======================================
// Pass 3: Prefiltered Specular (GGX)
// ======================================

float4 PrefilteredPS(VSOut i) : SV_Target
{
    float3 N = normalize(ReconstructWorldDir(i.uv, gFaceInvViewProj));
    float3 R = N;
    float3 V = R; // V = R = N assumption (split-sum approximation).

    static const uint SAMPLE_COUNT = 1024u;
    float totalWeight = 0.0f;
    float3 prefilteredColor = float3(0.0f, 0.0f, 0.0f);

    for (uint s = 0u; s < SAMPLE_COUNT; ++s)
    {
        float2 Xi = Hammersley(s, SAMPLE_COUNT);
        float3 H  = ImportanceSampleGGX(Xi, N, gRoughness);
        float3 L  = normalize(2.0f * dot(V, H) * H - V);

        float NdotL = saturate(dot(N, L));
        if (NdotL > 0.0f)
        {
            // Mip-level bias to reduce fireflies.
            float D     = DistributionGGX(saturate(dot(N, H)), gRoughness);
            float NdotH = saturate(dot(N, H));
            float HdotV = saturate(dot(H, V));
            float pdf   = D * NdotH / (4.0f * HdotV) + 0.0001f;

            float saTexel  = 4.0f * PI / (6.0f * gEnvCubeSize * gEnvCubeSize);
            float saSample = 1.0f / (float(SAMPLE_COUNT) * pdf + 0.0001f);
            float mipLevel = (gRoughness == 0.0f)
                           ? 0.0f
                           : 0.5f * log2(saSample / saTexel);

            prefilteredColor += gEnvCube.SampleLevel(gLinearSamp, L, mipLevel).rgb * NdotL;
            totalWeight += NdotL;
        }
    }
    return float4(prefilteredColor / max(totalWeight, 0.001f), 1.0f);
}

// ======================================
// Pass 4: BRDF Integration LUT
// ======================================

float2 IntegrateBRDF(float NdotV, float roughness)
{
    float3 V;
    V.x = sqrt(1.0f - NdotV * NdotV); // sin
    V.y = 0.0f;
    V.z = NdotV;                       // cos

    float3 N = float3(0.0f, 0.0f, 1.0f);

    float A = 0.0f;
    float B = 0.0f;

    static const uint SAMPLE_COUNT = 1024u;
    for (uint s = 0u; s < SAMPLE_COUNT; ++s)
    {
        float2 Xi = Hammersley(s, SAMPLE_COUNT);
        float3 H  = ImportanceSampleGGX(Xi, N, roughness);
        float3 L  = normalize(2.0f * dot(V, H) * H - V);

        float NdotL = saturate(L.z);
        float NdotH = saturate(H.z);
        float VdotH = saturate(dot(V, H));

        if (NdotL > 0.0f)
        {
            float G     = GeometrySmith_IBL(max(NdotV, 0.0f), NdotL, roughness);
            float G_Vis = (G * VdotH) / max(NdotH * NdotV, 1e-4f);
            float Fc    = pow(1.0f - VdotH, 5.0f);

            A += (1.0f - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }
    return float2(A, B) / float(SAMPLE_COUNT);
}

float4 BrdfLutPS(VSOut i) : SV_Target
{
    float2 brdf = IntegrateBRDF(max(i.uv.x, 0.001f), i.uv.y);
    return float4(brdf, 0.0f, 1.0f);
}
