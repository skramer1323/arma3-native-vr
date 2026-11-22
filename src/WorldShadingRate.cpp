#include "WorldShadingRate.h"

#include "Log.h"

#include <Windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace a3vr {
namespace {

using Microsoft::WRL::ComPtr;

// NVIDIA NVAPI D3D11 ABI (nvapi.h and nvapi_interface.h).
constexpr UINT kTile = 16;
enum class PixelRate : UINT { PerPixel = 5, TwoByOne = 6, OneByTwo = 7, TwoByTwo = 8 };
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
    ComPtr<ID3D11View> view;
    UINT width = 0;
    UINT height = 0;
    bool active = false;
    bool unavailable = false;
};
State g_vrs;

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
                   const float (&eyeCenters)[2][2])
{
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    const UINT columns = (width + kTile - 1) / kTile;
    const UINT rows = (height + kTile - 1) / kTile;
    const float halfWidth = width * 0.5F;
    std::vector<std::uint8_t> rates(static_cast<std::size_t>(columns) * rows);
    for (UINT y = 0; y < rows; ++y) {
        for (UINT x = 0; x < columns; ++x) {
            const float px = (x + 0.5F) * kTile;
            const unsigned eye = px >= halfWidth ? 1U : 0U;
            const float offsetX = 2.0F * (px / halfWidth - eye) - 1.0F - eyeCenters[eye][0];
            const float offsetY = 1.0F - 2.0F * (y + 0.5F) * kTile / height - eyeCenters[eye][1];
            const float radius2 = offsetX * offsetX + offsetY * offsetY;
            rates[static_cast<std::size_t>(y) * columns + x] = radius2 <= 0.70F * 0.70F ? 0 :
                radius2 > 1.10F * 1.10F ? 3 : std::fabs(offsetX) > std::fabs(offsetY) ? 1 : 2;
        }
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = columns;
    desc.Height = rows;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8_UINT;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{rates.data(), columns, 0};
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, &data, &texture))) return false;
    RateViewDesc viewDesc{sizeof(RateViewDesc) | (1U << 16), desc.Format, 4, 0, 0, 0};
    const int result = g_vrs.createView(device.Get(), texture.Get(), &viewDesc, &g_vrs.view);
    if (result != 0 || !g_vrs.view) {
        Log("World VRS rate view unavailable on this D3D11 device (NVAPI %d)", result);
        return false;
    }
    g_vrs.context = context;
    g_vrs.width = width;
    g_vrs.height = height;
    Log("World VRS mask prepared for two eyes at %ux%u", width, height);
    return true;
}

}  // namespace

bool SetWorldShadingRate(ID3D11DeviceContext* context, UINT width, UINT height,
                         const float (&eyeCenters)[2][2])
{
    if (g_vrs.unavailable) return false;
    if (g_vrs.context.Get() != context || g_vrs.width != width || g_vrs.height != height || !g_vrs.view) {
        ClearWorldShadingRate();
        g_vrs.view.Reset();
        if (!LoadNvapi() || !BuildRateView(context, width, height, eyeCenters)) {
            g_vrs.unavailable = true;
            Log("World VRS unavailable; rendering at full shading rate");
            return false;
        }
    }
    if (g_vrs.active) return true;
    ViewportRate viewport{true, {}};
    std::fill_n(viewport.table, 16, PixelRate::PerPixel);
    viewport.table[1] = PixelRate::TwoByOne;
    viewport.table[2] = PixelRate::OneByTwo;
    viewport.table[3] = PixelRate::TwoByTwo;
    ViewportRate viewports[]{viewport, viewport};
    ViewportRates rates{sizeof(ViewportRates) | (1U << 16), 2, viewports};
    const int ratesResult = g_vrs.setRates(context, &rates);
    const int viewResult = ratesResult == 0 ? g_vrs.setView(context, g_vrs.view.Get()) : ratesResult;
    if (viewResult != 0) {
        ViewportRates disabled{sizeof(ViewportRates) | (1U << 16), 0, nullptr};
        g_vrs.setRates(context, &disabled);
        g_vrs.setView(context, nullptr);
        g_vrs.unavailable = true;
        Log("World VRS activation failed (NVAPI %d)", viewResult);
        return false;
    }
    g_vrs.active = true;
    return true;
}

void ClearWorldShadingRate(ID3D11DeviceContext* context)
{
    if (!g_vrs.active || (context && context != g_vrs.context.Get())) return;
    ViewportRates disabled{sizeof(ViewportRates) | (1U << 16), 0, nullptr};
    g_vrs.setRates(g_vrs.context.Get(), &disabled);
    g_vrs.setView(g_vrs.context.Get(), nullptr);
    g_vrs.active = false;
}

}  // namespace a3vr
