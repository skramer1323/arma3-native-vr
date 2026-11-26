#include "Hooks.h"
#include "Perf.h"
#include "ResolutionHook.h"

#include "EngineStereo.h"
#include "FrameProbe.h"

#include "Config.h"
#include "Log.h"
#include "OpenXRRuntime.h"

#include <MinHook.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>

namespace a3vr {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                                    DXGI_FORMAT, UINT);

PresentFn g_originalPresent = nullptr;
Present1Fn g_originalPresent1 = nullptr;
ResizeBuffersFn g_originalResizeBuffers = nullptr;
std::atomic<bool> g_loggedPresent = false;
std::atomic<bool> g_loggedPresent1 = false;
// If dxgi ever routes Present through Present1 (or the reverse), only the outer
// call may drive the OpenXR frame.
thread_local int t_presentDepth = 0;

struct PresentScope {
    PresentScope() { ++t_presentDepth; }
    ~PresentScope() { --t_presentDepth; }
    PresentScope(const PresentScope&) = delete;
    PresentScope& operator=(const PresentScope&) = delete;
};

bool HookFunction(void* target, void* replacement, void** original, const char* name)
{
    const MH_STATUS created = MH_CreateHook(target, replacement, original);
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED) {
        Log("MinHook failed to create %s hook: %s", name, MH_StatusToString(created));
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED) {
        Log("MinHook failed to enable %s hook: %s", name, MH_StatusToString(enabled));
        return false;
    }
    Log("Hooked %s at %p", name, target);
    return true;
}

namespace {
std::uint64_t g_perfPresents = 0;
// Counts real presents on the OpenXR swapchain and writes PERF lines every 120.
void PerfAfterPresent(IDXGISwapChain* swapChain)
{
    ID3D11Device* device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) && device) {
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        if (context) {
            PerfOnPresentDone(context, ++g_perfPresents, 120);
            PerfFrameGpuBegin(swapChain, device, context, g_perfPresents + 1);
            context->Release();
        }
        device->Release();
    }
}

template <typename Present>
HRESULT PresentFrame(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, Present&& present)
{
    if ((flags & DXGI_PRESENT_TEST) != 0 || t_presentDepth != 0) {
        PresentScope scope;
        return present(syncInterval);
    }

    auto& runtime = OpenXRRuntime::Instance();
    const bool profile = PerfEnabled() && runtime.IsMainSwapChain(swapChain);
    const double started = profile ? PerfNowMs() : 0.0;
    if (profile) PerfFrameGpuMark(swapChain, 1);
    FrameProbeOnPresent(swapChain);
    EngineStereoOnPresent(swapChain);
    const bool prepareAfterPresent = runtime.OnPresent(swapChain);
    if (profile) {
        PerfFrameGpuMark(swapChain, 2);
        PerfAddPresentSide(PerfNowMs() - started, 0.0, 0.0);
    }

    PresentScope scope;
    // The runtime already paces the world frame when the next XR wait is deferred.
    const UINT desktopSync = prepareAfterPresent ? 0 : syncInterval;
    const bool timeDesktop = profile || prepareAfterPresent;
    const double desktopStart = timeDesktop ? PerfNowMs() : 0.0;
    const HRESULT result = present(desktopSync);
    const double desktopEnd = timeDesktop ? PerfNowMs() : 0.0;
    if (profile) PerfAddOriginalPresent(desktopEnd - desktopStart);
    if (prepareAfterPresent) {
        runtime.OnAfterPresent(swapChain, desktopEnd - desktopStart, desktopSync);
        if (profile) PerfAddPresentSide(PerfNowMs() - desktopEnd, 0.0, 0.0);
    }
    if (profile) PerfAfterPresent(swapChain);
    return result;
}
}  // namespace

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    if (!g_loggedPresent.exchange(true)) {
        Log("RUNG1 first IDXGISwapChain::Present: swapchain=%p sync=%u flags=0x%X thread=%lu",
            swapChain, syncInterval, flags, GetCurrentThreadId());
    }
    return PresentFrame(swapChain, syncInterval, flags,
                        [&](UINT interval) { return g_originalPresent(swapChain, interval, flags); });
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* swapChain, UINT syncInterval,
                                       UINT flags, const DXGI_PRESENT_PARAMETERS* parameters)
{
    if (!g_loggedPresent1.exchange(true)) {
        Log("RUNG1 first IDXGISwapChain1::Present1: swapchain=%p thread=%lu", swapChain,
            GetCurrentThreadId());
    }
    return PresentFrame(swapChain, syncInterval, flags, [&](UINT interval) {
        return g_originalPresent1(swapChain, interval, flags, parameters);
    });
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* swapChain, UINT bufferCount,
                                            UINT width, UINT height, DXGI_FORMAT format,
                                            UINT flags)
{
    OpenXRRuntime::Instance().OnResize(swapChain, width, height);
    const HRESULT result =
        g_originalResizeBuffers(swapChain, bufferCount, width, height, format, flags);
    Log("ResizeBuffers(%u, %ux%u, format=%d) -> 0x%08lX", bufferCount, width, height,
        static_cast<int>(format), static_cast<unsigned long>(result));
    return result;
}

