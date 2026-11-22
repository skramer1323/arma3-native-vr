#include "InputBlock.h"

#include "Log.h"
#include "ResolutionHook.h"

#include <MinHook.h>
#include <dinput.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace a3vr {
namespace {

enum class Kind { Mouse, Keyboard };

std::atomic<bool> g_active{false};
std::atomic<int> g_dx{0};
std::atomic<int> g_dy{0};
std::atomic<int> g_dz{0};
std::atomic<bool> g_button[2]{false, false};
std::mutex g_mutex;
std::unordered_map<void*, Kind> g_devices;  // device object -> kind

using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, IUnknown*);
using GetStateFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using RawInputFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);

CreateDeviceFn g_createDevice[2] = {nullptr, nullptr};
void* g_hookedCreate[2] = {nullptr, nullptr};
GetStateFn g_getState[2] = {nullptr, nullptr};
GetDataFn g_getData[2] = {nullptr, nullptr};
void* g_hookedState[2] = {nullptr, nullptr};
void* g_hookedData[2] = {nullptr, nullptr};
RawInputFn g_rawInput = nullptr;
bool g_rawHooked = false;

bool Lookup(void* device, Kind& kind)
{
    std::scoped_lock lock(g_mutex);
    const auto found = g_devices.find(device);
    if (found == g_devices.end()) {
        return false;
    }
    kind = found->second;
    return true;
}

template <int N>
HRESULT STDMETHODCALLTYPE HookGetDeviceState(void* self, DWORD size, LPVOID data)
{
    const HRESULT result = g_getState[N](self, size, data);
    Kind kind{};
    const bool known = SUCCEEDED(result) && data && Lookup(self, kind);
    if (!g_active.load() && known && kind == Kind::Mouse && size >= 16) {
        const auto* mouse = static_cast<const DIMOUSESTATE*>(data);
        VirtualCursorFeed(mouse->lX, mouse->lY, 2);
    }
    if (!g_active.load() || !known) {
        return result;
    }
    if (kind == Kind::Mouse && size >= 16) {
        const auto* state = static_cast<const DIMOUSESTATE*>(data);
        g_dx.fetch_add(state->lX);
        g_dy.fetch_add(state->lY);
        g_dz.fetch_add(state->lZ);
        g_button[0].store((state->rgbButtons[0] & 0x80) != 0);
        g_button[1].store((state->rgbButtons[1] & 0x80) != 0);
    }
    std::memset(data, 0, size);
    return result;
}

template <int N>
HRESULT STDMETHODCALLTYPE HookGetDeviceData(void* self, DWORD size, LPDIDEVICEOBJECTDATA data,
                                            LPDWORD count, DWORD flags)
{
    const HRESULT result = g_getData[N](self, size, data, count, flags);
    Kind kind{};
    const bool known = SUCCEEDED(result) && count && Lookup(self, kind);
    if (!g_active.load() && known && kind == Kind::Mouse && data && (flags & DIGDD_PEEK) == 0) {
        int sumX = 0;
        int sumY = 0;
        for (DWORD i = 0; i < *count; ++i) {
            const DIDEVICEOBJECTDATA& item = *reinterpret_cast<const DIDEVICEOBJECTDATA*>(
                reinterpret_cast<const BYTE*>(data) + i * size);
            if (item.dwOfs == DIMOFS_X) {
                sumX += static_cast<int>(item.dwData);
            } else if (item.dwOfs == DIMOFS_Y) {
                sumY += static_cast<int>(item.dwData);
            }
        }
        VirtualCursorFeed(sumX, sumY, 3);
    }
    if (!g_active.load() || !known) {
        return result;
    }
    if (kind == Kind::Mouse && data) {
        for (DWORD i = 0; i < *count; ++i) {
            const DIDEVICEOBJECTDATA& item = *reinterpret_cast<const DIDEVICEOBJECTDATA*>(
                reinterpret_cast<const BYTE*>(data) + i * size);
            if (item.dwOfs == DIMOFS_X) {
                g_dx.fetch_add(static_cast<int>(item.dwData));
            } else if (item.dwOfs == DIMOFS_Y) {
                g_dy.fetch_add(static_cast<int>(item.dwData));
            } else if (item.dwOfs == DIMOFS_Z) {
                g_dz.fetch_add(static_cast<int>(item.dwData));
            } else if (item.dwOfs == DIMOFS_BUTTON0) {
                g_button[0].store((item.dwData & 0x80) != 0);
            } else if (item.dwOfs == DIMOFS_BUTTON1) {
                g_button[1].store((item.dwData & 0x80) != 0);
            }
        }
    }
    if ((flags & DIGDD_PEEK) == 0) {
        *count = 0;  // consumed: the game sees nothing
    }
    return result;
}

