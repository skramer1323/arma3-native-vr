#pragma once

#include <Windows.h>

#include <cstdint>
#include <vector>

namespace a3vr {

// Arma 3 shows a Windows (hardware) cursor, which is not part of the back
// buffer. This turns the current HCURSOR into premultiplied 8-bit pixels so
// it can be shown as a small OpenXR quad on top of the game screen.
struct CursorImage {
    int width = 0;
    int height = 0;
    int hotX = 0;
    int hotY = 0;
    int visiblePixels = 0;
    std::vector<std::uint8_t> pixels;  // width * height * 4, premultiplied alpha
};

constexpr int kCursorMaxSize = 128;

// bgra = channel order of the target texture. Returns false when the cursor
// cannot be read or has no visible pixel.
bool RasterizeCursor(HCURSOR cursor, bool bgra, CursorImage& image);

}  // namespace a3vr
