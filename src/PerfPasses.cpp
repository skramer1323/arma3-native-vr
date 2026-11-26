#include "PerfPasses.h"

#include "Log.h"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <compare>
#include <map>
#include <mutex>
#include <vector>

namespace a3vr {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kMaxStamps = 1024;
constexpr std::uint64_t kSampleInterval = 180;
constexpr std::array kRepeatStride{8U, 3U, 4U, 3U};

enum class Role { NoTarget, DepthOnly, FirstPerson, ColorDepth, SmallColor, ColorOnly };

struct Group {
    Role role = Role::NoTarget;
    UINT width = 0;
    UINT height = 0;
    UINT format = 0;
    bool depth = false;
    std::uint64_t shader = 0;
    auto operator<=>(const Group&) const = default;
};

struct Pass {
    Group group;
    unsigned draws = 0;
};

struct Cost {
    double ms = 0;
    unsigned draws = 0;
    unsigned spans = 0;
};

// One sampled frame at a time. The ordinary frame timer supplies the enclosing
// disjoint query and frame boundaries. No readback is done from a draw hook.
struct Sample {
    ComPtr<ID3D11Device> device;
    std::array<ComPtr<ID3D11Query>, kMaxStamps> stamps;
    std::array<Pass, kMaxStamps> passes;
    std::array<PerfRepeatKind, kMaxStamps / 2> repeats;
    std::array<unsigned, kRepeatStride.size()> repeatCounts{};
    std::uint64_t present = 0;
    std::uint64_t nextPresent = 0;
    std::size_t count = 0;
    bool measureRepeats = false;
    bool truncated = false;
    bool unavailable = false;
};

Sample g_sample;
std::mutex g_sampleMutex;
std::atomic<ID3D11DeviceContext*> g_passContext{nullptr};
std::atomic<ID3D11DeviceContext*> g_repeatContext{nullptr};

bool EnsureQueries(ID3D11Device* device)
{
    if (g_sample.device.Get() == device) {
        return !g_sample.unavailable;
    }
    g_sample.device = device;
    g_sample.unavailable = false;
    constexpr D3D11_QUERY_DESC description{D3D11_QUERY_TIMESTAMP, 0};
    for (auto& stamp : g_sample.stamps) {
        if (FAILED(device->CreateQuery(&description, stamp.ReleaseAndGetAddressOf()))) {
            for (auto& allocated : g_sample.stamps) {
                allocated.Reset();
            }
            g_sample.unavailable = true;
            Log("PERF GPU profiling unavailable: failed to allocate timestamp queries");
            return false;
        }
    }
    Log("PERF GPU profiling enabled: alternate pass and repeat samples every %llu frames",
        static_cast<unsigned long long>(kSampleInterval));
    return true;
}

const char* RoleName(Role role)
{
    switch (role) {
        case Role::DepthOnly: return "depth-only";
        case Role::FirstPerson: return "first-person";
        case Role::ColorDepth: return "color+depth";
        case Role::SmallColor: return "quad-candidate";
        case Role::ColorOnly: return "color-only";
        case Role::NoTarget: return "no-target";
    }
    return "unknown";
}

void LogPasses(const std::array<UINT64, kMaxStamps>& times, UINT64 start, UINT64 end,
               double toMs)
{
    if (g_sample.count == 0) {
        Log("PERF GPU-PASSES frame %llu: no hooked draws on the immediate context",
            static_cast<unsigned long long>(g_sample.present));
        return;
    }

    std::map<Group, Cost> groups;
    const std::size_t measured = g_sample.count - (g_sample.truncated ? 1U : 0U);
    for (std::size_t i = 0; i < measured; ++i) {
        const auto& pass = g_sample.passes[i];
        const UINT64 next = i + 1 < g_sample.count ? times[i + 1] : end;
        auto& cost = groups[pass.group];
        cost.ms += static_cast<double>(next - times[i]) * toMs;
        cost.draws += pass.draws;
        ++cost.spans;
    }

    std::vector<std::pair<Group, Cost>> ranking(groups.begin(), groups.end());
    const std::size_t top = std::min<std::size_t>(10, ranking.size());
    std::partial_sort(ranking.begin(), ranking.begin() + top, ranking.end(),
                      [](const auto& left, const auto& right) { return left.second.ms > right.second.ms; });
    const double headMs = static_cast<double>(times[0] - start) * toMs;
    const double tailMs = g_sample.truncated
        ? static_cast<double>(end - times[g_sample.count - 1]) * toMs : 0.0;
    Log("PERF GPU-PASSES frame %llu: %zu groups, %zu spans, unlabelled head %.2f ms, "
        "unmeasured tail %.2f ms%s",
        static_cast<unsigned long long>(g_sample.present), groups.size(), measured, headMs,
        tailMs, g_sample.truncated ? " (query limit)" : "");
    for (std::size_t i = 0; i < top; ++i) {
        const auto& [group, cost] = ranking[i];
        Log("PERF GPU-PASS %2zu: %.3f ms | %s %ux%u fmt=%u depth=%d ps=%016llx | "
            "%u engine draws, %u spans",
            i + 1, cost.ms, RoleName(group.role), group.width, group.height, group.format,
            group.depth ? 1 : 0, static_cast<unsigned long long>(group.shader), cost.draws, cost.spans);
    }
}

void LogRepeats(const std::array<UINT64, kMaxStamps>& times, double toMs)
{
    constexpr std::array names{"world-second-eye", "first-person-second-eye", "overlay-second-eye",
                               "desktop-replay"};
    static_assert(names.size() == kRepeatStride.size());
    std::array<Cost, names.size()> costs{};
    for (std::size_t i = 0; i + 1 < g_sample.count; i += 2) {
        auto& cost = costs[static_cast<std::size_t>(g_sample.repeats[i / 2])];
        cost.ms += static_cast<double>(times[i + 1] - times[i]) * toMs;
        ++cost.draws;
    }
    for (std::size_t i = 0; i < costs.size(); ++i) {
        const auto total = g_sample.repeatCounts[i];
        const auto& cost = costs[i];
        if (cost.draws > 0 && !g_sample.truncated) {
            const double estimate = cost.ms * total / cost.draws;
            Log("PERF GPU-REPEAT %s: %.3f ms estimated from %.3f ms, %u/%u draws",
                names[i], estimate, cost.ms, cost.draws, total);
        } else {
            Log("PERF GPU-REPEAT %s: %.3f ms measured, %u/%u draws%s", names[i],
                cost.ms, cost.draws, total, g_sample.truncated ? " (query limit)" : "");
        }
    }
}

}  // namespace

