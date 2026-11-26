#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>

namespace a3vr {

// Performance measurements (INI PerfLog=1): where does a frame go? Cheap enough to leave on while
// testing (one QueryPerformanceCounter read costs ~25 ns). Everything is accumulated per frame and
// written to the log every 120 presents as "PERF ..." lines by PerfOnPresentDone().

inline double PerfNowMs()
{
    static const double toMs = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return 1000.0 / static_cast<double>(frequency.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * toMs;
}

bool PerfEnabled();

// One engine draw as seen by the hook: ours = everything the mod adds (checks, state queries,
// constant-buffer patching, viewport changes, restoring), original = the engine's own first draw
// call, second = the extra draw for the other eye.
void PerfAddDraw(double oursMs, double originalMs, double secondMs, bool repeated);
// Time inside the mod's Present-side work and its parts, and the original Present call.
void PerfAddPresentSide(double hookMs, double dlssCpuMs, double submitMs);
void PerfAddOriginalPresent(double ms);
void PerfAddDlssCpu(double ms);
void PerfAddXrWait(double ms);
void PerfAddXrImageWait(double ms);

// GPU timer around a block of our own GPU work (DLSS): Begin/End on the immediate context,
// results are read a few frames later without stalling.
class PerfGpuTimer final {
public:
    void Begin(ID3D11Device* device, ID3D11DeviceContext* context);
    void End(ID3D11DeviceContext* context);
    // Finished measurements since the last call (ms, sum and count); never blocks.
    void Collect(ID3D11DeviceContext* context, double& sumMs, int& count);

private:
    static constexpr int kRing = 8;
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> start;
        Microsoft::WRL::ComPtr<ID3D11Query> end;
        bool pending = false;
    };
    Slot m_slots[kRing]{};
    int m_current = -1;
    int m_next = 0;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
};

PerfGpuTimer& PerfDlssGpuTimer();
PerfGpuTimer& PerfStaleEyeGpuTimer();
PerfGpuTimer& PerfDibrGpuTimer();
PerfGpuTimer& PerfXrCopyGpuTimer();
PerfGpuTimer& PerfCasGpuTimer();

// Mark the queue interval after the previous desktop Present, before the current
// Present hook and at the end of its submission work. The interval may include GPU idle time.
void PerfFrameGpuBegin(IDXGISwapChain* swapChain, ID3D11Device* device,
                       ID3D11DeviceContext* context, std::uint64_t nextPresent);
void PerfFrameGpuMark(IDXGISwapChain* swapChain, int mark);

// Called once per Present after everything (log line every `every` presents).
void PerfOnPresentDone(ID3D11DeviceContext* context, std::uint64_t presents, int every);

// RAII helper for a draw wrapper: construct first thing, mark the points, destruct last.
struct PerfDrawScope {
    bool on = false;
    double t0 = 0.0;
    double tBegin = 0.0;
    double tOrig = 0.0;
    double tNext = 0.0;
    double tSecond = 0.0;
    bool second = false;
    PerfDrawScope();
    void AfterBegin() { if (on) tBegin = PerfNowMs(); }
    void AfterOriginal() { if (on) tOrig = PerfNowMs(); }
    void AfterNext(bool secondDraw)
    {
        if (on) {
            tNext = PerfNowMs();
            second = secondDraw;
        }
    }
    void AfterSecond() { if (on) tSecond = PerfNowMs(); }
    ~PerfDrawScope();
};

}  // namespace a3vr
