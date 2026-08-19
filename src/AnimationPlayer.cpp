#include "AnimationPlayer.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

// ============================================================================
// Helpers
// ============================================================================

static bool IsFiniteVector(FXMVECTOR value) {
  XMFLOAT4 components;
  XMStoreFloat4(&components, value);
  return std::isfinite(components.x) && std::isfinite(components.y) &&
         std::isfinite(components.z) && std::isfinite(components.w);
}

static bool IsFiniteMatrix(const XMMATRIX &matrix) {
  for (int row = 0; row < 4; ++row) {
    if (!IsFiniteVector(matrix.r[row]))
      return false;
  }
  return true;
}

static XMMATRIX FiniteMatrixOrIdentity(const XMMATRIX &matrix) {
  return IsFiniteMatrix(matrix) ? matrix : XMMatrixIdentity();
}

static bool TryInvertFiniteMatrix(const XMMATRIX &matrix,
                                  XMMATRIX &inverseOut) {
  inverseOut = XMMatrixIdentity();
  if (!IsFiniteMatrix(matrix))
    return false;

  XMVECTOR determinant = XMVectorZero();
  const XMMATRIX inverse = XMMatrixInverse(&determinant, matrix);
  const float determinantValue = XMVectorGetX(determinant);
  if (!std::isfinite(determinantValue) || determinantValue == 0.0f ||
      !IsFiniteMatrix(inverse)) {
    return false;
  }

  inverseOut = inverse;
  return true;
}

static bool IsUsableQuaternion(FXMVECTOR quaternion) {
  if (!IsFiniteVector(quaternion))
    return false;

  const float lengthSquared = XMVectorGetX(XMVector4LengthSq(quaternion));
  return std::isfinite(lengthSquared) && lengthSquared > 1.0e-12f;
}

static bool TryNormalizeQuaternion(FXMVECTOR quaternion,
                                   XMVECTOR &normalizedOut) {
  normalizedOut = XMQuaternionIdentity();
  if (!IsUsableQuaternion(quaternion))
    return false;

  const XMVECTOR normalized = XMQuaternionNormalize(quaternion);
  if (!IsFiniteVector(normalized))
    return false;

  normalizedOut = normalized;
  return true;
}

static XMVECTOR ExtractFiniteTranslation(const XMMATRIX &matrix) {
  const float x = XMVectorGetX(matrix.r[3]);
  const float y = XMVectorGetY(matrix.r[3]);
  const float z = XMVectorGetZ(matrix.r[3]);
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
    return XMVectorZero();
  return XMVectorSet(x, y, z, 0.0f);
}

// inverse bind matrix から global bind pose を復元する。
// 壊れた行列は identity に固定し、以降の palette を有限値に保つ。
static void ComputeGlobalBindPose(const Skeleton &skel, XMMATRIX *globalBind,
                                  int count) {
  for (int i = 0; i < count; ++i) {
    const XMMATRIX ibm =
        XMLoadFloat4x4(&skel.bones[i].inverseBindMatrix);
    if (!TryInvertFiniteMatrix(ibm, globalBind[i]))
      globalBind[i] = XMMatrixIdentity();
  }
}

// global bind pose から各 bone の local bind transform を復元する。
// 中間の non-joint node を含む VRM でも親子差分を正しく維持する。
static void ComputeLocalBindPose(const Skeleton &skel,
                                 const XMMATRIX *globalBind,
                                 XMMATRIX *localBind, int count) {
  for (int i = 0; i < count; ++i) {
    int parent = skel.bones[i].parentIndex;
    if (parent < 0 || parent >= count) {
      localBind[i] = globalBind[i];
    } else {
      XMMATRIX parentInverse = XMMatrixIdentity();
      if (!TryInvertFiniteMatrix(globalBind[parent], parentInverse)) {
        localBind[i] = globalBind[i];
        continue;
      }

      localBind[i] =
          FiniteMatrixOrIdentity(globalBind[i] * parentInverse);
    }
  }
}

