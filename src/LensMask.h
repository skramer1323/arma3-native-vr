#pragma once

#include <d3d11.h>

// Scope lens: writes a soft elliptical alpha mask (1 inside, 0 outside) into the alpha channel of
// one rectangle of a render target, colour untouched. The rectangle is the window of the eye image
// that the lens layer shows; the layer is blended with the picture's own alpha, so the scope is a
// round window over the world. rect = {x, y, width, height} in target pixels.
void LensMaskWriteAlpha(ID3D11Device* device, ID3D11DeviceContext* context,
                        ID3D11RenderTargetView* target, const float rect[4]);
