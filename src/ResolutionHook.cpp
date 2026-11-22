// Adapted from SnowRunner VR (MIT). See licenses/SnowRunner-VR-LICENSE.txt.
// Make the game's main window
// exactly the headset's per-eye resolution, so the game renders one eye per frame at 1:1 pixels.

#include "ResolutionHook.h"

#include "Config.h"
#include "Log.h"

#include <MinHook.h>
#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace a3vr {
namespace {

std::atomic<int> g_forceW{0};
std::atomic<int> g_forceH{0};
std::atomic<bool> g_installed{false};

std::uintptr_t g_exeBase = 0;
std::uintptr_t g_exeEnd = 0;

void InitExeRange()
{
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) {
        return;
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<std::uint8_t*>(exe) +
                                                     dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }
    g_exeBase = reinterpret_cast<std::uintptr_t>(exe);
    g_exeEnd = g_exeBase + nt->OptionalHeader.SizeOfImage;
}

// Only calls made by game code count: our own overlay, the D3D runtime and anything else that
// creates a window stay out of it.
bool CallerIsExe(void* returnAddress)
{
    const auto address = reinterpret_cast<std::uintptr_t>(returnAddress);
    return g_exeBase && address >= g_exeBase && address < g_exeEnd;
}

// The window-manager ceiling has to clear the requested client size plus the window frame.
bool CeilingSize(LONG& cx, LONG& cy)
{
    const int w = g_forceW.load();
    const int h = g_forceH.load();
    if (w <= 0 || h <= 0) {
        return false;
    }
    cx = static_cast<LONG>(w) + 512;
    cy = static_cast<LONG>(h) + 512;
    return true;
}

using CreateWindowExWFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND,
                                        HMENU, HINSTANCE, LPVOID);
using RegisterClassWFn = ATOM(WINAPI*)(const WNDCLASSW*);
using RegisterClassExWFn = ATOM(WINAPI*)(const WNDCLASSEXW*);

CreateWindowExWFn g_realCreateWindowExW = nullptr;
RegisterClassWFn g_realRegisterClassW = nullptr;
RegisterClassExWFn g_realRegisterClassExW = nullptr;

struct Subclassed {
    std::atomic<HWND> hwnd{nullptr};
    std::atomic<WNDPROC> original{nullptr};
};
constexpr int kMaxSubclassed = 32;
Subclassed g_sub[kMaxSubclassed]{};
std::atomic<int> g_subCount{0};

struct ClassEntry {
    ATOM atom;
    WNDPROC original;
};
constexpr int kMaxClasses = 16;
ClassEntry g_cls[kMaxClasses]{};
std::atomic<int> g_clsCount{0};

WNDPROC ClassOriginalForAtom(ATOM atom)
{
    if (!atom) {
        return nullptr;
    }
    for (const ClassEntry& entry : g_cls) {
        if (entry.atom == atom) {
            return entry.original;
        }
    }
    return nullptr;
}

WNDPROC OriginalProcFor(HWND hwnd)
{
    for (const Subclassed& entry : g_sub) {
        if (entry.hwnd == hwnd) {
            return entry.original.load();
        }
    }
    return nullptr;
}

void RememberWindow(HWND hwnd, WNDPROC original)
{
    const int slot = g_subCount.fetch_add(1);
    if (slot >= kMaxSubclassed) {
        return;
    }
    g_sub[slot].original = original;
    g_sub[slot].hwnd = hwnd;  // published last
}

// ---- virtual cursor ----------------------------------------------------------------------
// When the game window is larger than the desktop, the Windows cursor cannot reach all of it. The
// real mouse movement (raw input) drives a virtual cursor over the whole client area; the game is
// told that position (GetCursorPos, SetCursorPos, window mouse messages) and the headset overlay
// draws the cursor there.
std::atomic<HWND> g_vcWindow{nullptr};
std::mutex g_cursorMutex;
bool g_vcWasActive = false;
float g_vcX = 0;
float g_vcY = 0;
int g_vcSource = 0;

using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using GetCursorInfoFn = BOOL(WINAPI*)(PCURSORINFO);
GetCursorPosFn g_realGetCursorPos = nullptr;
SetCursorPosFn g_realSetCursorPos = nullptr;
GetCursorInfoFn g_realGetCursorInfo = nullptr;

// The picture the game draws is what the mouse coordinates are measured in (the engine does not
// rescale them): its size comes from the swapchain, set by the headset code every frame.
int g_vcPictureW = 0;
int g_vcPictureH = 0;
std::atomic<UINT> g_resizeMessage{0};