// A temporary swapchain provides the shared DXGI entry points used by the game.
bool InstallSwapChainHooks()
{
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"Arma3NativeVRDummy";
    RegisterClassExW(&windowClass);
    const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_OVERLAPPED, 0, 0,
                                        64, 64, nullptr, nullptr, windowClass.hInstance,
                                        nullptr);
    if (!window) {
        Log("Could not create the dummy window for the DXGI vtable");
        return false;
    }
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 1;
    description.BufferDesc.Width = 64;
    description.BufferDesc.Height = 64;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    HRESULT result = E_FAIL;
    for (const D3D_DRIVER_TYPE driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        result = D3D11CreateDeviceAndSwapChain(nullptr, driver, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &description, &swapChain,
                                               &device, nullptr, &context);
        if (SUCCEEDED(result)) {
            break;
        }
    }
    bool installed = false;
    if (SUCCEEDED(result)) {
        void** vtable = *reinterpret_cast<void***>(swapChain.Get());
        installed = HookFunction(vtable[8], reinterpret_cast<void*>(&HookPresent),
                                 reinterpret_cast<void**>(&g_originalPresent),
                                 "IDXGISwapChain::Present");
        installed &= HookFunction(vtable[13], reinterpret_cast<void*>(&HookResizeBuffers),
                                  reinterpret_cast<void**>(&g_originalResizeBuffers),
                                  "IDXGISwapChain::ResizeBuffers");
        Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
        if (SUCCEEDED(swapChain.As(&swapChain1))) {
            void** vtable1 = *reinterpret_cast<void***>(swapChain1.Get());
            HookFunction(vtable1[22], reinterpret_cast<void*>(&HookPresent1),
                         reinterpret_cast<void**>(&g_originalPresent1),
                         "IDXGISwapChain1::Present1");
        }
        if (GetConfig().frameProbe || GetConfig().engineStereo) {
            InstallFrameProbe(device.Get(), context.Get());
        }
    } else {
        Log("Dummy D3D11 device/swapchain failed: 0x%08lX", static_cast<unsigned long>(result));
    }
    swapChain.Reset();
    context.Reset();
    device.Reset();
    DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
    return installed;
}

// ---- FreeTrack registry answer (HeadTracking=1) --------------------------
// The engine only loads FreeTrackClient64.dll when
// HKCU\Software\Freetrack\FreetrackClient "Path" exists (FUN_141552740). These
// hooks answer that one read inside the game process with our own folder, so
// nothing is written to the user's registry.
using RegOpenKeyExAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
using RegQueryValueExAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegCloseKeyFn = LSTATUS(WINAPI*)(HKEY);

RegOpenKeyExAFn g_originalRegOpenKeyExA = nullptr;
RegQueryValueExAFn g_originalRegQueryValueExA = nullptr;
RegCloseKeyFn g_originalRegCloseKey = nullptr;
const HKEY kFreeTrackKey = reinterpret_cast<HKEY>(static_cast<ULONG_PTR>(0xA3F7EE01));
std::atomic<int> g_freeTrackKeyLogs = 0;

