#define FEATURE_DIRECTIONAL_LIGHT (1u << 0)
#define FEATURE_POINT_LIGHTS       (1u << 1)
#define FEATURE_SHADOWS            (1u << 2)
#define FEATURE_SSAO               (1u << 3)
#define FEATURE_FOG                (1u << 4)
#define FEATURE_NORMAL_DETAIL      (1u << 5)
#define FEATURE_PBR                (1u << 6)
#define FEATURE_EMISSIVE           (1u << 7)
#define FEATURE_PROCEDURAL         (1u << 8)
#define FEATURE_WAVES              (1u << 9)
#define FEATURE_SSR                (1u << 10)
#define FEATURE_HDR                (1u << 11)
#define FEATURE_BLOOM              (1u << 12)
#define FEATURE_TONEMAP            (1u << 13)
#define FEATURE_FXAA               (1u << 14)
#define FEATURE_PHASE2             (1u << 15)
#define FEATURE_UV_ANIMATION       (1u << 16)

#define ENTITY_TRANSPARENT   (1u << 0)
#define ENTITY_EMISSIVE      (1u << 1)
#define ENTITY_WATER         (1u << 2)
#define ENTITY_INSTANCED     (1u << 3)
#define ENTITY_COLLIDER      (1u << 4)
#define ENTITY_HOLOGRAM      (1u << 5)
#define ENTITY_TELEGRAPH     (1u << 6)
#define ENTITY_PARTICLE      (1u << 7)

struct PointLightData
{
    float4 positionRange;
    float4 colorIntensity;
};

cbuffer FrameCB : register(b0)
{
    row_major float4x4 ViewProj;
    row_major float4x4 LightViewProj;
    float4 CameraPositionTime;
    float4 CameraRightViewportX;
    float4 CameraUpViewportY;
    float4 SunDirectionIntensity;
    float4 SunColor;
    PointLightData PointLights[8];
    uint PointLightCount;
    uint FeatureFlags;
    uint AttackType;
    uint PhaseTwo;
};

cbuffer ObjectCB : register(b1)
{
    row_major float4x4 World;
    float4 BaseColor;
    float4 MaterialParams; // metallic, roughness, emissive, pattern
    uint EntityFlags;
    uint ObjectType;       // 0=normal, 1=cluster, 2=spark, 3=rain, 4=grid
    uint InstanceCount;
    uint ObjectPadding;
};

Texture2D<float4> GBufferAlbedo : register(t0);
Texture2D<float4> GBufferNormal : register(t1);
Texture2D<float4> GBufferPosition : register(t2);
Texture2D<float> ShadowTexture : register(t3);
Texture2D<float> AoTexture : register(t4);
Texture2D<float4> SceneTexture : register(t5);
Texture2D<float4> CompositeTexture : register(t6);
Texture2D<float4> BloomTextureA : register(t7);
Texture2D<float4> BloomTextureB : register(t8);

SamplerState LinearClampSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VertexOutput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    nointerpolation uint instanceId : TEXCOORD3;
};

float Hash11(uint value)
{
    value ^= value * 2747636419u;
    value ^= value >> 16;
    value *= 2654435769u;
    value ^= value >> 16;
    return (value & 0x00FFFFFFu) / 16777215.0;
}

float3 InstanceOffset(uint instanceId)
{
    if (ObjectType == 1)
    {
        uint x = instanceId % 7u;
        uint z = instanceId / 7u;
        float edge = (x == 0u || x == 6u || z == 0u || z == 6u) ? 1.0 : 0.0;
        return float3((int(x) - 3) * 3.1, edge * 0.8, (int(z) - 3) * 3.1 + 2.0);
    }
    return 0.0.xxx;
}

