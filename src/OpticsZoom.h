#pragma once

#include <algorithm>
#include <cmath>

namespace a3vr {

inline constexpr float kOpticsRatio = 1.25F;

inline bool ValidZoomProjection(float p00, float p11, float wFromZ)
{
    if (!(p00 > 0.2F)) return false;
    if (p00 < 8.0F) return true;
    // Strong optics can produce a very narrow but otherwise normal perspective.
    return p00 <= 128.0F && p11 >= 0.2F * p00 && p11 <= 5.0F * p00 &&
           std::fabs(wFromZ - 1.0F) < 0.001F;
}

inline bool NativeZoomMayUpdateReference(float current, float reference, bool opticsInput)
{
    // While looking through an optic, keep the last ordinary world projection.
    return !opticsInput && (reference > 0.0F ? current < reference * kOpticsRatio : current < 1.25F);
}

inline bool KeepNativeOptics(float current, float reference, bool opticsInput, bool wasConfirmed)
{
    // A sight can stay up after the button is released. Once confirmed by input,
    // retain the narrow projection until the engine returns to the ordinary view.
    return (opticsInput || wasConfirmed) && reference > 0.0F &&
           current >= reference * kOpticsRatio;
}

inline float NativeOpticsZoom(float current, float reference, bool mouseOpticsMode, bool opticsInput)
{
    if (!(reference > 0.0F)) return 1.0F;
    const float ratio = current / reference;
    // A keyboard-raised instrument still has a narrow native projection.
    // Small changes between normal views continue to use the headset frustum.
    if (mouseOpticsMode && (ratio < kOpticsRatio || (!opticsInput && current < 1.25F))) return 1.0F;
    return std::clamp(ratio, 1.0F, 80.0F);
}

}  // namespace a3vr
