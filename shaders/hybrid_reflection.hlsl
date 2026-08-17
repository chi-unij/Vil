// ======================================
// File: hybrid_reflection.hlsl
// Purpose: Inline RayQuery reflection for data-driven water and mirror receivers.
// ======================================

cbuffer ReflectionConstants : register(b0)
{
    float4x4 gInvViewProj;
    float4x4 gViewProj;
    float4 gCameraAndMaxDistance;
    float4 gReflectionParams;
    float4 gLightDirectionIntensity;
    float4 gLightColorAmbient;
    float4 gLightCounts; // x=point light 数、y=spot light 数
    float4 gEnvironmentParams; // x=sky exposure
};

struct InstanceShadingData
{
    float4 baseColor;
    float4 materialParams; // x=metallic, y=roughness
    float4 uvTilingOffset;
    float4x4 world;
    uint triangleVertexAttributeOffset;
    uint baseColorTextureIndex;
    uint2 padding;
};

struct PointLight
{
    float3 position;
    float range;
    float3 color;
    float intensity;
    float3 padding;
    float padding2;
};

struct SpotLight
{
    float3 position;
    float range;
    float3 color;
    float intensity;
    float3 direction;
    float innerConeAngleCos;
    float outerConeAngleCos;
    float3 padding;
};

RaytracingAccelerationStructure gScene : register(t0);
Texture2D<float4> gSceneColor : register(t1);
Texture2D<float4> gNormalTexture : register(t2);
Texture2D<float4> gMaterialTexture : register(t3);
Texture2D<float> gDepthTexture : register(t4);
StructuredBuffer<InstanceShadingData> gInstanceMetadata : register(t5);
StructuredBuffer<float4> gTriangleVertexNormals : register(t6);
StructuredBuffer<float4> gTriangleVertexUvs : register(t7);
StructuredBuffer<PointLight> gPointLights : register(t8);
StructuredBuffer<SpotLight> gSpotLights : register(t9);
Texture2D<float4> gBaseColorTextures[64] : register(t10);
Texture2D<float4> gEnvironmentLatLong : register(t74);
RWTexture2D<float4> gReflectionOutput : register(u0);

SamplerState gMaterialSampler : register(s0);
SamplerState gEnvironmentSampler : register(s1);

static const float PI = 3.14159265f;
static const float TWO_PI = 6.28318530718f;

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(1.0f - cosTheta, 5.0f);
}

float DistributionGGX(float NdotH, float roughness)
{
    const float alpha = roughness * roughness;
    const float alphaSquared = alpha * alpha;
    const float denominator = NdotH * NdotH * (alphaSquared - 1.0f) + 1.0f;
    return alphaSquared / max(PI * denominator * denominator, 1e-6f);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    const float r = roughness + 1.0f;
    const float k = r * r / 8.0f;
    return NdotV / max(NdotV * (1.0f - k) + k, 1e-6f);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    return GeometrySchlickGGX(NdotV, roughness)
        * GeometrySchlickGGX(NdotL, roughness);
}

float3 EvaluateDirectLight(float3 normal, float3 viewDirection,
                           float3 lightDirection, float3 radiance,
                           float3 albedo, float metallic, float roughness,
                           float3 F0)
{
    const float3 halfVector = normalize(viewDirection + lightDirection);
    const float NdotL = saturate(dot(normal, lightDirection));
    const float NdotV = saturate(dot(normal, viewDirection));
    const float NdotH = saturate(dot(normal, halfVector));
    const float VdotH = saturate(dot(viewDirection, halfVector));
    const float3 fresnel = FresnelSchlick(VdotH, F0);
    const float distribution = DistributionGGX(NdotH, roughness);
    const float geometry = GeometrySmith(NdotV, NdotL, roughness);
    const float3 specular = distribution * geometry * fresnel
        / max(4.0f * NdotV * NdotL, 1e-4f);
    const float3 diffuse = (1.0f - fresnel) * (1.0f - metallic)
        * albedo / PI;
    return (diffuse + specular) * radiance * NdotL;
}

float DistanceAttenuation(float distanceToLight, float range)
{
    const float distanceSquared = distanceToLight * distanceToLight;
    const float rangeSquared = max(range * range, 1e-4f);
    const float window = saturate(
        1.0f - distanceSquared * distanceSquared
            / (rangeSquared * rangeSquared));
    return window * window / max(distanceSquared, 1e-4f);
}

float SpotAngleAttenuation(float3 lightDirection, float3 spotDirection,
                           float innerCos, float outerCos)
{
    const float cosAngle = dot(-lightDirection, spotDirection);
    return saturate((cosAngle - outerCos) / max(innerCos - outerCos, 0.001f));
}

float2 DirectionToLatLongUv(float3 direction)
{
    direction = normalize(direction);
    const float phi = atan2(direction.z, direction.x);
    const float theta = acos(clamp(direction.y, -1.0f, 1.0f));
    return float2(phi / TWO_PI + 0.5f, theta / PI);
}

