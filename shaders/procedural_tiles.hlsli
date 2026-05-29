// ======================================
// File: procedural_tiles.hlsli
// Purpose: Procedural tile shader library (Phase 9).
//          Noise functions + per-tile-type procedural generators.
//          Included by gbuffer.hlsl and mesh.hlsl.
// ======================================

#ifndef PROCEDURAL_TILES_HLSLI
#define PROCEDURAL_TILES_HLSLI

// ---- Result struct ----
struct ProceduralResult
{
    float3 albedo;
    float3 emissive;
    float3 normalTS;   // tangent-space normal perturbation
    float  metallic;
    float  roughness;
    float  ao;
};

// ============================================================================
// Noise Foundation
// ============================================================================

float hash21(float2 p)
{
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

float2 hash22(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * float3(0.1031f, 0.1030f, 0.0973f));
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.xx + p3.yz) * p3.zy);
}

float valueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0f - 2.0f * f); // smoothstep

    float a = hash21(i);
    float b = hash21(i + float2(1.0f, 0.0f));
    float c = hash21(i + float2(0.0f, 1.0f));
    float d = hash21(i + float2(1.0f, 1.0f));

    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float gradientNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0f - 2.0f * f);

    float2 ga = hash22(i) * 2.0f - 1.0f;
    float2 gb = hash22(i + float2(1.0f, 0.0f)) * 2.0f - 1.0f;
    float2 gc = hash22(i + float2(0.0f, 1.0f)) * 2.0f - 1.0f;
    float2 gd = hash22(i + float2(1.0f, 1.0f)) * 2.0f - 1.0f;

    float va = dot(ga, f);
    float vb = dot(gb, f - float2(1.0f, 0.0f));
    float vc = dot(gc, f - float2(0.0f, 1.0f));
    float vd = dot(gd, f - float2(1.0f, 1.0f));

    return lerp(lerp(va, vb, u.x), lerp(vc, vd, u.x), u.y) * 0.5f + 0.5f;
}

float fbm(float2 p, int octaves)
{
    float value = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;
    for (int i = 0; i < octaves; ++i)
    {
        value += amplitude * valueNoise(p * frequency);
        frequency *= 2.0f;
        amplitude *= 0.5f;
    }
    return value;
}

// Voronoi: returns float2(cellDist, edgeDist)
float2 voronoi(float2 p)
{
    float2 n = floor(p);
    float2 f = frac(p);

    float minDist = 8.0f;
    float secondDist = 8.0f;

    [unroll]
    for (int j = -1; j <= 1; ++j)
    {
        [unroll]
        for (int i = -1; i <= 1; ++i)
        {
            float2 neighbor = float2((float)i, (float)j);
            float2 pt = hash22(n + neighbor);
            float2 diff = neighbor + pt - f;
            float d = dot(diff, diff);
            if (d < minDist)
            {
                secondDist = minDist;
                minDist = d;
            }
            else if (d < secondDist)
            {
                secondDist = d;
            }
        }
    }

    return float2(sqrt(minDist), sqrt(secondDist) - sqrt(minDist));
}

float warpedFbm(float2 p, float time, int octaves)
{
    float2 q = float2(fbm(p + float2(0.0f, 0.0f), octaves),
                      fbm(p + float2(5.2f, 1.3f), octaves));
    float2 r = float2(fbm(p + 4.0f * q + float2(1.7f, 9.2f) + 0.15f * time, octaves),
                      fbm(p + 4.0f * q + float2(8.3f, 2.8f) + 0.12f * time, octaves));
    return fbm(p + 4.0f * r, octaves);
}

// ============================================================================
// Per-Tile Procedural Generators
// ============================================================================

// ---- 1: Fire / Lava ----
ProceduralResult ProceduralFire(float2 worldXZ, float time)
{
    ProceduralResult r;

    float2 uv = worldXZ * 2.0f;

    // Two-layer domain-warped FBM for flowing lava
    float lava1 = warpedFbm(uv * 1.5f, time * 0.8f, 4);
    float lava2 = warpedFbm(uv * 0.8f + float2(3.1f, 7.4f), time * 0.5f, 3);
    float lava = lerp(lava1, lava2, 0.4f);

    // Wave distortion
    float wave = sin(uv.x * 3.0f + time * 1.5f) * 0.1f +
                 sin(uv.y * 2.5f + time * 1.2f) * 0.08f;
    lava = saturate(lava + wave);

    // Color ramp: dark crust -> orange -> bright yellow
    float3 darkCrust = float3(0.15f, 0.02f, 0.01f);
    float3 orange    = float3(0.9f, 0.3f, 0.02f);
    float3 bright    = float3(1.2f, 0.9f, 0.2f);

    float3 color;
    if (lava < 0.45f)
        color = lerp(darkCrust, orange, smoothstep(0.2f, 0.45f, lava));
    else
        color = lerp(orange, bright, smoothstep(0.45f, 0.75f, lava));

    r.albedo = color;

    // Emissive: hot areas glow for bloom
    float emissiveStr = smoothstep(0.35f, 0.7f, lava) * 3.0f;
    r.emissive = color * emissiveStr;

    // Normal perturbation: cooled crust is raised, hot channels are flat
    float nx = (valueNoise(uv * 4.0f + float2(0.5f, 0.0f)) -
                valueNoise(uv * 4.0f - float2(0.5f, 0.0f)));
    float ny = (valueNoise(uv * 4.0f + float2(0.0f, 0.5f)) -
                valueNoise(uv * 4.0f - float2(0.0f, 0.5f)));
    float crustFactor = 1.0f - smoothstep(0.35f, 0.6f, lava);
    r.normalTS = normalize(float3(nx * crustFactor * 0.5f,
                                  ny * crustFactor * 0.5f,
                                  1.0f));

    // Material properties
    r.metallic  = 0.0f;
    r.roughness = lerp(0.3f, 0.9f, 1.0f - smoothstep(0.35f, 0.7f, lava));
    r.ao        = lerp(0.6f, 1.0f, lava);

    return r;
}

