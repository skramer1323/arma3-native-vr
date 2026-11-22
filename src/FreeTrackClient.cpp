// FreeTrackClient64.dll shipped with Arma3NativeVR. arma3_x64.exe loads it from
// the folder named in HKCU\Software\Freetrack\FreetrackClient "Path" (dinput8.dll
// answers that registry read inside the game process) and polls FTGetData.
// It only copies the block that dinput8.dll writes. No threads, no file I/O.

#include "HeadTracking.h"

#include <cstring>

namespace {

HANDLE g_mapping = nullptr;
HANDLE g_mutex = nullptr;
std::uint8_t* g_shared = nullptr;
bool g_canCount = false;

bool OpenShared()
{
    if (g_shared) {
        return true;
    }
    g_mapping = OpenFileMappingA(FILE_MAP_WRITE, FALSE, a3vr::kFreeTrackMapping);
    if (!g_mapping) {
        return false;
    }
    g_shared = static_cast<std::uint8_t*>(
        MapViewOfFile(g_mapping, FILE_MAP_WRITE, 0, 0, a3vr::kFreeTrackMappingSize));
    g_canCount = g_shared != nullptr;
    if (!g_shared) {
        g_shared = static_cast<std::uint8_t*>(
            MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, sizeof(a3vr::FreeTrackData)));
    }
    if (!g_shared) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return false;
    }
    g_mutex = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, a3vr::kFreeTrackMutex);
    return true;
}

}  // namespace

extern "C" __declspec(dllexport) BOOL __stdcall FTGetData(a3vr::FreeTrackData* data)
{
    if (!data || !OpenShared()) {
        return FALSE;
    }
    const bool locked = g_mutex && WaitForSingleObject(g_mutex, 1) == WAIT_OBJECT_0;
    std::memcpy(data, g_shared, sizeof(a3vr::FreeTrackData));
    if (locked) {
        ReleaseMutex(g_mutex);
    }
    if (g_canCount) {
        InterlockedExchange(
            reinterpret_cast<volatile LONG*>(g_shared + a3vr::kFreeTrackLastReturnedOffset),
            static_cast<LONG>(data->dataId));
        InterlockedIncrement(
            reinterpret_cast<volatile LONG*>(g_shared + a3vr::kFreeTrackCallCounterOffset));
    }
    return TRUE;
}

extern "C" __declspec(dllexport) const char* __stdcall FTGetDllVersion()
{
    return "1.0.0.0";
}

extern "C" __declspec(dllexport) void __stdcall FTReportName(int)
{
}

extern "C" __declspec(dllexport) void __stdcall FTReportID(int)
{
}

extern "C" __declspec(dllexport) const char* __stdcall FTProvider()
{
    return "Arma3NativeVR";
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
