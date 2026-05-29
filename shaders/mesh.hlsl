cbuffer MeshCB : register(b0)
{
    float4x4 gView;                // view matrix (for SSAO view-space normals)
    float4x4 gProj;                // projection matrix
    float4   gCameraPos;           // xyz
    float4   gLightDirIntensity;   // xyz = direction of sun rays, w = intensity
    float4   gLightColorRoughness; // rgb = color, w = roughness (fallback)
    float4   gMetallicPad;         // x = metallic (fallback), y = iblIntensity, z = cascadeDebug
    float4x4 gCascadeLightViewProj[4]; // per-cascade world -> light clip
    float4   gShadowParams;        // xy = texelSize, z = bias, w = strength
    float4   gCascadeSplits;       // xyz = split distances (view-space), w = cascadeCount
    float4   gEmissiveFactor;      // rgb = emissive factor, w = unused
    float4   gPOMParams;          // x = heightScale, y = minLayers, z = maxLayers, w = enabled
    float4   gBaseColorFactor;    // rgba multiplier for base color
    float4   gUVTilingOffset;     // xy=tiling, zw=offset
    float4   gAnimParams;        // x=gameTime, y=materialTypeId
};

// Per-instance world matrices (Phase 12.5 — Instanced Rendering).
StructuredBuffer<float4x4> gInstanceWorlds : register(t10);

// Bone palette for skeletal animation (Phase 2C).
StructuredBuffer<float4x4> gBones : register(t11);

struct VSIn
{
    float3 pos      : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float4 tangent  : TANGENT;   // xyz = tangent dir, w = handedness
    uint4  boneIdx  : BLENDINDICES;
    float4 boneWgt  : BLENDWEIGHT;
};

struct PSIn
{
    float4 pos       : SV_POSITION;
    float3 nrmW      : TEXCOORD1;
    float2 uv        : TEXCOORD0;
    float3 posW      : TEXCOORD2;
    float  viewZ     : TEXCOORD3; // view-space depth for cascade selection
    float3 tangentW  : TEXCOORD4;
    float  tangentW_w: TEXCOORD5; // handedness
};

// MRT output: HDR color + view-space normal for SSAO.
struct PSOut
{
    float4 color    : SV_TARGET0;
    float4 normalVS : SV_TARGET1;
};

PSIn VSMain(VSIn v, uint instId : SV_InstanceID)
{
    // Skeletal skinning: if bone weights sum > 0, apply bone transforms.
    float3 skinnedPos = v.pos;
    float3 skinnedNrm = v.normal;
    float3 skinnedTan = v.tangent.xyz;

    float wSum = v.boneWgt.x + v.boneWgt.y + v.boneWgt.z + v.boneWgt.w;
    if (wSum > 0.001f)
    {
        float4 nw = v.boneWgt / wSum; // normalize weights
        float4x4 skin = nw.x * gBones[v.boneIdx.x]
                       + nw.y * gBones[v.boneIdx.y]
                       + nw.z * gBones[v.boneIdx.z]
                       + nw.w * gBones[v.boneIdx.w];
        skinnedPos = mul(float4(v.pos, 1.0f), skin).xyz;
        skinnedNrm = mul(float4(v.normal, 0.0f), skin).xyz;
        skinnedTan = mul(float4(v.tangent.xyz, 0.0f), skin).xyz;
    }

    float4x4 world = gInstanceWorlds[instId];
    float4x4 wvp = mul(world, mul(gView, gProj));

    PSIn o;
    o.pos = mul(float4(skinnedPos, 1.0f), wvp);
    float4 posW = mul(float4(skinnedPos, 1.0f), world);
    o.posW = posW.xyz;
    o.nrmW = mul(float4(skinnedNrm, 0.0f), world).xyz;
    o.uv = v.uv;
    o.viewZ = o.pos.w;
    o.tangentW = mul(float4(skinnedTan, 0.0f), world).xyz;
    o.tangentW_w = v.tangent.w;
    return o;
}

// Material textures (t0-t5).
Texture2D gBaseColorMap   : register(t0);
Texture2D gNormalMap      : register(t1);
Texture2D gMetalRoughMap  : register(t2);
Texture2D gAOMap          : register(t3);
Texture2D gEmissiveMap    : register(t4);
Texture2D gHeightMap      : register(t5);
SamplerState gSam         : register(s0);

// Shadow map (t6).
Texture2DArray<float> gShadowMap : register(t6);
SamplerComparisonState gShadowSamp : register(s1);

// IBL textures (t7-t9).
TextureCube<float4> gIrradianceMap  : register(t7);
TextureCube<float4> gPrefilteredMap : register(t8);
Texture2D<float2>   gBrdfLUT       : register(t9);
SamplerState        gIBLSampler    : register(s2);

#include "procedural_tiles.hlsli"

