#include "GltfLoader.h"
#include "AnimationPlayer.h" // kMaxBones
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

// stb_image is compiled inside tinygltf; we only need the header for stbi_load.
#include "stb_image.h"

// IMPORTANT:
// We link against the `tinygltf` library via CMake. Do NOT compile the
// implementation in this translation unit (no TINYGLTF_IMPLEMENTATION / STB
// implementation macros), otherwise we risk ODR issues / duplicate
// implementations and hard-to-debug crashes.
#include "tiny_gltf.h"

using namespace DirectX;

static bool GetAccessorFloatData(const tinygltf::Model &model,
                                 const tinygltf::Accessor &acc,
                                 int expectedType, // e.g. TINYGLTF_TYPE_VEC3
                                 const float *&outData, size_t &outStrideBytes) {
  outData = nullptr;
  outStrideBytes = 0;

  if (acc.bufferView < 0 ||
      acc.bufferView >= static_cast<int>(model.bufferViews.size()))
    return false;
  if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
    return false;
  if (acc.type != expectedType)
    return false;
  if (acc.count == 0)
    return false;

  const auto &bv = model.bufferViews[acc.bufferView];
  if (bv.buffer < 0 || bv.buffer >= static_cast<int>(model.buffers.size()))
    return false;
  const auto &buf = model.buffers[bv.buffer];

  // Element sizes (float).
  size_t elemBytes = 0;
  if (expectedType == TINYGLTF_TYPE_VEC2)
    elemBytes = sizeof(float) * 2;
  else if (expectedType == TINYGLTF_TYPE_VEC3)
    elemBytes = sizeof(float) * 3;
  else if (expectedType == TINYGLTF_TYPE_VEC4)
    elemBytes = sizeof(float) * 4;
  else if (expectedType == TINYGLTF_TYPE_MAT4)
    elemBytes = sizeof(float) * 16;
  else
    return false;

  const size_t stride =
      bv.byteStride ? static_cast<size_t>(bv.byteStride) : elemBytes;
  if (stride < elemBytes)
    return false;

  const size_t start =
      static_cast<size_t>(bv.byteOffset) + static_cast<size_t>(acc.byteOffset);

  // Bounds check last element.
  const size_t last =
      start + (static_cast<size_t>(acc.count) - 1u) * stride + elemBytes;
  if (start >= buf.data.size() || last > buf.data.size())
    return false;

  outData = reinterpret_cast<const float *>(&buf.data[start]);
  outStrideBytes = stride;
  return true;
}

static bool GetAccessorIndexData(const tinygltf::Model &model,
                                 const tinygltf::Accessor &acc,
                                 const uint8_t *&outBytes,
                                 size_t &outStrideBytes,
                                 size_t &outIndexElemBytes) {
  outBytes = nullptr;
  outStrideBytes = 0;
  outIndexElemBytes = 0;

  if (acc.bufferView < 0 ||
      acc.bufferView >= static_cast<int>(model.bufferViews.size()))
    return false;
  if (acc.type != TINYGLTF_TYPE_SCALAR)
    return false;
  if (acc.count == 0)
    return false;

  if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
    outIndexElemBytes = 1;
  else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
    outIndexElemBytes = 2;
  else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
    outIndexElemBytes = 4;
  else
    return false;

  const auto &bv = model.bufferViews[acc.bufferView];
  if (bv.buffer < 0 || bv.buffer >= static_cast<int>(model.buffers.size()))
    return false;
  const auto &buf = model.buffers[bv.buffer];

  const size_t stride =
      bv.byteStride ? static_cast<size_t>(bv.byteStride) : outIndexElemBytes;
  if (stride < outIndexElemBytes)
    return false;

  const size_t start =
      static_cast<size_t>(bv.byteOffset) + static_cast<size_t>(acc.byteOffset);
  const size_t last =
      start + (static_cast<size_t>(acc.count) - 1u) * stride + outIndexElemBytes;
  if (start >= buf.data.size() || last > buf.data.size())
    return false;

  outBytes = &buf.data[start];
  outStrideBytes = stride;
  return true;
}

// Read raw accessor data for joint indices (JOINTS_0).
// glTF stores these as UNSIGNED_BYTE or UNSIGNED_SHORT, VEC4.
static bool GetAccessorJointData(const tinygltf::Model &model,
                                 const tinygltf::Accessor &acc,
                                 const uint8_t *&outBytes,
                                 size_t &outStrideBytes,
                                 int &outComponentType) {
  outBytes = nullptr;
  outStrideBytes = 0;
  outComponentType = 0;

  if (acc.bufferView < 0 ||
      acc.bufferView >= static_cast<int>(model.bufferViews.size()))
    return false;
  if (acc.type != TINYGLTF_TYPE_VEC4)
    return false;
  if (acc.count == 0)
    return false;

  // JOINTS must be UNSIGNED_BYTE or UNSIGNED_SHORT per glTF spec.
  size_t elemBytes = 0;
  if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
    elemBytes = 4;
  else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
    elemBytes = 8;
  else
    return false;

  outComponentType = acc.componentType;

  const auto &bv = model.bufferViews[acc.bufferView];
  if (bv.buffer < 0 || bv.buffer >= static_cast<int>(model.buffers.size()))
    return false;
  const auto &buf = model.buffers[bv.buffer];

  const size_t stride =
      bv.byteStride ? static_cast<size_t>(bv.byteStride) : elemBytes;
  if (stride < elemBytes)
    return false;

  const size_t start =
      static_cast<size_t>(bv.byteOffset) + static_cast<size_t>(acc.byteOffset);
  const size_t last =
      start + (static_cast<size_t>(acc.count) - 1u) * stride + elemBytes;
  if (start >= buf.data.size() || last > buf.data.size())
    return false;

  outBytes = &buf.data[start];
  outStrideBytes = stride;
  return true;
}

static LoadedImage ConvertToRgba(const tinygltf::Image &img) {
  LoadedImage out;
  out.width = img.width;
  out.height = img.height;
  out.channels = 4;

  if (img.width <= 0 || img.height <= 0 || img.image.empty())
    return out;

  const int w = img.width;
  const int h = img.height;
  const int c = img.component;

  out.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);

  const unsigned char *src = img.image.data();
  unsigned char *dst = out.pixels.data();

  if (c == 4) {
    memcpy(dst, src, out.pixels.size());
    return out;
  }

  for (int i = 0; i < w * h; ++i) {
    unsigned char r = 0, g = 0, b = 0, a = 255;
    if (c == 3) {
      r = src[i * 3 + 0];
      g = src[i * 3 + 1];
      b = src[i * 3 + 2];
    } else if (c == 2) {
      r = g = b = src[i * 2 + 0];
      a = src[i * 2 + 1];
    } else if (c == 1) {
      r = g = b = src[i];
    }
    dst[i * 4 + 0] = r;
    dst[i * 4 + 1] = g;
    dst[i * 4 + 2] = b;
    dst[i * 4 + 3] = a;
  }

  return out;
}