// 親が後方 index にある skeleton も DFS で先に解決する。
// cycle を検出した bone は local transform を root として固定する。
static void ResolveGlobalTransform(const Skeleton &skel,
                                   const XMMATRIX *locals,
                                   XMMATRIX *globalOut,
                                   unsigned char *visitState, int boneIndex,
                                   int count) {
  if (visitState[boneIndex] == 2)
    return;

  const XMMATRIX local = FiniteMatrixOrIdentity(locals[boneIndex]);
  if (visitState[boneIndex] == 1) {
    globalOut[boneIndex] = local;
    visitState[boneIndex] = 2;
    return;
  }

  visitState[boneIndex] = 1;
  const int parent = skel.bones[boneIndex].parentIndex;
  if (parent < 0 || parent >= count) {
    globalOut[boneIndex] = local;
  } else {
    ResolveGlobalTransform(skel, locals, globalOut, visitState, parent, count);
    if (visitState[boneIndex] == 2)
      return;

    const XMMATRIX combined = local * globalOut[parent];
    globalOut[boneIndex] =
        IsFiniteMatrix(combined) ? combined : local;
  }

  visitState[boneIndex] = 2;
}

static void ComputeGlobalTransforms(const Skeleton &skel,
                                    const XMMATRIX *locals, XMMATRIX *globalOut,
                                    int count) {
  unsigned char visitState[kMaxBones] = {};
  for (int i = 0; i < count; ++i)
    ResolveGlobalTransform(skel, locals, globalOut, visitState, i, count);
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
    return XMQuaternionNormalize(XMQuaternionSlerp(a, b, t));
  } else {
    return XMVectorLerp(a, b, t);
  }
}

static float LoopClipTime(const AnimationClip &clip, float time) {
  if (clip.duration <= 0.0f)
    return time;

  float loopedTime = fmodf(time, clip.duration);
  if (loopedTime < 0.0f)
    loopedTime += clip.duration;
  return loopedTime;
}

static void InitializePalette(int count, BonePalette &out) {
  out.boneCount = count;
  for (int i = 0; i < kMaxBones; ++i)
    out.matrices[i] = XMMatrixIdentity();
}

// 各 bone を正しい bind-local transform で初期化してから、clip の local TRS を
// sample する。省略された channel は bind pose の成分をそのまま維持する。
static void SampleClipLocalPose(const Skeleton &skel, const AnimationClip &clip,
                                float time, XMVECTOR *boneT, XMVECTOR *boneR,
                                XMVECTOR *boneS, int count) {
  XMMATRIX globalBind[kMaxBones];
  XMMATRIX localBind[kMaxBones];
  ComputeGlobalBindPose(skel, globalBind, count);
  ComputeLocalBindPose(skel, globalBind, localBind, count);

  for (int i = 0; i < count; ++i) {
    // Decompose 失敗時にも未初期化 vector を残さない。
    boneT[i] = ExtractFiniteTranslation(localBind[i]);
    boneR[i] = XMQuaternionIdentity();
    boneS[i] = XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);

    XMVECTOR decomposedT = boneT[i];
    XMVECTOR decomposedR = boneR[i];
    XMVECTOR decomposedS = boneS[i];
    XMVECTOR normalizedR = XMQuaternionIdentity();
    if (XMMatrixDecompose(&decomposedS, &decomposedR, &decomposedT,
                          localBind[i]) &&
        IsFiniteVector(decomposedT) && IsFiniteVector(decomposedS) &&
        TryNormalizeQuaternion(decomposedR, normalizedR)) {
      boneT[i] = decomposedT;
      boneR[i] = normalizedR;
      boneS[i] = decomposedS;
    }
  }

  const float loopedTime = LoopClipTime(clip, time);
  for (const auto &track : clip.tracks) {
    if (track.boneIndex < 0 || track.boneIndex >= count ||
        track.keyframes.empty()) {
      continue;
    }

    const XMVECTOR value = SampleTrack(track, loopedTime);
    if (!IsFiniteVector(value))
      continue;

    switch (track.path) {
    case AnimTargetPath::Translation:
      boneT[track.boneIndex] = value;
      break;
    case AnimTargetPath::Rotation: {
      XMVECTOR normalized = XMQuaternionIdentity();
      if (TryNormalizeQuaternion(value, normalized))
        boneR[track.boneIndex] = normalized;
      break;
    }
    case AnimTargetPath::Scale:
      boneS[track.boneIndex] = value;
      break;
    }
  }
}