static const float PI = 3.14159265f;

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(1.0f - cosTheta, 5.0f);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    float3 maxR = max((1.0f - roughness).xxx, F0);
    return F0 + (maxR - F0) * pow(1.0f - cosTheta, 5.0f);
}

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * denom * denom, 1e-6f);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotV / max(NdotV * (1.0f - k) + k, 1e-6f);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    return GeometrySchlickGGX(NdotV, roughness) *
           GeometrySchlickGGX(NdotL, roughness);
}

// Parallax Occlusion Mapping: ray-march through height field to find offset UV.
// Uses SampleLevel (mip 0) inside the loop to avoid gradient issues in dynamic branches.
float2 ParallaxOcclusionMap(float2 uv, float3 viewDirTS, float heightScale,
                            float minLayers, float maxLayers)
{
    // More layers at grazing angles where parallax is most visible.
    float numLayers = lerp(maxLayers, minLayers, abs(viewDirTS.z));
    float layerDepth = 1.0f / numLayers;
    float currentLayerDepth = 0.0f;

    // UV offset per layer: project view dir onto surface plane.
    float2 P = viewDirTS.xy * heightScale;
    float2 deltaUV = P / numLayers;

    float2 currentUV = uv;
    float currentHeight = 1.0f - gHeightMap.SampleLevel(gSam, currentUV, 0).r;

    // Step through layers until ray goes below the height field.
    [loop]
    for (int step = 0; step < (int)maxLayers; ++step)
    {
        if (currentLayerDepth >= currentHeight)
            break;
        currentUV -= deltaUV;
        currentHeight = 1.0f - gHeightMap.SampleLevel(gSam, currentUV, 0).r;
        currentLayerDepth += layerDepth;
    }

    // Linear interpolation between last two samples for sub-step precision.
    float2 prevUV = currentUV + deltaUV;
    float afterDepth  = currentHeight - currentLayerDepth;
    float beforeDepth = (1.0f - gHeightMap.SampleLevel(gSam, prevUV, 0).r)
                        - (currentLayerDepth - layerDepth);
    float weight = afterDepth / (afterDepth - beforeDepth);
    return lerp(currentUV, prevUV, weight);
}

