#pragma once

#include <Windows.h>

namespace a3vr {

// The game reads its mouse and keyboard through DirectInput (this DLL is the dinput8 proxy).
// While the settings menu is open the game must not see them: the device methods are hooked
// (vtable of the devices the game creates), the real mouse movement is taken for the menu cursor
// and the game gets zeros.

// Called after the real DirectInput8Create succeeded: hooks IDirectInput8::CreateDevice.
void InputBlockInstall(void* directInput);
void InputBlockSetActive(bool active);
bool InputBlockActive();
// Mouse movement and buttons accumulated while active; resets the accumulators.
void InputBlockTakeMouse(float& dx, float& dy, float& wheel);
bool InputBlockMouseButton(int index);  // 0 left, 1 right

}  // namespace a3vr