float3 SampleEnvironment(float3 direction)
{
    const float2 environmentUv = DirectionToLatLongUv(direction);
    float3 environment = gEnvironmentLatLong.SampleLevel(
        gEnvironmentSampler, environmentUv, 0.0f).rgb;
    environment *= gEnvironmentParams.x;

    const float sunAngularRadius = 0.035f;
    const float3 sunViewDirection =
        normalize(-gLightDirectionIntensity.xyz);
    const float cosToSun = dot(normalize(direction), sunViewDirection);
    const float sunCore = smoothstep(
        cos(sunAngularRadius * 1.10f),
        cos(sunAngularRadius * 0.72f), cosToSun);
    const float sunGlow = smoothstep(
        cos(sunAngularRadius * 7.0f),
        cos(sunAngularRadius * 1.25f), cosToSun);
    const float horizonWarmth = saturate(cosToSun * 0.5f + 0.5f);
    environment += gLightColorAmbient.rgb * gLightDirectionIntensity.w
        * (sunCore * 7.0f + sunGlow * 0.45f);
    environment += gLightColorAmbient.rgb * gEnvironmentParams.x
        * horizonWarmth * sunGlow * 0.20f;
    return environment;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width = 0;
    uint height = 0;
    gReflectionOutput.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
        return;

    const uint2 pixel = dispatchThreadId.xy;
    const float4 sceneColor = gSceneColor.Load(int3(pixel, 0));
    const float4 material = gMaterialTexture.Load(int3(pixel, 0));
    const bool isWaterReceiver =
        material.a >= 0.0625f && material.a < 0.5625f;
    const bool isMirrorReceiver = material.a >= 0.5625f;
    const float receiverMask = isMirrorReceiver
        ? saturate((material.a - 0.625f) / 0.375f)
        : (isWaterReceiver
            ? saturate((material.a - 0.125f) / 0.375f)
            : 0.0f);
    const float depth = gDepthTexture.Load(int3(pixel, 0));
    if (receiverMask <= 0.001f || depth >= 1.0f)
    {
        gReflectionOutput[pixel] = sceneColor;
        return;
    }

    const float2 uv = (float2(pixel) + 0.5f) / float2(width, height);
    const float2 ndc = float2(uv.x * 2.0f - 1.0f,
                              1.0f - uv.y * 2.0f);
    float4 worldSurface = mul(float4(ndc, depth, 1.0f), gInvViewProj);
    worldSurface.xyz /= worldSurface.w;

    const float3 normalW = normalize(
        gNormalTexture.Load(int3(pixel, 0)).xyz);
    const float3 viewRay = normalize(
        worldSurface.xyz - gCameraAndMaxDistance.xyz);
    const float3 reflectionRay = normalize(reflect(viewRay, normalW));

    RayDesc ray;
    ray.Origin = worldSurface.xyz + normalW * 0.04f;
    ray.Direction = reflectionRay;
    ray.TMin = 0.01f;
    ray.TMax = gCameraAndMaxDistance.w;

    // Water は receiver geometry を除外し、Mirror は textured ground を含める。
    const uint instanceInclusionMask = isMirrorReceiver ? 0x03u : 0x01u;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(gScene, RAY_FLAG_NONE, instanceInclusionMask, ray);
    while (query.Proceed())
    {
    }

    const bool hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    float3 reflectionColor = SampleEnvironment(reflectionRay);

    if (hit)
    {
        const float rayDistance = query.CommittedRayT();
        const float distanceFade = isMirrorReceiver
            ? 1.0f
            : 0.62f + 0.38f * saturate(1.0f - rayDistance / ray.TMax);
        const uint instanceId = query.CommittedInstanceID();
        const InstanceShadingData instance = gInstanceMetadata[instanceId];

        const uint primitiveAttributeBase =
            instance.triangleVertexAttributeOffset
            + query.CommittedPrimitiveIndex() * 3u;
        const float2 barycentrics = query.CommittedTriangleBarycentrics();
        const float barycentric0 =
            1.0f - barycentrics.x - barycentrics.y;
        float3 hitNormalObject =
            gTriangleVertexNormals[primitiveAttributeBase].xyz * barycentric0
            + gTriangleVertexNormals[primitiveAttributeBase + 1u].xyz
                * barycentrics.x
            + gTriangleVertexNormals[primitiveAttributeBase + 2u].xyz
                * barycentrics.y;
        float2 hitUv =
            gTriangleVertexUvs[primitiveAttributeBase].xy * barycentric0
            + gTriangleVertexUvs[primitiveAttributeBase + 1u].xy
                * barycentrics.x
            + gTriangleVertexUvs[primitiveAttributeBase + 2u].xy
                * barycentrics.y;
        hitUv = hitUv * instance.uvTilingOffset.xy
            + instance.uvTilingOffset.zw;
        float3 hitNormalWorld = normalize(
            mul(float4(normalize(hitNormalObject), 0.0f), instance.world).xyz);
        const float3 hitViewDirection = normalize(-ray.Direction);
        if (dot(hitNormalWorld, hitViewDirection) < 0.0f)
            hitNormalWorld = -hitNormalWorld;

        // 主カメラの color buffer に依存せず、hit geometry の normal と
        // material scalar から安定した一次照明を再構成する。
        const float3 reflectedHitPosition =
            ray.Origin + ray.Direction * rayDistance;
        float3 albedo = instance.baseColor.rgb;
        if (instance.baseColorTextureIndex < 64u)
        {
            const uint textureIndex =
                NonUniformResourceIndex(instance.baseColorTextureIndex);
            albedo *= gBaseColorTextures[textureIndex].SampleLevel(
                gMaterialSampler, hitUv, 0.0f).rgb;
        }
        const float metallic = saturate(instance.materialParams.x);
        const float hitRoughness =
            clamp(instance.materialParams.y, 0.045f, 1.0f);
        const float3 F0 = lerp(0.04f.xxx, albedo, metallic);

        // Main deferred pass と同じ direct-light BRDF を hit point で評価する。
        const float3 sunDirection = normalize(-gLightDirectionIntensity.xyz);
        const float3 sunRadiance =
            gLightColorAmbient.rgb * gLightDirectionIntensity.w;
        float3 hitLighting = EvaluateDirectLight(
            hitNormalWorld, hitViewDirection, sunDirection, sunRadiance,
            albedo, metallic, hitRoughness, F0);

        const uint pointLightCount = min((uint)gLightCounts.x, 32u);
        for (uint lightIndex = 0; lightIndex < pointLightCount; ++lightIndex)
        {
            const PointLight light = gPointLights[lightIndex];
            const float3 lightVector = light.position - reflectedHitPosition;
            const float lightDistance = length(lightVector);
            const float3 lightDirection =
                lightVector / max(lightDistance, 1e-4f);
            const float attenuation =
                DistanceAttenuation(lightDistance, light.range);
            const float3 radiance =
                light.color * light.intensity * attenuation;
            hitLighting += EvaluateDirectLight(
                hitNormalWorld, hitViewDirection, lightDirection, radiance,
                albedo, metallic, hitRoughness, F0);
        }

        const uint spotLightCount = min((uint)gLightCounts.y, 32u);
        for (uint lightIndex = 0; lightIndex < spotLightCount; ++lightIndex)
        {
            const SpotLight light = gSpotLights[lightIndex];
            const float3 lightVector = light.position - reflectedHitPosition;
            const float lightDistance = length(lightVector);
            const float3 lightDirection =
                lightVector / max(lightDistance, 1e-4f);
            const float distanceAttenuation =
                DistanceAttenuation(lightDistance, light.range);
            const float angleAttenuation = SpotAngleAttenuation(
                lightDirection, normalize(light.direction),
                light.innerConeAngleCos, light.outerConeAngleCos);
            const float3 radiance = light.color * light.intensity
                * distanceAttenuation * angleAttenuation;
            hitLighting += EvaluateDirectLight(
                hitNormalWorld, hitViewDirection, lightDirection, radiance,
                albedo, metallic, hitRoughness, F0);
        }

        // IBL texture は未接続のため、material-aware ambient を維持する。
        const float ambientStrength =
            0.18f + 0.10f * saturate(gLightColorAmbient.w);
        const float3 ambient = albedo * ambientStrength
            * (1.0f - metallic * 0.65f);
        reflectionColor = (hitLighting + ambient) * distanceFade;

        // Water は画面内 hit の raster color を再利用できる。
        // Mirror では反射側の hit と主カメラの可視面が一致しないため、
        // screen-space 再投影を避けて安定した instance material 色を使う。
        const float4 hitClip =
            mul(float4(reflectedHitPosition, 1.0f), gViewProj);
        if (!isMirrorReceiver && hitClip.w > 1e-5f)
        {
            const float2 hitNdc = hitClip.xy / hitClip.w;
            const float2 hitUv = float2(hitNdc.x * 0.5f + 0.5f,
                                        -hitNdc.y * 0.5f + 0.5f);
            if (all(hitUv > 0.0f) && all(hitUv < 1.0f))
            {
                const uint2 hitPixel = min(
                    uint2(hitUv * float2(width, height)),
                    uint2(width - 1, height - 1));
                reflectionColor =
                    gSceneColor.Load(int3(hitPixel, 0)).rgb * distanceFade;
            }
        }
    }

    const float roughness = saturate(material.g);
    const float fresnel = pow(
        1.0f - saturate(dot(-viewRay, normalW)), 5.0f);
    const float surfaceFresnel = isMirrorReceiver
        ? lerp(0.92f, 1.0f, fresnel)
        : lerp(0.48f, 0.94f, fresnel);
    const float roughnessAttenuation = isMirrorReceiver
        ? (1.0f - roughness * 0.45f)
        : (1.0f - roughness);
    const float reflectionWeight = saturate(
        receiverMask * gReflectionParams.x * surfaceFresnel *
        roughnessAttenuation);
    gReflectionOutput[pixel] = float4(
        lerp(sceneColor.rgb, reflectionColor, reflectionWeight), 1.0f);
}