template <int N>
void HookSlot(void* stateTarget, void* dataTarget)
{
    const bool state = MH_CreateHook(stateTarget, reinterpret_cast<void*>(&HookGetDeviceState<N>),
                                     reinterpret_cast<void**>(&g_getState[N])) == MH_OK;
    const bool data = MH_CreateHook(dataTarget, reinterpret_cast<void*>(&HookGetDeviceData<N>),
                                    reinterpret_cast<void**>(&g_getData[N])) == MH_OK;
    if (state && data && MH_EnableHook(stateTarget) == MH_OK && MH_EnableHook(dataTarget) == MH_OK) {
        g_hookedState[N] = stateTarget;
        g_hookedData[N] = dataTarget;
        Log("INPUT device methods hooked in slot %d (GetDeviceState %p, GetDeviceData %p)", N,
            stateTarget, dataTarget);
    } else {
        Log("INPUT device hooks failed in slot %d; the menu will not block that device", N);
    }
}

// Mouse and keyboard may use different method bodies: hook every distinct pair (two at most).
void HookDeviceMethods(void* device)
{
    void** vtable = *reinterpret_cast<void***>(device);
    // IDirectInputDevice8: 9 = GetDeviceState, 10 = GetDeviceData.
    void* stateTarget = vtable[9];
    void* dataTarget = vtable[10];
    for (int i = 0; i < 2; ++i) {
        if (g_hookedState[i] == stateTarget && g_hookedData[i] == dataTarget) {
            return;
        }
    }
    if (!g_hookedState[0]) {
        HookSlot<0>(stateTarget, dataTarget);
    } else if (!g_hookedState[1]) {
        HookSlot<1>(stateTarget, dataTarget);
    }
}

template <int N>
HRESULT STDMETHODCALLTYPE HookCreateDevice(void* self, REFGUID guid, void** device,
                                           IUnknown* outer)
{
    const HRESULT result = g_createDevice[N](self, guid, device, outer);
    if (SUCCEEDED(result) && device && *device) {
        Kind kind{};
        bool known = false;
        // The game creates devices from enumerated instance GUIDs, not GUID_SysMouse: ask the
        // device what it is.
        DIDEVCAPS caps{};
        caps.dwSize = sizeof(DIDEVCAPS);
        if (SUCCEEDED(static_cast<IDirectInputDevice8W*>(*device)->GetCapabilities(&caps))) {
            const DWORD type = GET_DIDEVICE_TYPE(caps.dwDevType);
            if (type == DI8DEVTYPE_MOUSE) {
                kind = Kind::Mouse;
                known = true;
            } else if (type == DI8DEVTYPE_KEYBOARD) {
                kind = Kind::Keyboard;
                known = true;
            }
        }
        (void)guid;
        if (known) {
            {
                std::scoped_lock lock(g_mutex);
                g_devices[*device] = kind;
            }
            HookDeviceMethods(*device);
            Log("INPUT %s device %p seen", kind == Kind::Mouse ? "mouse" : "keyboard", *device);
        }
    }
    return result;
}

