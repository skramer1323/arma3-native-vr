#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <string>
#include <vector>

namespace a3vr {

bool EnableProjectionCacheForContext(ID3D11DeviceContext* context);

// Frame probe (FrameProbe=1): step 0 of the mod's own geometry stereo.
// Read-only. Observes how the engine draws one frame: which render targets and
// depth buffers, how many draws, which vertex shaders and which constant
// buffers they read. Nothing in the picture is changed.
//
// Ctrl+F2 in the game window records the next frame into
//   arma3_native_vr.frame<N>.txt    passes, vertex shaders, reflection
//   arma3_native_vr.frame<N>.cb.bin constant buffer samples
//   arma3_vr_shaders\vs_<hash>.dxbc vertex shader bytecode
//
// The hooks patch the d3d11.dll functions themselves (addresses taken from a
// throw-away device), so they see every device and context in the process.
bool InstallFrameProbe(ID3D11Device* anyDevice, ID3D11DeviceContext* anyContext);
// From the Present hook, before the original Present.
void FrameProbeOnPresent(IDXGISwapChain* swapChain);
// Only read on the Present thread, after the game frame probe.
int FrameProbeCaptureNumber();

// Shader browser (a hunting tool for screen-space effects that are wrong in the side-by-side
// picture: shadows, light, glow): while enabled, the small full-screen draws seen so far are
// listed in order of first appearance; the selected one is skipped, so its effect disappears from
// the picture. Step to the next/previous one, mark the guilty one (it is written to
// arma3_native_vr.shaders.txt next to the DLL).
void ShaderBrowseEnable(bool enabled);
// 0 off, 1 screen-space shaders, 2 UI draws (icons, markers: shader + texture kinds).
void ShaderBrowseSetMode(int mode);
int ShaderBrowseMode();
bool ShaderBrowseEnabled();
void ShaderBrowseStep(int delta);
void ShaderBrowseMark();
std::string ShaderBrowseDescribe();
std::vector<std::string> ShaderBrowseMarkedLines();

}  // namespace a3vr
