#include "Perf.h"

#include "Config.h"
#include "EngineStereo.h"
#include "Log.h"
#include "PerfPasses.h"

#include <algorithm>
#include <cstdint>

namespace a3vr {
namespace {

// Accumulators for the current logging interval. Draws and Present run on the game's render
// thread; atomics keep it safe if a second thread ever draws.
std::atomic<double> g_oursMs{0.0};
std::atomic<double> g_originalMs{0.0};
std::atomic<double> g_secondMs{0.0};
std::atomic<std::uint64_t> g_draws{0};
std::atomic<std::uint64_t> g_secondDraws{0};
std::atomic<double> g_hookMs{0.0};
std::atomic<double> g_dlssCpuMs{0.0};
std::atomic<double> g_submitMs{0.0};
std::atomic<double> g_origPresentMs{0.0};
std::atomic<double> g_xrWaitMs{0.0};
double g_xrImageWaitMs = 0.0;
std::atomic<std::uint64_t> g_frames{0};
std::atomic<double> g_worstFrame{0.0};
double g_lastPresentTime = 0.0;
double g_intervalSum = 0.0;
PerfGpuTimer g_dlssGpu;
PerfGpuTimer g_staleGpu;
PerfGpuTimer g_dibrGpu;
PerfGpuTimer g_xrCopyGpu;

}  // namespace

bool PerfEnabled()
{
    return GetConfig().perfLog;
}

PerfDrawScope::PerfDrawScope()
{
    on = PerfEnabled();
    if (on) {
        t0 = PerfNowMs();
    }
}

PerfDrawScope::~PerfDrawScope()
{
    if (!on) {
        return;
    }
    const double tEnd = PerfNowMs();
    // ours = before the original draw (cull check, Record, StereoDraw begin) +
    //        between the original draw and the second one (switching to the other eye) +
    //        after the last draw (restoring state in the destructor)
    const double lastDraw = second ? tSecond : tNext;
    const double beforeOriginal = tBegin - t0;
    const double switching = tNext - tOrig;
    const double restoring = tEnd - lastDraw;
    const double original = tOrig - tBegin;
    const double secondDraw = second ? tSecond - tNext : 0.0;
    PerfAddDraw(beforeOriginal + switching + restoring, original, secondDraw, second);
}

void PerfAddDraw(double oursMs, double originalMs, double secondMs, bool repeated)
{
    g_oursMs.fetch_add(oursMs, std::memory_order_relaxed);
    g_originalMs.fetch_add(originalMs, std::memory_order_relaxed);
    g_secondMs.fetch_add(secondMs, std::memory_order_relaxed);
    g_draws.fetch_add(1, std::memory_order_relaxed);
    if (repeated) {
        g_secondDraws.fetch_add(1, std::memory_order_relaxed);
    }
}

void PerfAddPresentSide(double hookMs, double dlssCpuMs, double submitMs)
{
    g_hookMs.fetch_add(hookMs, std::memory_order_relaxed);
    g_dlssCpuMs.fetch_add(dlssCpuMs, std::memory_order_relaxed);
    g_submitMs.fetch_add(submitMs, std::memory_order_relaxed);
}

void PerfAddOriginalPresent(double ms)
{
    g_origPresentMs.fetch_add(ms, std::memory_order_relaxed);
}

void PerfAddDlssCpu(double ms)
{
    g_dlssCpuMs.fetch_add(ms, std::memory_order_relaxed);
}

PerfGpuTimer& PerfDlssGpuTimer()
{
    return g_dlssGpu;
}

PerfGpuTimer& PerfStaleEyeGpuTimer()
{
    return g_staleGpu;
}

PerfGpuTimer& PerfDibrGpuTimer()
{
    return g_dibrGpu;
}

PerfGpuTimer& PerfXrCopyGpuTimer()
{
    return g_xrCopyGpu;
}

void PerfAddXrImageWait(double ms)
{
    g_xrImageWaitMs += ms;
}

void PerfGpuTimer::Begin(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_device = device;
    m_current = m_next;
    m_next = (m_next + 1) % kRing;
    Slot& slot = m_slots[m_current];
    if (slot.pending) {
        m_current = -1;  // the ring is full of unread results: skip this measurement
        return;
    }
    if (!slot.disjoint) {
        D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(device->CreateQuery(&disjoint, &slot.disjoint)) ||
            FAILED(device->CreateQuery(&stamp, &slot.start)) ||
            FAILED(device->CreateQuery(&stamp, &slot.end))) {
            m_current = -1;
            return;
        }
    }
    context->Begin(slot.disjoint);
    context->End(slot.start);
}

