#include "WorldShadingRate.h"

#include "Log.h"

#include <Windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace a3vr {
namespace {

using Microsoft::WRL::ComPtr;

// NVIDIA NVAPI D3D11 ABI (nvapi.h and nvapi_interface.h).
enum class PixelRate : UINT { PerPixel = 5, TwoByOne = 6, OneByTwo = 7, TwoByTwo = 8, FourByFour = 11 };
struct ViewportRate {
    bool enabled;
    PixelRate table[16];
};
struct ViewportRates {
    UINT version, count;
    ViewportRate* viewports;
};
struct RateViewDesc {
    UINT version;
    DXGI_FORMAT format;
    UINT dimension;
    UINT mipSlice, firstArraySlice, arraySize;
};
static_assert(sizeof(ViewportRate) == 68 && sizeof(ViewportRates) == 16 && sizeof(RateViewDesc) == 24);
using Query = void* (__cdecl*)(UINT);
using Initialize = int (__cdecl*)();
using SetRates = int (__cdecl*)(IUnknown*, ViewportRates*);
using CreateRateView = int (__cdecl*)(ID3D11Device*, ID3D11Resource*, RateViewDesc*, ID3D11View**);
using SetRateView = int (__cdecl*)(IUnknown*, ID3D11View*);

struct State {
    HMODULE library = nullptr;
    SetRates setRates = nullptr;
    CreateRateView createView = nullptr;
    SetRateView setView = nullptr;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext> failedContext;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11View> view;
    UINT width = 0;
    UINT height = 0;
    int innerDegrees = 0;
    int middleDegrees = 0;
    float farTangent = 0.0F;
    WorldShadingProjection eyes[2]{};
    unsigned visibilityRevision = 0;
    bool coarseOuter = false;
    bool hasCoarseTiles = false;
    bool coarseSupported = true;
    bool ratesInstalled = false;
    bool active = false;
};
State g_vrs;
std::mutex g_visibilityMutex;
WorldVisibilityContours g_visibilityContours;
std::atomic<unsigned> g_visibilityRevision{0};

bool LoadNvapi()
{
    if (g_vrs.library) return g_vrs.setRates && g_vrs.createView && g_vrs.setView;
    g_vrs.library = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_vrs.library) return false;
    const auto query = reinterpret_cast<Query>(GetProcAddress(g_vrs.library, "nvapi_QueryInterface"));
    const auto initialize = query ? reinterpret_cast<Initialize>(query(0x0150e828)) : nullptr;
    if (!initialize || initialize() != 0) return false;
    g_vrs.setRates = reinterpret_cast<SetRates>(query(0x34f7938f));
    g_vrs.createView = reinterpret_cast<CreateRateView>(query(0x99ca2dff));
    g_vrs.setView = reinterpret_cast<SetRateView>(query(0x1b0c2f83));
    return g_vrs.setRates && g_vrs.createView && g_vrs.setView;
}

