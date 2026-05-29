// ======================================
// File: ShaderCompiler.h
// Purpose: Safe shader compilation wrapper for hot-reload (Phase 8).
//          Returns success/failure + error message instead of throwing.
// ======================================

#pragma once

#include <d3dcompiler.h>
#include <string>
#include <wrl.h>

struct ShaderCompileResult {
  Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
  bool success = false;
  std::string errorMessage;
};

inline ShaderCompileResult CompileShaderSafe(const wchar_t *filePath,
                                             const char *entryPoint,
                                             const char *target) {
  UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
  flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

  ShaderCompileResult result;
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  HRESULT hr =
      D3DCompileFromFile(filePath, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                         entryPoint, target, flags, 0, &result.bytecode, &errors);

  if (FAILED(hr)) {
    result.success = false;
    if (errors) {
      result.errorMessage.assign(
          static_cast<const char *>(errors->GetBufferPointer()),
          errors->GetBufferSize());
    } else {
      result.errorMessage = "D3DCompileFromFile failed (HRESULT 0x" +
                            std::to_string(static_cast<unsigned long>(hr)) + ")";
    }
    return result;
  }

  result.success = true;
  return result;
}