static void BuildPaletteFromLocalPose(const Skeleton &skel,
                                      const XMVECTOR *boneT,
                                      const XMVECTOR *boneR,
                                      const XMVECTOR *boneS, int count,
                                      BonePalette &out) {
  XMMATRIX locals[kMaxBones];
  for (int i = 0; i < count; ++i) {
    const XMVECTOR translation =
        IsFiniteVector(boneT[i]) ? boneT[i] : XMVectorZero();
    const XMVECTOR scale = IsFiniteVector(boneS[i])
                               ? boneS[i]
                               : XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);
    const XMVECTOR rotation = IsUsableQuaternion(boneR[i])
                                  ? boneR[i]
                                  : XMQuaternionIdentity();

    const XMMATRIX local = XMMatrixScalingFromVector(scale) *
                           XMMatrixRotationQuaternion(rotation) *
                           XMMatrixTranslationFromVector(translation);
    locals[i] = FiniteMatrixOrIdentity(local);
  }

  XMMATRIX globals[kMaxBones];
  ComputeGlobalTransforms(skel, locals, globals, count);

  for (int i = 0; i < count; ++i) {
    const XMMATRIX ibm = XMLoadFloat4x4(&skel.bones[i].inverseBindMatrix);
    XMMATRIX unusedInverse = XMMatrixIdentity();
    if (!TryInvertFiniteMatrix(ibm, unusedInverse)) {
      out.matrices[i] = XMMatrixIdentity();
      continue;
    }

    out.matrices[i] = FiniteMatrixOrIdentity(ibm * globals[i]);
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

// bind pose 基準の local TRS を sample し、hierarchy と inverse bind matrix を
// 適用して最終 bone palette を構築する。
static void EvaluateClipCore(const Skeleton &skel, const AnimationClip &clip,
                             float time, BonePalette &out) {
  int count = static_cast<int>(skel.bones.size());
  count = (std::min)(count, kMaxBones);
  InitializePalette(count, out);

  if (count == 0)
    return;

  XMVECTOR boneT[kMaxBones], boneR[kMaxBones], boneS[kMaxBones];
  SampleClipLocalPose(skel, clip, time, boneT, boneR, boneS, count);
  BuildPaletteFromLocalPose(skel, boneT, boneR, boneS, count, out);
}

void EvaluateAnimation(const Skeleton &skel, const AnimationClip &clip,
                       float time, BonePalette &out) {
  EvaluateClipCore(skel, clip, time, out);
}

// ============================================================================
// Blended Animation (crossfade between two clips)
// ============================================================================

void EvaluateAnimationBlend(const Skeleton &skel, const AnimationClip &clipA,
                            float timeA, const AnimationClip &clipB,
                            float timeB, float blendFactor, BonePalette &out) {
  int count = static_cast<int>(skel.bones.size());
  count = (std::min)(count, kMaxBones);
  InitializePalette(count, out);

  if (count == 0)
    return;

  XMVECTOR boneTA[kMaxBones], boneRA[kMaxBones], boneSA[kMaxBones];
  XMVECTOR boneTB[kMaxBones], boneRB[kMaxBones], boneSB[kMaxBones];
  SampleClipLocalPose(skel, clipA, timeA, boneTA, boneRA, boneSA, count);
  SampleClipLocalPose(skel, clipB, timeB, boneTB, boneRB, boneSB, count);

  const float bf = (std::max)(0.0f, (std::min)(1.0f, blendFactor));
  XMVECTOR blendedT[kMaxBones], blendedR[kMaxBones], blendedS[kMaxBones];
  for (int i = 0; i < count; ++i) {
    blendedT[i] = XMVectorLerp(boneTA[i], boneTB[i], bf);
    blendedR[i] =
        XMQuaternionNormalize(XMQuaternionSlerp(boneRA[i], boneRB[i], bf));
    blendedS[i] = XMVectorLerp(boneSA[i], boneSB[i], bf);
  }

  BuildPaletteFromLocalPose(skel, blendedT, blendedR, blendedS, count, out);
}