LSTATUS WINAPI HookRegOpenKeyExA(HKEY key, LPCSTR subKey, DWORD options, REGSAM access,
                                 PHKEY result)
{
    if (key == HKEY_CURRENT_USER && subKey && result &&
        _stricmp(subKey, "Software\\Freetrack\\FreetrackClient") == 0) {
        *result = kFreeTrackKey;
        if (g_freeTrackKeyLogs++ < 5) {
            Log("HEAD the game asked for the FreeTrack client path (thread %lu)",
                GetCurrentThreadId());
        }
        return ERROR_SUCCESS;
    }
    return g_originalRegOpenKeyExA(key, subKey, options, access, result);
}

LSTATUS WINAPI HookRegQueryValueExA(HKEY key, LPCSTR valueName, LPDWORD reserved, LPDWORD type,
                                    LPBYTE data, LPDWORD dataSize)
{
    if (key != kFreeTrackKey) {
        return g_originalRegQueryValueExA(key, valueName, reserved, type, data, dataSize);
    }
    if (!valueName || _stricmp(valueName, "Path") != 0 || !dataSize) {
        return ERROR_FILE_NOT_FOUND;
    }
    const std::string path = ModuleDirectory().string();
    const DWORD needed = static_cast<DWORD>(path.size() + 1);
    if (type) {
        *type = REG_SZ;
    }
    if (!data) {
        *dataSize = needed;
        return ERROR_SUCCESS;
    }
    if (*dataSize < needed) {
        *dataSize = needed;
        return ERROR_MORE_DATA;
    }
    std::memcpy(data, path.c_str(), needed);
    *dataSize = needed;
    return ERROR_SUCCESS;
}

LSTATUS WINAPI HookRegCloseKey(HKEY key)
{
    if (key == kFreeTrackKey) {
        return ERROR_SUCCESS;
    }
    return g_originalRegCloseKey(key);
}

bool InstallFreeTrackRegistryHooks()
{
    const HMODULE advapi = LoadLibraryW(L"advapi32.dll");
    if (!advapi) {
        return false;
    }
    void* open = reinterpret_cast<void*>(GetProcAddress(advapi, "RegOpenKeyExA"));
    void* query = reinterpret_cast<void*>(GetProcAddress(advapi, "RegQueryValueExA"));
    void* close = reinterpret_cast<void*>(GetProcAddress(advapi, "RegCloseKey"));
    if (!open || !query || !close) {
        Log("HEAD registry functions not found; FreeTrack path cannot be answered");
        return false;
    }
    // Close first: once Open can hand out our key, Close must already know it.
    return HookFunction(close, reinterpret_cast<void*>(&HookRegCloseKey),
                        reinterpret_cast<void**>(&g_originalRegCloseKey), "RegCloseKey") &&
           HookFunction(query, reinterpret_cast<void*>(&HookRegQueryValueExA),
                        reinterpret_cast<void**>(&g_originalRegQueryValueExA),
                        "RegQueryValueExA") &&
           HookFunction(open, reinterpret_cast<void*>(&HookRegOpenKeyExA),
                        reinterpret_cast<void**>(&g_originalRegOpenKeyExA), "RegOpenKeyExA");
}

}  // namespace

bool InstallHooks()
{
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MinHook initialization failed: %s", MH_StatusToString(status));
        return false;
    }
    if (GetConfig().enableVR && GetConfig().headTracking) {
        const bool client =
            std::filesystem::exists(ModuleDirectory() / L"FreeTrackClient64.dll");
        Log("HEAD FreeTrackClient64.dll next to dinput8.dll: %s", client ? "found" : "MISSING");
        if (client && !InstallFreeTrackRegistryHooks()) {
            Log("HEAD registry answer not installed; head look will not work");
        }
    }
    InstallResolutionHooks();
    return InstallSwapChainHooks();
}

}  // namespace a3vr
