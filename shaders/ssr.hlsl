// ======================================
// File: ssr.hlsl
// Purpose: Screen-space reflection resolve for water and puddle surfaces.
//          This is raster SSR ray marching, not DXR / hardware ray tracing.
// ======================================

cbuffer SSRCB : register(b0)
{
    float4x4 gInvViewProj;
    float4x4 gView;
    float4x4 gProj;
    float4   gScreenParams;      // xy = size, zw = 1 / size
    float4   gReflectionParams;  // x = strength, y = maxDistance, z = thickness, w = stride
};

Texture2D<float4> gSceneTex    : register(t0);
Texture2D<float4> gAlbedoTex   : register(t1);
Texture2D<float4> gNormalTex   : register(t2);
Texture2D<float4> gMaterialTex : register(t3);
Texture2D<float>  gDepthTex    : register(t4);

SamplerState gPointClamp  : register(s0);
SamplerState gLinearClamp : register(s1);

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSFullscreen(uint vid : SV_VertexID)
{
    VSOut o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float4 ndc = float4(uv * 2.0f - 1.0f, depth, 1.0f);
    ndc.y = -ndc.y;
    float4 worldPos = mul(ndc, gInvViewProj);
    return worldPos.xyz / worldPos.w;
}

float2 ProjectViewToUv(float3 posV)
{
    float4 clip = mul(float4(posV, 1.0f), gProj);
    float2 ndc = clip.xy / max(clip.w, 1e-5f);
    return float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
}

float ComputeWaterMask(float3 albedo, float roughness)
{
    float lowRoughness = 1.0f - smoothstep(0.10f, 0.24f, roughness);
    float waterHue = saturate((albedo.g + albedo.b * 1.25f - albedo.r * 1.45f) * 1.65f);
    float darkPuddle = (1.0f - smoothstep(0.16f, 0.34f, dot(albedo, float3(0.299f, 0.587f, 0.114f))))
                     * saturate((albedo.g + albedo.b - albedo.r * 1.2f) * 3.0f);
    return saturate(max(waterHue, darkPuddle) * lowRoughness);
}

float EdgeFade(float2 uv)
{
    float2 edge = min(uv, 1.0f - uv);
    return smoothstep(0.0f, 0.08f, min(edge.x, edge.y));
}

float4 PSMain(VSOut pin) : SV_TARGET
{
    float3 sceneColor = gSceneTex.Sample(gLinearClamp, pin.uv).rgb;
    float depth = gDepthTex.Sample(gPointClamp, pin.uv);
    if (depth >= 1.0f)
        return float4(sceneColor, 1.0f);

    float3 albedo = gAlbedoTex.Sample(gPointClamp, pin.uv).rgb;
    float3 normalW = normalize(gNormalTex.Sample(gPointClamp, pin.uv).xyz);
    float roughness = gMaterialTex.Sample(gPointClamp, pin.uv).g;

    float waterMask = ComputeWaterMask(albedo, roughness);
    if (waterMask <= 0.001f)
        return float4(sceneColor, 1.0f);

    float3 posW = ReconstructWorldPos(pin.uv, depth);
    float3 posV = mul(float4(posW, 1.0f), gView).xyz;
    float3 normalV = normalize(mul(float4(normalW, 0.0f), gView).xyz);

    float3 viewRay = normalize(posV);
    float3 reflectRay = normalize(reflect(viewRay, normalV));
    if (reflectRay.z <= 0.02f)
    {
        float3 fallback = lerp(float3(0.05f, 0.14f, 0.18f), float3(0.20f, 0.36f, 0.42f),
                               saturate(normalW.y));
        float fallbackMix = waterMask * gReflectionParams.x * (1.0f - roughness);
        return float4(lerp(sceneColor, fallback, fallbackMix * 0.35f), 1.0f);
    }

    float maxDistance = max(gReflectionParams.y, 0.5f);
    float thickness = max(gReflectionParams.z, 0.01f);
    float stride = max(gReflectionParams.w, 0.05f);
    float3 hitColor = 0.0f;
    float hitWeight = 0.0f;

    [loop]
    for (int stepIndex = 1; stepIndex <= 48; ++stepIndex)
    {
        float travel = (float)stepIndex * stride;
        if (travel > maxDistance)
            break;

        float3 sampleV = posV + reflectRay * travel;
        float2 sampleUv = ProjectViewToUv(sampleV);
        if (sampleUv.x <= 0.0f || sampleUv.x >= 1.0f ||
            sampleUv.y <= 0.0f || sampleUv.y >= 1.0f)
            break;

        float sampleDepth = gDepthTex.SampleLevel(gPointClamp, sampleUv, 0).r;
        if (sampleDepth >= 1.0f)
            continue;

        float3 sceneW = ReconstructWorldPos(sampleUv, sampleDepth);
        float3 sceneV = mul(float4(sceneW, 1.0f), gView).xyz;
        float dz = sampleV.z - sceneV.z;
        if (dz >= 0.0f && dz < thickness + travel * 0.015f)
        {
            hitColor = gSceneTex.SampleLevel(gLinearClamp, sampleUv, 0).rgb;
            hitWeight = EdgeFade(sampleUv) * saturate(1.0f - travel / maxDistance);
            break;
        }
    }

    float3 fallbackColor = lerp(float3(0.04f, 0.12f, 0.16f), float3(0.18f, 0.34f, 0.40f),
                                saturate(normalW.y));
    float3 reflectionColor = lerp(fallbackColor, hitColor, hitWeight);
    float fresnel = pow(1.0f - saturate(dot(-viewRay, normalV)), 5.0f);
    float reflectionStrength = waterMask * gReflectionParams.x
                             * lerp(0.18f, 0.72f, fresnel)
                             * (1.0f - saturate(roughness));

    return float4(lerp(sceneColor, reflectionColor, reflectionStrength), 1.0f);
}
