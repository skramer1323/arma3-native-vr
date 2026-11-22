#pragma once

#include <d3d11.h>
#include <span>

namespace a3vr {

// Build a clip shader that preserves the game's vertex-to-pixel shader signature.
bool BuildBodyMaskShader(ID3D11Device* device, std::span<const unsigned char> vertexCode,
                         ID3D11GeometryShader** shader);

}  // namespace a3vr