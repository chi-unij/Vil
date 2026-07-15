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

float EdgeFade(float2 uv)
{
    float2 edge = min(uv, 1.0f - uv);
    return smoothstep(0.0f, 0.08f, min(edge.x, edge.y));
}

float3 SampleReflectionColor(float2 uv, float roughness)
{
    // 中央の低 roughness は鮮明に保ち、外周だけを画面空間で滑らかにぼかす。
    float blurAmount = saturate((roughness - 0.035f) / 0.125f);
    float2 radius = gScreenParams.zw * (blurAmount * 7.0f);

    float3 color = gSceneTex.SampleLevel(gLinearClamp, uv, 0).rgb * 0.28f;
    color += gSceneTex.SampleLevel(gLinearClamp, uv + float2( radius.x, 0.0f), 0).rgb * 0.12f;
    color += gSceneTex.SampleLevel(gLinearClamp, uv + float2(-radius.x, 0.0f), 0).rgb * 0.12f;
    color += gSceneTex.SampleLevel(gLinearClamp, uv + float2(0.0f,  radius.y), 0).rgb * 0.12f;
    color += gSceneTex.SampleLevel(gLinearClamp, uv + float2(0.0f, -radius.y), 0).rgb * 0.12f;
    color += gSceneTex.SampleLevel(gLinearClamp, uv + float2( radius.x,  radius.y), 0).rgb * 0.06f;
    color += gSceneTex.SampleLevel(gLinearClamp, uv + float2(-radius.x,  radius.y), 0).rgb * 0.06f;
    color += gSceneTex.SampleLevel(gLinearClamp, uv + float2( radius.x, -radius.y), 0).rgb * 0.06f;
    color += gSceneTex.SampleLevel(gLinearClamp, uv + float2(-radius.x, -radius.y), 0).rgb * 0.06f;
    return color;
}

float4 PSMain(VSOut pin) : SV_TARGET
{
    float3 sceneColor = gSceneTex.Sample(gLinearClamp, pin.uv).rgb;
    float depth = gDepthTex.Sample(gPointClamp, pin.uv);
    if (depth >= 1.0f)
        return float4(sceneColor, 1.0f);

    float3 normalW = normalize(gNormalTex.Sample(gPointClamp, pin.uv).xyz);
    float4 material = gMaterialTex.Sample(gPointClamp, pin.uv);
    float roughness = material.g;
    // 0.01 は hit exclusion の予約値。0.05 以上だけを反射面として扱う。
    float waterMask = (material.a >= 0.05f) ? material.a : 0.0f;

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
    float3 skyReflectionColor = lerp(float3(0.04f, 0.12f, 0.16f),
                                     float3(0.18f, 0.34f, 0.40f),
                                     saturate(normalW.y));
    float skyReflectionWeight = 0.0f;

    // 水面自身を出発点として拾わないよう、法線方向へわずかにずらす。
    float3 rayOriginV = posV + normalV * 0.035f;

    [loop]
    for (int stepIndex = 1; stepIndex <= 128; ++stepIndex)
    {
        float travel = (float)stepIndex * stride;
        if (travel > maxDistance)
            break;

        float3 sampleV = rayOriginV + reflectRay * travel;
        float2 sampleUv = ProjectViewToUv(sampleV);
        if (sampleUv.x <= 0.0f || sampleUv.x >= 1.0f ||
            sampleUv.y <= 0.0f || sampleUv.y >= 1.0f)
            break;

        float sampleDepth = gDepthTex.SampleLevel(gPointClamp, sampleUv, 0).r;
        if (sampleDepth >= 1.0f)
        {
            // Deferred パス後も gSceneTex には描画済みの空が残る。
            // 深度を持たない空でも、画面内の空と太陽を水面反射に利用する。
            float candidateWeight = EdgeFade(sampleUv);
            if (candidateWeight > skyReflectionWeight)
            {
                skyReflectionColor = SampleReflectionColor(sampleUv, roughness);
                skyReflectionWeight = candidateWeight;
            }
            continue;
        }

        // SSR の水面同士の自己交差は、視点移動時に長い縞として伸びるため除外する。
        const float sampleSsrMask =
            gMaterialTex.SampleLevel(gPointClamp, sampleUv, 0).a;
        if (sampleSsrMask > 0.001f)
            continue;

        float3 sceneW = ReconstructWorldPos(sampleUv, sampleDepth);
        float3 sceneV = mul(float4(sceneW, 1.0f), gView).xyz;
        float dz = sampleV.z - sceneV.z;
        if (dz >= 0.0f && dz < thickness)
        {
            // 反射レイに背を向ける面を採用すると、踏み石の上面などが
            // 水面上で横長に引き伸ばされる。正面側だけを滑らかに採用する。
            float3 sampleNormalW = normalize(
                gNormalTex.SampleLevel(gPointClamp, sampleUv, 0).xyz);
            float3 sampleNormalV = normalize(
                mul(float4(sampleNormalW, 0.0f), gView).xyz);
            float facingWeight = smoothstep(
                0.02f, 0.18f, dot(sampleNormalV, -reflectRay));
            if (facingWeight <= 0.001f)
                continue;

            hitColor = SampleReflectionColor(sampleUv, roughness);
            hitWeight = EdgeFade(sampleUv)
                      * saturate(1.0f - travel / maxDistance)
                      * facingWeight;
            break;
        }
    }

    float3 fallbackColor = lerp(float3(0.04f, 0.12f, 0.16f),
                                float3(0.18f, 0.34f, 0.40f),
                                saturate(normalW.y));
    float3 reflectionColor = lerp(fallbackColor, skyReflectionColor,
                                  skyReflectionWeight);
    reflectionColor = lerp(reflectionColor, hitColor, hitWeight);
    float fresnel = pow(1.0f - saturate(dot(-viewRay, normalV)), 5.0f);
    float surfaceFresnel = lerp(0.46f, 0.92f, fresnel);
    float reflectionStrength = saturate(waterMask * gReflectionParams.x
                                      * surfaceFresnel
                                      * (1.0f - saturate(roughness)));

    return float4(lerp(sceneColor, reflectionColor, reflectionStrength), 1.0f);
}