float3 ParticleCenter(uint instanceId)
{
    float time = CameraPositionTime.w;
    float h0 = Hash11(instanceId * 17u + 3u);
    float h1 = Hash11(instanceId * 31u + 11u);
    float h2 = Hash11(instanceId * 47u + 29u);

    if (ObjectType == 3)
    {
        float x = (h0 * 2.0 - 1.0) * 25.0;
        float z = (h1 * 2.0 - 1.0) * 28.0 + 3.0;
        float y = fmod(h2 * 18.0 - time * (8.0 + h0 * 5.0) + 180.0, 18.0);
        return float3(x, y, z);
    }

    if (ObjectType == 5)
    {
        float angle = h0 * 6.2831853 + time * (1.2 + h2 * 2.0);
        float radius = 0.4 + fmod(h1 * 3.2 + time * 1.8, 3.2);
        float pulse = 0.7 + 0.3 * sin(time * 6.0 + h2 * 12.0);
        return float3(-4.0 + cos(angle) * radius, 1.1 + h2 * 2.6 * pulse,
                      4.0 + sin(angle) * radius);
    }

    if (ObjectType == 5)
    {
        float angle = h0 * 6.2831853 + time * (1.2 + h2 * 2.0);
        float radius = 0.4 + fmod(h1 * 3.2 + time * 1.8, 3.2);
        float pulse = 0.7 + 0.3 * sin(time * 6.0 + h2 * 12.0);
        return float3(-4.0 + cos(angle) * radius, 1.1 + h2 * 2.6 * pulse,
                      4.0 + sin(angle) * radius);
    }

    float angle = h0 * 6.2831853 + time * (0.4 + h2 * 1.2);
    float radius = fmod(h1 * 8.0 + time * (0.6 + h0), 8.0);
    float y = 1.0 + h2 * 6.0 + sin(time * 2.0 + h0 * 8.0) * 0.8;
    return float3(cos(angle) * radius, y, 8.5 + sin(angle) * radius);
}

VertexOutput GeometryVS(VertexInput input, uint instanceId : SV_InstanceID)
{
    VertexOutput output;
    float3 localPosition = input.position;
    float3 localNormal = input.normal;

    if ((EntityFlags & ENTITY_WATER) != 0u && (FeatureFlags & FEATURE_WAVES) != 0u)
    {
        float2 waveUv = input.position.xz * 5.0;
        float time = CameraPositionTime.w;
        float wave = sin(waveUv.x * 1.7 + time * 1.8) * 0.07;
        wave += cos(waveUv.y * 2.2 - time * 1.35) * 0.05;
        localPosition.y += wave;
        localNormal = normalize(float3(-cos(waveUv.x * 1.7 + time * 1.8) * 0.12,
                                       1.0,
                                       sin(waveUv.y * 2.2 - time * 1.35) * 0.10));
    }

    if ((EntityFlags & ENTITY_PARTICLE) != 0u)
    {
        float3 center = ParticleCenter(instanceId);
        float2 billboardScale = float2(length(World[0].xyz), length(World[1].xyz));
        float3 right = CameraRightViewportX.xyz * input.position.x * billboardScale.x;
        float3 up = CameraUpViewportY.xyz * input.position.y * billboardScale.y;
        float3 worldPosition = center + right + up;
        output.worldPosition = worldPosition;
        output.normal = -normalize(CameraPositionTime.xyz - worldPosition);
        output.position = mul(float4(worldPosition, 1.0), ViewProj);
        output.uv = input.uv;
        output.instanceId = instanceId;
        return output;
    }

    if ((EntityFlags & ENTITY_INSTANCED) != 0u)
        localPosition += InstanceOffset(instanceId);

    float4 worldPosition4 = mul(float4(localPosition, 1.0), World);
    output.worldPosition = worldPosition4.xyz;
    output.normal = normalize(mul(float4(localNormal, 0.0), World).xyz);
    output.position = mul(worldPosition4, ViewProj);
    output.uv = input.uv;
    output.instanceId = instanceId;
    return output;
}

VertexOutput ShadowVS(VertexInput input, uint instanceId : SV_InstanceID)
{
    VertexOutput output;
    float3 localPosition = input.position;
    if ((EntityFlags & ENTITY_INSTANCED) != 0u && (EntityFlags & ENTITY_PARTICLE) == 0u)
        localPosition += InstanceOffset(instanceId);
    float4 worldPosition = mul(float4(localPosition, 1.0), World);
    output.position = mul(worldPosition, LightViewProj);
    output.worldPosition = worldPosition.xyz;
    output.normal = input.normal;
    output.uv = input.uv;
    output.instanceId = instanceId;
    return output;
}

float3 ApplyProceduralPattern(float3 color, float3 worldPosition, float2 uv)
{
    if ((FeatureFlags & FEATURE_PROCEDURAL) == 0u)
        return color;

    float pattern = MaterialParams.w;
    if (pattern < 0.5)
        return color;
    if (pattern < 1.5)
    {
        float2 grid = abs(frac(worldPosition.xz * 0.5) - 0.5);
        float lineMask = step(0.46, max(grid.x, grid.y));
        return lerp(color * 0.72, color * 1.15, lineMask);
    }
    if (pattern < 2.5)
    {
        float stone = sin(worldPosition.y * 8.0 + sin(worldPosition.x * 4.0)) * 0.5 + 0.5;
        return color * lerp(0.65, 1.15, stone);
    }
    if (pattern < 3.5)
    {
        float stripe = step(0.52, frac((worldPosition.x + worldPosition.y) * 0.8));
        return lerp(color * 0.55, color * 1.25, stripe);
    }
    float rings = sin(length(uv - 0.5) * 42.0) * 0.5 + 0.5;
    return color * lerp(0.7, 1.25, rings);
}