bool BuildRateView(ID3D11DeviceContext* context, UINT width, UINT height,
                   const WorldShadingProjection (&eyes)[2], int innerDegrees, int middleDegrees,
                   bool coarseOuter)
{
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    const UINT columns = (width + kWorldShadingTile - 1) / kWorldShadingTile;
    const UINT rows = (height + kWorldShadingTile - 1) / kWorldShadingTile;
    std::vector<std::uint8_t> rates(static_cast<std::size_t>(columns) * rows);
    std::array<unsigned, 5> counts{};
    const auto radii = WorldShadingRadiiFromDegrees(innerDegrees, middleDegrees);
    const float farTangent = coarseOuter
        ? std::tan(std::max(52, middleDegrees + 6) * 0.01745329252F) : 0.0F;
    WorldVisibilityContours contours;
    if (coarseOuter) {
        std::scoped_lock lock(g_visibilityMutex);
        contours = g_visibilityContours;
        g_vrs.visibilityRevision = g_visibilityRevision.load(std::memory_order_relaxed);
    }
    unsigned visibilityTiles = 0;
    const float eyeWidth = width * 0.5F;
    for (UINT y = 0; y < rows; ++y) {
        for (UINT x = 0; x < columns; ++x) {
            auto rate = WorldShadingTileRate(x, y, width, height, eyes, radii, farTangent);
            if (coarseOuter && rate == WorldTileRate::TwoByTwo) {
                const unsigned eye = x * kWorldShadingTile >= eyeWidth ? 1U : 0U;
                const float left = 2.0F * (x * kWorldShadingTile / eyeWidth - eye) - 1.0F;
                const float right = std::min(1.0F, left + 2.0F * kWorldShadingTile / eyeWidth);
                const float top = 1.0F - 2.0F * (y * kWorldShadingTile) / height;
                const float bottom = std::max(-1.0F, top - 2.0F * kWorldShadingTile / height);
                if (WorldTileOutsideVisibility(contours[eye], left, right, bottom, top)) {
                    rate = WorldTileRate::FourByFour;
                    ++visibilityTiles;
                }
            }
            const auto index = static_cast<std::uint8_t>(rate);
            rates[static_cast<std::size_t>(y) * columns + x] = index;
            ++counts[index];
        }
    }
    if (g_vrs.view && g_vrs.texture && g_vrs.context.Get() == context &&
        g_vrs.width == width && g_vrs.height == height) {
        context->UpdateSubresource(g_vrs.texture.Get(), 0, nullptr, rates.data(), columns, 0);
    } else {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = columns;
        desc.Height = rows;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8_UINT;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{rates.data(), columns, 0};
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11View> view;
        if (FAILED(device->CreateTexture2D(&desc, &data, &texture))) return false;
        RateViewDesc viewDesc{sizeof(RateViewDesc) | (1U << 16), desc.Format, 4, 0, 0, 0};
        const int result = g_vrs.createView(device.Get(), texture.Get(), &viewDesc, &view);
        if (result != 0 || !view) {
            Log("World VRS rate view unavailable on this D3D11 device (NVAPI %d)", result);
            return false;
        }
        g_vrs.texture = texture;
        g_vrs.view = view;
    }
    g_vrs.context = context;
    g_vrs.width = width;
    g_vrs.height = height;
    g_vrs.innerDegrees = innerDegrees;
    g_vrs.middleDegrees = middleDegrees;
    g_vrs.farTangent = farTangent;
    g_vrs.coarseOuter = coarseOuter;
    g_vrs.hasCoarseTiles = counts[4] != 0;
    std::copy_n(eyes, 2, g_vrs.eyes);
    Log("World VRS angular mask %ux%u (%d/%d deg): %u full-rate, %u half-rate, %u 2x2, "
        "%u 4x4 tiles (%u added by OpenXR visibility)",
        width, height, innerDegrees, middleDegrees, counts[0], counts[1] + counts[2],
        counts[3], counts[4], visibilityTiles);
    return true;
}

bool ProjectionMoved(const WorldShadingProjection (&eyes)[2], UINT width, UINT height)
{
    // Rebuild after about half a tile of NDC drift at the center or edge.
    const float toleranceX = 2.0F * kWorldShadingTile / width;
    const float toleranceY = static_cast<float>(kWorldShadingTile) / height;
    for (unsigned eye = 0; eye < 2; ++eye) {
        const auto& previous = g_vrs.eyes[eye];
        if (std::fabs(eyes[eye].offsetX - previous.offsetX) + std::fabs(eyes[eye].scaleX - previous.scaleX) >
                toleranceX ||
            std::fabs(eyes[eye].offsetY - previous.offsetY) + std::fabs(eyes[eye].scaleY - previous.scaleY) >
                toleranceY) {
            return true;
        }
    }
    return false;
}

}  // namespace

void SetWorldVisibilityContours(WorldVisibilityContours contours)
{
    std::scoped_lock lock(g_visibilityMutex);
    g_visibilityContours = std::move(contours);
    g_visibilityRevision.fetch_add(1, std::memory_order_release);
}

