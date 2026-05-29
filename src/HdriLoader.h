// ======================================
// File: HdriLoader.h
// Purpose: Load HDR environment images (EXR) into CPU RGBA float buffers.
// ======================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct HdriImageRgba32f
{
    int width = 0;
    int height = 0;
    // RGBA float32, row-major, size = width * height * 4
    std::vector<float> rgba;
};

// Loads an EXR file as RGBA float32 (linear space).
// Throws std::runtime_error on failure.
HdriImageRgba32f LoadExrRgba32f(const std::wstring& path);

