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
    float4   gMaterialFactors;  // x=metallic, y=roughness, z=SSR exclusion
    float4   gEmissiveFactor;   // rgb=emissive factor, w=unused
    float4   gPOMParams;        // x=heightScale, y=minLayers, z=maxLayers, w=enabled
    float4   gBaseColorFactor;  // rgba multiplier for base color
    float4   gUVTilingOffset;   // xy=tiling, zw=offset
    float4   gAnimParams;      // x=gameTime, y=materialTypeId, z=alphaCutoff, w=alphaCutout
    float4   gWaterWaveParams; // x=高さ, y=速度, z=周波数, w=頂点変形タイプ
    float4   gWetSurfaceParams; // x=強さ, y=乾燥秒数, z=幅, w=周期秒数
    float4   gPuddleParams;    // x=強さ, y=蓄積秒数, z=半径, w=波紋強さ
    float4   gPuddleVisualParams; // x=透明感, y=色味, z=法線強さ, w=unused
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
    float4 posW = mul(float4(skinnedPos, 1.0f), world);

    if ((int)(gWaterWaveParams.w + 0.5f) == 6)
    {
        float2 p = posW.xz * gWaterWaveParams.z;
        float t = gAnimParams.x * gWaterWaveParams.y;
        float height = gWaterWaveParams.x;
        float wave =
            sin(p.x * 1.4f + t * 3.0f) * 0.05f +
            sin((p.x * 0.7f + p.y * 1.2f) - t * 1.6f) * 0.032f +
            sin((p.x - p.y) * 2.1f + t * 1.1f) * 0.020f;
        float edgeDistance = min(min(v.uv.x, 1.0f - v.uv.x),
                                 min(v.uv.y, 1.0f - v.uv.y));
        float edgeFade = smoothstep(0.0f, 0.08f, edgeDistance);
        posW.y += wave * height * edgeFade;
    }

    float4x4 viewProj = mul(gView, gProj);

    PSIn o;
    o.pos = mul(posW, viewProj);
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
    float4 baseColorSample = gBaseColorMap.Sample(gSam, uv);
    float alpha = baseColorSample.a * gBaseColorFactor.a;
    if (gAnimParams.w > 0.5f && alpha < gAnimParams.z)
        discard;
    float3 albedo = baseColorSample.rgb * gBaseColorFactor.rgb;

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

    int proceduralTypeId = (int)(gAnimParams.y + 0.5f);
    float wetness = (proceduralTypeId == 7)
        ? ComputeWaterImpactWetness(i.posW.xz, gAnimParams.x, gWetSurfaceParams)
        : 0.0f;
    if (wetness > 0.001f)
    {
        float3 wetColor = albedo * float3(0.12f, 0.13f, 0.12f);
        albedo = lerp(albedo, wetColor, wetness);
        roughness = lerp(roughness, 0.16f, wetness);
        ao = lerp(ao, 0.82f, wetness * 0.6f);
    }

    float puddle = (proceduralTypeId == 7)
        ? ComputeWaterImpactPuddle(i.posW.xz, gAnimParams.x, gPuddleParams)
        : 0.0f;
    float puddleReflectionMask = (proceduralTypeId == 7)
        ? ComputeWaterImpactPuddleReflectionMask(i.posW.xz, gAnimParams.x,
                                                 gPuddleParams)
        : 0.0f;
    float puddleWetRim = (proceduralTypeId == 7)
        ? ComputeWaterImpactPuddleWetRim(i.posW.xz, gAnimParams.x,
                                         gWetSurfaceParams, gPuddleParams)
        : 0.0f;
    puddleWetRim *= (1.0f - puddle * 0.85f);
    if (puddleWetRim > 0.001f)
    {
        float3 rimColor = albedo * float3(0.075f, 0.082f, 0.075f);
        albedo = lerp(albedo, rimColor, puddleWetRim);
        roughness = lerp(roughness, 0.12f, puddleWetRim);
        ao = lerp(ao, 0.76f, puddleWetRim * 0.65f);
    }
    if (puddleReflectionMask > 0.001f)
    {
        float clarity = saturate(gPuddleVisualParams.x);
        float tintStrength = saturate(gPuddleVisualParams.y);
        float normalStrength = saturate(gPuddleVisualParams.z) * gPuddleParams.w;

        float2 rippleUv = i.posW.xz;
        float ripple = ComputeWaterImpactPuddleRipple(rippleUv, gAnimParams.x);
        float waveA = fbm(rippleUv * 6.0f +
                          float2(gAnimParams.x * 0.05f, -gAnimParams.x * 0.035f), 3);
        float waveB = fbm(rippleUv * 13.0f +
                          float2(-gAnimParams.x * 0.12f, gAnimParams.x * 0.09f), 2);

        float waterHeight = ripple * 0.32f + (waveA - 0.5f) * 0.42f +
                            (waveB - 0.5f) * 0.16f;
        float waterHeightX = ComputeWaterImpactPuddleRipple(rippleUv + float2(0.08f, 0.0f),
                                                            gAnimParams.x) * 0.32f +
                             (fbm((rippleUv + float2(0.08f, 0.0f)) * 6.0f +
                                  float2(gAnimParams.x * 0.05f, -gAnimParams.x * 0.035f), 3) - 0.5f) * 0.42f;
        float waterHeightZ = ComputeWaterImpactPuddleRipple(rippleUv + float2(0.0f, 0.08f),
                                                            gAnimParams.x) * 0.32f +
                             (fbm((rippleUv + float2(0.0f, 0.08f)) * 6.0f +
                                  float2(gAnimParams.x * 0.05f, -gAnimParams.x * 0.035f), 3) - 0.5f) * 0.42f;
        float2 rippleSlope = float2(waterHeightX - waterHeight,
                                    waterHeightZ - waterHeight) * normalStrength;

        float3 clearWaterTint = float3(0.045f, 0.13f, 0.15f);
        float3 preservedFloor = albedo * lerp(0.62f, 0.94f, clarity);
        float3 tintedFloor = preservedFloor + clearWaterTint * tintStrength;
        albedo = lerp(albedo, tintedFloor, puddle);

        float cleanWaterRoughness = lerp(0.085f, 0.026f, clarity);
        cleanWaterRoughness = saturate(cleanWaterRoughness + abs(ripple) * 0.010f * normalStrength);
        float clearCore = smoothstep(0.08f, 0.95f, puddle);
        float surfaceRoughness = lerp(0.15f, cleanWaterRoughness, clearCore);
        roughness = lerp(roughness, surfaceRoughness, puddleReflectionMask);
        ao = lerp(ao, lerp(0.88f, 0.98f, clearCore), puddleReflectionMask);

        // 中央では地面の法線を強く抑え、外周へ向けて連続的に戻す。
        // 反射の形は一枚の水面として保ち、粗さで外周だけをぼかす。
        float normalProfile = smoothstep(0.0f, 0.70f, puddleReflectionMask);
        float flattenStrength = normalProfile * lerp(0.78f, 0.96f, clearCore);
        float3 puddleBaseNormal = normalize(lerp(N, float3(0.0f, 1.0f, 0.0f),
                                                 saturate(flattenStrength)));
        float rippleProfile = puddleReflectionMask * lerp(0.55f, 1.0f, clearCore);
        N = normalize(puddleBaseNormal +
                      float3(rippleSlope.x * 0.045f, 0.0f,
                             rippleSlope.y * 0.045f) * rippleProfile);
    }

    // ---- Pack to G-buffer ----
    PSOut output;
    output.albedo   = float4(albedo, 1.0f);
    output.normal   = float4(N, 0.0f);
    // Material alpha は水面 mask と SSR hit exclusion を共有する。
    // exclusion は小さい予約値にし、反射を受ける水面 mask と区別する。
    float proceduralWaterMask = (proceduralTypeId == 6 || proceduralTypeId == 8)
        ? 1.0f
        : 0.0f;
    float ssrWaterMask = max(proceduralWaterMask, puddleReflectionMask);
    float ssrExclusionMask = (gMaterialFactors.z > 0.5f) ? 0.01f : 0.0f;
    output.material = float4(metallic, roughness, ao,
                             max(saturate(ssrWaterMask), ssrExclusionMask));
    output.emissive = float4(emissive, 0.0f);
    return output;
}
