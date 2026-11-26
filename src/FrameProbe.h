#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <string>
#include <vector>

namespace a3vr {

bool EnableProjectionCacheForContext(ID3D11DeviceContext* context);

// With FrameProbe=1, Ctrl+F2 records draw state, shader bytecode and buffer samples.
bool InstallFrameProbe(ID3D11Device* anyDevice, ID3D11DeviceContext* anyContext);
// From the Present hook, before the original Present.
void FrameProbeOnPresent(IDXGISwapChain* swapChain);
// Only read on the Present thread, after the game frame probe.
int FrameProbeCaptureNumber();

// The shader browser temporarily suppresses selected draws and records marked entries.
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
