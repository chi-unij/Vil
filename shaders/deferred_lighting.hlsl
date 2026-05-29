// ======================================
// File: deferred_lighting.hlsl
// Purpose: Deferred lighting pass (Phase 12.1 + 12.2 multi-light).
//          Fullscreen quad that reads G-buffer + depth, performs PBR lighting
//          (Cook-Torrance GGX + CSM shadows + IBL + point/spot lights),
//          writes to HDR target.
// ======================================

cbuffer LightingCB : register(b0)
{
    float4x4 gInvViewProj;          // for depth -> world pos reconstruction
    float4x4 gView;                 // for cascade selection (view-space Z)
    float4   gCameraPos;            // xyz
    float4   gLightDirIntensity;    // xyz = sun rays direction, w = intensity
    float4   gLightColor;           // rgb = light color, w = iblIntensity
    float4x4 gCascadeLightViewProj[4]; // per-cascade world -> light clip
    float4   gShadowParams;         // xy = texelSize, z = bias, w = strength
    float4   gCascadeSplits;        // xyz = split distances (view-space), w = cascadeCount
    float4   gCascadeDebug;         // x = cascadeDebug flag
    float4   gLightCounts;          // x = numPointLights, y = numSpotLights
};

// G-buffer SRVs (contiguous t0-t4).
Texture2D<float4> gAlbedoTex   : register(t0);
Texture2D<float4> gNormalTex   : register(t1);
Texture2D<float4> gMaterialTex : register(t2);
Texture2D<float4> gEmissiveTex : register(t3);
Texture2D<float>  gDepthTex    : register(t4);

// Shadow map (t5).
Texture2DArray<float> gShadowMap : register(t5);

// IBL textures (t6-t8).
TextureCube<float4> gIrradianceMap  : register(t6);
TextureCube<float4> gPrefilteredMap : register(t7);
Texture2D<float2>   gBrdfLUT       : register(t8);

// Point & spot light structured buffers (t9-t10, root SRVs).
struct PointLight {
    float3 position;
    float  range;
    float3 color;
    float  intensity;
    float3 _pad;
    float  _pad2;
};

struct SpotLight {
    float3 position;
    float  range;
    float3 color;
    float  intensity;
    float3 direction;
    float  innerConeAngleCos;
    float  outerConeAngleCos;
    float3 _pad;
};

StructuredBuffer<PointLight> gPointLights : register(t9);
StructuredBuffer<SpotLight>  gSpotLights  : register(t10);

SamplerState           gSampPoint   : register(s0); // point clamp for G-buffer
SamplerComparisonState gShadowSamp  : register(s1); // shadow PCF
SamplerState           gIBLSampler  : register(s2); // linear clamp for IBL

static const float PI = 3.14159265f;

// ---- PBR BRDF functions ----
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

// ---- Shared BRDF evaluation for any light source ----
float3 EvaluateBRDF(float3 N, float3 V, float3 L, float3 radiance,
                    float3 albedo, float metallic, float roughness, float3 F0)
{
    float3 H = normalize(V + L);
    // 计算餘弦，且卡在 0-1 之間 （防止負數會把過後的計算無效 (NaN)，同時餘弦是負數證明光是從背面打過來的 <- 背面的光不會被計算到）
    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    // 「這些朝向 H 的微表面，反射多少能量、吸收多少？」
    float3  F = FresnelSchlick(VdotH, F0);
    // 有多少微表面剛好朝向 H？
    float   D = DistributionGGX(NdotH, roughness);
    // 朝向 H 的微表面，有多少被旁邊的小山丘擋住
    float   G = GeometrySmith(NdotV, NdotL, roughness);

    // D * G * F = 三者相乘 = 「能成功反射到視線的鏡面光能量」
    
    // 把 D、F、G 三個因子組合起來，除以一個校正項，得到 Cook-Torrance 鏡面反射 BRDF
    float3  spec = (D * G) * F / max(4.0f * NdotV * NdotL, 1e-4f);

    float3 kD = (1.0f - F) * (1.0f - metallic);
    float3 diff = kD * albedo / PI;

    // 把漫反射和鏡面反射的能量相加，乘以光源的強度，得到最終的光照結果 （漫反射 + 鏡面反射 = 總 BRDF）
    return (diff + spec) * radiance * NdotL;
}

// ---- Attenuation helpers ----
// 光源減衰
float DistanceAttenuation(float dist, float range)
{
    // 光源到片段的距離平方
    float d2 = dist * dist;
    // 光源影響範圍平方
    float r2 = range * range;
    float num = saturate(1.0f - (d2 * d2) / (r2 * r2));
    return (num * num) / max(d2, 0.0001f);
}

float SpotAngleAttenuation(float3 L, float3 spotDir, float innerCos, float outerCos)
{
    float cosAngle = dot(-L, spotDir);
    return saturate((cosAngle - outerCos) / max(innerCos - outerCos, 0.001f));
}