// Compute tangents from UV-based per-triangle method (Lengyel's method).
// Fallback when glTF does not provide TANGENT attribute.
// NOTE: For production use, MikkTSpace is the standard. This is simpler and
// adequate for learning.
static void ComputeTangents(LoadedMesh &mesh) {
  const size_t vertCount = mesh.vertices.size();
  const size_t idxCount = mesh.indices.size();

  // Accumulate tangent per vertex.
  std::vector<float> tanAccum(vertCount * 3, 0.0f);

  for (size_t i = 0; i + 2 < idxCount; i += 3) {
    const uint32_t i0 = mesh.indices[i + 0];
    const uint32_t i1 = mesh.indices[i + 1];
    const uint32_t i2 = mesh.indices[i + 2];

    const auto &v0 = mesh.vertices[i0];
    const auto &v1 = mesh.vertices[i1];
    const auto &v2 = mesh.vertices[i2];

    float e1x = v1.pos[0] - v0.pos[0];
    float e1y = v1.pos[1] - v0.pos[1];
    float e1z = v1.pos[2] - v0.pos[2];

    float e2x = v2.pos[0] - v0.pos[0];
    float e2y = v2.pos[1] - v0.pos[1];
    float e2z = v2.pos[2] - v0.pos[2];

    float du1 = v1.uv[0] - v0.uv[0];
    float dv1 = v1.uv[1] - v0.uv[1];
    float du2 = v2.uv[0] - v0.uv[0];
    float dv2 = v2.uv[1] - v0.uv[1];

    float det = du1 * dv2 - du2 * dv1;
    if (std::abs(det) < 1e-8f)
      det = 1.0f; // degenerate UV, use identity
    float invDet = 1.0f / det;

    float tx = (dv2 * e1x - dv1 * e2x) * invDet;
    float ty = (dv2 * e1y - dv1 * e2y) * invDet;
    float tz = (dv2 * e1z - dv1 * e2z) * invDet;

    // Accumulate for all 3 vertices of the triangle.
    for (uint32_t idx : {i0, i1, i2}) {
      tanAccum[idx * 3 + 0] += tx;
      tanAccum[idx * 3 + 1] += ty;
      tanAccum[idx * 3 + 2] += tz;
    }
  }

  // Normalize accumulated tangents and store. w=1.0 (right-handed assumption).
  for (size_t v = 0; v < vertCount; ++v) {
    float tx = tanAccum[v * 3 + 0];
    float ty = tanAccum[v * 3 + 1];
    float tz = tanAccum[v * 3 + 2];
    float len = std::sqrt(tx * tx + ty * ty + tz * tz);
    if (len > 1e-8f) {
      mesh.vertices[v].tangent[0] = tx / len;
      mesh.vertices[v].tangent[1] = ty / len;
      mesh.vertices[v].tangent[2] = tz / len;
    } else {
      // Degenerate: fall back to +X.
      mesh.vertices[v].tangent[0] = 1.0f;
      mesh.vertices[v].tangent[1] = 0.0f;
      mesh.vertices[v].tangent[2] = 0.0f;
    }
    mesh.vertices[v].tangent[3] = 1.0f;
  }
}

// Helper: extract a texture image from a glTF material texture reference.
static LoadedImage ExtractTextureImage(const tinygltf::Model &model,
                                       int textureIndex) {
  LoadedImage result{};
  if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
    return result;
  const int srcImg = model.textures[textureIndex].source;
  if (srcImg < 0 || srcImg >= static_cast<int>(model.images.size()))
    return result;
  return ConvertToRgba(model.images[srcImg]);
}

// ============================================================================
// Skeleton extraction from glTF skin
// ============================================================================
void GltfLoader::ExtractSkeleton(const tinygltf::Model &model) {
  if (model.skins.empty())
    return;

  const auto &skin = model.skins[0]; // use first skin
  const size_t jointCount = skin.joints.size();
  if (jointCount == 0)
    return;

  m_mesh.skeleton.bones.resize(jointCount);
  m_mesh.skeleton.jointNodeIndices = skin.joints;

  // Read inverse bind matrices.
  std::vector<XMFLOAT4X4> ibms(jointCount);
  if (skin.inverseBindMatrices >= 0 &&
      skin.inverseBindMatrices < static_cast<int>(model.accessors.size())) {
    const auto &acc = model.accessors[skin.inverseBindMatrices];
    const float *ibmData = nullptr;
    size_t ibmStride = 0;
    if (GetAccessorFloatData(model, acc, TINYGLTF_TYPE_MAT4, ibmData, ibmStride)) {
      for (size_t i = 0; i < jointCount && i < acc.count; ++i) {
        const float *m = reinterpret_cast<const float *>(
            reinterpret_cast<const uint8_t *>(ibmData) + i * ibmStride);
        // glTF stores matrices in column-major order. Reading column-major
        // data directly into row-major XMFLOAT4X4 naturally gives the
        // transpose, which IS the row-vector equivalent for DirectXMath.
        ibms[i] = XMFLOAT4X4(
            m[0],  m[1],  m[2],  m[3],
            m[4],  m[5],  m[6],  m[7],
            m[8],  m[9],  m[10], m[11],
            m[12], m[13], m[14], m[15]);
      }
    }
  } else {
    // No IBM accessor: use identity.
    for (size_t i = 0; i < jointCount; ++i)
      XMStoreFloat4x4(&ibms[i], XMMatrixIdentity());
  }

  // Build a map: node index -> bone index (for parent lookup).
  std::unordered_map<int, int> nodeToJoint;
  for (size_t i = 0; i < jointCount; ++i)
    nodeToJoint[skin.joints[i]] = static_cast<int>(i);

  // Populate bones.
  for (size_t i = 0; i < jointCount; ++i) {
    const int nodeIdx = skin.joints[i];
    const auto &node = model.nodes[nodeIdx];

    auto &bone = m_mesh.skeleton.bones[i];
    bone.name = node.name;
    bone.inverseBindMatrix = ibms[i];

    // Compute local transform from TRS or matrix.
    XMMATRIX local = XMMatrixIdentity();
    if (!node.matrix.empty() && node.matrix.size() == 16) {
      // Column-major in glTF -> read directly into row-major XMFLOAT4X4.
      const auto &gm = node.matrix;
      XMFLOAT4X4 mat(
          (float)gm[0],  (float)gm[1],  (float)gm[2],  (float)gm[3],
          (float)gm[4],  (float)gm[5],  (float)gm[6],  (float)gm[7],
          (float)gm[8],  (float)gm[9],  (float)gm[10], (float)gm[11],
          (float)gm[12], (float)gm[13], (float)gm[14], (float)gm[15]);
      local = XMLoadFloat4x4(&mat);
    } else {
      XMVECTOR T = XMVectorSet(0, 0, 0, 0);
      XMVECTOR R = XMQuaternionIdentity();
      XMVECTOR S = XMVectorSet(1, 1, 1, 0);

      if (node.translation.size() == 3)
        T = XMVectorSet((float)node.translation[0], (float)node.translation[1],
                        (float)node.translation[2], 0.0f);
      if (node.rotation.size() == 4)
        R = XMVectorSet((float)node.rotation[0], (float)node.rotation[1],
                        (float)node.rotation[2], (float)node.rotation[3]);
      if (node.scale.size() == 3)
        S = XMVectorSet((float)node.scale[0], (float)node.scale[1],
                        (float)node.scale[2], 0.0f);

      local = XMMatrixScalingFromVector(S) *
              XMMatrixRotationQuaternion(R) *
              XMMatrixTranslationFromVector(T);
    }
    XMStoreFloat4x4(&bone.localTransform, local);

    // Find parent: walk UP the node tree through intermediate non-joint nodes
    // until we find an ancestor that IS a joint. This handles VRoid models
    // that have intermediate helper/collider nodes between joint bones.
    bone.parentIndex = -1;
    {
      // Build node→parent map (only once, but cheap enough per-bone).
      // Walk up from nodeIdx through parents until we find a joint.
      // First: find the immediate parent of nodeIdx in the node tree.
      int currentNode = nodeIdx;
      bool found = false;
      // Walk up through ancestors (max depth safety = 64).
      for (int depth = 0; depth < 64 && !found; ++depth) {
        // Find which node has currentNode as a child.
        int parentOfCurrent = -1;
        for (int n = 0; n < static_cast<int>(model.nodes.size()); ++n) {
          for (int childIdx : model.nodes[n].children) {
            if (childIdx == currentNode) {
              parentOfCurrent = n;
              goto foundNodeParent;
            }
          }
        }
        foundNodeParent:;
        if (parentOfCurrent < 0)
          break; // reached root of scene, no parent

        // Check if this parent node is a joint.
        auto jit = nodeToJoint.find(parentOfCurrent);
        if (jit != nodeToJoint.end()) {
          bone.parentIndex = jit->second;
          found = true;
        } else {
          // Not a joint — continue walking up.
          currentNode = parentOfCurrent;
        }
      }
    }
  }

  m_mesh.hasSkeleton = true;

  // Log parent hierarchy for debugging.
  int rootCount = 0;
  for (size_t i = 0; i < jointCount; ++i) {
    if (m_mesh.skeleton.bones[i].parentIndex < 0)
      rootCount++;
  }
  std::cout << "Skeleton: " << jointCount << " bones extracted, "
            << rootCount << " root bone(s).\n";
}

