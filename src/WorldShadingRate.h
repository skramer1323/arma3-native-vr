#pragma once

#include "WorldShadingMask.h"

#include <d3d11.h>

namespace a3vr {

bool SetWorldShadingRate(ID3D11DeviceContext* context, UINT width, UINT height,
                         const WorldShadingProjection (&eyes)[2]);
void ClearWorldShadingRate(ID3D11DeviceContext* context = nullptr);

}  // namespace a3vr
