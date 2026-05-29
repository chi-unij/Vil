#include "AnimationPlayer.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

// ============================================================================
// Helpers
// ============================================================================

// Compute the TRUE global bind pose from inverse bind matrices.
// globalBind[i] = inverse(IBM[i]).
static void ComputeGlobalBindPose(const Skeleton &skel, XMMATRIX *globalBind,
                                  int count) {
  for (int i = 0; i < count; ++i) {
    XMMATRIX ibm = XMLoadFloat4x4(&skel.bones[i].inverseBindMatrix);
    XMVECTOR det;
    globalBind[i] = XMMatrixInverse(&det, ibm);
  }
}

// Compute true local bind transforms from global bind poses.
// localBind[i] = globalBind[i] * inverse(globalBind[parent]).
// This correctly accounts for intermediate non-joint nodes in VRM.
static void ComputeLocalBindPose(const Skeleton &skel,
                                 const XMMATRIX *globalBind,
                                 XMMATRIX *localBind, int count) {
  for (int i = 0; i < count; ++i) {
    int parent = skel.bones[i].parentIndex;
    if (parent < 0 || parent >= count) {
      localBind[i] = globalBind[i];
    } else {
      XMVECTOR det;
      XMMATRIX parentInv = XMMatrixInverse(&det, globalBind[parent]);
      localBind[i] = globalBind[i] * parentInv;
    }
  }
}

// Walk skeleton hierarchy: compute global transform for each bone.
static void ComputeGlobalTransforms(const Skeleton &skel,
                                    const XMMATRIX *locals,
                                    XMMATRIX *globalOut, int count) {
  for (int i = 0; i < count; ++i) {
    int parent = skel.bones[i].parentIndex;
    if (parent < 0 || parent >= count)
      globalOut[i] = locals[i];
    else
      globalOut[i] = locals[i] * globalOut[parent];
  }
}

// Find the two keyframes surrounding 'time' and return interpolation factor.
static void FindKeyframePair(const std::vector<AnimKeyframe> &keys, float time,
                             size_t &lo, size_t &hi, float &t) {
  if (keys.size() <= 1) {
    lo = hi = 0;
    t = 0.0f;
    return;
  }

  if (time <= keys.front().time) {
    lo = hi = 0;
    t = 0.0f;
    return;
  }
  if (time >= keys.back().time) {
    lo = hi = keys.size() - 1;
    t = 0.0f;
    return;
  }

  for (size_t i = 0; i < keys.size() - 1; ++i) {
    if (time >= keys[i].time && time < keys[i + 1].time) {
      lo = i;
      hi = i + 1;
      float span = keys[hi].time - keys[lo].time;
      t = (span > 1e-6f) ? (time - keys[lo].time) / span : 0.0f;
      return;
    }
  }

  lo = hi = keys.size() - 1;
  t = 0.0f;
}

// Interpolate a single track at a given time.
static XMVECTOR SampleTrack(const AnimTrack &track, float time) {
  size_t lo, hi;
  float t;
  FindKeyframePair(track.keyframes, time, lo, hi, t);

  const XMFLOAT4 &vA = track.keyframes[lo].value;
  const XMFLOAT4 &vB = track.keyframes[hi].value;

  XMVECTOR a = XMLoadFloat4(&vA);
  XMVECTOR b = XMLoadFloat4(&vB);

  if (track.path == AnimTargetPath::Rotation) {
    return XMQuaternionSlerp(a, b, t);
  } else {
    return XMVectorLerp(a, b, t);
  }
}

// ============================================================================
// Bind Pose
// ============================================================================

void ComputeBindPose(const Skeleton &skel, BonePalette &out) {
  int count = static_cast<int>(skel.bones.size());
  count = (std::min)(count, kMaxBones);
  out.boneCount = count;

  // Bind pose skin matrices are identity by definition:
  // skinMatrix = IBM * globalBind = IBM * inverse(IBM) = identity.
  for (int i = 0; i < kMaxBones; ++i)
    out.matrices[i] = XMMatrixIdentity();
}

// ============================================================================
// Procedural Idle (fallback)
// ============================================================================

void ComputeProceduralIdle(const Skeleton &skel, float time, BonePalette &out) {
  int count = static_cast<int>(skel.bones.size());
  count = (std::min)(count, kMaxBones);
  if (count == 0) {
    out.boneCount = 0;
    return;
  }
  out.boneCount = count;

  float bobY = sinf(time * 2.0f * 3.14159f * 0.5f) * 0.02f;
  XMMATRIX delta = XMMatrixTranslation(0.0f, bobY, 0.0f);

  for (int i = 0; i < kMaxBones; ++i)
    out.matrices[i] = delta;
}

