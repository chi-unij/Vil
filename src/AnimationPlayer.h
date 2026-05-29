#pragma once

#include "GltfLoader.h"
#include <DirectXMath.h>

static constexpr int kMaxBones = 256;

struct BonePalette {
  DirectX::XMMATRIX matrices[kMaxBones];
  int boneCount = 0;
};

// Compute the bind pose bone palette from a skeleton using hierarchy walk.
// Result: each matrix = globalTransform[i] * inverseBindMatrix[i].
void ComputeBindPose(const Skeleton &skel, BonePalette &out);

// Compute a procedural idle animation (gentle breathing bob).
// Fallback when no animation clips are available.
void ComputeProceduralIdle(const Skeleton &skel, float time, BonePalette &out);

// Evaluate an animation clip at a given time and produce a bone palette.
// Loops the animation automatically. Uses skeleton hierarchy for global transforms.
void EvaluateAnimation(const Skeleton &skel, const AnimationClip &clip,
                       float time, BonePalette &out);

// Evaluate two clips and blend between them (crossfade).
// blendFactor: 0.0 = fully clipA, 1.0 = fully clipB.
void EvaluateAnimationBlend(const Skeleton &skel,
                            const AnimationClip &clipA, float timeA,
                            const AnimationClip &clipB, float timeB,
                            float blendFactor, BonePalette &out);