// True while the virtual cursor stands in for the Windows cursor: the window cannot show the whole
// picture (picture larger than the desktop or than the window client), and the cursor is shown.
// `pictureOut` receives {0, 0, pictureW, pictureH}.
bool VirtualCursorOn(RECT* pictureOut = nullptr)
{
    const HWND window = g_vcWindow.load();
    const int pictureW = g_vcPictureW;
    const int pictureH = g_vcPictureH;
    const auto deactivate = [] {
        g_vcWasActive = false;
        g_vcSource = 0;
        return false;
    };
    if (!window || !GetConfig().virtualCursor || pictureW <= 0 || pictureH <= 0 ||
        GetAncestor(GetForegroundWindow(), GA_ROOT) != GetAncestor(window, GA_ROOT)) {
        return deactivate();
    }
    RECT client{};
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) {
        return deactivate();
    }
    POINT topLeft{};
    POINT bottomRight{client.right - 1, client.bottom - 1};
    ClientToScreen(window, &topLeft);
    ClientToScreen(window, &bottomRight);
    if (client.right == pictureW && client.bottom == pictureH &&
        MonitorFromPoint(topLeft, MONITOR_DEFAULTTONULL) &&
        MonitorFromPoint(bottomRight, MONITOR_DEFAULTTONULL)) {
        return deactivate();
    }
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    if (!g_realGetCursorInfo || !g_realGetCursorInfo(&info) || !(info.flags & CURSOR_SHOWING)) {
        return deactivate();
    }
    if (!g_vcWasActive) {
        g_vcWasActive = true;
        // Becomes active: continue from where the real cursor is (client pixels scaled to the
        // picture).
        POINT point = info.ptScreenPos;
        ScreenToClient(window, &point);
        const double scaleX = client.right > 0 ? static_cast<double>(pictureW) / client.right : 1.0;
        const double scaleY = client.bottom > 0 ? static_cast<double>(pictureH) / client.bottom : 1.0;
        g_vcX = std::clamp(static_cast<float>(point.x * scaleX), 0.0F, static_cast<float>(pictureW - 1));
        g_vcY = std::clamp(static_cast<float>(point.y * scaleY), 0.0F, static_cast<float>(pictureH - 1));
    }
    if (pictureOut) {
        *pictureOut = {0, 0, pictureW, pictureH};
    }
    return true;
}

POINT VirtualCursorScreen(HWND window)
{
    POINT origin{0, 0};
    ClientToScreen(window, &origin);
    return {origin.x + std::lround(g_vcX), origin.y + std::lround(g_vcY)};
}

BOOL WINAPI DetourGetCursorPos(LPPOINT point)
{
    if (point && CallerIsExe(_ReturnAddress())) {
        std::scoped_lock lock(g_cursorMutex);
        if (VirtualCursorOn()) {
            *point = VirtualCursorScreen(g_vcWindow.load());
            return TRUE;
        }
    }
    return g_realGetCursorPos(point);
}

BOOL WINAPI DetourGetCursorInfo(PCURSORINFO info)
{
    const BOOL result = g_realGetCursorInfo(info);
    if (result && info && CallerIsExe(_ReturnAddress())) {
        std::scoped_lock lock(g_cursorMutex);
        if (VirtualCursorOn()) {
            info->ptScreenPos = VirtualCursorScreen(g_vcWindow.load());
        }
    }
    return result;
}

BOOL WINAPI DetourSetCursorPos(int x, int y)
{
    if (CallerIsExe(_ReturnAddress())) {
        std::scoped_lock lock(g_cursorMutex);
        RECT picture{};
        if (VirtualCursorOn(&picture)) {
            POINT point{x, y};
            ScreenToClient(g_vcWindow.load(), &point);
            g_vcX = static_cast<float>(std::clamp(point.x, 0L, picture.right - 1));
            g_vcY = static_cast<float>(std::clamp(point.y, 0L, picture.bottom - 1));
            return TRUE;
        }
    }
    return g_realSetCursorPos(x, y);
}

