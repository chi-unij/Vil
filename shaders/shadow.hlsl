cbuffer ShadowCB : register(b0)
{
    float4x4 gLightViewProj;
};

// Per-instance world matrices (Phase 12.5 — Instanced Rendering).
StructuredBuffer<float4x4> gInstanceWorlds : register(t0);

// Bone palette for skeletal animation (Phase 2C).
StructuredBuffer<float4x4> gBones : register(t1);

struct VSIn
{
    float3 pos      : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float4 tangent  : TANGENT;
    uint4  boneIdx  : BLENDINDICES;
    float4 boneWgt  : BLENDWEIGHT;
};

struct VSOut
{
    float4 pos : SV_POSITION;
};

VSOut VSMain(VSIn v, uint instId : SV_InstanceID)
{
    // Skeletal skinning.
    float3 skinnedPos = v.pos;
    float wSum = v.boneWgt.x + v.boneWgt.y + v.boneWgt.z + v.boneWgt.w;
    if (wSum > 0.001f)
    {
        float4 nw = v.boneWgt / wSum; // normalize weights
        float4x4 skin = nw.x * gBones[v.boneIdx.x]
                       + nw.y * gBones[v.boneIdx.y]
                       + nw.z * gBones[v.boneIdx.z]
                       + nw.w * gBones[v.boneIdx.w];
        skinnedPos = mul(float4(v.pos, 1.0f), skin).xyz;
    }

    float4x4 world = gInstanceWorlds[instId];
    float4 posW = mul(float4(skinnedPos, 1.0f), world);
    VSOut o;
    o.pos = mul(posW, gLightViewProj);
    return o;
}

// Depth-only pass: no pixel shader needed.