struct GBufferOutput
{
    float4 albedoMetallic : SV_TARGET0;
    float4 normalRoughness : SV_TARGET1;
    float4 positionEmissive : SV_TARGET2;
};

GBufferOutput GBufferPS(VertexOutput input)
{
    GBufferOutput output;
    float2 uv = input.uv;
    if ((FeatureFlags & FEATURE_UV_ANIMATION) != 0u)
        uv += float2(CameraPositionTime.w * 0.025, -CameraPositionTime.w * 0.018);

    float3 albedo = ApplyProceduralPattern(BaseColor.rgb, input.worldPosition, uv);
    float3 normal = normalize(input.normal);
    if ((FeatureFlags & FEATURE_NORMAL_DETAIL) != 0u)
    {
        float3 detail = float3(sin(input.worldPosition.z * 7.0 + uv.x * 11.0),
                               0.0,
                               cos(input.worldPosition.x * 7.0 + uv.y * 13.0));
        normal = normalize(normal + detail * 0.08);
    }

    output.albedoMetallic = float4(albedo, saturate(MaterialParams.x));
    output.normalRoughness = float4(normal * 0.5 + 0.5, saturate(MaterialParams.y));
    float emissive = ((FeatureFlags & FEATURE_EMISSIVE) != 0u) ? MaterialParams.z : 0.0;
    output.positionEmissive = float4(input.worldPosition, emissive + 0.001);
    return output;
}

float ShadowAmount(float3 worldPosition)
{
    if ((FeatureFlags & FEATURE_SHADOWS) == 0u)
        return 1.0;
    float4 lightClip = mul(float4(worldPosition, 1.0), LightViewProj);
    float3 ndc = lightClip.xyz / max(lightClip.w, 0.0001);
    float2 uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z <= 0.0 || ndc.z >= 1.0)
        return 1.0;
    float result = 0.0;
    float2 texel = 1.0 / float2(2048.0, 2048.0);
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
            result += ShadowTexture.SampleCmpLevelZero(ShadowSampler, uv + float2(x, y) * texel,
                                                       ndc.z - 0.0015);
    }
    return lerp(0.28, 1.0, result / 9.0);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float DistributionGGX(float3 normal, float3 halfVector, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float nDotH = max(dot(normal, halfVector), 0.0);
    float denom = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * denom * denom, 0.0001);
}

float GeometrySchlickGGX(float nDotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 0.0001);
}

float3 EvaluateLight(float3 albedo, float metallic, float roughness,
                     float3 normal, float3 viewDirection, float3 lightDirection,
                     float3 radiance)
{
    float nDotL = max(dot(normal, lightDirection), 0.0);
    if ((FeatureFlags & FEATURE_PBR) == 0u)
        return albedo * radiance * (0.12 + nDotL);

    float3 halfVector = normalize(viewDirection + lightDirection);
    float3 f0 = lerp(0.04.xxx, albedo, metallic);
    float3 fresnel = FresnelSchlick(max(dot(halfVector, viewDirection), 0.0), f0);
    float distribution = DistributionGGX(normal, halfVector, max(roughness, 0.05));
    float geometry = GeometrySchlickGGX(max(dot(normal, viewDirection), 0.0), roughness) *
                     GeometrySchlickGGX(nDotL, roughness);
    float3 specular = distribution * geometry * fresnel /
                      max(4.0 * max(dot(normal, viewDirection), 0.0) * nDotL, 0.001);
    float3 diffuseWeight = (1.0 - fresnel) * (1.0 - metallic);
    return (diffuseWeight * albedo / 3.14159265 + specular) * radiance * nDotL;
}