// Mouse messages carry client (or, for the wheel, screen) coordinates of the real cursor.
LPARAM RewriteMouseMessage(HWND hwnd, UINT message, LPARAM lParam)
{
    const bool buttonOrMove = (message >= WM_MOUSEMOVE && message <= WM_MBUTTONDBLCLK) ||
                              (message >= WM_XBUTTONDOWN && message <= WM_XBUTTONDBLCLK);
    const bool wheel = message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL;
    if (!(buttonOrMove || wheel) || hwnd != g_vcWindow.load()) {
        return lParam;
    }
    std::scoped_lock lock(g_cursorMutex);
    if (!VirtualCursorOn()) {
        return lParam;
    }
    if (wheel) {
        const POINT screen = VirtualCursorScreen(hwnd);
        return MAKELPARAM(screen.x, screen.y);
    }
    return MAKELPARAM(std::lround(g_vcX), std::lround(g_vcY));
}

void ResizeMainWindow(HWND window, int width, int height)
{
    RECT client{}, outer{};
    if (IsIconic(window) || !GetClientRect(window, &client) || !GetWindowRect(window, &outer)) {
        return;
    }
    const int outerWidth = width + (outer.right - outer.left) - client.right;
    const int outerHeight = height + (outer.bottom - outer.top) - client.bottom;
    constexpr UINT flags = SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE;
    SetWindowPos(window, nullptr, 0, 0, outerWidth, outerHeight, flags);
    GetClientRect(window, &client);
    if (client.right != width || client.bottom != height) {
        SetWindowPos(window, nullptr, 0, 0, outerWidth, outerHeight, flags | SWP_NOSENDCHANGING);
        GetClientRect(window, &client);
    }
    Log("RESOLUTION requested %dx%d; got client %ldx%ld", width, height, client.right, client.bottom);
}

template <bool Unicode>
LRESULT CALLBACK WndProcImpl(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    WNDPROC original = OriginalProcFor(hwnd);
    if (!original) {
        original = ClassOriginalForAtom(static_cast<ATOM>(GetClassWord(hwnd, GCW_ATOM)));
        if (original) {
            RememberWindow(hwnd, original);
        }
    }
    if (message != 0 && message == g_resizeMessage.load() && hwnd == g_vcWindow.load()) {
        const auto width = static_cast<int>(wParam);
        const auto height = static_cast<int>(lParam);
        if (width == g_forceW.load() && height == g_forceH.load()) {
            ResizeMainWindow(hwnd, width, height);
        }
        return 0;
    }
    lParam = RewriteMouseMessage(hwnd, message, lParam);
    const LRESULT result = original ? (Unicode ? CallWindowProcW(original, hwnd, message, wParam, lParam)
                                               : CallWindowProcA(original, hwnd, message, wParam, lParam))
                                    : (Unicode ? DefWindowProcW(hwnd, message, wParam, lParam)
                                               : DefWindowProcA(hwnd, message, wParam, lParam));
    // After the real handler: DefWindowProc is what fills MINMAXINFO in.
    if (message == WM_GETMINMAXINFO && lParam) {
        LONG cx = 0;
        LONG cy = 0;
        if (CeilingSize(cx, cy)) {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            if (info->ptMaxTrackSize.x < cx) {
                info->ptMaxTrackSize.x = cx;
            }
            if (info->ptMaxTrackSize.y < cy) {
                info->ptMaxTrackSize.y = cy;
            }
            if (info->ptMaxSize.x < cx) {
                info->ptMaxSize.x = cx;
            }
            if (info->ptMaxSize.y < cy) {
                info->ptMaxSize.y = cy;
            }
        }
    }
    if (message == WM_NCDESTROY) {
        for (Subclassed& entry : g_sub) {
            if (entry.hwnd == hwnd) {
                entry.hwnd = nullptr;
                break;
            }
        }
    }
    return result;
}

