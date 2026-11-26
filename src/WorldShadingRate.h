#pragma once

#include "WorldShadingMask.h"

#include <d3d11.h>
#include <array>
#include <vector>

namespace a3vr {

using WorldVisibilityContours = std::array<std::vector<WorldVisibilityPoint>, 2>;
void SetWorldVisibilityContours(WorldVisibilityContours contours);

bool SetWorldShadingRate(ID3D11DeviceContext* context, UINT width, UINT height,
                         const WorldShadingProjection (&eyes)[2], int innerDegrees, int middleDegrees,
                         bool coarseOuter = false);
// Use full-rate shading between eligible draws on the same world target.
void SuspendWorldShadingRate(ID3D11DeviceContext* context);
void ClearWorldShadingRate(ID3D11DeviceContext* context = nullptr);
bool WorldShadingCoarseActive();
bool GetWorldShadingGuide(UINT width, UINT height, WorldShadingGuide& guide);

}  // namespace a3vr
