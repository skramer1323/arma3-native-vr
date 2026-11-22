#include "Log.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <atomic>
#include <mutex>

namespace a3vr {
namespace {

HMODULE g_selfModule = nullptr;
std::mutex g_logMutex;
std::atomic<void (*)(const char*)> g_logObserver = nullptr;

}  // namespace

void SetSelfModule(HMODULE module)
{
    g_selfModule = module;
}

std::filesystem::path ModuleDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(g_selfModule, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

void SetLogObserver(void (*observer)(const char* message))
{
    g_logObserver.store(observer);
}

void Log(const char* format, ...)
{
    char message[2048]{};
    va_list args;
    va_start(args, format);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);

    if (const auto observer = g_logObserver.load()) {
        observer(message);
    }
    std::scoped_lock lock(g_logMutex);
    std::ofstream output(ModuleDirectory() / L"arma3_native_vr.log", std::ios::app);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    output << '[' << now.wHour << ':' << now.wMinute << ':' << now.wSecond << "] "
           << message << '\n';
}

}  // namespace a3vr