void PerfGpuTimer::End(ID3D11DeviceContext* context)
{
    if (m_current < 0) {
        return;
    }
    Slot& slot = m_slots[m_current];
    context->End(slot.end);
    context->End(slot.disjoint);
    slot.pending = true;
    m_current = -1;
}

void PerfGpuTimer::Collect(ID3D11DeviceContext* context, double& sumMs, int& count)
{
    for (Slot& slot : m_slots) {
        if (!slot.pending) {
            continue;
        }
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        if (context->GetData(slot.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) !=
            S_OK) {
            continue;
        }
        UINT64 start = 0;
        UINT64 end = 0;
        if (context->GetData(slot.start, &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(slot.end, &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            continue;
        }
        slot.pending = false;
        if (!disjoint.Disjoint && disjoint.Frequency > 0 && end >= start) {
            sumMs += 1000.0 * static_cast<double>(end - start) /
                     static_cast<double>(disjoint.Frequency);
            ++count;
        }
    }
}

void PerfCollectFrameGpu(ID3D11DeviceContext* context);
void PerfLogFrameGpu();
void PerfOnPresentDone(ID3D11DeviceContext* context, std::uint64_t presents, int every)
{
    if (!PerfEnabled()) {
        return;
    }
    const double now = PerfNowMs();
    if (g_lastPresentTime > 0.0) {
        const double interval = now - g_lastPresentTime;
        g_intervalSum += interval;
        g_worstFrame.store(std::max(g_worstFrame.load(), interval));
        g_frames.fetch_add(1, std::memory_order_relaxed);
    }
    g_lastPresentTime = now;
    PerfCollectFrameGpu(context);
    if (every <= 0 || presents % static_cast<std::uint64_t>(every) != 0) {
        return;
    }
    const double frames = static_cast<double>(std::max<std::uint64_t>(1, g_frames.load()));
    const double draws = static_cast<double>(std::max<std::uint64_t>(1, g_draws.load()));
    double gpuSum = 0.0;
    int gpuCount = 0;
    g_dlssGpu.Collect(context, gpuSum, gpuCount);
    double staleSum = 0.0;
    int staleCount = 0;
    g_staleGpu.Collect(context, staleSum, staleCount);
    if (staleCount > 0) {
        Log("PERF AER stale-eye warp %.2f ms GPU (%d samples)", staleSum / staleCount, staleCount);
    }
    double dibrSum = 0.0;
    int dibrCount = 0;
    g_dibrGpu.Collect(context, dibrSum, dibrCount);
    if (dibrCount > 0) {
        Log("PERF DIBR reconstruction %.2f ms GPU (%d samples)", dibrSum / dibrCount, dibrCount);
    }
    double copySum = 0.0;
    int copyCount = 0;
    g_xrCopyGpu.Collect(context, copySum, copyCount);
    if (copyCount > 0) {
        Log("PERF XR scene copy %.3f ms GPU (%d samples) | swapchain image wait %.3f ms CPU per frame",
            copySum / copyCount, copyCount, g_xrImageWaitMs / frames);
    }
    g_xrImageWaitMs = 0.0;
    const double interval = g_intervalSum / frames;
    const double hookMs = g_hookMs.load() / frames;
    const double origPresent = g_origPresentMs.load() / frames;
    const double ours = g_oursMs.load() / frames;
    const double original = g_originalMs.load() / frames;
    const double second = g_secondMs.load() / frames;
    const double engineCpu = interval - hookMs - origPresent;  // engine work + the draw hooks
    Log("PERF frame %.2f ms (%.1f fps, worst %.1f ms) | outside Present %.2f ms (engine+draw hooks) "
        "| in our Present hook %.2f ms (xrWaitFrame %.2f, DLSS cpu %.2f, submit %.2f) | original Present %.2f ms",
        interval, 1000.0 / std::max(0.001, interval), g_worstFrame.load(), engineCpu, hookMs, g_xrWaitMs.load() / frames,
        g_dlssCpuMs.load() / frames, g_submitMs.load() / frames, origPresent);
    Log("PERF draws %.0f per frame (%.0f doubled): our hook overhead %.2f ms/frame (%.2f us per "
        "draw), engine's own draw calls %.2f ms, second-eye draw calls %.2f ms | DLSS gpu %.2f ms "
        "(%d samples)",
        draws / frames, static_cast<double>(g_secondDraws.load()) / frames, ours,
        1000.0 * ours / (draws / frames), original, second,
        gpuCount > 0 ? gpuSum / gpuCount : 0.0, gpuCount);
    PerfLogFrameGpu();
    g_oursMs = g_originalMs = g_secondMs = 0.0;
    g_draws = g_secondDraws = g_frames = 0;
    g_hookMs = g_dlssCpuMs = g_submitMs = g_origPresentMs = g_xrWaitMs = 0.0;
    g_worstFrame = 0.0;
    g_intervalSum = 0.0;
}

}  // namespace a3vr

namespace a3vr {
void PerfAddXrWait(double ms)
{
    g_xrWaitMs.fetch_add(ms, std::memory_order_relaxed);
}
}  // namespace a3vr

namespace a3vr {
namespace {
struct FrameSlot {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* ts[3]{};
    bool pending = false;
    bool passSample = false;
};
constexpr int kFrameRing = 8;
FrameSlot g_frameSlots[kFrameRing];
int g_frameCurrent = -1;
int g_frameNext = 0;
double g_gpuEngineSum = 0.0, g_gpuPostSum = 0.0, g_gpuTotalSum = 0.0;
int g_gpuFrames = 0;
ID3D11Device* g_frameDevice = nullptr;
}  // namespace

void PerfFrameGpuMark(IDXGISwapChain* swapChain, int mark, std::uint64_t nextPresent)
{
    if (!PerfEnabled() || !swapChain) {
        return;
    }
    ID3D11Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) ||
        !device) {
        return;
    }
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (context) {
        if (mark == 0) {
            g_frameCurrent = -1;
            FrameSlot& slot = g_frameSlots[g_frameNext];
            if (!slot.pending) {
                bool ok = true;
                if (!slot.disjoint) {
                    D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                    D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0};
                    ok = SUCCEEDED(device->CreateQuery(&disjoint, &slot.disjoint));
                    for (auto& q : slot.ts) {
                        ok = ok && SUCCEEDED(device->CreateQuery(&stamp, &q));
                    }
                }
                if (ok) {
                    context->Begin(slot.disjoint);
                    context->End(slot.ts[0]);
                    slot.passSample = EngineStereoPerfWorld() && PerfGpuPassStart(device, context, nextPresent);
                    g_frameCurrent = g_frameNext;
                    g_frameNext = (g_frameNext + 1) % kFrameRing;
                    g_frameDevice = device;
                }
            }
        } else if (g_frameCurrent >= 0) {
            FrameSlot& slot = g_frameSlots[g_frameCurrent];
            if (mark == 1 && slot.passSample) {
                PerfGpuPassFinish();
            }
            context->End(slot.ts[mark]);
            if (mark == 2) {
                context->End(slot.disjoint);
                slot.pending = true;
                g_frameCurrent = -1;
            }
        }
        context->Release();
    }
    device->Release();
}

