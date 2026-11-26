#pragma once

#include <cmath>

namespace a3vr {

// The stock inventory close button is placed in a centred 40-by-25 layout.
// Its texture is shared with Dynamic Groups and the editor, whose button bounds differ.
inline bool IsInventoryCloseButton(float left, float top, float right, float bottom)
{
    const float width = right - left;
    const float height = top - bottom;
    return width > 0.0F && height > 0.0F &&
           std::fabs(left / width - 18.0F) < 0.75F &&
           std::fabs(top / height - 11.5F) < 0.5F;
}

}  // namespace a3vr