// ============================================================================
// Animation clip extraction from glTF
// ============================================================================
void GltfLoader::ExtractAnimations(const tinygltf::Model &model) {
  if (model.animations.empty() || !m_mesh.hasSkeleton)
    return;

  // Map: glTF node index -> bone index.
  std::unordered_map<int, int> nodeToJoint;
  for (size_t i = 0; i < m_mesh.skeleton.jointNodeIndices.size(); ++i)
    nodeToJoint[m_mesh.skeleton.jointNodeIndices[i]] = static_cast<int>(i);

  for (const auto &anim : model.animations) {
    AnimationClip clip;
    clip.name = anim.name.empty() ? "Anim_" + std::to_string(m_mesh.animations.size()) : anim.name;
    clip.duration = 0.0f;

    for (const auto &channel : anim.channels) {
      if (channel.target_node < 0)
        continue;
      auto it = nodeToJoint.find(channel.target_node);
      if (it == nodeToJoint.end())
        continue; // channel targets non-skeleton node

      AnimTrack track;
      track.boneIndex = it->second;

      if (channel.target_path == "translation")
        track.path = AnimTargetPath::Translation;
      else if (channel.target_path == "rotation")
        track.path = AnimTargetPath::Rotation;
      else if (channel.target_path == "scale")
        track.path = AnimTargetPath::Scale;
      else
        continue;

      // Read sampler input (timestamps) and output (values).
      const auto &sampler = anim.samplers[channel.sampler];

      // Input: timestamps (scalar float).
      const auto &inputAcc = model.accessors[sampler.input];
      const float *timeData = nullptr;
      size_t timeStride = 0;
      // Timestamps are stored as scalar floats, not VEC types.
      // Read raw from buffer.
      if (inputAcc.bufferView < 0)
        continue;
      const auto &timeBv = model.bufferViews[inputAcc.bufferView];
      const auto &timeBuf = model.buffers[timeBv.buffer];
      const size_t timeStart = timeBv.byteOffset + inputAcc.byteOffset;
      timeStride = timeBv.byteStride ? timeBv.byteStride : sizeof(float);
      timeData = reinterpret_cast<const float *>(&timeBuf.data[timeStart]);

      // Output: values (VEC3 for T/S, VEC4 for R).
      const auto &outputAcc = model.accessors[sampler.output];
      int expectedType = (track.path == AnimTargetPath::Rotation)
                             ? TINYGLTF_TYPE_VEC4
                             : TINYGLTF_TYPE_VEC3;
      const float *valData = nullptr;
      size_t valStride = 0;
      if (!GetAccessorFloatData(model, outputAcc, expectedType, valData, valStride))
        continue;

      size_t keyCount = std::min(inputAcc.count, outputAcc.count);
      track.keyframes.resize(keyCount);

      for (size_t k = 0; k < keyCount; ++k) {
        const float *tPtr = reinterpret_cast<const float *>(
            reinterpret_cast<const uint8_t *>(timeData) + k * timeStride);
        const float *vPtr = reinterpret_cast<const float *>(
            reinterpret_cast<const uint8_t *>(valData) + k * valStride);

        track.keyframes[k].time = *tPtr;
        if (track.path == AnimTargetPath::Rotation) {
          track.keyframes[k].value = {vPtr[0], vPtr[1], vPtr[2], vPtr[3]};
        } else {
          track.keyframes[k].value = {vPtr[0], vPtr[1], vPtr[2], 0.0f};
        }

        if (*tPtr > clip.duration)
          clip.duration = *tPtr;
      }

      clip.tracks.push_back(std::move(track));
    }

    if (!clip.tracks.empty()) {
      std::cout << "Animation \"" << clip.name << "\": " << clip.tracks.size()
                << " tracks, " << clip.duration << "s\n";
      m_mesh.animations.push_back(std::move(clip));
    }
  }

  std::cout << "Extracted " << m_mesh.animations.size() << " animation clip(s).\n";
}

