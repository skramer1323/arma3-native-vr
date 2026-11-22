#include "CursorImage.h"

#include <algorithm>
#include <cstring>

namespace a3vr {
namespace {

// Draws the cursor on a background filled with `fill` and returns BGRA bytes.
bool DrawOnBackground(HCURSOR cursor, int width, int height, std::uint8_t fill,
                      std::vector<std::uint8_t>& out)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        return false;
    }
    const HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        DeleteDC(dc);
        return false;
    }
    const std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    std::memset(bits, fill, bytes);
    const HGDIOBJ previous = SelectObject(dc, bitmap);
    const BOOL drawn = DrawIconEx(dc, 0, 0, cursor, width, height, 0, nullptr, DI_NORMAL);
    GdiFlush();
    if (drawn) {
        out.assign(static_cast<const std::uint8_t*>(bits),
                   static_cast<const std::uint8_t*>(bits) + bytes);
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return drawn != FALSE;
}

}  // namespace

bool RasterizeCursor(HCURSOR cursor, bool bgra, CursorImage& image)
{
    image = {};
    ICONINFO iconInfo{};
    if (!cursor || !GetIconInfo(cursor, &iconInfo)) {
        return false;
    }
    BITMAP bitmap{};
    int width = 0;
    int height = 0;
    if (iconInfo.hbmColor && GetObjectW(iconInfo.hbmColor, sizeof(bitmap), &bitmap)) {
        width = bitmap.bmWidth;
        height = bitmap.bmHeight;
    } else if (iconInfo.hbmMask && GetObjectW(iconInfo.hbmMask, sizeof(bitmap), &bitmap)) {
        width = bitmap.bmWidth;
        height = bitmap.bmHeight / 2;  // monochrome: AND mask above XOR mask
    }
    if (iconInfo.hbmColor) {
        DeleteObject(iconInfo.hbmColor);
    }
    if (iconInfo.hbmMask) {
        DeleteObject(iconInfo.hbmMask);
    }
    if (width <= 0 || height <= 0) {
        return false;
    }
    width = std::min(width, kCursorMaxSize);
    height = std::min(height, kCursorMaxSize);

    // Alpha is recovered from two renders: on black the result is colour*alpha,
    // on white it is colour*alpha + 255*(1-alpha). This works for cursors with
    // an alpha channel and for old mask-only cursors alike.
    std::vector<std::uint8_t> onBlack;
    std::vector<std::uint8_t> onWhite;
    if (!DrawOnBackground(cursor, width, height, 0x00, onBlack) ||
        !DrawOnBackground(cursor, width, height, 0xFF, onWhite)) {
        return false;
    }
    image.width = width;
    image.height = height;
    image.hotX = static_cast<int>(iconInfo.xHotspot);
    image.hotY = static_cast<int>(iconInfo.yHotspot);
    image.pixels.resize(onBlack.size());
    for (std::size_t i = 0; i < onBlack.size(); i += 4) {
        const int difference = static_cast<int>(onWhite[i + 1]) - static_cast<int>(onBlack[i + 1]);
        const int alpha = std::clamp(255 - difference, 0, 255);
        const std::uint8_t blue = onBlack[i];
        const std::uint8_t green = onBlack[i + 1];
        const std::uint8_t red = onBlack[i + 2];
        image.pixels[i] = bgra ? blue : red;
        image.pixels[i + 1] = green;
        image.pixels[i + 2] = bgra ? red : blue;
        image.pixels[i + 3] = static_cast<std::uint8_t>(alpha);
        if (alpha > 8) {
            ++image.visiblePixels;
        }
    }
    return image.visiblePixels > 0;
}

}  // namespace a3vr