template <int N>
void HookCreateSlot(void* target)
{
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookCreateDevice<N>),
                      reinterpret_cast<void**>(&g_createDevice[N])) == MH_OK &&
        MH_EnableHook(target) == MH_OK) {
        g_hookedCreate[N] = target;
        Log("INPUT IDirectInput8::CreateDevice hooked in slot %d (%p)", N, target);
    } else {
        Log("INPUT CreateDevice hook failed in slot %d", N);
    }
}

// The game may also read the mouse through raw input: take the movement for the menu and give the
// game an empty record while the menu is open.
UINT WINAPI HookGetRawInputData(HRAWINPUT handle, UINT command, LPVOID data, PUINT size,
                                UINT headerSize)
{
    const UINT result = g_rawInput(handle, command, data, size, headerSize);
    if (!g_active.load() && data && command == RID_INPUT && result != static_cast<UINT>(-1)) {
        const auto* record = static_cast<const RAWINPUT*>(data);
        if (record->header.dwType == RIM_TYPEMOUSE &&
            (record->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
            VirtualCursorFeed(record->data.mouse.lLastX, record->data.mouse.lLastY, 1);
        }
    }
    if (!g_active.load() || !data || command != RID_INPUT || result == static_cast<UINT>(-1)) {
        return result;
    }
    auto* raw = static_cast<RAWINPUT*>(data);
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("INPUT raw input is read by the game (type %lu)", raw->header.dwType);
    }
    if (raw->header.dwType == RIM_TYPEMOUSE) {
        RAWMOUSE& mouse = raw->data.mouse;
        if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
            g_dx.fetch_add(mouse.lLastX);
            g_dy.fetch_add(mouse.lLastY);
        }
        if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) {
            g_button[0] = true;
        }
        if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) {
            g_button[0] = false;
        }
        if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) {
            g_button[1] = true;
        }
        if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP) {
            g_button[1] = false;
        }
        if (mouse.usButtonFlags & RI_MOUSE_WHEEL) {
            g_dz.fetch_add(static_cast<SHORT>(mouse.usButtonData));
        }
        mouse.lLastX = 0;
        mouse.lLastY = 0;
        mouse.usButtonFlags = 0;
        mouse.usButtonData = 0;
    } else if (raw->header.dwType == RIM_TYPEKEYBOARD) {
        raw->data.keyboard.VKey = 0;
        raw->data.keyboard.MakeCode = 0;
        raw->data.keyboard.Flags |= RI_KEY_BREAK;
    }
    return result;
}

}  // namespace

void InputBlockInstall(void* directInput)
{
    if (!g_rawHooked) {
        g_rawHooked = true;
        void* target = reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetRawInputData"));
        if (target && MH_CreateHook(target, reinterpret_cast<void*>(&HookGetRawInputData),
                                    reinterpret_cast<void**>(&g_rawInput)) == MH_OK &&
            MH_EnableHook(target) == MH_OK) {
            Log("INPUT GetRawInputData hooked");
        } else {
            Log("INPUT GetRawInputData hook failed");
        }
    }
    if (!directInput) {
        return;
    }
    // IDirectInput8 (the A and W objects have different method tables): 3 = CreateDevice.
    void* target = (*reinterpret_cast<void***>(directInput))[3];
    for (int i = 0; i < 2; ++i) {
        if (g_hookedCreate[i] == target) {
            return;
        }
    }
    if (!g_hookedCreate[0]) {
        HookCreateSlot<0>(target);
    } else if (!g_hookedCreate[1]) {
        HookCreateSlot<1>(target);
    }
}

void InputBlockSetActive(bool active)
{
    if (g_active.exchange(active) != active) {
        g_dx = g_dy = g_dz = 0;
        g_button[0] = g_button[1] = false;
    }
}

bool InputBlockActive()
{
    return g_active.load();
}

void InputBlockTakeMouse(float& dx, float& dy, float& wheel)
{
    dx = static_cast<float>(g_dx.exchange(0));
    dy = static_cast<float>(g_dy.exchange(0));
    wheel = static_cast<float>(g_dz.exchange(0));
}

bool InputBlockMouseButton(int index)
{
    return index >= 0 && index < 2 && g_button[index].load();
}

}  // namespace a3vr
