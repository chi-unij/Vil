// Tonemapping pass: HDR scene + bloom + AO -> LDR output.
// ACES filmic tonemapping (Narkowicz fit) + gamma.
// Stores luminance in alpha for FXAA.

cbuffer TonemapCB : register(b0)
{
    float gExposure;
    float gBloomIntensity;
    float gAOStrength;
    float gInkWashStrength;
    float gInkFlowStrength;
    float gGameTime;
    float gInkFlowSpeed;
    float gPad1;
};

Texture2D<float4> gHdrScene : register(t0);
Texture2D<float4> gBloom    : register(t1);
Texture2D<float>  gAOTex    : register(t2);
SamplerState gSamp : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSFullscreen(uint vid : SV_VertexID)
{
    float2 p;
    p.x = (vid == 2) ? 3.0 : -1.0;
    p.y = (vid == 1) ? -3.0 : 1.0;

    VSOut o;
    o.pos = float4(p, 0.0, 1.0);
    o.uv = float2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);
    return o;
}

// ACES filmic tonemapping (Narkowicz 2015 fit).
float3 ACESFilm(float3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 SampleTonemappedColor(float2 uv)
{
    float3 hdr = gHdrScene.SampleLevel(gSamp, uv, 0).rgb;
    hdr += gBloom.SampleLevel(gSamp, uv, 0).rgb * gBloomIntensity;

    float ao = gAOTex.SampleLevel(gSamp, uv, 0);
    hdr *= lerp(1.0, ao, gAOStrength);
    hdr *= gExposure;

    float3 ldr = ACESFilm(hdr);
    return pow(max(ldr, 0.0), 1.0 / 2.2);
}

float Hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float2 GradientDirection(float2 cell)
{
    float angle = Hash21(cell) * 6.28318530718;
    return float2(cos(angle), sin(angle));
}

float GradientNoise(float2 p)
{
    float2 cell = floor(p);
    float2 local = frac(p);
    float2 fade = local * local * local *
                  (local * (local * 6.0 - 15.0) + 10.0);

    float a = dot(GradientDirection(cell), local);
    float b = dot(GradientDirection(cell + float2(1.0, 0.0)),
                  local - float2(1.0, 0.0));
    float c = dot(GradientDirection(cell + float2(0.0, 1.0)),
                  local - float2(0.0, 1.0));
    float d = dot(GradientDirection(cell + float2(1.0, 1.0)),
                  local - float2(1.0, 1.0));
    return saturate(lerp(lerp(a, b, fade.x),
                         lerp(c, d, fade.x), fade.y) * 0.72 + 0.5);
}

float2 RotateNoiseDomain(float2 p)
{
    return float2(p.x * 0.80 - p.y * 0.60,
                  p.x * 0.60 + p.y * 0.80);
}

float InkFbm(float2 p)
{
    float value = 0.0;
    float amplitude = 0.5;
    [unroll]
    for (int octave = 0; octave < 4; ++octave)
    {
        value += GradientNoise(p) * amplitude;
        p = RotateNoiseDomain(p) * 2.07 + float2(11.73, 7.91);
        amplitude *= 0.5;
    }
    return value / 0.9375;
}

float GameplayCuePreservation(float3 color)
{
    float maxChannel = max(color.r, max(color.g, color.b));
    float minChannel = min(color.r, min(color.g, color.b));
    float chroma = maxChannel - minChannel;
    return smoothstep(0.16, 0.48, chroma) *
           smoothstep(0.22, 0.72, maxChannel);
}

float PaperGrain(float2 pixelPosition)
{
    // 画面に固定した低振幅ノイズで、移動時のちらつきを避ける。
    float2 cell = floor(pixelPosition * 0.5);
    return Hash21(cell) - 0.5;
}

float3 ApplyInkWash(float3 sourceColor, float2 uv, float2 pixelPosition,
                    float strength)
{
    uint textureWidth;
    uint textureHeight;
    gHdrScene.GetDimensions(textureWidth, textureHeight);
    float2 texelSize = 1.0 / max(float2(textureWidth, textureHeight), 1.0);

    float lumaLeft = Luminance(SampleTonemappedColor(uv - float2(texelSize.x, 0.0)));
    float lumaRight = Luminance(SampleTonemappedColor(uv + float2(texelSize.x, 0.0)));
    float lumaUp = Luminance(SampleTonemappedColor(uv - float2(0.0, texelSize.y)));
    float lumaDown = Luminance(SampleTonemappedColor(uv + float2(0.0, texelSize.y)));

    // 輝度勾配から柔らかな墨線を作る。FXAA が後段で輪郭を整える。
    float edgeGradient = length(float2(lumaRight - lumaLeft, lumaDown - lumaUp));
    float inkEdge = smoothstep(0.045, 0.24, edgeGradient);

    float sourceLuma = Luminance(sourceColor);
    // 赤い危険表示と青緑の安全表示は水墨化を弱め、ゲーム情報を守る。
    float cuePreservation = GameplayCuePreservation(sourceColor);
    float stylizeMask = 1.0 - cuePreservation * 0.82;

    float pooledLuma = floor(saturate(sourceLuma) * 6.0 + 0.5) / 6.0;
    float3 stylized = sourceColor;
    stylized += (pooledLuma - sourceLuma) * 0.24 * stylizeMask;

    float shadowWash = 1.0 - smoothstep(0.16, 0.70, sourceLuma);
    float3 coolInk = sourceLuma * float3(0.92, 0.97, 1.0);
    stylized = lerp(stylized, coolInk, shadowWash * 0.16 * stylizeMask);

    float edgeDarkening = inkEdge * (0.42 + shadowWash * 0.18) * stylizeMask;
    stylized *= 1.0 - edgeDarkening;

    float grain = PaperGrain(pixelPosition);
    float grainMask = smoothstep(0.08, 0.82, sourceLuma) * stylizeMask;
    stylized += grain * 0.014 * grainMask;

    return lerp(sourceColor, saturate(stylized), strength);
}

float3 ApplyInkFlow(float3 sourceColor, float2 uv, float strength)
{
    uint textureWidth;
    uint textureHeight;
    gHdrScene.GetDimensions(textureWidth, textureHeight);
    float aspect = (float)textureWidth / max((float)textureHeight, 1.0);

    float2 p = uv * 2.0 - 1.0;
    p.x *= aspect;

    // 回転した勾配ノイズで格子感を消し、画面端に流れる筆跡を作る。
    float time = gGameTime * max(gInkFlowSpeed, 0.0) * 0.32;
    float2 drift = float2(time * 0.18, -time * 0.12);
    float2 floatingOffset =
        float2(sin(time * 0.83), cos(time * 0.61)) * float2(0.080, 0.052);
    float warpX = InkFbm(RotateNoiseDomain(p) * 2.70 + drift +
                         float2(3.7, 8.1));
    float warpY = InkFbm(p * 3.10 - drift * 0.83 + float2(9.2, 2.4));
    float2 warpedP = p + floatingOffset +
                     (float2(warpX, warpY) - 0.5) * 0.14;
    float broadNoise = InkFbm(warpedP * 2.35 + drift);
    float detailNoise = InkFbm(RotateNoiseDomain(warpedP) * 6.20 -
                               drift * 1.45 + float2(4.2, 7.7));

    // 太い墨溜まりと細い毛先を重ね、均一な円ではなく破れた筆弧にする。
    float2 arcCoordA = warpedP - float2(0.32, -0.08);
    float angleA = atan2(arcCoordA.y, arcCoordA.x);
    float radiusA = 1.00 + sin(angleA * 2.0 + time * 0.70) * 0.055 +
                    (broadNoise - 0.5) * 0.12;
    float radialA = length(arcCoordA) - radiusA;
    float bodyA = 1.0 - smoothstep(0.045, 0.155, abs(radialA));
    float bristleA = 1.0 - smoothstep(
        0.012, 0.048,
        abs(radialA - sin(angleA * 17.0 + detailNoise * 5.0) * 0.032));
    float sweepA = smoothstep(-0.62, 0.08,
                              sin(angleA - 0.18 + time * 0.11));
    float breakupA = smoothstep(0.34, 0.61,
                                detailNoise + sin(angleA * 7.0) * 0.12);
    float arcA = max(bodyA * 0.72, bristleA * 0.66) * sweepA * breakupA;

    float2 arcCoordB = warpedP - float2(-0.70, 0.20);
    float angleB = atan2(arcCoordB.y, arcCoordB.x);
    float radiusB = 0.78 + sin(angleB * 2.7 - time * 0.62) * 0.040 +
                    (detailNoise - 0.5) * 0.075;
    float radialB = length(arcCoordB) - radiusB;
    float bodyB = 1.0 - smoothstep(0.032, 0.120, abs(radialB));
    float sweepB = smoothstep(-0.48, 0.16,
                              -sin(angleB + 0.84 - time * 0.09));
    float breakupB = smoothstep(0.39, 0.64,
                                broadNoise + sin(angleB * 9.0) * 0.10);
    float arcB = bodyB * sweepB * breakupB * 0.72;

    // 方向性のある薄い墨糸。セル単位の濃淡を直接画面へ出さない。
    float2 flowCoord = float2(warpedP.x * 0.78 + warpedP.y * 0.52,
                              -warpedP.x * 0.32 + warpedP.y * 0.96);
    float upperPath = -0.72 + sin(flowCoord.x * 2.8 + time * 0.75) * 0.085 +
                      (warpY - 0.5) * 0.10;
    float lowerPath = 0.70 + sin(flowCoord.x * 2.2 - time * 0.57) * 0.070 +
                      (warpX - 0.5) * 0.085;
    float upperWisp = 1.0 - smoothstep(0.018, 0.090,
                                      abs(flowCoord.y - upperPath));
    float lowerWisp = 1.0 - smoothstep(0.016, 0.075,
                                      abs(flowCoord.y - lowerPath));
    float wispWindow = 1.0 - smoothstep(0.52, 1.62, abs(flowCoord.x));
    float wisps = (upperWisp + lowerWisp * 0.68) * wispWindow *
                  smoothstep(0.35, 0.67, detailNoise);

    float brushArcs = arcA + arcB;
    float centerClear = smoothstep(0.23, 0.50, length(p));
    float border = smoothstep(0.38, 0.94,
                              max(abs(p.x) / max(aspect, 0.001), abs(p.y)));
    float placement = centerClear * saturate(0.30 + border * 0.90);
    float inkDensity = saturate((brushArcs + wisps * 0.46) * placement);

    float cuePreservation = GameplayCuePreservation(sourceColor);
    float stylizeMask = 1.0 - cuePreservation * 0.88;
    float darkInk = inkDensity * (0.30 + detailNoise * 0.38) *
                    stylizeMask * strength;
    float paleWash = inkDensity * (1.0 - detailNoise) * 0.12 *
                     stylizeMask * strength;

    float3 inked = sourceColor * (1.0 - darkInk);
    float3 smokeGray = float3(0.16, 0.18, 0.19);
    inked = lerp(inked, smokeGray, paleWash);
    return saturate(inked);
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 ldr = SampleTonemappedColor(i.uv);
    if (gInkWashStrength > 0.0001)
        ldr = ApplyInkWash(ldr, i.uv, i.pos.xy, saturate(gInkWashStrength));
    if (gInkFlowStrength > 0.0001)
        ldr = ApplyInkFlow(ldr, i.uv, saturate(gInkFlowStrength));

    // Store luminance in alpha for FXAA.
    float luma = Luminance(ldr);
    return float4(ldr, luma);
}
