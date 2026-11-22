#pragma once

#include <Windows.h>

#include <filesystem>

namespace a3vr {

void SetSelfModule(HMODULE module);
std::filesystem::path ModuleDirectory();
void Log(const char* format, ...);
// Optional observer that sees each formatted message (called outside the log
// lock); used to show tuning values in the headset.
void SetLogObserver(void (*observer)(const char* message));

}  // namespace a3vr