bool GltfLoader::LoadModel(const std::string &path) {
  tinygltf::Model model;
  tinygltf::TinyGLTF loader;
  std::string err;
  std::string warn;

  // Detect binary format by extension (.glb, .vrm).
  bool isBinary = false;
  {
    std::string ext;
    auto dot = path.rfind('.');
    if (dot != std::string::npos) {
      ext = path.substr(dot);
      for (auto &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    isBinary = (ext == ".glb" || ext == ".vrm");
  }

  bool ret = isBinary
    ? loader.LoadBinaryFromFile(&model, &err, &warn, path)
    : loader.LoadASCIIFromFile(&model, &err, &warn, path);

  if (!warn.empty()) {
    std::cout << "WARN: " << warn << std::endl;
  }

  if (!err.empty()) {
    std::cerr << "ERR: " << err << std::endl;
  }

  if (!ret) {
    std::cerr << "Failed to parse glTF: " << path << std::endl;
    return false;
  }

  // Extract all meshes and primitives, merging into a single LoadedMesh.
  if (model.meshes.empty())
    return false;

  m_mesh.vertices.clear();
  m_mesh.indices.clear();
  m_mesh.skeleton = {};
  m_mesh.animations.clear();
  m_mesh.hasSkeleton = false;
  bool anyTangentsMissing = false;
  int firstMaterialIdx = -1; // track first material for texture extraction

  // Check if this model has skin data (for JOINTS_0 / WEIGHTS_0 parsing).
  bool hasSkin = !model.skins.empty();

  for (const auto &mesh : model.meshes) {
    for (const auto &prim : mesh.primitives) {
      // POSITION (required)
      if (prim.attributes.find("POSITION") == prim.attributes.end())
        continue;
      const int posIdx = prim.attributes.at("POSITION");
      if (posIdx < 0 || posIdx >= static_cast<int>(model.accessors.size()))
        continue;
      const auto &posAcc = model.accessors[posIdx];
      const float *posData = nullptr;
      size_t posStride = 0;
      if (!GetAccessorFloatData(model, posAcc, TINYGLTF_TYPE_VEC3, posData, posStride))
        continue;

      // NORMAL
      const float *normData = nullptr;
      size_t normStride = 0;
      if (prim.attributes.find("NORMAL") != prim.attributes.end()) {
        const int ni = prim.attributes.at("NORMAL");
        if (ni >= 0 && ni < static_cast<int>(model.accessors.size()))
          GetAccessorFloatData(model, model.accessors[ni], TINYGLTF_TYPE_VEC3, normData, normStride);
      }

      // TEXCOORD_0
      const float *uvData = nullptr;
      size_t uvStride = 0;
      if (prim.attributes.find("TEXCOORD_0") != prim.attributes.end()) {
        const int ui = prim.attributes.at("TEXCOORD_0");
        if (ui >= 0 && ui < static_cast<int>(model.accessors.size()))
          GetAccessorFloatData(model, model.accessors[ui], TINYGLTF_TYPE_VEC2, uvData, uvStride);
      }

      // TANGENT
      const float *tanData = nullptr;
      size_t tanStride = 0;
      bool hasTangents = false;
      if (prim.attributes.find("TANGENT") != prim.attributes.end()) {
        const int ti = prim.attributes.at("TANGENT");
        if (ti >= 0 && ti < static_cast<int>(model.accessors.size()))
          hasTangents = GetAccessorFloatData(model, model.accessors[ti], TINYGLTF_TYPE_VEC4, tanData, tanStride);
      }

      // JOINTS_0 (bone indices per vertex)
      const uint8_t *jointBytes = nullptr;
      size_t jointStride = 0;
      int jointComponentType = 0;
      bool hasJoints = false;
      if (hasSkin && prim.attributes.find("JOINTS_0") != prim.attributes.end()) {
        const int ji = prim.attributes.at("JOINTS_0");
        if (ji >= 0 && ji < static_cast<int>(model.accessors.size()))
          hasJoints = GetAccessorJointData(model, model.accessors[ji], jointBytes, jointStride, jointComponentType);
      }

      // WEIGHTS_0 (bone weights per vertex)
      const float *weightData = nullptr;
      size_t weightStride = 0;
      bool hasWeights = false;
      if (hasSkin && prim.attributes.find("WEIGHTS_0") != prim.attributes.end()) {
        const int wi = prim.attributes.at("WEIGHTS_0");
        if (wi >= 0 && wi < static_cast<int>(model.accessors.size()))
          hasWeights = GetAccessorFloatData(model, model.accessors[wi], TINYGLTF_TYPE_VEC4, weightData, weightStride);
      }

      // INDICES
      if (prim.indices < 0 || prim.indices >= static_cast<int>(model.accessors.size()))
        continue;
      const auto &idxAcc = model.accessors[prim.indices];
      const uint8_t *idxBytes = nullptr;
      size_t idxStride = 0, idxElemBytes = 0;
      if (!GetAccessorIndexData(model, idxAcc, idxBytes, idxStride, idxElemBytes))
        continue;

      // Vertex base offset for merging multiple primitives.
      const uint32_t vertBase = static_cast<uint32_t>(m_mesh.vertices.size());

      // Interleave vertices.
      const size_t vCount = posAcc.count;
      m_mesh.vertices.resize(vertBase + vCount);
      for (size_t i = 0; i < vCount; ++i) {
        auto &v = m_mesh.vertices[vertBase + i];
        const float *p = reinterpret_cast<const float *>(
            reinterpret_cast<const uint8_t *>(posData) + i * posStride);
        v.pos[0] = p[0]; v.pos[1] = p[1]; v.pos[2] = p[2];

        if (normData) {
          const float *n = reinterpret_cast<const float *>(
              reinterpret_cast<const uint8_t *>(normData) + i * normStride);
          v.normal[0] = n[0]; v.normal[1] = n[1]; v.normal[2] = n[2];
        } else {
          v.normal[0] = 0.0f; v.normal[1] = 1.0f; v.normal[2] = 0.0f;
        }

        if (uvData) {
          const float *uv = reinterpret_cast<const float *>(
              reinterpret_cast<const uint8_t *>(uvData) + i * uvStride);
          v.uv[0] = uv[0]; v.uv[1] = uv[1];
        } else {
          v.uv[0] = 0.0f; v.uv[1] = 0.0f;
        }

        if (hasTangents && tanData) {
          const float *t = reinterpret_cast<const float *>(
              reinterpret_cast<const uint8_t *>(tanData) + i * tanStride);
          v.tangent[0] = t[0]; v.tangent[1] = t[1]; v.tangent[2] = t[2]; v.tangent[3] = t[3];
        } else {
          v.tangent[0] = 1.0f; v.tangent[1] = 0.0f; v.tangent[2] = 0.0f; v.tangent[3] = 1.0f;
        }

        // Bone indices (clamped to valid range to prevent out-of-bounds GPU reads)
        if (hasJoints && jointBytes) {
          const uint8_t *jPtr = jointBytes + i * jointStride;
          if (jointComponentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
            v.boneIndices[0] = jPtr[0];
            v.boneIndices[1] = jPtr[1];
            v.boneIndices[2] = jPtr[2];
            v.boneIndices[3] = jPtr[3];
          } else { // UNSIGNED_SHORT
            const uint16_t *jPtr16 = reinterpret_cast<const uint16_t *>(jPtr);
            v.boneIndices[0] = jPtr16[0];
            v.boneIndices[1] = jPtr16[1];
            v.boneIndices[2] = jPtr16[2];
            v.boneIndices[3] = jPtr16[3];
          }
          // Clamp to min(jointCount-1, kMaxBones-1) to prevent GPU OOB reads.
          const size_t jointMax = model.skins[0].joints.size() > 0
              ? model.skins[0].joints.size() - 1 : 0;
          const uint16_t maxIdx = static_cast<uint16_t>(
              (std::min)(jointMax, static_cast<size_t>(kMaxBones - 1)));
          for (int bi = 0; bi < 4; ++bi) {
            if (v.boneIndices[bi] > maxIdx)
              v.boneIndices[bi] = 0;
          }
        } else {
          v.boneIndices[0] = v.boneIndices[1] = v.boneIndices[2] = v.boneIndices[3] = 0;
        }

        // Bone weights
        if (hasWeights && weightData) {
          const float *w = reinterpret_cast<const float *>(
              reinterpret_cast<const uint8_t *>(weightData) + i * weightStride);
          v.boneWeights[0] = w[0]; v.boneWeights[1] = w[1];
          v.boneWeights[2] = w[2]; v.boneWeights[3] = w[3];
        } else {
          v.boneWeights[0] = v.boneWeights[1] = v.boneWeights[2] = v.boneWeights[3] = 0.0f;
        }
      }
      if (!hasTangents) anyTangentsMissing = true;

      // Indices — offset by vertBase.
      for (size_t i = 0; i < idxAcc.count; ++i) {
        const uint8_t *e = idxBytes + i * idxStride;
        uint32_t idx = 0;
        if (idxElemBytes == 1) idx = *e;
        else if (idxElemBytes == 2) idx = *reinterpret_cast<const uint16_t *>(e);
        else if (idxElemBytes == 4) idx = *reinterpret_cast<const uint32_t *>(e);
        m_mesh.indices.push_back(vertBase + idx);
      }

      // Track first material found.
      if (firstMaterialIdx < 0 && prim.material >= 0)
        firstMaterialIdx = prim.material;
    }
  }

  if (m_mesh.vertices.empty() || m_mesh.indices.empty())
    return false;

  // Compute tangents from UVs if any primitive lacked them.
  if (anyTangentsMissing) {
    ComputeTangents(m_mesh);
    std::cout << "Computed tangents from UVs (some primitives had no TANGENT attribute).\n";
  }

  // Log max bone index for diagnostics.
  if (hasSkin) {
    const size_t jointCount = model.skins[0].joints.size();
    if (jointCount > static_cast<size_t>(kMaxBones))
      std::cerr << "WARNING: Model has " << jointCount << " joints but kMaxBones="
                << kMaxBones << ". Excess bone indices clamped to 0.\n";
    uint16_t maxBoneIdx = 0;
    for (const auto &v : m_mesh.vertices)
      for (int bi = 0; bi < 4; ++bi)
        if (v.boneWeights[bi] > 0.0f && v.boneIndices[bi] > maxBoneIdx)
          maxBoneIdx = v.boneIndices[bi];
    std::cout << "Max bone index in vertices: " << maxBoneIdx
              << " (joint count: " << jointCount
              << ", kMaxBones: " << kMaxBones << ")\n";
  }

  std::cout << "Extracted " << m_mesh.vertices.size() << " vertices, "
            << m_mesh.indices.size() << " indices from "
            << model.meshes.size() << " mesh(es).\n";

  // Extract skeleton and animations.
  ExtractSkeleton(model);
  ExtractAnimations(model);

  // ---- Extract material textures (from first material found) ----
  m_baseColorImage = {};
  m_normalImage = {};
  m_metalRoughImage = {};
  m_aoImage = {};
  m_emissiveImage = {};
  m_emissiveFactor[0] = m_emissiveFactor[1] = m_emissiveFactor[2] = 0.0f;

  if (firstMaterialIdx >= 0 && firstMaterialIdx < static_cast<int>(model.materials.size())) {
    const auto &mat = model.materials[firstMaterialIdx];

    // BaseColor texture
    int imageIndex = -1;
    const int bcTexIndex = mat.pbrMetallicRoughness.baseColorTexture.index;
    if (bcTexIndex >= 0 && bcTexIndex < static_cast<int>(model.textures.size())) {
      const int srcImg = model.textures[bcTexIndex].source;
      if (srcImg >= 0 && srcImg < static_cast<int>(model.images.size()))
        imageIndex = srcImg;
    }
    // Fallback: if there is no baseColor texture specified, just pick image[0].
    if (imageIndex < 0 && !model.images.empty())
      imageIndex = 0;
    if (imageIndex >= 0 && imageIndex < static_cast<int>(model.images.size())) {
      m_baseColorImage = ConvertToRgba(model.images[imageIndex]);
      std::cout << "BaseColor Image: " << model.images[imageIndex].width << "x"
                << model.images[imageIndex].height << " ("
                << model.images[imageIndex].component << " -> RGBA)\n";
    } else {
      std::cout << "No baseColor texture image found in glTF.\n";
    }

    // Normal map texture
    const int normTexIndex = mat.normalTexture.index;
    m_normalImage = ExtractTextureImage(model, normTexIndex);
    if (!m_normalImage.pixels.empty())
      std::cout << "Normal map: " << m_normalImage.width << "x" << m_normalImage.height << "\n";

    // MetallicRoughness texture (glTF: G=roughness, B=metallic)
    const int mrTexIndex = mat.pbrMetallicRoughness.metallicRoughnessTexture.index;
    m_metalRoughImage = ExtractTextureImage(model, mrTexIndex);
    if (!m_metalRoughImage.pixels.empty())
      std::cout << "MetalRough map: " << m_metalRoughImage.width << "x" << m_metalRoughImage.height << "\n";

    // Occlusion (AO) texture
    const int aoTexIndex = mat.occlusionTexture.index;
    m_aoImage = ExtractTextureImage(model, aoTexIndex);
    if (!m_aoImage.pixels.empty())
      std::cout << "AO map: " << m_aoImage.width << "x" << m_aoImage.height << "\n";

    // Emissive texture + factor
    const int emTexIndex = mat.emissiveTexture.index;
    m_emissiveImage = ExtractTextureImage(model, emTexIndex);
    if (!m_emissiveImage.pixels.empty())
      std::cout << "Emissive map: " << m_emissiveImage.width << "x" << m_emissiveImage.height << "\n";

    if (mat.emissiveFactor.size() >= 3) {
      m_emissiveFactor[0] = static_cast<float>(mat.emissiveFactor[0]);
      m_emissiveFactor[1] = static_cast<float>(mat.emissiveFactor[1]);
      m_emissiveFactor[2] = static_cast<float>(mat.emissiveFactor[2]);
    }

    // Phase 11.5: populate Material struct from glTF scalars
    const auto &bcf = mat.pbrMetallicRoughness.baseColorFactor;
    if (bcf.size() >= 4) {
      m_material.baseColorFactor = {
        static_cast<float>(bcf[0]), static_cast<float>(bcf[1]),
        static_cast<float>(bcf[2]), static_cast<float>(bcf[3])};
    }
    m_material.metallicFactor  = static_cast<float>(mat.pbrMetallicRoughness.metallicFactor);
    m_material.roughnessFactor = static_cast<float>(mat.pbrMetallicRoughness.roughnessFactor);
    m_material.emissiveFactor  = {m_emissiveFactor[0], m_emissiveFactor[1], m_emissiveFactor[2]};

    // Texture presence flags
    m_material.hasBaseColor  = !m_baseColorImage.pixels.empty();
    m_material.hasNormal     = !m_normalImage.pixels.empty();
    m_material.hasMetalRough = !m_metalRoughImage.pixels.empty();
    m_material.hasAO         = !m_aoImage.pixels.empty();
    m_material.hasEmissive   = !m_emissiveImage.pixels.empty();
    // hasHeight set later in LoadHeightMap()
  } else {
    // No material: try image[0] as baseColor fallback.
    if (!model.images.empty()) {
      m_baseColorImage = ConvertToRgba(model.images[0]);
      std::cout << "No material; using image[0] as baseColor.\n";
    }
  }

  return true;
}

bool GltfLoader::LoadHeightMap(const std::string &path) {
  m_heightImage = {};

  int w = 0, h = 0, c = 0;
  unsigned char *data = stbi_load(path.c_str(), &w, &h, &c, 4); // force RGBA
  if (!data) {
    std::cerr << "Failed to load height map: " << path << "\n";
    return false;
  }

  m_heightImage.width = w;
  m_heightImage.height = h;
  m_heightImage.channels = 4;
  m_heightImage.pixels.assign(data, data + static_cast<size_t>(w) * h * 4);
  stbi_image_free(data);

  m_material.hasHeight = true;

  std::cout << "Height map: " << w << "x" << h << " (" << c << " -> RGBA)\n";
  return true;
}

// ---- Standalone image loader (for editor texture slot assignment) ----

bool LoadImageFile(const std::string &path, LoadedImage &outImage) {
  outImage = {};
  int w = 0, h = 0, c = 0;
  unsigned char *data = stbi_load(path.c_str(), &w, &h, &c, 4); // force RGBA
  if (!data) {
    std::cerr << "Failed to load image: " << path << "\n";
    return false;
  }
  outImage.width = w;
  outImage.height = h;
  outImage.channels = 4;
  outImage.pixels.assign(data, data + static_cast<size_t>(w) * h * 4);
  stbi_image_free(data);
  return true;
}

// ============================================================================
// Mixamo -> VRoid bone name remapping
// ============================================================================

// Strip common prefixes: "mixamorig:" or "Armature|mixamorig:"
static std::string StripMixamoPrefix(const std::string &name) {
  // Handle "mixamorig:" prefix.
  const std::string prefix1 = "mixamorig:";
  auto pos = name.find(prefix1);
  if (pos != std::string::npos)
    return name.substr(pos + prefix1.size());
  return name;
}

// Build a mapping from Mixamo bone names (after prefix strip) to VRoid J_Bip names.
static std::unordered_map<std::string, std::string> BuildMixamoToVRoidMap() {
  std::unordered_map<std::string, std::string> m;

  // Core body
  m["Hips"]          = "J_Bip_C_Hips";
  m["Spine"]         = "J_Bip_C_Spine";
  m["Spine1"]        = "J_Bip_C_Chest";
  m["Spine2"]        = "J_Bip_C_UpperChest";
  m["Neck"]          = "J_Bip_C_Neck";
  m["Head"]          = "J_Bip_C_Head";

  // Left arm
  m["LeftShoulder"]  = "J_Bip_L_Shoulder";
  m["LeftArm"]       = "J_Bip_L_UpperArm";
  m["LeftForeArm"]   = "J_Bip_L_LowerArm";
  m["LeftHand"]      = "J_Bip_L_Hand";

  // Right arm
  m["RightShoulder"] = "J_Bip_R_Shoulder";
  m["RightArm"]      = "J_Bip_R_UpperArm";
  m["RightForeArm"]  = "J_Bip_R_LowerArm";
  m["RightHand"]     = "J_Bip_R_Hand";

  // Left leg
  m["LeftUpLeg"]     = "J_Bip_L_UpperLeg";
  m["LeftLeg"]       = "J_Bip_L_LowerLeg";
  m["LeftFoot"]      = "J_Bip_L_Foot";
  m["LeftToeBase"]   = "J_Bip_L_ToeBase";

  // Right leg
  m["RightUpLeg"]    = "J_Bip_R_UpperLeg";
  m["RightLeg"]      = "J_Bip_R_LowerLeg";
  m["RightFoot"]     = "J_Bip_R_Foot";
  m["RightToeBase"]  = "J_Bip_R_ToeBase";

  // Left fingers
  m["LeftHandThumb1"]  = "J_Bip_L_Thumb1";
  m["LeftHandThumb2"]  = "J_Bip_L_Thumb2";
  m["LeftHandThumb3"]  = "J_Bip_L_Thumb3";
  m["LeftHandIndex1"]  = "J_Bip_L_Index1";
  m["LeftHandIndex2"]  = "J_Bip_L_Index2";
  m["LeftHandIndex3"]  = "J_Bip_L_Index3";
  m["LeftHandMiddle1"] = "J_Bip_L_Middle1";
  m["LeftHandMiddle2"] = "J_Bip_L_Middle2";
  m["LeftHandMiddle3"] = "J_Bip_L_Middle3";
  m["LeftHandRing1"]   = "J_Bip_L_Ring1";
  m["LeftHandRing2"]   = "J_Bip_L_Ring2";
  m["LeftHandRing3"]   = "J_Bip_L_Ring3";
  m["LeftHandPinky1"]  = "J_Bip_L_Little1";
  m["LeftHandPinky2"]  = "J_Bip_L_Little2";
  m["LeftHandPinky3"]  = "J_Bip_L_Little3";

  // Right fingers
  m["RightHandThumb1"]  = "J_Bip_R_Thumb1";
  m["RightHandThumb2"]  = "J_Bip_R_Thumb2";
  m["RightHandThumb3"]  = "J_Bip_R_Thumb3";
  m["RightHandIndex1"]  = "J_Bip_R_Index1";
  m["RightHandIndex2"]  = "J_Bip_R_Index2";
  m["RightHandIndex3"]  = "J_Bip_R_Index3";
  m["RightHandMiddle1"] = "J_Bip_R_Middle1";
  m["RightHandMiddle2"] = "J_Bip_R_Middle2";
  m["RightHandMiddle3"] = "J_Bip_R_Middle3";
  m["RightHandRing1"]   = "J_Bip_R_Ring1";
  m["RightHandRing2"]   = "J_Bip_R_Ring2";
  m["RightHandRing3"]   = "J_Bip_R_Ring3";
  m["RightHandPinky1"]  = "J_Bip_R_Little1";
  m["RightHandPinky2"]  = "J_Bip_R_Little2";
  m["RightHandPinky3"]  = "J_Bip_R_Little3";

  return m;
}

// ============================================================================
// Load animation clips from a standalone GLB file (Mixamo export)
// ============================================================================

bool LoadAnimationFile(const std::string &path,
                       const Skeleton &targetSkeleton,
                       std::vector<AnimationClip> &outClips) {
  outClips.clear();

  tinygltf::Model model;
  tinygltf::TinyGLTF loader;
  std::string err, warn;

  // Detect binary format.
  bool isBinary = false;
  {
    std::string ext;
    auto dot = path.rfind('.');
    if (dot != std::string::npos) {
      ext = path.substr(dot);
      for (auto &c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    isBinary = (ext == ".glb" || ext == ".vrm");
  }

  bool ret = isBinary ? loader.LoadBinaryFromFile(&model, &err, &warn, path)
                      : loader.LoadASCIIFromFile(&model, &err, &warn, path);
  if (!warn.empty())
    std::cout << "AnimLoad WARN: " << warn << "\n";
  if (!err.empty())
    std::cerr << "AnimLoad ERR: " << err << "\n";
  if (!ret) {
    std::cerr << "Failed to load animation file: " << path << "\n";
    return false;
  }

  if (model.animations.empty()) {
    std::cerr << "No animations found in: " << path << "\n";
    return false;
  }

  // Build Mixamo->VRoid name map.
  auto mixamoMap = BuildMixamoToVRoidMap();

  // Build target skeleton bone name -> index lookup.
  std::unordered_map<std::string, int> targetBoneMap;
  for (size_t i = 0; i < targetSkeleton.bones.size(); ++i)
    targetBoneMap[targetSkeleton.bones[i].name] = static_cast<int>(i);

  // Build node index -> remapped bone index for the animation file's nodes.
  // The animation file has its own skeleton; we remap by name.
  std::unordered_map<int, int> animNodeToTargetBone;

  for (int ni = 0; ni < static_cast<int>(model.nodes.size()); ++ni) {
    const std::string &nodeName = model.nodes[ni].name;

    // Try direct match first (same skeleton).
    auto directIt = targetBoneMap.find(nodeName);
    if (directIt != targetBoneMap.end()) {
      animNodeToTargetBone[ni] = directIt->second;
      continue;
    }

    // Try Mixamo name remapping.
    std::string stripped = StripMixamoPrefix(nodeName);
    auto mixIt = mixamoMap.find(stripped);
    if (mixIt != mixamoMap.end()) {
      auto targetIt = targetBoneMap.find(mixIt->second);
      if (targetIt != targetBoneMap.end()) {
        animNodeToTargetBone[ni] = targetIt->second;
        continue;
      }
    }

    // No match — this bone's animation will be skipped.
  }

  std::cout << "Animation bone remapping: " << animNodeToTargetBone.size()
            << "/" << model.nodes.size() << " nodes matched to target skeleton.\n";

  // ---- Global-space retargeting ----
  // Mixamo re-oriented the bone axes significantly (e.g., UpperLeg has ~180°
  // different rest rotation). Local delta retargeting can't handle this.
  // Instead: evaluate Mixamo hierarchy to get GLOBAL rotations, then convert
  // to VRoid LOCAL rotations. This works because both skeletons share the
  // same T-pose joint positions — only local axis orientations differ.

  // Build Mixamo node -> parent map.
  std::unordered_map<int, int> mixNodeParent;
  for (int n = 0; n < static_cast<int>(model.nodes.size()); ++n)
    for (int c : model.nodes[n].children)
      mixNodeParent[c] = n;

  // Extract Mixamo rest local rotations for ALL nodes (not just matched bones).
  int numNodes = static_cast<int>(model.nodes.size());
  std::vector<XMVECTOR> mixNodeLocalR(numNodes, XMQuaternionIdentity());
  for (int n = 0; n < numNodes; ++n) {
    const auto &node = model.nodes[n];
    if (node.rotation.size() == 4)
      mixNodeLocalR[n] = XMQuaternionNormalize(XMVectorSet(
          (float)node.rotation[0], (float)node.rotation[1],
          (float)node.rotation[2], (float)node.rotation[3]));
  }

  // Compute Mixamo GLOBAL rest rotations (walk from root to leaves).
  // Build topological order (parents before children).
  std::vector<int> topoOrder;
  {
    std::vector<bool> visited(numNodes, false);
    std::function<void(int)> visit = [&](int n) {
      if (n < 0 || n >= numNodes || visited[n]) return;
      auto pit = mixNodeParent.find(n);
      if (pit != mixNodeParent.end()) visit(pit->second);
      visited[n] = true;
      topoOrder.push_back(n);
    };
    for (int n = 0; n < numNodes; ++n) visit(n);
  }

  std::vector<XMVECTOR> mixGlobalR(numNodes, XMQuaternionIdentity());
  for (int n : topoOrder) {
    auto pit = mixNodeParent.find(n);
    if (pit != mixNodeParent.end()) {
      // XMQuaternionMultiply(A, B) = "first A, then B" in DirectXMath
      // global = compose(local, parentGlobal) = "first local, then parent"
      mixGlobalR[n] = XMQuaternionMultiply(mixNodeLocalR[n], mixGlobalR[pit->second]);
    } else {
      mixGlobalR[n] = mixNodeLocalR[n];
    }
  }

  // Compute VRoid GLOBAL bind rotations from IBMs.
  int boneCount = static_cast<int>(targetSkeleton.bones.size());
  std::vector<XMVECTOR> vroidGlobalBindR(boneCount, XMQuaternionIdentity());
  {
    std::vector<XMMATRIX> globalBind(boneCount);
    for (int i = 0; i < boneCount; ++i) {
      XMMATRIX ibm = XMLoadFloat4x4(&targetSkeleton.bones[i].inverseBindMatrix);
      XMVECTOR det;
      globalBind[i] = XMMatrixInverse(&det, ibm);
    }
    for (int i = 0; i < boneCount; ++i) {
      XMVECTOR s, t;
      XMMatrixDecompose(&s, &vroidGlobalBindR[i], &t, globalBind[i]);
      vroidGlobalBindR[i] = XMQuaternionNormalize(vroidGlobalBindR[i]);
    }
  }

  // Compute VRoid LOCAL bind rotations: localR[i] = inv(globalR[parent]) * globalR[i]
  std::vector<XMVECTOR> vroidLocalBindR(boneCount, XMQuaternionIdentity());
  for (int i = 0; i < boneCount; ++i) {
    int parent = targetSkeleton.bones[i].parentIndex;
    if (parent >= 0 && parent < boneCount) {
      // Hamilton: local = inv(parent) * global
      // DXMath:   XMQuaternionMultiply(global, inv(parent))
      vroidLocalBindR[i] = XMQuaternionNormalize(
          XMQuaternionMultiply(
              vroidGlobalBindR[i],
              XMQuaternionInverse(vroidGlobalBindR[parent])));
    } else {
      vroidLocalBindR[i] = vroidGlobalBindR[i];
    }
  }

  // Reverse map: target bone index -> Mixamo node index.
  std::unordered_map<int, int> boneToMixNode;
  for (const auto &pair : animNodeToTargetBone)
    boneToMixNode[pair.second] = pair.first;

  // ---- Extract animation clips using global-space retargeting ----
  for (const auto &anim : model.animations) {
    AnimationClip clip;
    clip.name = anim.name.empty()
                    ? "Anim_" + std::to_string(outClips.size())
                    : anim.name;
    clip.duration = 0.0f;

    // Step 1: Collect all rotation channels indexed by Mixamo node.
    // For each node, store (sampler input accessor, sampler output accessor).
    struct ChannelData {
      int mixNodeIdx;
      int targetBone;
      const float *timeData;
      size_t timeStride;
      const float *valData;
      size_t valStride;
      size_t keyCount;
    };
    std::vector<ChannelData> rotChannels;
    std::vector<float> allTimes; // union of all keyframe times

    for (const auto &channel : anim.channels) {
      if (channel.target_node < 0 || channel.target_path != "rotation")
        continue;
      auto remapIt = animNodeToTargetBone.find(channel.target_node);
      if (remapIt == animNodeToTargetBone.end())
        continue;

      const auto &sampler = anim.samplers[channel.sampler];
      const auto &inputAcc = model.accessors[sampler.input];
      if (inputAcc.bufferView < 0) continue;
      const auto &timeBv = model.bufferViews[inputAcc.bufferView];
      const auto &timeBuf = model.buffers[timeBv.buffer];
      const size_t timeStart = timeBv.byteOffset + inputAcc.byteOffset;
      size_t timeStride = timeBv.byteStride ? timeBv.byteStride : sizeof(float);

      const auto &outputAcc = model.accessors[sampler.output];
      const float *valData = nullptr;
      size_t valStride = 0;
      if (!GetAccessorFloatData(model, outputAcc, TINYGLTF_TYPE_VEC4, valData,
                                valStride))
        continue;

      size_t keyCount = std::min(inputAcc.count, outputAcc.count);
      const float *timeData = reinterpret_cast<const float *>(
          &timeBuf.data[timeStart]);

      rotChannels.push_back({channel.target_node, remapIt->second,
                             timeData, timeStride, valData, valStride,
                             keyCount});

      // Collect all unique timestamps.
      for (size_t k = 0; k < keyCount; ++k) {
        float t = *reinterpret_cast<const float *>(
            reinterpret_cast<const uint8_t *>(timeData) + k * timeStride);
        allTimes.push_back(t);
        if (t > clip.duration) clip.duration = t;
      }
    }

    if (rotChannels.empty()) continue;

    // Remove duplicate times and sort.
    std::sort(allTimes.begin(), allTimes.end());
    allTimes.erase(std::unique(allTimes.begin(), allTimes.end(),
        [](float a, float b) { return std::fabs(a - b) < 1e-6f; }),
        allTimes.end());

    // Step 2: For each keyframe time, evaluate ALL Mixamo bones to get globals,
    // then convert to VRoid local.
    // Use per-bone tracks indexed by target bone index.
    std::unordered_map<int, AnimTrack> trackMap;

    for (size_t ti = 0; ti < allTimes.size(); ++ti) {
      float time = allTimes[ti];

      // Set Mixamo local rotations: start with rest, override with animation.
      std::vector<XMVECTOR> mixLocalR = mixNodeLocalR; // copy rest
      for (const auto &ch : rotChannels) {
        // Find the keyframe pair for this time in this channel.
        size_t lo = 0, hi = 0;
        float frac = 0.0f;
        if (ch.keyCount <= 1) {
          lo = hi = 0; frac = 0.0f;
        } else {
          float firstT = *reinterpret_cast<const float *>(
              reinterpret_cast<const uint8_t *>(ch.timeData));
          float lastT = *reinterpret_cast<const float *>(
              reinterpret_cast<const uint8_t *>(ch.timeData) +
              (ch.keyCount - 1) * ch.timeStride);
          if (time <= firstT) { lo = hi = 0; frac = 0.0f; }
          else if (time >= lastT) { lo = hi = ch.keyCount - 1; frac = 0.0f; }
          else {
            for (size_t k = 0; k < ch.keyCount - 1; ++k) {
              float tA = *reinterpret_cast<const float *>(
                  reinterpret_cast<const uint8_t *>(ch.timeData) +
                  k * ch.timeStride);
              float tB = *reinterpret_cast<const float *>(
                  reinterpret_cast<const uint8_t *>(ch.timeData) +
                  (k + 1) * ch.timeStride);
              if (time >= tA && time < tB) {
                lo = k; hi = k + 1;
                float span = tB - tA;
                frac = (span > 1e-6f) ? (time - tA) / span : 0.0f;
                break;
              }
            }
          }
        }

        const float *vA = reinterpret_cast<const float *>(
            reinterpret_cast<const uint8_t *>(ch.valData) + lo * ch.valStride);
        const float *vB = reinterpret_cast<const float *>(
            reinterpret_cast<const uint8_t *>(ch.valData) + hi * ch.valStride);
        XMVECTOR qA = XMVectorSet(vA[0], vA[1], vA[2], vA[3]);
        XMVECTOR qB = XMVectorSet(vB[0], vB[1], vB[2], vB[3]);
        XMVECTOR animR = XMQuaternionNormalize(XMQuaternionSlerp(qA, qB, frac));

        mixLocalR[ch.mixNodeIdx] = animR;
      }

      // Walk Mixamo hierarchy to compute global rotations at this frame.
      std::vector<XMVECTOR> mixAnimGlobalR(numNodes, XMQuaternionIdentity());
      for (int n : topoOrder) {
        auto pit = mixNodeParent.find(n);
        if (pit != mixNodeParent.end())
          mixAnimGlobalR[n] = XMQuaternionMultiply(
              mixLocalR[n], mixAnimGlobalR[pit->second]);
        else
          mixAnimGlobalR[n] = mixLocalR[n];
      }

      // Global-space DELTA retargeting:
      // 1. delta[i] = how much bone i rotated from Mixamo rest in global space
      //    animGlobal = restGlobal * delta  →  delta = inv(restGlobal) * animGlobal
      //    DXMath: delta = XMQuaternionMultiply(inv(restGlobal), animGlobal)
      // 2. Apply delta to VRoid global: vroidAnimGlobal = vroidBindGlobal * delta
      //    DXMath: XMQuaternionMultiply(vroidBindGlobal, delta)
      // 3. Convert to VRoid local: local = animGlobal * inv(parentAnimGlobal)
      //    DXMath: XMQuaternionMultiply(animGlobal, inv(parentAnimGlobal))

      // First compute per-bone deltas and VRoid animated globals.
      std::vector<XMVECTOR> vroidAnimGlobal(boneCount);
      for (int bi = 0; bi < boneCount; ++bi)
        vroidAnimGlobal[bi] = vroidGlobalBindR[bi]; // default: bind pose

      for (const auto &ch : rotChannels) {
        int bi = ch.targetBone;
        int mixNode = ch.mixNodeIdx;

        // Delta in global space.
        XMVECTOR delta = XMQuaternionNormalize(
            XMQuaternionMultiply(
                XMQuaternionInverse(mixGlobalR[mixNode]),
                mixAnimGlobalR[mixNode]));

        // Apply delta to VRoid global bind.
        vroidAnimGlobal[bi] = XMQuaternionNormalize(
            XMQuaternionMultiply(vroidGlobalBindR[bi], delta));
      }

      // Walk VRoid hierarchy to convert globals to locals.
      for (int bi = 0; bi < boneCount; ++bi) {
        auto mixIt = boneToMixNode.find(bi);
        if (mixIt == boneToMixNode.end())
          continue; // unmatched bone: no track, EvaluateClipCore uses bind

        int vroidParent = targetSkeleton.bones[bi].parentIndex;
        XMVECTOR parentAnimGlobal =
            (vroidParent >= 0 && vroidParent < boneCount)
                ? vroidAnimGlobal[vroidParent]
                : XMQuaternionIdentity();

        // local = animGlobal * inv(parentAnimGlobal)
        XMVECTOR vroidLocalR = XMQuaternionNormalize(
            XMQuaternionMultiply(
                vroidAnimGlobal[bi],
                XMQuaternionInverse(parentAnimGlobal)));

        auto &track = trackMap[bi];
        if (track.keyframes.empty()) {
          track.boneIndex = bi;
          track.path = AnimTargetPath::Rotation;
        }
        AnimKeyframe kf;
        kf.time = time;
        XMStoreFloat4(&kf.value, vroidLocalR);
        track.keyframes.push_back(kf);
      }
    }

    // Collect tracks into clip.
    for (auto &pair : trackMap)
      clip.tracks.push_back(std::move(pair.second));

    if (!clip.tracks.empty()) {
      std::cout << "Loaded animation \"" << clip.name << "\": "
                << clip.tracks.size() << " tracks, " << clip.duration << "s\n";
      outClips.push_back(std::move(clip));
    }
  }

  std::cout << "Loaded " << outClips.size() << " animation clip(s) from "
            << path << "\n";
  return !outClips.empty();
}