LRESULT CALLBACK DetourWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    return WndProcImpl<true>(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK DetourWndProcA(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    return WndProcImpl<false>(hwnd, message, wParam, lParam);
}

// Substitute the window procedure at registration time so the ceiling exists before the window.
ATOM WINAPI DetourRegisterClassW(const WNDCLASSW* wc)
{
    void* returnAddress = _ReturnAddress();
    if (!wc || !wc->lpfnWndProc || !CallerIsExe(returnAddress)) {
        return g_realRegisterClassW(wc);
    }
    const int slot = g_clsCount.fetch_add(1);
    if (slot >= kMaxClasses) {
        return g_realRegisterClassW(wc);
    }
    WNDCLASSW copy = *wc;
    copy.lpfnWndProc = &DetourWndProc;
    const ATOM atom = g_realRegisterClassW(&copy);
    if (!atom) {
        return g_realRegisterClassW(wc);
    }
    g_cls[slot].original = wc->lpfnWndProc;
    g_cls[slot].atom = atom;  // published last
    Log("RESOLUTION window class atom=0x%04X hooked at registration", atom);
    return atom;
}

// Arma may register its window class with the Ex variant.
ATOM WINAPI DetourRegisterClassExW(const WNDCLASSEXW* wc)
{
    void* returnAddress = _ReturnAddress();
    if (!wc || !wc->lpfnWndProc || !CallerIsExe(returnAddress)) {
        return g_realRegisterClassExW(wc);
    }
    const int slot = g_clsCount.fetch_add(1);
    if (slot >= kMaxClasses) {
        return g_realRegisterClassExW(wc);
    }
    WNDCLASSEXW copy = *wc;
    copy.lpfnWndProc = &DetourWndProc;
    const ATOM atom = g_realRegisterClassExW(&copy);
    if (!atom) {
        return g_realRegisterClassExW(wc);
    }
    g_cls[slot].original = wc->lpfnWndProc;
    g_cls[slot].atom = atom;
    Log("RESOLUTION window class atom=0x%04X hooked at registration (Ex)", atom);
    return atom;
}

HWND WINAPI DetourCreateWindowExW(DWORD exStyle, LPCWSTR cls, LPCWSTR name, DWORD style, int x,
                                  int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst,
                                  LPVOID param)
{
    void* returnAddress = _ReturnAddress();
    static std::atomic<bool> forced{false};
    const int fw = g_forceW.load();
    const int fh = g_forceH.load();
    if (CallerIsExe(returnAddress)) {
        static std::atomic<int> windowLogs{0};
        if (windowLogs.fetch_add(1) < 12) {
            Log("RESOLUTION CreateWindowExW by the game: style=0x%08lX ex=0x%08lX at (%d,%d) size "
                "%dx%d parent=%p menu=%p", static_cast<unsigned long>(style),
                static_cast<unsigned long>(exStyle), x, y, w, h, static_cast<void*>(parent),
                static_cast<void*>(menu));
        }
    }
    if (fw > 0 && fh > 0 && CallerIsExe(returnAddress) && !(style & WS_CHILD) &&
        (style & WS_CAPTION) && w > 0 && h > 0) {
        bool expected = false;
        if (forced.compare_exchange_strong(expected, true)) {
            RECT want{0, 0, fw, fh};
            AdjustWindowRectEx(&want, style, menu != nullptr, exStyle);
            const int newW = want.right - want.left;
            const int newH = want.bottom - want.top;
            Log("RESOLUTION main window %dx%d -> %dx%d (client %dx%d)", w, h, newW, newH, fw, fh);
            w = newW;
            h = newH;
        }
    }
    HWND hwnd = g_realCreateWindowExW(exStyle, cls, name, style, x, y, w, h, parent, menu, inst,
                                      param);
    if (!hwnd || !CallerIsExe(returnAddress) || (style & WS_CHILD)) {
        return hwnd;
    }
    const bool classHooked =
        ClassOriginalForAtom(static_cast<ATOM>(GetClassWord(hwnd, GCW_ATOM))) != nullptr;
    if (!classHooked) {
        const int slot = g_subCount.fetch_add(1);
        if (slot >= kMaxSubclassed) {
            return hwnd;
        }
        auto previous = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DetourWndProc)));
        if (!previous) {
            return hwnd;
        }
        g_sub[slot].original = previous;
        g_sub[slot].hwnd = hwnd;
    }
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    const int gotW = rect.right - rect.left;
    const int gotH = rect.bottom - rect.top;
    if (gotW != w || gotH != h) {
        Log("RESOLUTION game window asked %dx%d, got %dx%d [class hook %s]", w, h, gotW, gotH,
            classHooked ? "ACTIVE" : "missed, subclassed after the fact");
    }
    // The window was born clamped: ask again now that our procedure is on it.
    if (w > 0 && h > 0 && (gotW < w || gotH < h) && fw > 0 && !(style & WS_POPUP)) {
        SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT after{};
        GetWindowRect(hwnd, &after);
        int nowW = after.right - after.left;
        int nowH = after.bottom - after.top;
        if (nowW < w || nowH < h) {
            SetWindowPos(hwnd, nullptr, 0, 0, w, h,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
            GetWindowRect(hwnd, &after);
            nowW = after.right - after.left;
            nowH = after.bottom - after.top;
        }
        Log("RESOLUTION corrective resize to %dx%d -> %dx%d %s", w, h, nowW, nowH,
            (nowW >= w && nowH >= h) ? "OK" : "STILL CLAMPED");
    }
    return hwnd;
}

