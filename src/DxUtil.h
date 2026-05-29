// ======================================
// File: DxUtil.h
// Purpose: Small DX/Win32 helpers (ThrowIfFailed, string conversion, debug print)
// ======================================

#pragma once

#include <windows.h>
#include <string>
#include <stdexcept>

#include <wrl.h>

inline void ThrowIfFailed(HRESULT hr, const char* msg = "HRESULT failed")
{
    if (FAILED(hr))
    {
        throw std::runtime_error(msg);
    }
}

inline std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

std::wstring HrToString(HRESULT hr);
void DebugPrint(const std::wstring& msg);