// ---- 2: Ice ----
ProceduralResult ProceduralIce(float2 worldXZ, float time)
{
    ProceduralResult r;

    float2 uv = worldXZ * 3.0f;

    // Voronoi for crystal facet pattern
    float2 vor = voronoi(uv);
    float cellDist = vor.x;
    float edgeDist = vor.y;

    // Frost lines at cell edges
    float frost = 1.0f - smoothstep(0.0f, 0.12f, edgeDist);

    // Per-cell shimmer sparkle
    float2 cellId = floor(uv);
    float cellHash = hash21(cellId);
    float sparkle = pow(saturate(sin(time * 3.0f + cellHash * 6.28f)), 8.0f);

    // Base ice color: pale blue-white with frost highlights
    float3 iceBase  = float3(0.6f, 0.75f, 0.85f);
    float3 iceFrost = float3(0.9f, 0.95f, 1.0f);
    float3 iceDeep  = float3(0.15f, 0.3f, 0.45f);

    float3 color = lerp(iceBase, iceDeep, cellDist * 0.6f);
    color = lerp(color, iceFrost, frost * 0.7f);
    color += sparkle * float3(0.3f, 0.35f, 0.4f) * (1.0f - frost);

    r.albedo = color;

    // Emissive: subtle frost glow + sparkle
    r.emissive = float3(0.05f, 0.15f, 0.25f) * frost +
                 float3(0.1f, 0.2f, 0.35f) * sparkle;

    // Normal: facet edges create ridges
    float edgeNx = (voronoi(uv + float2(0.02f, 0.0f)).y -
                    voronoi(uv - float2(0.02f, 0.0f)).y);
    float edgeNy = (voronoi(uv + float2(0.0f, 0.02f)).y -
                    voronoi(uv - float2(0.0f, 0.02f)).y);
    r.normalTS = normalize(float3(edgeNx * 2.0f, edgeNy * 2.0f, 1.0f));

    // Reflective surface
    r.metallic  = 0.1f;
    r.roughness = lerp(0.05f, 0.3f, cellDist);
    r.ao        = lerp(0.8f, 1.0f, edgeDist);

    return r;
}

// ---- 3: Lightning ----
ProceduralResult ProceduralLightning(float2 worldXZ, float time)
{
    ProceduralResult r;

    float2 uv = worldXZ * 2.5f;

    // Dark metallic base
    float3 baseColor = float3(0.04f, 0.05f, 0.08f);

    // Two scrolling electric arc lines
    float arc1UV = uv.x + uv.y * 0.3f + time * 2.0f;
    float arc1 = valueNoise(float2(arc1UV * 3.0f, uv.y * 8.0f + time));
    float arc1Line = exp(-abs(uv.y - arc1 * 0.4f + 0.2f) * 12.0f);

    float arc2UV = uv.x * 0.7f - uv.y * 0.5f + time * 1.5f;
    float arc2 = valueNoise(float2(arc2UV * 4.0f, uv.x * 6.0f - time * 0.8f));
    float arc2Line = exp(-abs(uv.x - arc2 * 0.3f + 0.15f) * 10.0f);

    float arcs = saturate(arc1Line + arc2Line);

    // Periodic flash burst
    float flash = pow(saturate(sin(time * 4.0f)), 20.0f) * 0.5f;

    // Cyan/white glow
    float3 arcColor = float3(0.3f, 0.7f, 1.0f);
    float3 flashColor = float3(0.8f, 0.9f, 1.0f);

    float3 color = baseColor + arcColor * arcs + flashColor * flash;

    r.albedo = baseColor;
    r.emissive = arcColor * arcs * 2.5f + flashColor * flash * 3.0f;

    // Subtle normal variation
    float n = gradientNoise(uv * 5.0f + time * 0.3f);
    r.normalTS = normalize(float3((n - 0.5f) * 0.15f,
                                  (gradientNoise(uv * 5.0f + float2(7.0f, 0.0f)) - 0.5f) * 0.15f,
                                  1.0f));

    r.metallic  = 0.3f;
    r.roughness = 0.6f;
    r.ao        = 1.0f;

    return r;
}