float3 ShadeSurface(float3 albedo, float metallic, float roughness,
                    float3 normal, float3 worldPosition, float ao, float emissive)
{
    float3 viewDirection = normalize(CameraPositionTime.xyz - worldPosition);
    float3 color = albedo * 0.035 * ao;

    if ((FeatureFlags & FEATURE_DIRECTIONAL_LIGHT) != 0u)
    {
        float3 lightDirection = normalize(-SunDirectionIntensity.xyz);
        float3 radiance = SunColor.rgb * SunDirectionIntensity.w * ShadowAmount(worldPosition);
        color += EvaluateLight(albedo, metallic, roughness, normal, viewDirection,
                               lightDirection, radiance) * ao;
    }

    if ((FeatureFlags & FEATURE_POINT_LIGHTS) != 0u)
    {
        [loop]
        for (uint lightIndex = 0u; lightIndex < min(PointLightCount, 8u); ++lightIndex)
        {
            float3 delta = PointLights[lightIndex].positionRange.xyz - worldPosition;
            float distance = length(delta);
            float range = PointLights[lightIndex].positionRange.w;
            float attenuation = saturate(1.0 - distance / max(range, 0.01));
            attenuation *= attenuation;
            float3 radiance = PointLights[lightIndex].colorIntensity.rgb *
                              PointLights[lightIndex].colorIntensity.w * attenuation;
            color += EvaluateLight(albedo, metallic, roughness, normal, viewDirection,
                                   normalize(delta), radiance);
        }
    }

    if ((FeatureFlags & FEATURE_EMISSIVE) != 0u)
        color += albedo * emissive;

    if ((FeatureFlags & FEATURE_FOG) != 0u)
    {
        float distance = length(CameraPositionTime.xyz - worldPosition);
        float fog = 1.0 - exp(-distance * 0.035);
        float3 fogColor = PhaseTwo != 0u ? float3(0.15, 0.025, 0.035)
                                        : float3(0.035, 0.11, 0.14);
        color = lerp(color, fogColor, saturate(fog * 0.82));
    }
    return color;
}

float4 ForwardPS(VertexOutput input) : SV_TARGET
{
    float3 normal = normalize(input.normal);
    if ((FeatureFlags & FEATURE_NORMAL_DETAIL) != 0u)
        normal = normalize(normal + float3(sin(input.worldPosition.z * 7.0), 0.0,
                                            cos(input.worldPosition.x * 7.0)) * 0.08);
    float3 albedo = ApplyProceduralPattern(BaseColor.rgb, input.worldPosition, input.uv);
    float emissive = ((FeatureFlags & FEATURE_EMISSIVE) != 0u) ? MaterialParams.z : 0.0;
    return float4(ShadeSurface(albedo, MaterialParams.x, MaterialParams.y, normal,
                               input.worldPosition, 1.0, emissive), 1.0);
}

struct FullscreenOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

FullscreenOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    FullscreenOutput output;
    float2 position = vertexId == 0u ? float2(-1.0, -1.0)
                    : vertexId == 1u ? float2(-1.0, 3.0)
                                     : float2(3.0, -1.0);
    output.position = float4(position, 0.0, 1.0);
    output.uv = float2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
    return output;
}

float SsaoSample(float2 uv, float3 centerPosition, float3 centerNormal)
{
    float3 samplePosition = GBufferPosition.SampleLevel(LinearClampSampler, uv, 0).xyz;
    float3 delta = samplePosition - centerPosition;
    float distance = length(delta);
    if (distance < 0.01 || distance > 3.0)
        return 0.0;
    float normalTerm = saturate(dot(centerNormal, normalize(delta)) - 0.08);
    return normalTerm * saturate(1.0 - distance / 3.0);
}

float SSAOPS(FullscreenOutput input) : SV_TARGET
{
    if ((FeatureFlags & FEATURE_SSAO) == 0u)
        return 1.0;
    float3 center = GBufferPosition.SampleLevel(LinearClampSampler, input.uv, 0).xyz;
    float3 normal = normalize(GBufferNormal.SampleLevel(LinearClampSampler, input.uv, 0).xyz * 2.0 - 1.0);
    if (length(center) < 0.001)
        return 1.0;
    float2 texel = float2(CameraRightViewportX.w, CameraUpViewportY.w);
    float occlusion = 0.0;
    [unroll]
    for (int sampleIndex = 0; sampleIndex < 12; ++sampleIndex)
    {
        float angle = sampleIndex * 2.399963;
        float radius = 2.0 + (sampleIndex % 4) * 2.5;
        occlusion += SsaoSample(input.uv + float2(cos(angle), sin(angle)) * texel * radius,
                                center, normal);
    }
    return saturate(1.0 - occlusion / 12.0 * 2.4);
}

