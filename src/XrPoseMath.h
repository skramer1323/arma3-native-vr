#pragma once

#include <openxr/openxr.h>

#include <cmath>

namespace a3vr {

// Keep the angular scale of a static optic while putting its axis at the centre
// of the image. The game projection and the submitted OpenXR view use this same FOV.
inline XrFovf CenteredOpticsFov(const XrFovf& fov)
{
    const float horizontal = std::atan(0.5F * (std::tan(fov.angleRight) - std::tan(fov.angleLeft)));
    const float vertical = std::atan(0.5F * (std::tan(fov.angleUp) - std::tan(fov.angleDown)));
    return {-horizontal, horizontal, vertical, -vertical};
}

inline XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline XrQuaternionf Conjugate(const XrQuaternionf& q)
{
    return {-q.x, -q.y, -q.z, q.w};
}

inline XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v)
{
    const float cx = q.y * v.z - q.z * v.y;
    const float cy = q.z * v.x - q.x * v.z;
    const float cz = q.x * v.y - q.y * v.x;
    const float ccx = q.y * cz - q.z * cy;
    const float ccy = q.z * cx - q.x * cz;
    const float ccz = q.x * cy - q.y * cx;
    return {v.x + 2.0F * (q.w * cx + ccx), v.y + 2.0F * (q.w * cy + ccy),
            v.z + 2.0F * (q.w * cz + ccz)};
}

inline constexpr XrPosef kIdentityXrPose{{0.0F, 0.0F, 0.0F, 1.0F}, {}};

// Express a stable application origin in the runtime's new natural LOCAL space.
inline XrPosef RebaseLocalOffset(const XrPosef& stableInOld, const XrPosef& newInOld)
{
    const auto oldToNew = Conjugate(newInOld.orientation);
    const auto& stable = stableInOld.position;
    const auto& origin = newInOld.position;
    return {Multiply(oldToNew, stableInOld.orientation),
            Rotate(oldToNew, {stable.x - origin.x, stable.y - origin.y, stable.z - origin.z})};
}

}  // namespace a3vr