void PerfCollectFrameGpu(ID3D11DeviceContext* context)
{
    for (FrameSlot& slot : g_frameSlots) {
        if (!slot.pending) {
            continue;
        }
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 t[3]{};
        if (context->GetData(slot.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(slot.ts[0], &t[0], 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(slot.ts[1], &t[1], 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(slot.ts[2], &t[2], 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            continue;
        }
        if (slot.passSample && !PerfGpuPassCollect(context, t[0], t[1], t[2], disjoint)) {
            continue;
        }
        slot.pending = false;
        if (disjoint.Disjoint || disjoint.Frequency == 0 || t[1] < t[0] || t[2] < t[1]) {
            continue;
        }
        const double toMs = 1000.0 / static_cast<double>(disjoint.Frequency);
        g_gpuEngineSum += (t[1] - t[0]) * toMs;
        g_gpuPostSum += (t[2] - t[1]) * toMs;
        g_gpuTotalSum += (t[2] - t[0]) * toMs;
        ++g_gpuFrames;
    }
}

void PerfLogFrameGpu()
{
    if (g_gpuFrames > 0) {
        Log("PERF gpu timeline (ms/frame, %d frames): before Present %.2f | Present-side work %.2f | "
            "total previous-Present..our-Present-end %.2f",
            g_gpuFrames, g_gpuEngineSum / g_gpuFrames, g_gpuPostSum / g_gpuFrames,
            g_gpuTotalSum / g_gpuFrames);
    }
    g_gpuEngineSum = g_gpuPostSum = g_gpuTotalSum = 0.0;
    g_gpuFrames = 0;
}
}  // namespace a3vr