float4 DeferredLightingPS(FullscreenOutput input) : SV_TARGET
{
    float4 albedoMetal = GBufferAlbedo.SampleLevel(LinearClampSampler, input.uv, 0);
    float4 normalRough = GBufferNormal.SampleLevel(LinearClampSampler, input.uv, 0);
    float4 positionEmissive = GBufferPosition.SampleLevel(LinearClampSampler, input.uv, 0);
    if (length(positionEmissive.xyz) < 0.001 && dot(albedoMetal.rgb, 1.0.xxx) < 0.001)
    {
        float horizon = saturate(1.0 - input.uv.y);
        float3 sky = PhaseTwo != 0u ? lerp(float3(0.015, 0.006, 0.012), float3(0.19, 0.018, 0.035), horizon)
                                    : lerp(float3(0.01, 0.025, 0.045), float3(0.025, 0.18, 0.22), horizon);
        return float4(sky, 1.0);
    }
    float3 normal = normalize(normalRough.xyz * 2.0 - 1.0);
    float ao = ((FeatureFlags & FEATURE_SSAO) != 0u)
                   ? AoTexture.SampleLevel(LinearClampSampler, input.uv, 0)
                   : 1.0;
    float emissive = max(0.0, positionEmissive.w - 0.001);
    float3 color = ShadeSurface(albedoMetal.rgb, albedoMetal.a, normalRough.a,
                                normal, positionEmissive.xyz, ao, emissive);
    return float4(color, 1.0);
}

float3 ScreenSpaceReflection(float2 uv, float3 worldPosition, float3 normal)
{
    float3 viewDirection = normalize(worldPosition - CameraPositionTime.xyz);
    float3 reflected = reflect(viewDirection, normal);
    float2 direction = normalize(reflected.xz + float2(0.0001, 0.0001));
    float2 rayUv = uv;
    float3 hitColor = 0.0;
    float hit = 0.0;
    [loop]
    for (int step = 0; step < 28; ++step)
    {
        rayUv += direction * (0.004 + step * 0.00045);
        if (any(rayUv < 0.01) || any(rayUv > 0.99))
            break;
        float3 samplePosition = GBufferPosition.SampleLevel(LinearClampSampler, rayUv, 0).xyz;
        if (length(samplePosition) > 0.01 && samplePosition.y > worldPosition.y + 0.05)
        {
            hitColor = SceneTexture.SampleLevel(LinearClampSampler, rayUv, 0).rgb;
            hit = 1.0;
            break;
        }
    }
    return lerp(float3(0.02, 0.18, 0.24), hitColor, hit * 0.72);
}

float4 TransparentPS(VertexOutput input) : SV_TARGET
{
    float2 screenUv = input.position.xy *
                      float2(CameraRightViewportX.w, CameraUpViewportY.w);
    float2 uv = input.uv;
    if ((FeatureFlags & FEATURE_UV_ANIMATION) != 0u)
        uv += float2(CameraPositionTime.w * 0.035, CameraPositionTime.w * -0.02);

    float3 color = ApplyProceduralPattern(BaseColor.rgb, input.worldPosition, uv);
    float alpha = BaseColor.a;

    if ((EntityFlags & ENTITY_WATER) != 0u)
    {
        float fresnel = pow(1.0 - saturate(dot(normalize(input.normal),
                                           normalize(CameraPositionTime.xyz - input.worldPosition))), 4.0);
        float3 reflected = ((FeatureFlags & FEATURE_SSR) != 0u)
                               ? ScreenSpaceReflection(screenUv, input.worldPosition, normalize(input.normal))
                               : float3(0.025, 0.17, 0.22);
        float3 underneath = SceneTexture.SampleLevel(LinearClampSampler, screenUv, 0).rgb;
        color = lerp(underneath * float3(0.56, 0.82, 0.9), reflected, 0.25 + fresnel * 0.62);
        alpha = saturate(BaseColor.a + fresnel * 0.24);
    }
    else if ((EntityFlags & ENTITY_PARTICLE) != 0u)
    {
        float2 centered = uv * 2.0 - 1.0;
        float falloff = saturate(1.0 - dot(centered, centered));
        alpha *= falloff * falloff;
        color *= 1.0 + falloff * MaterialParams.z;
    }
    else if ((EntityFlags & ENTITY_HOLOGRAM) != 0u)
    {
        float scan = 0.45 + 0.55 * sin((uv.y + CameraPositionTime.w * 0.55) * 72.0);
        color *= 0.7 + scan * 0.8;
        alpha *= 0.72 + scan * 0.22;
    }
    else if ((EntityFlags & ENTITY_TELEGRAPH) != 0u)
    {
        float pulse = 0.72 + 0.28 * sin(CameraPositionTime.w * 8.0);
        color *= pulse * (2.0 + MaterialParams.z);
    }
    else if (ObjectType == 4u)
    {
        float2 lines = abs(frac(input.worldPosition.xz) - 0.5);
        alpha *= step(0.475, max(lines.x, lines.y));
    }

    return float4(color, alpha);
}

