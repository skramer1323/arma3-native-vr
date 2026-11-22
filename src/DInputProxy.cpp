// DirectInput proxy adapted from KCD1VR (MIT). See licenses/KCD1VR-LICENSE.txt.
// Defer initialization until DirectInput8Create runs outside the loader lock.

#include "Config.h"
#include "Hooks.h"
#include "InputBlock.h"
#include "Log.h"

#include <Windows.h>
#include <dinput.h>

#include <atomic>

namespace {

HMODULE g_realDInput = nullptr;
std::atomic<bool> g_initialized = false;

// PE TimeDateStamp and SizeOfImage of the tested Arma 3 executable.
constexpr const wchar_t* kVerifiedBuild = L"6ABB7A6F-02675000";

FARPROC ResolveRealDInputProc(const char* name)
{
    if (!g_realDInput) {
        wchar_t systemDirectory[MAX_PATH]{};
        GetSystemDirectoryW(systemDirectory, MAX_PATH);
        wchar_t realPath[MAX_PATH]{};
        wsprintfW(realPath, L"%s\\dinput8.dll", systemDirectory);
        g_realDInput = LoadLibraryW(realPath);
    }
    return g_realDInput ? GetProcAddress(g_realDInput, name) : nullptr;
}

void LogGameBuild()
{
    const HMODULE game = GetModuleHandleW(nullptr);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const BYTE*>(game) + dos->e_lfanew);
    wchar_t current[32]{};
    wsprintfW(current, L"%08X-%08X", nt->FileHeader.TimeDateStamp,
              nt->OptionalHeader.SizeOfImage);
    const bool match = lstrcmpiW(current, kVerifiedBuild) == 0;
    a3vr::Log("Arma 3 build %ls: %s (tested %ls = 2.22.0.154089)", current,
              match ? "matches tested build" : "untested build",
              kVerifiedBuild);
}

void InitializeMod()
{
    if (g_initialized.exchange(true)) {
        return;
    }
    a3vr::Log("==== Arma 3 Native VR " ARMA_NATIVE_VR_VERSION " ====");
    LogGameBuild();
    a3vr::LoadConfig();
    if (!a3vr::InstallHooks()) {
        a3vr::Log("DXGI hooks could not be installed; the mod does nothing this run");
    }
}

}  // namespace

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid,
                                               LPVOID* output, LPUNKNOWN outer)
{
    InitializeMod();
    using Function = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    const auto function = reinterpret_cast<Function>(ResolveRealDInputProc("DirectInput8Create"));
    const HRESULT result = function ? function(instance, version, iid, output, outer) : E_FAIL;
    if (SUCCEEDED(result) && output && *output) {
        a3vr::InputBlockInstall(*output);  // the menu needs to take the mouse and keys from the game
    }
    return result;
}

extern "C" HRESULT WINAPI DllCanUnloadNow()
{
    using Function = HRESULT(WINAPI*)();
    const auto function = reinterpret_cast<Function>(ResolveRealDInputProc("DllCanUnloadNow"));
    return function ? function() : S_FALSE;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* output)
{
    using Function = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    const auto function = reinterpret_cast<Function>(ResolveRealDInputProc("DllGetClassObject"));
    return function ? function(clsid, iid, output) : CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT WINAPI DllRegisterServer()
{
    using Function = HRESULT(WINAPI*)();
    const auto function = reinterpret_cast<Function>(ResolveRealDInputProc("DllRegisterServer"));
    return function ? function() : E_NOTIMPL;
}

extern "C" HRESULT WINAPI DllUnregisterServer()
{
    using Function = HRESULT(WINAPI*)();
    const auto function = reinterpret_cast<Function>(ResolveRealDInputProc("DllUnregisterServer"));
    return function ? function() : E_NOTIMPL;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        a3vr::SetSelfModule(module);
    }
    return TRUE;
}