template <typename Fn>
bool HookApi(const wchar_t* module, const char* name, Fn detour, Fn* original)
{
    void* target = nullptr;
    if (MH_CreateHookApiEx(module, name, reinterpret_cast<void*>(detour),
                           reinterpret_cast<void**>(original), &target) != MH_OK) {
        return false;
    }
    return MH_EnableHook(target) == MH_OK;
}

}  // namespace

void InstallResolutionHooks()
{
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) {
        return;
    }
    InitExeRange();
    g_resizeMessage.store(RegisterWindowMessageW(L"Arma3NativeVR.ResizeMainWindow"));
    if (GetConfig().virtualCursor) {
        const bool getOk =
            HookApi(L"user32", "GetCursorPos", &DetourGetCursorPos, &g_realGetCursorPos);
        const bool infoOk =
            HookApi(L"user32", "GetCursorInfo", &DetourGetCursorInfo, &g_realGetCursorInfo);
        const bool setOk =
            HookApi(L"user32", "SetCursorPos", &DetourSetCursorPos, &g_realSetCursorPos);
        Log("CURSOR virtual cursor hooks: GetCursorPos %s, GetCursorInfo %s, SetCursorPos %s",
            getOk ? "ok" : "FAILED", infoOk ? "ok" : "FAILED", setOk ? "ok" : "FAILED");
    }
    int wantW = GetConfig().forceGameWidth;
    int wantH = GetConfig().forceGameHeight;
    if (GetConfig().virtualBackBuffer || wantW <= 0 || wantH <= 0) {
        Log("RESOLUTION leaving the desktop window at the game's requested size");
        return;
    }
    // The Windows cursor cannot leave the desktop, so a window larger than the desktop has parts
    // of the menu the mouse cannot reach. Fit the wanted per-eye size (same aspect) into the
    // desktop; a taller virtual display (custom resolution) gives the full size automatically.
    // AllowOversizeWindow=1 turns the fitting off.
    {
        int fitW = wantW;
        int fitH = wantH;
        FitWindowToDesktop(wantW, wantH, fitW, fitH);  // no-op while the virtual cursor is on
        if (fitW != wantW || fitH != wantH) {
            Log("RESOLUTION wanted %dx%d does not fit the %dx%d desktop: window fitted to %dx%d "
                "(a larger virtual display gives more)",
                wantW, wantH, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), fitW,
                fitH);
        }
        wantW = fitW;
        wantH = fitH;
    }
    g_forceW.store(wantW);
    g_forceH.store(wantH);
    const bool createOk =
        HookApi(L"user32", "CreateWindowExW", &DetourCreateWindowExW, &g_realCreateWindowExW);
    const bool classOk =
        HookApi(L"user32", "RegisterClassW", &DetourRegisterClassW, &g_realRegisterClassW);
    const bool classExOk =
        HookApi(L"user32", "RegisterClassExW", &DetourRegisterClassExW, &g_realRegisterClassExW);
    Log("RESOLUTION forcing %dx%d: CreateWindowExW %s, RegisterClassW %s, RegisterClassExW %s",
        g_forceW.load(), g_forceH.load(), createOk ? "ok" : "FAILED", classOk ? "ok" : "FAILED",
        classExOk ? "ok" : "FAILED");
}

}  // namespace a3vr

namespace a3vr {

void FitWindowToDesktop(int wantW, int wantH, int& outW, int& outH)
{
    outW = wantW;
    outH = wantH;
    if (wantW <= 0 || wantH <= 0 || GetConfig().allowOversizeWindow || GetConfig().virtualCursor) {
        return;
    }
    // The Windows cursor cannot leave the desktop, so a window larger than the desktop has parts
    // the mouse cannot reach. Same aspect, room left for the window frame.
    const double deskW = static_cast<double>(GetSystemMetrics(SM_CXSCREEN)) - 16.0;
    const double deskH = static_cast<double>(GetSystemMetrics(SM_CYSCREEN)) - 64.0;
    const double scale = std::min(1.0, std::min(deskW / wantW, deskH / wantH));
    if (scale < 1.0 && scale > 0.2) {
        outW = static_cast<int>(wantW * scale) & ~1;
        outH = static_cast<int>(wantH * scale) & ~1;
    }
}

}  // namespace a3vr