// ---- 4: Spike ----
ProceduralResult ProceduralSpike(float2 worldXZ, float time)
{
    ProceduralResult r;

    float2 uv = worldXZ * 3.0f;

    // FBM surface texture on dark metal
    float surface = fbm(uv * 2.0f, 4);
    float detail  = fbm(uv * 6.0f + float2(3.7f, 1.2f), 3);

    float3 darkMetal = float3(0.08f, 0.07f, 0.06f);
    float3 lightMetal = float3(0.18f, 0.16f, 0.14f);
    float3 color = lerp(darkMetal, lightMetal, surface * 0.5f + detail * 0.2f);

    r.albedo = color;

    // Subtle orange glow in surface grooves
    float groove = 1.0f - smoothstep(0.3f, 0.5f, surface);
    float pulse = 0.8f + 0.2f * sin(time * 2.0f);
    r.emissive = float3(0.4f, 0.15f, 0.03f) * groove * 0.5f * pulse;

    // Normal from FBM
    float nx = fbm(uv * 2.0f + float2(0.1f, 0.0f), 4) - fbm(uv * 2.0f - float2(0.1f, 0.0f), 4);
    float ny = fbm(uv * 2.0f + float2(0.0f, 0.1f), 4) - fbm(uv * 2.0f - float2(0.0f, 0.1f), 4);
    r.normalTS = normalize(float3(nx * 1.5f, ny * 1.5f, 1.0f));

    r.metallic  = 0.7f;
    r.roughness = 0.25f;
    r.ao        = lerp(0.7f, 1.0f, surface);

    return r;
}

// ---- 5: Crumble ----
ProceduralResult ProceduralCrumble(float2 worldXZ, float time)
{
    ProceduralResult r;

    float2 uv = worldXZ * 3.0f;

    // Voronoi crack pattern
    float2 vor = voronoi(uv * 1.5f);
    float crackLine = 1.0f - smoothstep(0.0f, 0.08f, vor.y);

    // FBM stone texture
    float stone = fbm(uv * 2.0f, 5);

    // Stone colors
    float3 stoneLight = float3(0.22f, 0.18f, 0.15f);
    float3 stoneDark  = float3(0.08f, 0.06f, 0.05f);
    float3 crackColor = float3(0.03f, 0.02f, 0.02f);

    float3 color = lerp(stoneDark, stoneLight, stone);
    color = lerp(color, crackColor, crackLine);

    r.albedo = color;

    // Minimal emissive — just a subtle warm glow in cracks
    float crumblePulse = 0.7f + 0.3f * sin(time * 1.0f);
    r.emissive = float3(0.06f, 0.03f, 0.01f) * crackLine * crumblePulse;

    // Normal perturbation for crack depth
    float cnx = (voronoi((uv + float2(0.02f, 0.0f)) * 1.5f).y -
                 voronoi((uv - float2(0.02f, 0.0f)) * 1.5f).y);
    float cny = (voronoi((uv + float2(0.0f, 0.02f)) * 1.5f).y -
                 voronoi((uv - float2(0.0f, 0.02f)) * 1.5f).y);
    // Add stone FBM normal
    float snx = fbm(uv * 2.0f + float2(0.1f, 0.0f), 4) - fbm(uv * 2.0f - float2(0.1f, 0.0f), 4);
    float sny = fbm(uv * 2.0f + float2(0.0f, 0.1f), 4) - fbm(uv * 2.0f - float2(0.0f, 0.1f), 4);
    r.normalTS = normalize(float3((cnx * 3.0f + snx) * 0.5f,
                                  (cny * 3.0f + sny) * 0.5f,
                                  1.0f));

    r.metallic  = 0.0f;
    r.roughness = 0.85f;
    r.ao        = lerp(0.5f, 1.0f, 1.0f - crackLine);

    return r;
}

// ============================================================================
// Dispatcher
// ============================================================================

// Returns true if procedural override was applied, false otherwise.
// worldPos: world-space position of the fragment (for spatial noise).
// time: game time for animation.
// typeId: material type ID (0 = none).
bool ApplyProceduralTile(float3 worldPos, float time, float typeId,
                         out ProceduralResult result)
{
    result = (ProceduralResult)0;
    result.normalTS = float3(0.0f, 0.0f, 1.0f);
    result.ao = 1.0f;

    int id = (int)(typeId + 0.5f);
    if (id == 0)
        return false;

    float2 xz = worldPos.xz;

    if (id == 1)
        result = ProceduralFire(xz, time);
    else if (id == 2)
        result = ProceduralIce(xz, time);
    else if (id == 3)
        result = ProceduralLightning(xz, time);
    else if (id == 4)
        result = ProceduralSpike(xz, time);
    else if (id == 5)
        result = ProceduralCrumble(xz, time);
    else
        return false;

    return true;
}

#endif // PROCEDURAL_TILES_HLSLI
