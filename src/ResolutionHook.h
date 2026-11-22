#pragma once

#include <Windows.h>

namespace a3vr {

// Ported from the SnowRunner VR mod (hooks/resolution_hook.cpp). The game sizes its swapchain from
// the client area of its main window, so the window is the source of truth for the render size.
// With ForceGameWidth/ForceGameHeight set, the first captioned top-level window the game creates is
// made exactly that client size (the headset's per-eye resolution), and Windows' own ceiling on a
// window larger than the desktop (WM_GETMINMAXINFO) is raised so the size sticks. Needs windowed
// mode in the game. An already-created main window is bound once the swapchain is identified.
void InstallResolutionHooks();
// Fits a wanted client size (same aspect) into the current desktop, leaving room for the frame.
void FitWindowToDesktop(int wantW, int wantH, int& outW, int& outH);
// Virtual cursor (window larger than the desktop): raw mouse movement drives a cursor over the whole
// client area; the game sees it through GetCursorPos / window mouse messages.
void VirtualCursorSetWindow(HWND window);
// Bind cursor messages to the actual swapchain window and request its client size asynchronously.
void AdoptMainWindow(HWND window, int width, int height);
void VirtualCursorSetPicture(int width, int height);  // size of the picture the game draws
bool VirtualCursorActive();
bool VirtualCursorClientPosition(POINT& point, int& width, int& height);
void VirtualCursorFeed(int dx, int dy, int source);  // source: 1 raw input, 2 polled DirectInput, 3 buffered DirectInput

}  // namespace a3vr
