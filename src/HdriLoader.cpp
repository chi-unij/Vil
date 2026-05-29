// ======================================
// File: HdriLoader.cpp
// Purpose: EXR loader implementation using TinyEXR.
// ======================================

#include "HdriLoader.h"

#define TINYEXR_USE_MINIZ 1
#include "tinyexr.h"

#include <stdexcept>

HdriImageRgba32f LoadExrRgba32f(const std::wstring& path)
{
    // TinyEXR wants UTF-8 paths.
    std::string u8;
    u8.reserve(path.size());
    for (wchar_t wc : path)
    {
        // This is enough for ASCII paths like our repo-relative ones.
        // If we ever need full UTF-16 -> UTF-8 for arbitrary filenames, use DxUtil::Utf8/Win32 conversion.
        if (wc > 0x7F) throw std::runtime_error("LoadExrRgba32f: non-ASCII path not supported yet");
        u8.push_back(static_cast<char>(wc));
    }

    float* out = nullptr;
    int w = 0;
    int h = 0;
    const char* err = nullptr;

    int ret = LoadEXR(&out, &w, &h, u8.c_str(), &err);
    if (ret != TINYEXR_SUCCESS)
    {
        std::string msg = "LoadEXR failed";
        if (err)
        {
            msg += ": ";
            msg += err;
            FreeEXRErrorMessage(err);
        }
        throw std::runtime_error(msg);
    }

    HdriImageRgba32f img;
    img.width = w;
    img.height = h;
    img.rgba.assign(out, out + (static_cast<size_t>(w) * static_cast<size_t>(h) * 4u));
    free(out);
    return img;
}