// ============================================================================
// Animation Clip Evaluation
// ============================================================================

// Core: decompose local bind transforms, override with animation data,
// recompose, walk hierarchy, multiply by IBM.
static void EvaluateClipCore(const Skeleton &skel, const AnimationClip &clip,
                             float time, BonePalette &out) {
  int count = static_cast<int>(skel.bones.size());
  count = (std::min)(count, kMaxBones);
  out.boneCount = count;

  for (int i = 0; i < kMaxBones; ++i)
    out.matrices[i] = XMMatrixIdentity();

  if (count == 0 || clip.tracks.empty())
    return;

  // Step 1: Compute true bind poses from IBMs.
  XMMATRIX globalBind[kMaxBones];
  XMMATRIX localBind[kMaxBones];
  ComputeGlobalBindPose(skel, globalBind, count);
  ComputeLocalBindPose(skel, globalBind, localBind, count);

  // Step 2: Decompose local bind transforms into T, R, S.
  XMVECTOR boneT[kMaxBones], boneR[kMaxBones], boneS[kMaxBones];
  for (int i = 0; i < count; ++i) {
    XMMatrixDecompose(&boneS[i], &boneR[i], &boneT[i], localBind[i]);
  }

  // Step 3: Override with animation track values.
  for (const auto &track : clip.tracks) {
    if (track.boneIndex < 0 || track.boneIndex >= count)
      continue;
    if (track.keyframes.empty())
      continue;

    XMVECTOR val = SampleTrack(track, time);
    switch (track.path) {
    case AnimTargetPath::Translation:
      boneT[track.boneIndex] = val;
      break;
    case AnimTargetPath::Rotation:
      boneR[track.boneIndex] = val;
      break;
    case AnimTargetPath::Scale:
      boneS[track.boneIndex] = val;
      break;
    }
  }

  // Step 4: Recompose local matrices and walk hierarchy.
  XMMATRIX locals[kMaxBones];
  for (int i = 0; i < count; ++i) {
    locals[i] = XMMatrixScalingFromVector(boneS[i]) *
                XMMatrixRotationQuaternion(boneR[i]) *
                XMMatrixTranslationFromVector(boneT[i]);
  }

  XMMATRIX globals[kMaxBones];
  ComputeGlobalTransforms(skel, locals, globals, count);

  // Step 5: Final skin matrix = IBM * animGlobal.
  for (int i = 0; i < count; ++i) {
    XMMATRIX ibm = XMLoadFloat4x4(&skel.bones[i].inverseBindMatrix);
    out.matrices[i] = ibm * globals[i];
  }
}

void EvaluateAnimation(const Skeleton &skel, const AnimationClip &clip,
                       float time, BonePalette &out) {
  // Loop the animation time.
  float loopedTime = time;
  if (clip.duration > 0.0f) {
    loopedTime = fmodf(time, clip.duration);
    if (loopedTime < 0.0f)
      loopedTime += clip.duration;
  }

  EvaluateClipCore(skel, clip, loopedTime, out);
}

// ============================================================================
// Blended Animation (crossfade between two clips)
// ============================================================================

void EvaluateAnimationBlend(const Skeleton &skel,
                            const AnimationClip &clipA, float timeA,
                            const AnimationClip &clipB, float timeB,
                            float blendFactor, BonePalette &out) {
  int count = static_cast<int>(skel.bones.size());
  count = (std::min)(count, kMaxBones);
  out.boneCount = count;

  for (int i = 0; i < kMaxBones; ++i)
    out.matrices[i] = XMMatrixIdentity();

  if (count == 0)
    return;

  // Evaluate both clips separately, then blend the final matrices.
  BonePalette palA, palB;
  EvaluateAnimation(skel, clipA, timeA, palA);
  EvaluateAnimation(skel, clipB, timeB, palB);

  float bf = (std::max)(0.0f, (std::min)(1.0f, blendFactor));

  // Blend per-bone: decompose each final matrix, slerp/lerp, recompose.
  // Simpler approach: linear matrix interpolation (works well for small blend windows).
  for (int i = 0; i < kMaxBones; ++i) {
    // Weighted average of the two skin matrices.
    out.matrices[i] = palA.matrices[i] * (1.0f - bf) + palB.matrices[i] * bf;
  }
}
