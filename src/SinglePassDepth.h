#pragma once

#include <d3d11.h>
#include <wrl/client.h>

namespace a3vr {

enum class SinglePassLayout { PositionOnly, WorldPrepass };

// The caller has already bound the left eye's normal projection constants.
// WorldPrepass consumes the same patched cb2 as the normal right-eye vertex shader.
bool BeginSinglePassDepth(ID3D11DeviceContext* context, ID3D11Device* device,
                          const D3D11_VIEWPORT& fullViewport, const float* left, const float* right,
                          Microsoft::WRL::ComPtr<ID3D11Buffer>& previousGeometryConstants,
                          SinglePassLayout layout = SinglePassLayout::PositionOnly,
                          ID3D11Buffer* rightEyeProjection = nullptr);
void EndSinglePassDepth(ID3D11DeviceContext* context, ID3D11Buffer* previousGeometryConstants);

} // namespace a3vr