bool SetWorldShadingRate(ID3D11DeviceContext* context, UINT width, UINT height,
                         const WorldShadingProjection (&eyes)[2], int innerDegrees, int middleDegrees,
                         bool coarseOuter)
{
    if (!context || g_vrs.failedContext.Get() == context) return false;
    if (!width || !height || !ValidWorldShadingProjection(eyes[0]) ||
        !ValidWorldShadingProjection(eyes[1])) {
        ClearWorldShadingRate(context);
        return false;
    }
    if (g_vrs.context.Get() != context) g_vrs.coarseSupported = true;
    coarseOuter = coarseOuter && g_vrs.coarseSupported;
    const bool visibilityChanged =
        coarseOuter && g_vrs.visibilityRevision != g_visibilityRevision.load(std::memory_order_acquire);
    if (g_vrs.context.Get() != context || g_vrs.width != width || g_vrs.height != height ||
        g_vrs.innerDegrees != innerDegrees || g_vrs.middleDegrees != middleDegrees ||
        g_vrs.coarseOuter != coarseOuter || visibilityChanged || !g_vrs.view ||
        ProjectionMoved(eyes, width, height)) {
        ClearWorldShadingRate();
        if (!LoadNvapi() || !BuildRateView(context, width, height, eyes, innerDegrees, middleDegrees,
                                         coarseOuter)) {
            g_vrs.failedContext = context;
            Log("World VRS unavailable; rendering at full shading rate");
            return false;
        }
    }
    if (g_vrs.active) return true;
    int ratesResult = 0;
    if (!g_vrs.ratesInstalled) {
        // A null rate view uses the default index. Table entry zero must stay per pixel.
        ViewportRate viewport{true, {}};
        std::fill_n(viewport.table, 16, PixelRate::PerPixel);
        viewport.table[1] = PixelRate::TwoByOne;
        viewport.table[2] = PixelRate::OneByTwo;
        viewport.table[3] = PixelRate::TwoByTwo;
        const bool useCoarse = g_vrs.hasCoarseTiles && coarseOuter;
        viewport.table[4] = useCoarse ? PixelRate::FourByFour : PixelRate::TwoByTwo;
        ViewportRate viewports[]{viewport, viewport};
        ViewportRates rates{sizeof(ViewportRates) | (1U << 16), 2, viewports};
        ratesResult = g_vrs.setRates(context, &rates);
        if (ratesResult != 0 && useCoarse) {
            Log("World VRS 4x4 rejected (NVAPI %d); retrying with 2x2 outer tiles", ratesResult);
            viewports[0].table[4] = viewports[1].table[4] = PixelRate::TwoByTwo;
            ratesResult = g_vrs.setRates(context, &rates);
            g_vrs.coarseSupported = false;
        }
        g_vrs.ratesInstalled = ratesResult == 0;
    }
    const int viewResult = ratesResult == 0 ? g_vrs.setView(context, g_vrs.view.Get()) : ratesResult;
    if (viewResult != 0) {
        ViewportRates disabled{sizeof(ViewportRates) | (1U << 16), 0, nullptr};
        g_vrs.setRates(context, &disabled);
        g_vrs.setView(context, nullptr);
        g_vrs.ratesInstalled = false;
        g_vrs.failedContext = context;
        Log("World VRS activation failed (NVAPI %d)", viewResult);
        return false;
    }
    g_vrs.active = true;
    return true;
}

bool WorldShadingCoarseActive()
{
    return g_vrs.active && g_vrs.hasCoarseTiles && g_vrs.coarseSupported;
}

void SuspendWorldShadingRate(ID3D11DeviceContext* context)
{
    if (!g_vrs.active || context != g_vrs.context.Get()) return;
    if (g_vrs.setView(context, nullptr) != 0) {
        ClearWorldShadingRate(context);
        return;
    }
    g_vrs.active = false;
}

void ClearWorldShadingRate(ID3D11DeviceContext* context)
{
    if (!g_vrs.ratesInstalled || (context && context != g_vrs.context.Get())) return;
    ViewportRates disabled{sizeof(ViewportRates) | (1U << 16), 0, nullptr};
    g_vrs.setRates(g_vrs.context.Get(), &disabled);
    if (g_vrs.active) g_vrs.setView(g_vrs.context.Get(), nullptr);
    g_vrs.active = false;
    g_vrs.ratesInstalled = false;
}

bool GetWorldShadingGuide(UINT width, UINT height, WorldShadingGuide& guide)
{
    if (!g_vrs.view || width != g_vrs.width || height != g_vrs.height) return false;
    std::copy_n(g_vrs.eyes, 2, guide.eyes);
    guide.radii = WorldShadingRadiiFromDegrees(g_vrs.innerDegrees, g_vrs.middleDegrees);
    guide.farTangent = g_vrs.hasCoarseTiles && g_vrs.coarseSupported ? g_vrs.farTangent : 0.0F;
    return true;
}

}  // namespace a3vr
