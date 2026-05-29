// ======================================
// File: gbuffer.hlsl
// Purpose: G-buffer generation pass (Phase 12.1 — Deferred Rendering).
//          Outputs surface properties to 4 MRT (albedo, normal, material, emissive).
//          No lighting computation — that happens in deferred_lighting.hlsl.
// ======================================

cbuffer GBufferCB : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
    float4   gCameraPos;        // xyz
    float4   gMaterialFactors;  // x=metallic, y=roughness, z=unused, w=unused
    float4   gEmissiveFactor;   // rgb=emissive factor, w=unused
    float4   gPOMParams;        // x=heightScale, y=minLayers, z=maxLayers, w=enabled
    float4   gBaseColorFactor;  // rgba multiplier for base color
    float4   gUVTilingOffset;   // xy=tiling, zw=offset
    float4   gAnimParams;      // x=gameTime, y=materialTypeId
};

// Per-instance world matrices (Phase 12.5 — Instanced Rendering).
StructuredBuffer<float4x4> gInstanceWorlds : register(t6);

// Bone palette for skeletal animation (Phase 2C).
StructuredBuffer<float4x4> gBones : register(t7);

struct VSIn
{
    float3 pos      : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float4 tangent  : TANGENT;
    uint4  boneIdx  : BLENDINDICES;
    float4 boneWgt  : BLENDWEIGHT;
};

struct PSIn
{
    float4 pos       : SV_POSITION;
    float3 nrmW      : TEXCOORD1;
    float2 uv        : TEXCOORD0;
    float3 posW      : TEXCOORD2;
    float3 tangentW  : TEXCOORD3;
    float  tangentW_w: TEXCOORD4;
};

struct PSOut
{
    float4 albedo   : SV_TARGET0;  // R8G8B8A8_UNORM
    float4 normal   : SV_TARGET1;  // R16G16B16A16_FLOAT (world-space)
    float4 material : SV_TARGET2;  // R8G8B8A8_UNORM (R=metallic, G=roughness, B=AO)
    float4 emissive : SV_TARGET3;  // R11G11B10_FLOAT (HDR)
};

// Material textures (t0-t5).
Texture2D gBaseColorMap   : register(t0);
Texture2D gNormalMap      : register(t1);
Texture2D gMetalRoughMap  : register(t2);
Texture2D gAOMap          : register(t3);
Texture2D gEmissiveMap    : register(t4);
Texture2D gHeightMap      : register(t5);
SamplerState gSam         : register(s0);

#include "procedural_tiles.hlsli"

// Parallax Occlusion Mapping: ray-march through height field to find offset UV.
float2 ParallaxOcclusionMap(float2 uv, float3 viewDirTS, float heightScale,
                            float minLayers, float maxLayers)
{
    float numLayers = lerp(maxLayers, minLayers, abs(viewDirTS.z));
    float layerDepth = 1.0f / numLayers;
    float currentLayerDepth = 0.0f;

    float2 P = viewDirTS.xy * heightScale;
    float2 deltaUV = P / numLayers;

    float2 currentUV = uv;
    float currentHeight = 1.0f - gHeightMap.SampleLevel(gSam, currentUV, 0).r;

    [loop]
    for (int step = 0; step < (int)maxLayers; ++step)
    {
        if (currentLayerDepth >= currentHeight)
            break;
        currentUV -= deltaUV;
        currentHeight = 1.0f - gHeightMap.SampleLevel(gSam, currentUV, 0).r;
        currentLayerDepth += layerDepth;
    }

    float2 prevUV = currentUV + deltaUV;
    float afterDepth  = currentHeight - currentLayerDepth;
    float beforeDepth = (1.0f - gHeightMap.SampleLevel(gSam, prevUV, 0).r)
                        - (currentLayerDepth - layerDepth);
    float weight = afterDepth / (afterDepth - beforeDepth);
    return lerp(currentUV, prevUV, weight);
}

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
    o.tangentW = mul(float4(skinnedTan, 0.0f), world).xyz;
    o.tangentW_w = v.tangent.w;
    return o;
}

PSOut PSMain(PSIn i)
{
    // ---- TBN normal mapping ----
    float3 N = normalize(i.nrmW);
    float3 T = normalize(i.tangentW);
    T = normalize(T - dot(T, N) * N);
    float3 B = cross(N, T) * i.tangentW_w;
    float3x3 TBN = float3x3(T, B, N);

    // ---- UV tiling/offset ----
    float2 uv = i.uv * gUVTilingOffset.xy + gUVTilingOffset.zw;

    // ---- Parallax Occlusion Mapping (UV offset) ----
    if (gPOMParams.w > 0.5f)
    {
        float3 V_world = normalize(gCameraPos.xyz - i.posW);
        float3 viewDirTS = normalize(float3(dot(V_world, T), dot(V_world, B), dot(V_world, N)));
        uv = ParallaxOcclusionMap(uv, viewDirTS, gPOMParams.x, gPOMParams.y, gPOMParams.z);
    }

    // Sample normal map (tangent-space).
    float3 normalTS = gNormalMap.Sample(gSam, uv).rgb * 2.0f - 1.0f;
    N = normalize(mul(normalTS, TBN));

    // ---- Sample material textures ----
    float3 albedo = gBaseColorMap.Sample(gSam, uv).rgb * gBaseColorFactor.rgb;

    float4 mr = gMetalRoughMap.Sample(gSam, uv);
    float roughness = saturate(mr.g * gMaterialFactors.y);
    roughness = max(roughness, 0.045f);
    float metallic = saturate(mr.b * gMaterialFactors.x);

    float ao = gAOMap.Sample(gSam, uv).r;

    float3 emissive = gEmissiveMap.Sample(gSam, uv).rgb * gEmissiveFactor.rgb;

    // ---- Procedural tile override (Phase 9) ----
    ProceduralResult procResult;
    if (ApplyProceduralTile(i.posW, gAnimParams.x, gAnimParams.y, procResult))
    {
        albedo    = procResult.albedo;
        emissive  = procResult.emissive;
        metallic  = procResult.metallic;
        roughness = max(procResult.roughness, 0.045f);
        ao        = procResult.ao;
        // Apply procedural normal in tangent space
        N = normalize(mul(procResult.normalTS, TBN));
    }

    // ---- Pack to G-buffer ----
    PSOut output;
    output.albedo   = float4(albedo, 1.0f);
    output.normal   = float4(N, 0.0f);
    output.material = float4(metallic, roughness, ao, 0.0f);
    output.emissive = float4(emissive, 0.0f);
    return output;
}