bool PerfGpuPassStart(ID3D11Device* device, ID3D11DeviceContext* context, std::uint64_t present)
{
    std::scoped_lock lock(g_sampleMutex);
    if (g_sample.present == 0 && g_sample.device.Get() != device) {
        EnsureQueries(device);
        g_sample.nextPresent = present + 45;
        return false;
    }
    if (g_sample.present != 0 || present < g_sample.nextPresent || !EnsureQueries(device)) {
        return false;
    }
    g_sample.present = present;
    g_sample.nextPresent = present + kSampleInterval;
    g_sample.count = 0;
    g_sample.repeatCounts.fill(0);
    g_sample.truncated = false;
    (g_sample.measureRepeats ? g_repeatContext : g_passContext).store(context);
    return true;
}

void PerfGpuPassFinish()
{
    std::scoped_lock lock(g_sampleMutex);
    g_passContext.store(nullptr);
    g_repeatContext.store(nullptr);
}

void PerfGpuPassDiscard()
{
    std::scoped_lock lock(g_sampleMutex);
    g_passContext.store(nullptr);
    g_repeatContext.store(nullptr);
    g_sample.present = 0;
}

bool PerfGpuPassCollect(ID3D11DeviceContext* context, UINT64 start, UINT64 end, UINT64 post,
                        const D3D11_QUERY_DATA_TIMESTAMP_DISJOINT& clock)
{
    // Finish has switched off writes from draw threads before the owning frame is queued.
    std::scoped_lock lock(g_sampleMutex);
    if (clock.Disjoint || clock.Frequency == 0 || start > end || end > post) {
        Log("PERF GPU sample frame %llu dropped: invalid timestamp clock",
            static_cast<unsigned long long>(g_sample.present));
        g_sample.present = 0;
        return true;
    }

    std::array<UINT64, kMaxStamps> times{};
    UINT64 previous = start;
    for (std::size_t i = 0; i < g_sample.count; ++i) {
        const auto result = context->GetData(g_sample.stamps[i].Get(), &times[i], sizeof(UINT64),
                                             D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (result == S_FALSE) {
            return false;
        }
        if (FAILED(result) || times[i] < previous || times[i] > end) {
            Log("PERF GPU sample frame %llu dropped: unreadable or out-of-order timestamp",
                static_cast<unsigned long long>(g_sample.present));
            g_sample.present = 0;
            return true;
        }
        previous = times[i];
    }

    const double toMs = 1000.0 / static_cast<double>(clock.Frequency);
    Log("PERF GPU sample frame %llu %s: before Present %.2f ms, Present-side %.2f ms, %zu stamps",
        static_cast<unsigned long long>(g_sample.present), g_sample.measureRepeats ? "repeats" : "passes",
        static_cast<double>(end - start) * toMs, static_cast<double>(post - end) * toMs, g_sample.count);
    if (g_sample.measureRepeats) {
        LogRepeats(times, toMs);
    } else {
        LogPasses(times, start, end, toMs);
    }
    g_sample.measureRepeats = !g_sample.measureRepeats;
    g_sample.present = 0;
    return true;
}

bool PerfGpuPassWantsDraw(ID3D11DeviceContext* context)
{
    return g_passContext.load() == context;
}

void PerfGpuPassDraw(ID3D11DeviceContext* context, UINT width, UINT height, UINT format,
                     bool color, bool depth, float maxDepth, bool quad, std::uint64_t shader)
{
    std::scoped_lock lock(g_sampleMutex);
    if (g_passContext.load() != context || g_sample.truncated) {
        return;
    }
    Role role = Role::NoTarget;
    if (!color && depth) {
        role = Role::DepthOnly;
    } else if (color) {
        role = Role::ColorOnly;
        if (depth && maxDepth > 0.0F && maxDepth <= 0.0105F) {
            role = Role::FirstPerson;
        } else if (quad) {
            role = Role::SmallColor;
        } else if (depth) {
            role = Role::ColorDepth;
        }
    }
    const Group group{role, width, height, format, depth, role == Role::SmallColor ? shader : 0};
    if (g_sample.count > 0 && g_sample.passes[g_sample.count - 1].group == group) {
        ++g_sample.passes[g_sample.count - 1].draws;
        return;
    }
    if (g_sample.count == g_sample.stamps.size()) {
        g_sample.truncated = true;
        return;
    }
    const std::size_t i = g_sample.count++;
    g_sample.passes[i] = {group, 1};
    context->End(g_sample.stamps[i].Get());
}

bool PerfGpuRepeatBegin(ID3D11DeviceContext* context, PerfRepeatKind kind)
{
    if (g_repeatContext.load() != context) {
        return false;
    }
    std::scoped_lock lock(g_sampleMutex);
    if (g_repeatContext.load() != context) {
        return false;
    }
    const auto i = static_cast<std::size_t>(kind);
    const unsigned ordinal = g_sample.repeatCounts[i]++;
    const unsigned stride = kRepeatStride[i];
    // Sample one draw per block. Change the picked draw between blocks and frames
    // so regular batches do not always contribute the same position to the estimate.
    const auto block = ordinal / stride;
    const std::uint64_t seed = (g_sample.present + block * 0x9e3779b9ULL) * 0xbf58476d1ce4e5b9ULL;
    if (ordinal % stride != (seed >> 32) % stride) {
        return false;
    }
    if (g_sample.count + 2 > g_sample.stamps.size()) {
        g_sample.truncated = true;
        return false;
    }
    // A nested or concurrently submitted draw cannot own the open pair.
    if (g_sample.count % 2 != 0) {
        return false;
    }
    g_sample.repeats[g_sample.count / 2] = kind;
    context->End(g_sample.stamps[g_sample.count++].Get());
    return true;
}

void PerfGpuRepeatEnd(ID3D11DeviceContext* context)
{
    if (g_repeatContext.load() != context) {
        return;
    }
    std::scoped_lock lock(g_sampleMutex);
    if (g_repeatContext.load() == context && g_sample.count % 2 != 0) {
        context->End(g_sample.stamps[g_sample.count++].Get());
    }
}

}  // namespace a3vr
