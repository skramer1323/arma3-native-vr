#include "Perf.h"

#include "Config.h"
#include "EngineStereo.h"
#include "Log.h"
#include "Menu.h"
#include "PerfPasses.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

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
std::vector<double> g_presentIntervals;
PerfGpuTimer g_dlssGpu;
PerfGpuTimer g_staleGpu;
PerfGpuTimer g_dibrGpu;
PerfGpuTimer g_xrCopyGpu;
PerfGpuTimer g_casGpu;

double Percentile(const std::vector<double>& sorted, std::size_t percent)
{
    return sorted[(sorted.size() * percent + 99) / 100 - 1];
}

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

PerfGpuTimer& PerfCasGpuTimer()
{
    return g_casGpu;
}

void PerfAddXrImageWait(double ms)
{
    g_xrImageWaitMs += ms;
}

void PerfGpuTimer::Begin(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (m_context.Get() != context) {
        *this = PerfGpuTimer{};
        m_context = context;
    }
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
        Slot queries;
        if (FAILED(device->CreateQuery(&disjoint, &queries.disjoint)) ||
            FAILED(device->CreateQuery(&stamp, &queries.start)) ||
            FAILED(device->CreateQuery(&stamp, &queries.end))) {
            m_current = -1;
            return;
        }
        slot = std::move(queries);
    }
    context->Begin(slot.disjoint.Get());
    context->End(slot.start.Get());
}

void PerfGpuTimer::End(ID3D11DeviceContext* context)
{
    if (m_current < 0) {
        return;
    }
    Slot& slot = m_slots[m_current];
    context->End(slot.end.Get());
    context->End(slot.disjoint.Get());
    slot.pending = true;
    m_current = -1;
}