// ---- Fullscreen triangle ----
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSFullscreen(uint vid : SV_VertexID) {
    VSOut o;
    o.uv  = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

// ---- World position reconstruction from depth ----
float3 ReconstructWorldPos(float2 uv, float depth) {
    float4 ndc = float4(uv * 2.0f - 1.0f, depth, 1.0f);
    ndc.y = -ndc.y; // DX UV convention
    float4 worldPos = mul(ndc, gInvViewProj);
    return worldPos.xyz / worldPos.w;
}

float4 PSMain(VSOut pin) : SV_TARGET {
    // Sample depth — skip sky pixels.
    float depth = gDepthTex.Sample(gSampPoint, pin.uv);
    if (depth >= 1.0f)
        discard;

    // Sample G-buffer.
    float3 albedo   = gAlbedoTex.Sample(gSampPoint, pin.uv).rgb;
    float3 N        = normalize(gNormalTex.Sample(gSampPoint, pin.uv).xyz);
    float4 matData  = gMaterialTex.Sample(gSampPoint, pin.uv);
    float metallic  = matData.r;
    float roughness = matData.g;
    float ao        = matData.b;
    float3 emissive = gEmissiveTex.Sample(gSampPoint, pin.uv).rgb;

    // Reconstruct world position.
    float3 posW = ReconstructWorldPos(pin.uv, depth);

    float3 V = normalize(gCameraPos.xyz - posW);
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    // ---- Directional light ----
    float3 raysDir = normalize(gLightDirIntensity.xyz);
    float3 sunL = normalize(-raysDir);
    float3 sunRadiance = gLightColor.rgb * gLightDirIntensity.w;
    float3 color = EvaluateBRDF(N, V, sunL, sunRadiance, albedo, metallic, roughness, F0);

    // ---- CSM shadow sampling ----
    float shadowFactor = 1.0f;
    if (gShadowParams.w > 0.0f && gShadowParams.x > 0.0f) {
        int cascadeCount = (int)gCascadeSplits.w;

        // Compute view-space Z for cascade selection.
        float viewZ = mul(float4(posW, 1.0f), gView).z;

        int cascade = cascadeCount - 1;
        float splitDists[3] = { gCascadeSplits.x, gCascadeSplits.y, gCascadeSplits.z };
        for (int c = 0; c < cascadeCount; ++c) {
            if (viewZ < splitDists[c]) {
                cascade = c;
                break;
            }
        }

        float4 posLS = mul(float4(posW, 1.0f), gCascadeLightViewProj[cascade]);
        float3 ndc = posLS.xyz / posLS.w;

        float2 shadowUV = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
        float smDepth = ndc.z;

        if (shadowUV.x >= 0.0f && shadowUV.x <= 1.0f &&
            shadowUV.y >= 0.0f && shadowUV.y <= 1.0f &&
            smDepth >= 0.0f && smDepth <= 1.0f) {
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
                        smDepth - bias);
                }
            }
            float shadow = sum / 9.0f;
            shadowFactor = lerp(1.0f, shadow, saturate(gShadowParams.w));
        }
    }

    color *= shadowFactor;

    // Debug: tint by cascade index.
    if (gCascadeDebug.x > 0.5f && gShadowParams.w > 0.0f) {
        int cascadeCount = (int)gCascadeSplits.w;
        float viewZ = mul(float4(posW, 1.0f), gView).z;
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

    // ---- Point lights ----
    uint numPointLights = (uint)gLightCounts.x;
    for (uint i = 0; i < numPointLights; ++i) {
        PointLight pl = gPointLights[i];
        float3 toLight = pl.position - posW;
        float dist = length(toLight);
        float3 L = toLight / max(dist, 0.0001f);
        float atten = DistanceAttenuation(dist, pl.range);
        float3 radiance = pl.color * pl.intensity * atten;
        color += EvaluateBRDF(N, V, L, radiance, albedo, metallic, roughness, F0);
    }

    // ---- Spot lights ----
    uint numSpotLights = (uint)gLightCounts.y;
    for (uint j = 0; j < numSpotLights; ++j) {
        SpotLight sl = gSpotLights[j];
        float3 toLight = sl.position - posW;
        float dist = length(toLight);
        float3 L = toLight / max(dist, 0.0001f);
        float atten = DistanceAttenuation(dist, sl.range);
        float spot  = SpotAngleAttenuation(L, normalize(sl.direction),
                                           sl.innerConeAngleCos, sl.outerConeAngleCos);
        float3 radiance = sl.color * sl.intensity * atten * spot;
        color += EvaluateBRDF(N, V, L, radiance, albedo, metallic, roughness, F0);
    }

    // ---- IBL ambient (split-sum approximation) ----
    {
        float NdotV = saturate(dot(N, V));
        float3 F0_ibl = F0;
        float3 kS_ibl = FresnelSchlickRoughness(max(NdotV, 0.0f), F0_ibl, roughness);
        float3 kD_ibl = (1.0f - kS_ibl) * (1.0f - metallic);

        float3 irradiance  = gIrradianceMap.Sample(gIBLSampler, N).rgb;
        float3 diffuseIBL  = kD_ibl * albedo * irradiance;

        float3 R = reflect(-V, N);
        float3 prefilteredColor = gPrefilteredMap.SampleLevel(gIBLSampler, R, roughness * 4.0f).rgb;
        float2 brdf = gBrdfLUT.Sample(gIBLSampler, float2(max(NdotV, 0.0f), roughness)).rg;
        float3 specularIBL = prefilteredColor * (F0_ibl * brdf.x + brdf.y);

        float iblIntensity = gLightColor.w;
        color += (diffuseIBL + specularIBL) * iblIntensity * ao;
    }

    // Add emissive.
    color += emissive;

    return float4(color, 1.0f);
}