PSOut PSMain(PSIn i)
{
    // ---- TBN normal mapping ----
    float3 N = normalize(i.nrmW);
    float3 T = normalize(i.tangentW);
    // Gram-Schmidt re-orthogonalize T against N.
    T = normalize(T - dot(T, N) * N);
    float3 B = cross(N, T) * i.tangentW_w; // handedness
    float3x3 TBN = float3x3(T, B, N);

    // ---- UV tiling/offset ----
    float2 uv = i.uv * gUVTilingOffset.xy + gUVTilingOffset.zw;

    // ---- Parallax Occlusion Mapping (UV offset) ----
    if (gPOMParams.w > 0.5f)
    {
        float3 V_world = normalize(gCameraPos.xyz - i.posW);
        // Transform view direction to tangent space (TBN rows = T, B, N).
        float3 viewDirTS = normalize(float3(dot(V_world, T), dot(V_world, B), dot(V_world, N)));
        uv = ParallaxOcclusionMap(uv, viewDirTS, gPOMParams.x, gPOMParams.y, gPOMParams.z);
    }

    // Sample normal map (tangent-space). Default flat normal = (0,0,1).
    float3 normalTS = gNormalMap.Sample(gSam, uv).rgb * 2.0f - 1.0f;
    N = normalize(mul(normalTS, TBN));

    // ---- Sample material textures ----
    // BaseColor sampled as SRGB view => returned as LINEAR here.
    float3 albedo = gBaseColorMap.Sample(gSam, uv).rgb * gBaseColorFactor.rgb;

    // MetallicRoughness: glTF convention — G=roughness, B=metallic.
    // Multiply by per-material factors (Phase 11.5) so sliders scale the texture.
    float4 mr = gMetalRoughMap.Sample(gSam, uv);
    float roughness = saturate(mr.g * gLightColorRoughness.w);
    roughness = max(roughness, 0.045f);
    float metallic = saturate(mr.b * gMetallicPad.x);

    // AO texture (single channel, applied to ambient only).
    float ao = gAOMap.Sample(gSam, uv).r;

    // ---- Procedural tile override (Phase 9) ----
    float3 procEmissive = float3(0.0f, 0.0f, 0.0f);
    bool procActive = false;
    {
        ProceduralResult procResult;
        if (ApplyProceduralTile(i.posW, gAnimParams.x, gAnimParams.y, procResult))
        {
            albedo    = procResult.albedo;
            procEmissive = procResult.emissive;
            metallic  = procResult.metallic;
            roughness = max(procResult.roughness, 0.045f);
            ao        = procResult.ao;
            N = normalize(mul(procResult.normalTS, TBN));
            procActive = true;
        }
    }

    float3 V = normalize(gCameraPos.xyz - i.posW);

    // Directional light: store "sun rays direction" (direction light travels).
    // For shading we want L = direction FROM surface TO light = -raysDir.
    float3 raysDir = normalize(gLightDirIntensity.xyz);
    float3 L = normalize(-raysDir);
    float3 H = normalize(V + L);

    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float3 lightColor = gLightColorRoughness.rgb * gLightDirIntensity.w;

    // Cook-Torrance BRDF (GGX)
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3  F = FresnelSchlick(VdotH, F0);
    float   D = DistributionGGX(NdotH, roughness);
    float   G = GeometrySmith(NdotV, NdotL, roughness);
    float3  spec = (D * G) * F / max(4.0f * NdotV * NdotL, 1e-4f);

    float3 kS = F;
    float3 kD = (1.0f - kS) * (1.0f - metallic);
    float3 diff = kD * albedo / PI;

    float3 color = (diff + spec) * lightColor * NdotL;

    // CSM: select cascade by view-space depth and sample shadow map array.
    float shadowFactor = 1.0f;
    if (gShadowParams.w > 0.0f && gShadowParams.x > 0.0f) {
        int cascadeCount = (int)gCascadeSplits.w;
        float viewZ = i.viewZ;

        // Select cascade: first one whose split distance exceeds our depth.
        int cascade = cascadeCount - 1;
        float splitDists[3] = { gCascadeSplits.x, gCascadeSplits.y, gCascadeSplits.z };
        for (int c = 0; c < cascadeCount; ++c) {
            if (viewZ < splitDists[c]) {
                cascade = c;
                break;
            }
        }

        // Transform world position to this cascade's light clip space.
        float4 posLS = mul(float4(i.posW, 1.0f), gCascadeLightViewProj[cascade]);
        float3 ndc = posLS.xyz / posLS.w;

        float2 shadowUV = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
        float depth = ndc.z;

        // Outside the shadow map / light frustum => treat as lit.
        if (shadowUV.x >= 0.0f && shadowUV.x <= 1.0f && shadowUV.y >= 0.0f && shadowUV.y <= 1.0f &&
            depth >= 0.0f && depth <= 1.0f) {
            float2 texel = gShadowParams.xy;
            float bias = gShadowParams.z;

            float sum = 0.0f;
            [unroll]
            for (int y = -1; y <= 1; ++y) {
                [unroll]
                for (int x = -1; x <= 1; ++x) {
                    float2 o = float2((float)x, (float)y) * texel;
                    sum += gShadowMap.SampleCmpLevelZero(
                        gShadowSamp,
                        float3(shadowUV + o, (float)cascade),
                        depth - bias);
                }
            }
            float shadow = sum / 9.0f; // 0..1 visibility
            shadowFactor = lerp(1.0f, shadow, saturate(gShadowParams.w));
        }
    }

    // Apply shadows only to direct lighting (ambient stays).
    color *= shadowFactor;

    // Debug: tint by cascade index (toggle via gMetallicPad.z).
    if (gMetallicPad.z > 0.5f && gShadowParams.w > 0.0f) {
        int cascadeCount = (int)gCascadeSplits.w;
        float viewZ = i.viewZ;
        int cascade = cascadeCount - 1;
        float splitDists[3] = { gCascadeSplits.x, gCascadeSplits.y, gCascadeSplits.z };
        for (int c = 0; c < cascadeCount; ++c) {
            if (viewZ < splitDists[c]) { cascade = c; break; }
        }
        float3 cascadeColors[4] = {
            float3(1,0.2,0.2), float3(0.2,1,0.2),
            float3(0.2,0.2,1), float3(1,1,0.2)
        };
        color = lerp(color, cascadeColors[cascade], 0.3f);
    }

    // IBL ambient (split-sum approximation). Apply AO to ambient only.
    {
        float3 F0_ibl = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
        float3 kS_ibl = FresnelSchlickRoughness(max(NdotV, 0.0f), F0_ibl, roughness);
        float3 kD_ibl = (1.0f - kS_ibl) * (1.0f - metallic);

        float3 irradiance  = gIrradianceMap.Sample(gIBLSampler, N).rgb;
        float3 diffuseIBL  = kD_ibl * albedo * irradiance;

        float3 R = reflect(-V, N);
        float3 prefilteredColor = gPrefilteredMap.SampleLevel(gIBLSampler, R, roughness * 4.0f).rgb;
        float2 brdf = gBrdfLUT.Sample(gIBLSampler, float2(max(NdotV, 0.0f), roughness)).rg;
        float3 specularIBL = prefilteredColor * (F0_ibl * brdf.x + brdf.y);

        float iblIntensity = gMetallicPad.y;
        color += (diffuseIBL + specularIBL) * iblIntensity * ao;
    }

    // Emissive: add before tonemap so bloom picks it up.
    if (procActive)
    {
        color += procEmissive;
    }
    else
    {
        float3 emissive = gEmissiveMap.Sample(gSam, uv).rgb;
        color += emissive * gEmissiveFactor.rgb;
    }

    // MRT output: linear HDR color + view-space normal (packed to [0,1]).
    PSOut output;
    output.color = float4(color, 1.0f);
    float3 normalView = normalize(mul(float4(N, 0.0f), gView).xyz);
    output.normalVS = float4(normalView * 0.5f + 0.5f, 1.0f);
    return output;
}