void PerfGpuTimer::Collect(ID3D11DeviceContext* context, double& sumMs, int& count)
{
    if (m_context.Get() != context) return;
    for (Slot& slot : m_slots) {
        if (!slot.pending) continue;
        constexpr UINT noFlush = D3D11_ASYNC_GETDATA_DONOTFLUSH;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 start = 0, end = 0;
        const auto clock = context->GetData(slot.disjoint.Get(), &disjoint, sizeof(disjoint), noFlush);
        if (clock == S_FALSE) continue;
        if (FAILED(clock)) {
            slot.pending = false;
            continue;
        }
        const auto first = context->GetData(slot.start.Get(), &start, sizeof(start), noFlush);
        const auto last = context->GetData(slot.end.Get(), &end, sizeof(end), noFlush);
        if (!FAILED(first) && !FAILED(last) && (first == S_FALSE || last == S_FALSE)) continue;
        slot.pending = false;
        if (first == S_OK && last == S_OK && !disjoint.Disjoint && disjoint.Frequency > 0 && end >= start) {
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
        g_presentIntervals.push_back(interval);
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
    double casSum = 0.0;
    int casCount = 0;
    g_casGpu.Collect(context, casSum, casCount);
    if (casCount > 0) {
        Log("PERF stereo CAS/ring overlay and source copy %.3f ms GPU (%d samples)", casSum / casCount, casCount);
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
    if (!g_presentIntervals.empty()) {
        std::sort(g_presentIntervals.begin(), g_presentIntervals.end());
        Log("PERF present intervals p50 %.2f ms p95 %.2f ms (%zu samples)",
            Percentile(g_presentIntervals, 50), Percentile(g_presentIntervals, 95),
            g_presentIntervals.size());
        g_presentIntervals.clear();
    }
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

using Microsoft::WRL::ComPtr;

enum class VrsMode { Off, Active, Inactive };

struct RenderProfile {
    VrsMode vrs = VrsMode::Off;
    int radius = 0;
    int cas = 0;
    bool coarse = false;
    bool operator==(const RenderProfile&) const = default;
};

struct FrameSlot {
    ComPtr<ID3D11Query> disjoint;
    std::array<ComPtr<ID3D11Query>, 3> stamps;
    RenderProfile profile;
    bool pending = false;
    bool passSample = false;
    bool world = false;
};

std::array<FrameSlot, 8> g_frameSlots;
IDXGISwapChain* g_frameSwapChain = nullptr;
ComPtr<ID3D11DeviceContext> g_frameContext;
int g_frameCurrent = -1;
std::size_t g_frameNext = 0;
double g_gpuEngineSum = 0.0;
double g_gpuPostSum = 0.0;
std::size_t g_gpuFrames = 0;
std::vector<double> g_gpuWorldSpans;
RenderProfile g_profile;
bool g_mixedProfile = false;

}  // namespace

void PerfFrameGpuBegin(IDXGISwapChain* swapChain, ID3D11Device* device,
                       ID3D11DeviceContext* context, std::uint64_t nextPresent)
{
    if (g_frameContext.Get() != context || g_frameSwapChain != swapChain) {
        PerfGpuPassDiscard();
        g_frameSlots = {};
        g_frameContext = context;
        g_frameSwapChain = swapChain;
        g_frameNext = 0;
    }
    g_frameCurrent = -1;
    auto& slot = g_frameSlots[g_frameNext];
    if (slot.pending) return;
    if (!slot.disjoint) {
        FrameSlot queries;
        constexpr D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        constexpr D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(device->CreateQuery(&disjoint, &queries.disjoint))) return;
        for (auto& query : queries.stamps) {
            if (FAILED(device->CreateQuery(&stamp, &query))) return;
        }
        slot = std::move(queries);
    }

    g_frameCurrent = static_cast<int>(g_frameNext);
    g_frameNext = (g_frameNext + 1) % g_frameSlots.size();
    const auto& config = GetConfig();
    slot.world = EngineStereoPerfWorld() && !Menu::Instance().IsOpen();
    slot.profile = {config.worldVrs ? VrsMode::Inactive : VrsMode::Off,
                    config.worldVrs ? config.worldVrsInnerDegrees : 0, config.casSharpnessPercent, false};
    context->Begin(slot.disjoint.Get());
    context->End(slot.stamps[0].Get());
    slot.passSample = slot.world && PerfGpuPassStart(device, context, nextPresent);
}

void PerfFrameGpuMark(IDXGISwapChain* swapChain, int mark)
{
    if (g_frameSwapChain != swapChain || g_frameCurrent < 0) return;
    auto& slot = g_frameSlots[g_frameCurrent];
    if (mark == 1 && slot.passSample) PerfGpuPassFinish();
    g_frameContext->End(slot.stamps[mark].Get());
    if (mark == 2) {
        slot.world = slot.world && !Menu::Instance().IsOpen() &&
                     !EngineStereoScopeUi() && EngineStereoDrawZoom() <= 1.03F;
        if (EngineStereoLastWorldVrs()) {
            slot.profile.vrs = VrsMode::Active;
            slot.profile.coarse = EngineStereoLastWorldVrsCoarse();
        }
        g_frameContext->End(slot.disjoint.Get());
        slot.pending = true;
        g_frameCurrent = -1;
    }
}

void PerfCollectFrameGpu(ID3D11DeviceContext* context)
{
    if (g_frameContext.Get() != context) return;
    for (auto& slot : g_frameSlots) {
        if (!slot.pending) continue;
        constexpr UINT noFlush = D3D11_ASYNC_GETDATA_DONOTFLUSH;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 times[3]{};
        const auto clock = context->GetData(slot.disjoint.Get(), &disjoint, sizeof(disjoint), noFlush);
        if (clock == S_FALSE) continue;
        std::array replies{clock, clock, clock};
        if (clock == S_OK) {
            for (std::size_t i = 0; i < replies.size(); ++i) {
                replies[i] = context->GetData(slot.stamps[i].Get(), &times[i], sizeof(UINT64), noFlush);
            }
        }
        if (std::any_of(replies.begin(), replies.end(), [](HRESULT reply) { return FAILED(reply); })) {
            if (slot.passSample) PerfGpuPassDiscard();
            slot.pending = false;
            continue;
        }
        if (std::any_of(replies.begin(), replies.end(), [](HRESULT reply) { return reply != S_OK; })) {
            continue;
        }
        if (slot.passSample && !PerfGpuPassCollect(context, times[0], times[1], times[2], disjoint)) continue;
        slot.pending = false;
        if (disjoint.Disjoint || disjoint.Frequency == 0 || times[1] < times[0] || times[2] < times[1]) {
            continue;
        }
        const double toMs = 1000.0 / static_cast<double>(disjoint.Frequency);
        const double span = static_cast<double>(times[2] - times[0]) * toMs;
        g_gpuEngineSum += static_cast<double>(times[1] - times[0]) * toMs;
        g_gpuPostSum += static_cast<double>(times[2] - times[1]) * toMs;
        ++g_gpuFrames;
        if (slot.world) {
            if (g_gpuWorldSpans.empty()) g_profile = slot.profile;
            g_mixedProfile |= g_profile != slot.profile;
            g_gpuWorldSpans.push_back(span);
        }
    }
}

void PerfLogFrameGpu()
{
    if (g_gpuFrames != 0) {
        const auto samples = g_gpuFrames;
        const double count = static_cast<double>(samples);
        const double before = g_gpuEngineSum / count;
        const double after = g_gpuPostSum / count;
        Log("PERF gpu timeline (ms/frame, %zu frames): before Present %.2f | Present-side work %.2f | "
            "total previous-Present..our-Present-end %.2f", samples, before, after, before + after);
        g_gpuFrames = 0;
    }
    if (!g_gpuWorldSpans.empty() && !g_mixedProfile) {
        std::sort(g_gpuWorldSpans.begin(), g_gpuWorldSpans.end());
        const char* mode = g_profile.vrs == VrsMode::Active ? "active" :
                           g_profile.vrs == VrsMode::Off ? "off" : "inactive";
        Log("PERF world queue span p50 %.2f ms p95 %.2f ms (%zu samples, may include idle): "
            "VRS=%s radius=%d deg outer4x4=%d CAS=%d%%",
            Percentile(g_gpuWorldSpans, 50), Percentile(g_gpuWorldSpans, 95),
            g_gpuWorldSpans.size(), mode, g_profile.radius, g_profile.coarse ? 1 : 0, g_profile.cas);
    }
    g_gpuWorldSpans.clear();
    g_mixedProfile = false;
    g_gpuEngineSum = g_gpuPostSum = 0.0;
}

}  // namespace a3vr
