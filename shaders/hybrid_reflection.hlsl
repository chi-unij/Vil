// ======================================
// File: hybrid_reflection.hlsl
// Purpose: Inline RayQuery reflection for data-driven water and mirror receivers.
// ======================================

cbuffer ProofConstants : register(b0)
{
    float4x4 gInvViewProj;
    float4x4 gViewProj;
    float4 gCameraAndMaxDistance;
    float4 gReflectionParams;
};

RaytracingAccelerationStructure gScene : register(t0);
Texture2D<float4> gSceneColor : register(t1);
Texture2D<float4> gNormalTexture : register(t2);
Texture2D<float4> gMaterialTexture : register(t3);
Texture2D<float> gDepthTexture : register(t4);
StructuredBuffer<float4> gInstanceColors : register(t5);
RWTexture2D<float4> gReflectionOutput : register(u0);

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

    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(gScene, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed())
    {
    }

    const bool hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    const float skyGradient = saturate(reflectionRay.y * 0.5f + 0.5f);
    float3 reflectionColor = isMirrorReceiver
        ? lerp(float3(0.025f, 0.03f, 0.04f),
               float3(0.24f, 0.27f, 0.32f), skyGradient)
        : lerp(float3(0.035f, 0.095f, 0.14f),
               float3(0.20f, 0.34f, 0.42f), skyGradient);

    if (hit)
    {
        const float rayDistance = query.CommittedRayT();
        const float distanceFade =
            0.62f + 0.38f * saturate(1.0f - rayDistance / ray.TMax);
        const uint instanceId = query.CommittedInstanceID();
        reflectionColor = gInstanceColors[instanceId].rgb * distanceFade;

        // 画面内 hit は raster color を再利用し、画面外 hit は mesh 色で示す。
        const float3 hitPosition = ray.Origin + ray.Direction * rayDistance;
        const float4 hitClip = mul(float4(hitPosition, 1.0f), gViewProj);
        if (hitClip.w > 1e-5f)
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