float4 CopyPS(FullscreenOutput input) : SV_TARGET
{
    return SceneTexture.SampleLevel(LinearClampSampler, input.uv, 0);
}

float4 BloomExtractPS(FullscreenOutput input) : SV_TARGET
{
    float3 color = CompositeTexture.SampleLevel(LinearClampSampler, input.uv, 0).rgb;
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    float weight = saturate((luminance - 0.85) / max(luminance, 0.001));
    return float4(color * weight, 1.0);
}

float4 BloomBlurHPS(FullscreenOutput input) : SV_TARGET
{
    float2 texel = float2(CameraRightViewportX.w, 0.0);
    float3 color = BloomTextureA.SampleLevel(LinearClampSampler, input.uv, 0).rgb * 0.227027;
    color += BloomTextureA.SampleLevel(LinearClampSampler, input.uv + texel * 1.384615, 0).rgb * 0.316216;
    color += BloomTextureA.SampleLevel(LinearClampSampler, input.uv - texel * 1.384615, 0).rgb * 0.316216;
    color += BloomTextureA.SampleLevel(LinearClampSampler, input.uv + texel * 3.230769, 0).rgb * 0.070270;
    color += BloomTextureA.SampleLevel(LinearClampSampler, input.uv - texel * 3.230769, 0).rgb * 0.070270;
    return float4(color, 1.0);
}

float4 BloomBlurVPS(FullscreenOutput input) : SV_TARGET
{
    float2 texel = float2(0.0, CameraUpViewportY.w);
    float3 color = BloomTextureB.SampleLevel(LinearClampSampler, input.uv, 0).rgb * 0.227027;
    color += BloomTextureB.SampleLevel(LinearClampSampler, input.uv + texel * 1.384615, 0).rgb * 0.316216;
    color += BloomTextureB.SampleLevel(LinearClampSampler, input.uv - texel * 1.384615, 0).rgb * 0.316216;
    color += BloomTextureB.SampleLevel(LinearClampSampler, input.uv + texel * 3.230769, 0).rgb * 0.070270;
    color += BloomTextureB.SampleLevel(LinearClampSampler, input.uv - texel * 3.230769, 0).rgb * 0.070270;
    return float4(color, 1.0);
}

float3 AcesToneMap(float3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float3 SampleFinal(float2 uv)
{
    float3 color = CompositeTexture.SampleLevel(LinearClampSampler, uv, 0).rgb;
    if ((FeatureFlags & FEATURE_BLOOM) != 0u)
        color += BloomTextureA.SampleLevel(LinearClampSampler, uv, 0).rgb * 0.72;
    if ((FeatureFlags & FEATURE_HDR) == 0u)
        color = saturate(color);
    if ((FeatureFlags & FEATURE_TONEMAP) != 0u)
        color = AcesToneMap(color);
    return color;
}

float4 TonemapPS(FullscreenOutput input) : SV_TARGET
{
    float3 color = SampleFinal(input.uv);
    if ((FeatureFlags & FEATURE_FXAA) != 0u)
    {
        float2 texel = float2(CameraRightViewportX.w, CameraUpViewportY.w);
        float3 north = SampleFinal(input.uv + float2(0.0, -texel.y));
        float3 south = SampleFinal(input.uv + float2(0.0, texel.y));
        float3 east = SampleFinal(input.uv + float2(texel.x, 0.0));
        float3 west = SampleFinal(input.uv + float2(-texel.x, 0.0));
        float centerLuma = dot(color, float3(0.299, 0.587, 0.114));
        float range = max(max(abs(centerLuma - dot(north, float3(0.299, 0.587, 0.114))),
                              abs(centerLuma - dot(south, float3(0.299, 0.587, 0.114)))),
                          max(abs(centerLuma - dot(east, float3(0.299, 0.587, 0.114))),
                              abs(centerLuma - dot(west, float3(0.299, 0.587, 0.114)))));
        color = lerp(color, (north + south + east + west + color * 2.0) / 6.0,
                     saturate(range * 2.5));
    }
    return float4(pow(saturate(color), 1.0 / 2.2), 1.0);
}