namespace a3vr {

void VirtualCursorSetWindow(HWND window)
{
    if (g_vcWindow.load() == window) {
        return;
    }
    std::scoped_lock lock(g_cursorMutex);
    if (g_vcWindow.exchange(window) != window) {
        g_vcWasActive = false;
        g_vcSource = 0;
    }
}

bool VirtualCursorActive()
{
    std::scoped_lock lock(g_cursorMutex);
    return VirtualCursorOn();
}

bool VirtualCursorClientPosition(POINT& point, int& width, int& height)
{
    std::scoped_lock lock(g_cursorMutex);
    RECT picture{};
    if (!VirtualCursorOn(&picture)) {
        return false;
    }
    point = {std::lround(g_vcX), std::lround(g_vcY)};
    width = picture.right;
    height = picture.bottom;
    return true;
}

void VirtualCursorFeed(int dx, int dy, int source)
{
    if (dx == 0 && dy == 0) {
        return;
    }
    std::scoped_lock lock(g_cursorMutex);
    RECT picture{};
    if (!VirtualCursorOn(&picture)) {
        return;
    }
    // A cursor session consumes one input stream, even if the game also polls the others.
    if (g_vcSource == 0) {
        g_vcSource = source;
    }
    if (g_vcSource != source) {
        return;
    }
    const float speed = GetConfig().virtualCursorSpeed;
    g_vcX = std::clamp(g_vcX + dx * speed, 0.0F, static_cast<float>(picture.right - 1));
    g_vcY = std::clamp(g_vcY + dy * speed, 0.0F, static_cast<float>(picture.bottom - 1));
}

}  // namespace a3vr

namespace a3vr {

void VirtualCursorSetPicture(int width, int height)
{
    std::scoped_lock lock(g_cursorMutex);
    if (width == g_vcPictureW && height == g_vcPictureH) {
        return;
    }
    if (g_vcWasActive && width > 0 && height > 0 && g_vcPictureW > 0 && g_vcPictureH > 0) {
        g_vcX = std::clamp(g_vcX * width / g_vcPictureW, 0.0F, static_cast<float>(width - 1));
        g_vcY = std::clamp(g_vcY * height / g_vcPictureH, 0.0F, static_cast<float>(height - 1));
    }
    g_vcPictureW = width;
    g_vcPictureH = height;
}

}  // namespace a3vr

namespace a3vr {

// The engine can create its real window before the resolution hooks are installed.
void AdoptMainWindow(HWND window, int width, int height)
{
    static HWND boundWindow = nullptr;  // called on the serialized main Present path
    static int requestedWidth = 0, requestedHeight = 0;
    VirtualCursorSetWindow(window);
    if (!window) {
        return;
    }

    if (window != boundWindow) {
        const bool unicode = IsWindowUnicode(window) != FALSE;
        const WNDPROC detour = unicode ? &DetourWndProc : &DetourWndProcA;
        const auto original = reinterpret_cast<WNDPROC>(unicode ? GetWindowLongPtrW(window, GWLP_WNDPROC)
                                                                 : GetWindowLongPtrA(window, GWLP_WNDPROC));
        if (original != detour) {
            const int slot = g_subCount.fetch_add(1);
            if (!original || slot >= kMaxSubclassed) {
                return;
            }
            g_sub[slot].original.store(original);
            g_sub[slot].hwnd.store(window);
            const auto previous = unicode
                ? SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(detour))
                : SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(detour));
            if (!previous) {
                g_sub[slot].hwnd.store(nullptr);
                Log("RESOLUTION main window procedure could not be attached (error %lu)", GetLastError());
                return;
            }
            g_sub[slot].original.store(reinterpret_cast<WNDPROC>(previous));
        }
        boundWindow = window;
        requestedWidth = requestedHeight = 0;
        Log("RESOLUTION attached to the main swapchain window");
    }

    RECT client{};
    const UINT message = g_resizeMessage.load();
    if (!message || width <= 0 || height <= 0 || IsIconic(window) || !GetClientRect(window, &client)) {
        return;
    }
    FitWindowToDesktop(width, height, width, height);
    g_forceW.store(width);
    g_forceH.store(height);
    if (client.right == width && client.bottom == height) {
        requestedWidth = requestedHeight = 0;
    } else if ((requestedWidth != width || requestedHeight != height) &&
               PostMessageW(window, message, static_cast<WPARAM>(width), static_cast<LPARAM>(height))) {
        // Resize on the window thread outside Present, where WM_SIZE may call DXGI ResizeBuffers.
        requestedWidth = width;
        requestedHeight = height;
    }
}

}  // namespace a3vr
