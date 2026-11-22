#include "FrameProbe.h"
#include "Perf.h"
#include "PerfPasses.h"

#include "Config.h"
#include "EngineStereo.h"
#include "Log.h"
#include "ProjectionCache.h"

#include <MinHook.h>
#include <d3d11_1.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace a3vr {
namespace {

using Microsoft::WRL::ComPtr;

using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn =
    void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
                                                         ID3D11ClassLinkage*,
                                                         ID3D11VertexShader**);

using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
                                                        ID3D11ClassLinkage*, ID3D11PixelShader**);

DrawIndexedFn g_drawIndexed = nullptr;
DrawFn g_draw = nullptr;
DrawIndexedInstancedFn g_drawIndexedInstanced = nullptr;
DrawInstancedFn g_drawInstanced = nullptr;
DrawIndirectFn g_drawIndexedInstancedIndirect = nullptr;
DrawIndirectFn g_drawInstancedIndirect = nullptr;
CreateVertexShaderFn g_createVertexShader = nullptr;
CreatePixelShaderFn g_createPixelShader = nullptr;

constexpr std::size_t kMaxRecords = 60000;
constexpr std::size_t kMaxSampleBytes = 16U * 1024U * 1024U;
constexpr UINT kMaxBufferSample = 16384;
constexpr int kMaxCaptures = 9;
constexpr UINT kSlots = 5;

struct TargetInfo {
    void* texture = nullptr;
    UINT width = 0;
    UINT height = 0;
    UINT format = 0;      // texture format
    UINT viewFormat = 0;
    UINT samples = 0;
};

struct DrawRecord {
    char kind = '?';      // I indexed, D plain, i indexed instanced, d instanced, x/y indirect
    UINT count = 0;
    UINT instances = 1;
    UINT targetCount = 0;
    TargetInfo target0;
    void* extraTargets[3]{};
    TargetInfo depth;
    float viewport[4]{};  // x, y, width, height
    float depthRange[2]{0.0F, 1.0F};  // viewport MinDepth, MaxDepth
    UINT bufferUsage[kSlots]{};
    UINT bufferCpu[kSlots]{};
    void* vertexShader = nullptr;
    void* pixelShader = nullptr;
    void* buffers[kSlots]{};
    UINT bufferSizes[kSlots]{};
    bool depthTest = true;
    bool depthWrite = true;
    bool deferred = false;
    DWORD thread = 0;
};

struct BufferSample {
    std::uint32_t draw = 0;
    std::uint32_t slot = 0;
    std::uint32_t fullSize = 0;
    std::vector<std::uint8_t> data;
};

std::atomic<std::uint32_t> g_drawsThisFrame = 0;
std::atomic<bool> g_capturing = false;
std::mutex g_cullMutex;
std::unordered_map<void*, std::uint64_t> g_pixelShaderHash;  // pixel shader -> FNV hash of bytecode
std::atomic<std::uint32_t> g_culledDraws{0};
// Pixel shaders using PSC_GodRayPars for sun glow in game build 2.22.0.154089.
constexpr std::uint64_t kGodRayShaders[] = {0x084b7cb9e89f267dULL, 0xa020fbf71e249da7ULL,
                                            0xaf824b34826d4e08ULL, 0xcedf2729f49513acULL,
                                            0xe2ac5c5ff69e0e8bULL};

std::mutex g_mutex;  // everything below
std::unordered_map<void*, std::vector<std::uint8_t>> g_vertexShaderCode;
std::unordered_map<void*, std::vector<std::uint8_t>> g_pixelShaderCode;
using CaptureStack = std::array<void*, 32>;
CaptureStack g_worldDrawStack{};
CaptureStack g_weaponDrawStack{};
std::vector<DrawRecord> g_records;
std::vector<BufferSample> g_samples;
std::size_t g_sampleBytes = 0;
std::set<std::tuple<void*, void*, UINT, bool>> g_sampled;  // shader, target, slot, first-person pass
std::map<UINT, ComPtr<ID3D11Buffer>> g_staging;       // by ByteWidth
bool g_dropped = false;

// Present thread only.
bool g_armed = false;
int g_captures = 0;
std::uint32_t g_lastFrameDraws = 0;
std::uint64_t g_presents = 0;
ULONGLONG g_firstPresentTick = 0;
int g_autoCaptures = 0;

void DescribeTexture(ID3D11View* view, TargetInfo& info)
{
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    info.texture = resource.Get();
    ComPtr<ID3D11Texture2D> texture;
    if (resource && SUCCEEDED(resource.As(&texture))) {
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        info.width = description.Width;
        info.height = description.Height;
        info.format = static_cast<UINT>(description.Format);
        info.samples = description.SampleDesc.Count;
    }
}

// g_mutex held. Copies a constant buffer through a staging buffer. Immediate
// context only: a deferred context cannot read back.
void SampleBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer, UINT size, UINT slot,
                  std::uint32_t draw)
{
    if (g_sampleBytes >= kMaxSampleBytes || size == 0) {
        return;
    }
    ComPtr<ID3D11Buffer>& staging = g_staging[size];
    if (!staging) {
        ComPtr<ID3D11Device> device;
        buffer->GetDevice(&device);
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = size;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (!device || FAILED(device->CreateBuffer(&description, nullptr, &staging))) {
            return;
        }
    }
    context->CopyResource(staging.Get(), buffer);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return;
    }
    BufferSample sample;
    sample.draw = draw;
    sample.slot = slot;
    sample.fullSize = size;
    const UINT take = size < kMaxBufferSample ? size : kMaxBufferSample;
    sample.data.assign(static_cast<const std::uint8_t*>(mapped.pData),
                       static_cast<const std::uint8_t*>(mapped.pData) + take);
    context->Unmap(staging.Get(), 0);
    g_sampleBytes += take;
    g_samples.push_back(std::move(sample));
}

void Record(ID3D11DeviceContext* context, char kind, UINT count, UINT instances)
{
    g_drawsThisFrame.fetch_add(1, std::memory_order_relaxed);
    if (PerfGpuPassWantsDraw(context)) {
        ComPtr<ID3D11RenderTargetView> color;
        ComPtr<ID3D11DepthStencilView> depth;
        context->OMGetRenderTargets(1, color.GetAddressOf(), depth.GetAddressOf());
        TargetInfo target;
        if (color) {
            DescribeTexture(color.Get(), target);
        } else if (depth) {
            DescribeTexture(depth.Get(), target);
        }
        D3D11_VIEWPORT viewport{};
        UINT viewports = 1;
        context->RSGetViewports(&viewports, &viewport);

        const bool quad = (kind == 'D' || kind == 'I') && (count == 3 || count == 4 || count == 6);
        std::uint64_t hash = 0;
        if (color && quad) {
            ComPtr<ID3D11PixelShader> shader;
            context->PSGetShader(shader.GetAddressOf(), nullptr, nullptr);
            std::scoped_lock lock(g_cullMutex);
            const auto found = g_pixelShaderHash.find(shader.Get());
            if (found != g_pixelShaderHash.end()) {
                hash = found->second;
            }
        }
        PerfGpuPassDraw(context, target.width, target.height, target.format, color.Get() != nullptr,
                        depth.Get() != nullptr, viewport.MaxDepth, quad, hash);
    }
    if (!g_capturing.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock lock(g_mutex);
    if (!g_capturing.load()) {
        return;
    }
    if (g_records.size() >= kMaxRecords) {
        g_dropped = true;
        return;
    }
    DrawRecord record;
    record.kind = kind;
    record.count = count;
    record.instances = instances;
    record.thread = GetCurrentThreadId();
    record.deferred = context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE;

    ID3D11RenderTargetView* targets[4]{};
    ID3D11DepthStencilView* depth = nullptr;
    context->OMGetRenderTargets(4, targets, &depth);
    for (UINT i = 0; i < 4; ++i) {
        if (!targets[i]) {
            continue;
        }
        ++record.targetCount;
        if (i == 0) {
            DescribeTexture(targets[i], record.target0);
            D3D11_RENDER_TARGET_VIEW_DESC description{};
            targets[i]->GetDesc(&description);
            record.target0.viewFormat = static_cast<UINT>(description.Format);
        } else {
            TargetInfo info;
            DescribeTexture(targets[i], info);
            record.extraTargets[i - 1] = info.texture;
        }
        targets[i]->Release();
    }
    if (depth) {
        DescribeTexture(depth, record.depth);
        D3D11_DEPTH_STENCIL_VIEW_DESC description{};
        depth->GetDesc(&description);
        record.depth.viewFormat = static_cast<UINT>(description.Format);
        depth->Release();
    }

    UINT viewportCount = 1;
    D3D11_VIEWPORT viewport{};
    context->RSGetViewports(&viewportCount, &viewport);
    if (viewportCount > 0) {
        record.viewport[0] = viewport.TopLeftX;
        record.viewport[1] = viewport.TopLeftY;
        record.viewport[2] = viewport.Width;
        record.viewport[3] = viewport.Height;
        record.depthRange[0] = viewport.MinDepth;
        record.depthRange[1] = viewport.MaxDepth;
    }

    if (GetConfig().detailedCapture && !record.deferred && record.depth.width >= 1024) {
        CaptureStack& stack = record.depthRange[1] <= 0.0105F ? g_weaponDrawStack : g_worldDrawStack;
        if (!stack[0]) {
            CaptureStackBackTrace(0, static_cast<DWORD>(stack.size()), stack.data(), nullptr);
        }
    }

    ID3D11VertexShader* vertexShader = nullptr;
    context->VSGetShader(&vertexShader, nullptr, nullptr);
    record.vertexShader = vertexShader;
    if (vertexShader) {
        vertexShader->Release();
    }
    ID3D11PixelShader* pixelShader = nullptr;
    context->PSGetShader(&pixelShader, nullptr, nullptr);
    record.pixelShader = pixelShader;
    if (pixelShader) {
        pixelShader->Release();
    }

    ID3D11DepthStencilState* state = nullptr;
    UINT stencilReference = 0;
    context->OMGetDepthStencilState(&state, &stencilReference);
    if (state) {
        D3D11_DEPTH_STENCIL_DESC description{};
        state->GetDesc(&description);
        record.depthTest = description.DepthEnable != FALSE;
        record.depthWrite = description.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
        state->Release();
    }

    const std::uint32_t drawIndex = static_cast<std::uint32_t>(g_records.size());
    ID3D11Buffer* buffers[kSlots]{};
    context->VSGetConstantBuffers(0, kSlots, buffers);
    for (UINT slot = 0; slot < kSlots; ++slot) {
        if (!buffers[slot]) {
            continue;
        }
        D3D11_BUFFER_DESC description{};
        buffers[slot]->GetDesc(&description);
        record.buffers[slot] = buffers[slot];
        record.bufferSizes[slot] = description.ByteWidth;
        record.bufferUsage[slot] = static_cast<UINT>(description.Usage);
        record.bufferCpu[slot] = description.CPUAccessFlags;
        const bool firstPerson = record.depthRange[1] <= 0.0105F;
        if (!record.deferred &&
            g_sampled.insert({record.vertexShader, record.target0.texture, slot, firstPerson}).second) {
            SampleBuffer(context, buffers[slot], description.ByteWidth, slot, drawIndex);
        }
        buffers[slot]->Release();
    }
    g_records.push_back(record);
}


// ---- shader browser ---------------------------------------------------------------------
struct BrowseShader {
    std::uint64_t hash = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    unsigned width = 0;
    unsigned height = 0;
};
std::atomic<bool> g_browse{false};
std::mutex g_browseMutex;
std::vector<BrowseShader> g_catalog;       // first-appearance order
std::vector<std::string> g_markedLines;    // what was marked this session
int g_browseIndex = 0;
std::atomic<int> g_browseMode{0};          // 0 off, 1 screen-space shaders, 2 UI draws (texture kinds)
std::vector<BrowseShader> g_uiCatalog;     // UI draw kinds: shader + texture description
int g_uiIndex = 0;

void RegisterBrowseShader(ID3D11DeviceContext* context, std::uint64_t hash)
{
    {
        std::scoped_lock lock(g_browseMutex);
        for (const BrowseShader& known : g_catalog) {
            if (known.hash == hash) {
                return;
            }
        }
    }
    BrowseShader entry;
    entry.hash = hash;
    ID3D11RenderTargetView* rtv = nullptr;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv) {
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        rtv->GetResource(&resource);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (resource && SUCCEEDED(resource.As(&texture))) {
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            entry.format = description.Format;
            entry.width = description.Width;
            entry.height = description.Height;
        }
        rtv->Release();
    }
    std::scoped_lock lock(g_browseMutex);
    g_catalog.push_back(entry);
    Log("SHADER browser: new screen-space shader %016llx (target format %d %ux%u), #%zu",
        static_cast<unsigned long long>(hash), static_cast<int>(entry.format), entry.width,
        entry.height, g_catalog.size());
}

// UI draw kinds (browser mode 2): the pixel shader plus the description of the texture bound at
// slot 0 (an icon atlas, the glyph page, a plain colour...). Returns true when this draw is the
// selected kind and must be skipped.
bool UiBrowseCulls(ID3D11DeviceContext* context, std::uint64_t hash)
{
    if (!EngineStereoTargetIsBackBuffer(context)) {
        return false;
    }
    BrowseShader kind;
    kind.hash = hash;
    ID3D11ShaderResourceView* srv = nullptr;
    context->PSGetShaderResources(0, 1, &srv);
    if (srv) {
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        srv->GetResource(&resource);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (resource && SUCCEEDED(resource.As(&texture))) {
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            kind.format = description.Format;
            kind.width = description.Width;
            kind.height = description.Height;
        }
        srv->Release();
    }
    std::scoped_lock lock(g_browseMutex);
    int found = -1;
    for (size_t i = 0; i < g_uiCatalog.size(); ++i) {
        const BrowseShader& known = g_uiCatalog[i];
        if (known.hash == kind.hash && known.format == kind.format && known.width == kind.width &&
            known.height == kind.height) {
            found = static_cast<int>(i);
            break;
        }
    }
    if (found < 0) {
        g_uiCatalog.push_back(kind);
        found = static_cast<int>(g_uiCatalog.size()) - 1;
        Log("UI browser: new UI draw kind #%d shader %016llx texture %d %ux%u", found + 1,
            static_cast<unsigned long long>(kind.hash), static_cast<int>(kind.format), kind.width,
            kind.height);
    }
    return found == g_uiIndex;
}

bool BrowseCulls(std::uint64_t hash)
{
    std::scoped_lock lock(g_browseMutex);
    return g_browseIndex >= 0 && g_browseIndex < static_cast<int>(g_catalog.size()) &&
           g_catalog[static_cast<size_t>(g_browseIndex)].hash == hash;
}
// True for a small screen-space draw of the god-ray effect into the scene colour target.
bool ShouldCullDraw(ID3D11DeviceContext* context, UINT elementCount)
{
    if (elementCount > 6 ||
        (!g_browse.load() && !GetConfig().cullGodRays && GetConfig().cullHashes.empty()) ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        return false;
    }
    ID3D11PixelShader* pixelShader = nullptr;
    context->PSGetShader(&pixelShader, nullptr, nullptr);
    if (!pixelShader) {
        return false;
    }
    std::uint64_t hash = 0;
    bool known = false;
    {
        std::scoped_lock lock(g_cullMutex);
        const auto found = g_pixelShaderHash.find(pixelShader);
        if (found != g_pixelShaderHash.end()) {
            hash = found->second;
            known = true;
        }
    }
    pixelShader->Release();
    if (known && g_browseMode.load() == 1) {
        RegisterBrowseShader(context, hash);
        if (BrowseCulls(hash)) {
            return true;
        }
    } else if (known && g_browseMode.load() == 2 && UiBrowseCulls(context, hash)) {
        return true;
    }
    if (known) {
        const auto& list = GetConfig().cullHashes;
        if (std::find(list.begin(), list.end(), hash) != list.end()) {
            static std::atomic<std::uint32_t> experimentLogs{0};
            if (experimentLogs.fetch_add(1) < 3) {
                Log("CULL experiment: draw with shader %016llx skipped (CullHashes)",
                    static_cast<unsigned long long>(hash));
            }
            return true;
        }
    }
    if (!known || !GetConfig().cullGodRays ||
        std::find(std::begin(kGodRayShaders), std::end(kGodRayShaders), hash) ==
            std::end(kGodRayShaders)) {
        return false;
    }
    bool sceneColour = false;
    ID3D11RenderTargetView* rtv = nullptr;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv) {
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        rtv->GetResource(&resource);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (resource && SUCCEEDED(resource.As(&texture))) {
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            unsigned sceneWidth = 0;
            unsigned sceneHeight = 0;
            EngineStereoSceneSize(sceneWidth, sceneHeight);
            sceneColour = description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                          description.Width == sceneWidth && description.Height == sceneHeight;
        }
        rtv->Release();
    }
    if (sceneColour) {
        const std::uint32_t count = g_culledDraws.fetch_add(1) + 1;
        if (count == 1 || count % 3000 == 0) {
            Log("CULL god-ray draw skipped (shader %016llx), %u so far",
                static_cast<unsigned long long>(hash), count);
        }
    }
    return sceneColour;
}

void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* context, UINT indexCount,
                                       UINT startIndex, INT baseVertex)
{
    if (ShouldCullDraw(context, indexCount)) {
        return;
    }
    PerfDrawScope perf;
    Record(context, 'I', indexCount, 1);
    StereoDraw stereo(context, indexCount, false);
    perf.AfterBegin();
    g_drawIndexed(context, indexCount, startIndex, baseVertex);
    perf.AfterOriginal();
    const bool second = stereo.NextEye();
    perf.AfterNext(second);
    if (second) {
        g_drawIndexed(context, indexCount, startIndex, baseVertex);
        perf.AfterSecond();
    }
}

void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertex)
{
    if (ShouldCullDraw(context, vertexCount)) {
        return;
    }
    PerfDrawScope perf;
    Record(context, 'D', vertexCount, 1);
    StereoDraw stereo(context, vertexCount, false);
    perf.AfterBegin();
    g_draw(context, vertexCount, startVertex);
    perf.AfterOriginal();
    const bool second = stereo.NextEye();
    perf.AfterNext(second);
    if (second) {
        g_draw(context, vertexCount, startVertex);
        perf.AfterSecond();
    }
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D11DeviceContext* context, UINT indexCount,
                                                UINT instanceCount, UINT startIndex,
                                                INT baseVertex, UINT startInstance)
{
    PerfDrawScope perf;
    Record(context, 'i', indexCount, instanceCount);
    StereoDraw stereo(context, indexCount, true);
    perf.AfterBegin();
    g_drawIndexedInstanced(context, indexCount, instanceCount, startIndex, baseVertex,
                           startInstance);
    perf.AfterOriginal();
    const bool second = stereo.NextEye();
    perf.AfterNext(second);
    if (second) {
        g_drawIndexedInstanced(context, indexCount, instanceCount, startIndex, baseVertex,
                               startInstance);
        perf.AfterSecond();
    }
}

void STDMETHODCALLTYPE HookDrawInstanced(ID3D11DeviceContext* context, UINT vertexCount,
                                         UINT instanceCount, UINT startVertex,
                                         UINT startInstance)
{
    PerfDrawScope perf;
    Record(context, 'd', vertexCount, instanceCount);
    StereoDraw stereo(context, vertexCount, true);
    perf.AfterBegin();
    g_drawInstanced(context, vertexCount, instanceCount, startVertex, startInstance);
    perf.AfterOriginal();
    const bool second = stereo.NextEye();
    perf.AfterNext(second);
    if (second) {
        g_drawInstanced(context, vertexCount, instanceCount, startVertex, startInstance);
        perf.AfterSecond();
    }
}

void STDMETHODCALLTYPE HookDrawIndexedInstancedIndirect(ID3D11DeviceContext* context,
                                                        ID3D11Buffer* arguments, UINT offset)
{
    PerfDrawScope perf;
    Record(context, 'x', 0, 0);
    StereoDraw stereo(context, 0, true);
    perf.AfterBegin();
    g_drawIndexedInstancedIndirect(context, arguments, offset);
    perf.AfterOriginal();
    const bool second = stereo.NextEye();
    perf.AfterNext(second);
    if (second) {
        g_drawIndexedInstancedIndirect(context, arguments, offset);
        perf.AfterSecond();
    }
}

void STDMETHODCALLTYPE HookDrawInstancedIndirect(ID3D11DeviceContext* context,
                                                 ID3D11Buffer* arguments, UINT offset)
{
    PerfDrawScope perf;
    Record(context, 'y', 0, 0);
    StereoDraw stereo(context, 0, true);
    perf.AfterBegin();
    g_drawInstancedIndirect(context, arguments, offset);
    perf.AfterOriginal();
    const bool second = stereo.NextEye();
    perf.AfterNext(second);
    if (second) {
        g_drawInstancedIndirect(context, arguments, offset);
        perf.AfterSecond();
    }
}

HRESULT STDMETHODCALLTYPE HookCreateVertexShader(ID3D11Device* device, const void* bytecode,
                                                 SIZE_T length, ID3D11ClassLinkage* linkage,
                                                 ID3D11VertexShader** shader)
{
    const HRESULT result = g_createVertexShader(device, bytecode, length, linkage, shader);
    if (SUCCEEDED(result) && shader && *shader && bytecode && length > 0) {
        EngineStereoNoteVertexShader(*shader, bytecode, length);
    }
    if (SUCCEEDED(result) && shader && *shader && bytecode && length > 0 &&
        length < 4U * 1024U * 1024U) {
        std::scoped_lock lock(g_mutex);
        const auto* bytes = static_cast<const std::uint8_t*>(bytecode);
        g_vertexShaderCode[*shader].assign(bytes, bytes + length);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreatePixelShader(ID3D11Device* device, const void* bytecode,
                                                SIZE_T length, ID3D11ClassLinkage* linkage,
                                                ID3D11PixelShader** shader)
{
    const HRESULT result = g_createPixelShader(device, bytecode, length, linkage, shader);
    if (SUCCEEDED(result) && shader && *shader && bytecode && length > 0 &&
        length < 4U * 1024U * 1024U) {
        const auto* bytes = static_cast<const std::uint8_t*>(bytecode);
        std::uint64_t hash = 1469598103934665603ULL;  // the same FNV hash as the file names
        for (SIZE_T i = 0; i < length; ++i) {
            hash = (hash ^ bytes[i]) * 1099511628211ULL;
        }
        {
            std::scoped_lock cullLock(g_cullMutex);
            g_pixelShaderHash[*shader] = hash;
        }
        EngineStereoNotePixelShader(*shader, hash);
        if (GetConfig().detailedCapture) {
            std::scoped_lock lock(g_mutex);
            g_pixelShaderCode[*shader].assign(bytes, bytes + length);
        }
    }
    return result;
}

bool Hook(void* target, void* replacement, void** original, const char* name)
{
    const MH_STATUS created = MH_CreateHook(target, replacement, original);
    if (created != MH_OK) {
        Log("PROBE hook %s failed: %s", name, MH_StatusToString(created));
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK) {
        Log("PROBE hook %s not enabled: %s", name, MH_StatusToString(enabled));
        return false;
    }
    return true;
}

void (STDMETHODCALLTYPE* g_unmap)(ID3D11DeviceContext*, ID3D11Resource*, UINT) = nullptr;
void (STDMETHODCALLTYPE* g_copyResource)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*) = nullptr;
void (STDMETHODCALLTYPE* g_copyRegion)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
                                      ID3D11Resource*, UINT, const D3D11_BOX*) = nullptr;
void (STDMETHODCALLTYPE* g_update)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                   const void*, UINT, UINT) = nullptr;
void (STDMETHODCALLTYPE* g_copyCount)(ID3D11DeviceContext*, ID3D11Buffer*, UINT,
                                     ID3D11UnorderedAccessView*) = nullptr;
void (STDMETHODCALLTYPE* g_execute)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL) = nullptr;
void (STDMETHODCALLTYPE* g_copyRegion1)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, UINT, UINT, UINT,
                                       ID3D11Resource*, UINT, const D3D11_BOX*, UINT) = nullptr;
void (STDMETHODCALLTYPE* g_update1)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                    const void*, UINT, UINT, UINT) = nullptr;
void (STDMETHODCALLTYPE* g_discard)(ID3D11DeviceContext1*, ID3D11Resource*) = nullptr;

void STDMETHODCALLTYPE CacheUnmap(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT sub)
{
    g_unmap(ctx, dst, sub);
    EngineProjectionCache().Written(dst);
}

void STDMETHODCALLTYPE CacheCopyResource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src)
{
    g_copyResource(ctx, dst, src);
    EngineProjectionCache().Written(dst);
}

void STDMETHODCALLTYPE CacheCopyRegion(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT sub,
                                        UINT x, UINT y, UINT z, ID3D11Resource* src, UINT srcSub,
                                        const D3D11_BOX* box)
{
    g_copyRegion(ctx, dst, sub, x, y, z, src, srcSub, box);
    EngineProjectionCache().Written(dst);
}

void STDMETHODCALLTYPE CacheUpdate(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT sub,
                                    const D3D11_BOX* box, const void* data, UINT row, UINT depth)
{
    g_update(ctx, dst, sub, box, data, row, depth);
    EngineProjectionCache().Written(dst);
}

void STDMETHODCALLTYPE CacheCopyCount(ID3D11DeviceContext* ctx, ID3D11Buffer* dst, UINT offset,
                                       ID3D11UnorderedAccessView* src)
{
    g_copyCount(ctx, dst, offset, src);
    EngineProjectionCache().Written(dst);
}

void STDMETHODCALLTYPE CacheExecute(ID3D11DeviceContext* ctx, ID3D11CommandList* list, BOOL restore)
{
    g_execute(ctx, list, restore);
    EngineProjectionCache().Invalidate();
}

void STDMETHODCALLTYPE CacheCopyRegion1(ID3D11DeviceContext1* ctx, ID3D11Resource* dst, UINT sub,
                                         UINT x, UINT y, UINT z, ID3D11Resource* src, UINT srcSub,
                                         const D3D11_BOX* box, UINT flags)
{
    g_copyRegion1(ctx, dst, sub, x, y, z, src, srcSub, box, flags);
    EngineProjectionCache().Written(dst);
}

void STDMETHODCALLTYPE CacheUpdate1(ID3D11DeviceContext1* ctx, ID3D11Resource* dst, UINT sub,
                                     const D3D11_BOX* box, const void* data, UINT row, UINT depth, UINT flags)
{
    g_update1(ctx, dst, sub, box, data, row, depth, flags);
    EngineProjectionCache().Written(dst);
}

void STDMETHODCALLTYPE CacheDiscard(ID3D11DeviceContext1* ctx, ID3D11Resource* dst)
{
    g_discard(ctx, dst);
    EngineProjectionCache().Written(dst);
}

struct ProjectionHook {
    UINT slot;
    void* replacement;
    void** original;
    const char* name;
    void* address = nullptr;
};

// A constant buffer cannot be bound as an RT, UAV or stream-out target. Watch
// copies and CPU updates, and invalidate conservatively when a command list runs.
ProjectionHook g_projectionHooks[]{
    {15, reinterpret_cast<void*>(&CacheUnmap), reinterpret_cast<void**>(&g_unmap), "Projection Unmap"},
    {46, reinterpret_cast<void*>(&CacheCopyRegion), reinterpret_cast<void**>(&g_copyRegion), "Projection CopySubresourceRegion"},
    {47, reinterpret_cast<void*>(&CacheCopyResource), reinterpret_cast<void**>(&g_copyResource), "Projection CopyResource"},
    {48, reinterpret_cast<void*>(&CacheUpdate), reinterpret_cast<void**>(&g_update), "Projection UpdateSubresource"},
    {49, reinterpret_cast<void*>(&CacheCopyCount), reinterpret_cast<void**>(&g_copyCount), "Projection CopyStructureCount"},
    {58, reinterpret_cast<void*>(&CacheExecute), reinterpret_cast<void**>(&g_execute), "Projection ExecuteCommandList"},
};
ProjectionHook g_projectionHooks1[]{
    {115, reinterpret_cast<void*>(&CacheCopyRegion1), reinterpret_cast<void**>(&g_copyRegion1), "Projection CopySubresourceRegion1"},
    {116, reinterpret_cast<void*>(&CacheUpdate1), reinterpret_cast<void**>(&g_update1), "Projection UpdateSubresource1"},
    {117, reinterpret_cast<void*>(&CacheDiscard), reinterpret_cast<void**>(&g_discard), "Projection DiscardResource"},
};
bool g_projectionTracking = false;
bool g_projectionContext1 = false;

void InstallProjectionHooks(ID3D11DeviceContext* ctx)
{
    const auto install = [](auto* object, auto& hooks) {
        void** table = *reinterpret_cast<void***>(object);
        bool ok = true;
        for (auto& hook : hooks) {
            hook.address = table[hook.slot];
            ok &= Hook(hook.address, hook.replacement, hook.original, hook.name);
        }
        return ok;
    };
    bool ok = install(ctx, g_projectionHooks);
    ComPtr<ID3D11DeviceContext1> context1;
    g_projectionContext1 = SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&context1)));
    if (g_projectionContext1) {
        ok &= install(context1.Get(), g_projectionHooks1);
    }
    g_projectionTracking = ok;
}

std::uint64_t Hash(const std::vector<std::uint8_t>& data)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint8_t byte : data) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    return hash;
}

struct ShaderUse {
    int id = 0;
    std::uint32_t draws = 0;
    std::set<int> passes;
    UINT bufferSizes[kSlots]{};
};

void WriteReflection(FILE* file, const std::vector<std::uint8_t>& code)
{
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(code.data(), code.size(), IID_ID3D11ShaderReflection,
                          reinterpret_cast<void**>(reflection.GetAddressOf()))) ||
        !reflection) {
        std::fprintf(file, "    reflection: FAILED\n");
        return;
    }
    D3D11_SHADER_DESC shader{};
    reflection->GetDesc(&shader);
    std::fprintf(file, "    instructions=%u inputs=%u outputs=%u cbuffers=%u\n",
                 shader.InstructionCount, shader.InputParameters, shader.OutputParameters,
                 shader.ConstantBuffers);
    std::fprintf(file, "    in :");
    for (UINT i = 0; i < shader.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC parameter{};
        if (SUCCEEDED(reflection->GetInputParameterDesc(i, &parameter))) {
            std::fprintf(file, " %s%u", parameter.SemanticName ? parameter.SemanticName : "?",
                         parameter.SemanticIndex);
        }
    }
    std::fprintf(file, "\n    out:");
    for (UINT i = 0; i < shader.OutputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC parameter{};
        if (SUCCEEDED(reflection->GetOutputParameterDesc(i, &parameter))) {
            std::fprintf(file, " %s%u(o%u mask=%X)",
                         parameter.SemanticName ? parameter.SemanticName : "?",
                         parameter.SemanticIndex, parameter.Register,
                         static_cast<unsigned>(parameter.Mask));
        }
    }
    std::fprintf(file, "\n");
    for (UINT i = 0; i < shader.ConstantBuffers; ++i) {
        ID3D11ShaderReflectionConstantBuffer* buffer = reflection->GetConstantBufferByIndex(i);
        D3D11_SHADER_BUFFER_DESC description{};
        if (!buffer || FAILED(buffer->GetDesc(&description))) {
            continue;
        }
        int slot = -1;
        D3D11_SHADER_INPUT_BIND_DESC bind{};
        if (description.Name &&
            SUCCEEDED(reflection->GetResourceBindingDescByName(description.Name, &bind))) {
            slot = static_cast<int>(bind.BindPoint);
        }
        std::fprintf(file, "    cbuffer %s slot=%d size=%u variables=%u\n",
                     description.Name ? description.Name : "?", slot, description.Size,
                     description.Variables);
        for (UINT v = 0; v < description.Variables && v < 48; ++v) {
            ID3D11ShaderReflectionVariable* variable = buffer->GetVariableByIndex(v);
            D3D11_SHADER_VARIABLE_DESC variableDescription{};
            if (!variable || FAILED(variable->GetDesc(&variableDescription))) {
                continue;
            }
            std::fprintf(file, "      +%-5u %-5u %s%s\n", variableDescription.StartOffset,
                         variableDescription.Size,
                         variableDescription.Name ? variableDescription.Name : "?",
                         (variableDescription.uFlags & D3D_SVF_USED) ? "" : " (unused)");
        }
    }
}

void WriteStack(FILE* file, const char* label, const CaptureStack& stack)
{
    std::fprintf(file, "\n==== %s stack (module + RVA) ====\n", label);
    for (void* address : stack) {
        if (!address) {
            break;
        }
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH]{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address), &module)) {
            GetModuleFileNameW(module, path, MAX_PATH);
        }
        const auto delta = reinterpret_cast<std::uintptr_t>(address) -
                           reinterpret_cast<std::uintptr_t>(module);
        const auto moduleName = std::filesystem::path(path).filename();
        std::fprintf(file, "  %ls + 0x%llx (%p)\n", moduleName.c_str(),
                     static_cast<unsigned long long>(delta), address);
    }
}

// Present thread, capture already stopped.
void WriteCapture(IDXGISwapChain* swapChain)
{
    std::vector<DrawRecord> records;
    std::vector<BufferSample> samples;
    std::unordered_map<void*, std::vector<std::uint8_t>> code;
    std::unordered_map<void*, std::vector<std::uint8_t>> pixelCode;
    CaptureStack worldDrawStack{};
    CaptureStack weaponDrawStack{};
    bool dropped = false;
    {
        std::scoped_lock lock(g_mutex);
        records.swap(g_records);
        samples.swap(g_samples);
        g_sampled.clear();
        g_staging.clear();
        g_sampleBytes = 0;
        dropped = g_dropped;
        g_dropped = false;
        worldDrawStack.swap(g_worldDrawStack);
        weaponDrawStack.swap(g_weaponDrawStack);
        for (const DrawRecord& record : records) {
            const auto pixel = g_pixelShaderCode.find(record.pixelShader);
            if (pixel != g_pixelShaderCode.end()) {
                pixelCode.try_emplace(record.pixelShader, pixel->second);
            }
            const auto found = g_vertexShaderCode.find(record.vertexShader);
            if (found != g_vertexShaderCode.end()) {
                code.emplace(record.vertexShader, found->second);
            }
        }
    }
    ++g_captures;
    const std::filesystem::path base =
        ModuleDirectory() / (L"arma3_native_vr.frame" + std::to_wstring(g_captures));
    FILE* file = nullptr;
    if (_wfopen_s(&file, (base.wstring() + L".txt").c_str(), L"w") != 0 || !file) {
        Log("PROBE could not write %ls.txt", base.filename().c_str());
        return;
    }

    DXGI_SWAP_CHAIN_DESC swap{};
    swapChain->GetDesc(&swap);
    ComPtr<ID3D11Texture2D> backBuffer;
    swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    std::fprintf(file, "Arma3NativeVR frame probe, capture %d\n", g_captures);
    std::fprintf(file, "back buffer %ux%u format=%u texture=%p\n", swap.BufferDesc.Width,
                 swap.BufferDesc.Height, static_cast<UINT>(swap.BufferDesc.Format),
                 static_cast<void*>(backBuffer.Get()));
    std::fprintf(file, "draws recorded=%zu%s, constant buffer samples=%zu\n", records.size(),
                 dropped ? " (LIMIT HIT, frame is cut)" : "", samples.size());

    using ProjectionViewFn = void (*)(float*);
    const auto projectionView = reinterpret_cast<ProjectionViewFn>(
        GetProcAddress(GetModuleHandleW(nullptr), "RVGetProjectionViewTransform"));
    if (projectionView) {
        float matrices[32]{};
        projectionView(matrices);
        std::fprintf(file, "RVGetProjectionViewTransform (16 projection, 16 view):\n");
        for (int row = 0; row < 8; ++row) {
            std::fprintf(file, "  %14.6f %14.6f %14.6f %14.6f\n", matrices[row * 4],
                         matrices[row * 4 + 1], matrices[row * 4 + 2], matrices[row * 4 + 3]);
        }
    }

    std::map<void*, int> textureIds;
    const auto textureId = [&textureIds](void* texture) {
        if (!texture) {
            return 0;
        }
        const auto inserted =
            textureIds.emplace(texture, static_cast<int>(textureIds.size()) + 1);
        return inserted.first->second;
    };
    std::map<void*, ShaderUse> shaders;
    std::set<DWORD> threads;
    std::uint32_t deferredDraws = 0;

    for (const DrawRecord& record : records) {
        if (record.bufferSizes[2] != 0) {
            std::fprintf(file, "slot 2 constant buffer: size=%u usage=%u (0 default, 2 dynamic) "
                               "cpuAccess=0x%X\n",
                         record.bufferSizes[2], record.bufferUsage[2], record.bufferCpu[2]);
            break;
        }
    }
    std::fprintf(file, "\n==== passes (a pass = consecutive draws into the same targets) ====\n");
    int pass = 0;
    std::size_t start = 0;
    while (start < records.size()) {
        std::size_t end = start;
        std::set<void*> passShaders;
        std::uint32_t instanced = 0;
        std::uint32_t noDepthTest = 0;
        std::uint32_t noDepthWrite = 0;
        std::uint64_t elements = 0;
        float depthMin = 1.0F;
        float depthMax = 0.0F;
        const DrawRecord& first = records[start];
        while (end < records.size() && records[end].target0.texture == first.target0.texture &&
               records[end].depth.texture == first.depth.texture &&
               records[end].targetCount == first.targetCount) {
            const DrawRecord& record = records[end];
            passShaders.insert(record.vertexShader);
            instanced += (record.kind == 'i' || record.kind == 'd') ? 1U : 0U;
            noDepthTest += record.depthTest ? 0U : 1U;
            noDepthWrite += record.depthWrite ? 0U : 1U;
            elements += static_cast<std::uint64_t>(record.count) * record.instances;
            depthMin = std::min(depthMin, record.depthRange[0]);
            depthMax = std::max(depthMax, record.depthRange[1]);
            threads.insert(record.thread);
            deferredDraws += record.deferred ? 1U : 0U;
            ShaderUse& use = shaders[record.vertexShader];
            if (use.id == 0) {
                use.id = static_cast<int>(shaders.size());
            }
            ++use.draws;
            use.passes.insert(pass);
            for (UINT slot = 0; slot < kSlots; ++slot) {
                if (record.bufferSizes[slot] > use.bufferSizes[slot]) {
                    use.bufferSizes[slot] = record.bufferSizes[slot];
                }
            }
            ++end;
        }
        std::fprintf(file,
                     "pass %3d: draws %5zu..%-5zu n=%-5zu | RT0=T%d %ux%u fmt=%u view=%u x%u%s"
                     " targets=%u",
                     pass, start, end - 1, end - start, textureId(first.target0.texture),
                     first.target0.width, first.target0.height, first.target0.format,
                     first.target0.viewFormat, first.target0.samples,
                     first.target0.texture == backBuffer.Get() ? " BACKBUFFER" : "",
                     first.targetCount);
        for (void* extra : first.extraTargets) {
            if (extra) {
                std::fprintf(file, " +T%d", textureId(extra));
            }
        }
        if (first.depth.texture) {
            std::fprintf(file, " | DS=T%d %ux%u fmt=%u view=%u x%u", textureId(first.depth.texture),
                         first.depth.width, first.depth.height, first.depth.format,
                         first.depth.viewFormat, first.depth.samples);
        } else {
            std::fprintf(file, " | DS=none");
        }
        std::fprintf(file,
                     " | vp %.0f,%.0f %.0fx%.0f z%.2f..%.2f | VS=%zu instanced=%u noZtest=%u"
                     " noZwrite=%u elements=%llu\n",
                     first.viewport[0], first.viewport[1], first.viewport[2], first.viewport[3],
                     depthMin, depthMax,
                     passShaders.size(), instanced, noDepthTest, noDepthWrite,
                     static_cast<unsigned long long>(elements));
        start = end;
        ++pass;
    }
    std::fprintf(file, "passes=%d threads=%zu deferred-context draws=%u\n", pass, threads.size(),
                 deferredDraws);

    std::error_code error;
    const std::filesystem::path shaderFolder = ModuleDirectory() / L"arma3_vr_shaders";
    std::filesystem::create_directories(shaderFolder, error);
    std::fprintf(file, "\n==== vertex shaders used in this frame: %zu ====\n", shaders.size());
    std::uint32_t withoutCode = 0;
    for (const auto& [pointer, use] : shaders) {
        std::fprintf(file, "VS%d %p draws=%u passes:", use.id, pointer, use.draws);
        int listed = 0;
        for (const int usedPass : use.passes) {
            if (listed++ < 12) {
                std::fprintf(file, " %d", usedPass);
            }
        }
        std::fprintf(file, "%s | bound cb sizes:", use.passes.size() > 12 ? " ..." : "");
        for (UINT slot = 0; slot < kSlots; ++slot) {
            std::fprintf(file, " %u", use.bufferSizes[slot]);
        }
        const auto found = code.find(pointer);
        if (found == code.end()) {
            ++withoutCode;
            std::fprintf(file, "\n    bytecode: NOT SEEN (created before the probe)\n");
            continue;
        }
        const std::uint64_t hash = Hash(found->second);
        std::fprintf(file, "\n    bytecode: %zu bytes, vs_%016llx.dxbc\n", found->second.size(),
                     static_cast<unsigned long long>(hash));
        wchar_t name[64]{};
        swprintf_s(name, L"vs_%016llx.dxbc", static_cast<unsigned long long>(hash));
        FILE* shaderFile = nullptr;
        if (_wfopen_s(&shaderFile, (shaderFolder / name).c_str(), L"wb") == 0 && shaderFile) {
            std::fwrite(found->second.data(), 1, found->second.size(), shaderFile);
            std::fclose(shaderFile);
        }
        WriteReflection(file, found->second);
    }

    if (GetConfig().detailedCapture) {
        CaptureStack presentStack{};
        CaptureStackBackTrace(0, static_cast<DWORD>(presentStack.size()), presentStack.data(), nullptr);
        WriteStack(file, "Present", presentStack);
        WriteStack(file, "world draw", worldDrawStack);
        WriteStack(file, "weapon draw", weaponDrawStack);
        std::fprintf(file, "\n==== pixel shader bytecode ====\n");
        for (const auto& [pointer, bytes] : pixelCode) {
            wchar_t name[64]{};
            swprintf_s(name, L"ps_%016llx.dxbc", static_cast<unsigned long long>(Hash(bytes)));
            std::fprintf(file, "PS %p %ls %zu bytes\n", pointer, name, bytes.size());
            FILE* shaderFile = nullptr;
            if (_wfopen_s(&shaderFile, (shaderFolder / name).c_str(), L"wb") == 0 && shaderFile) {
                std::fwrite(bytes.data(), 1, bytes.size(), shaderFile);
                std::fclose(shaderFile);
            }
        }
        std::fprintf(file, "\n==== draws (ordinal, kind, elements, instances, thread, VS, PS, RT, DS, depth range) ====\n");
        for (std::size_t draw = 0; draw < records.size(); ++draw) {
            const auto& record = records[draw];
            std::fprintf(file, "%zu %c %u %u %lu %p %p T%d T%d %.6f %.6f\n", draw,
                         record.kind, record.count, record.instances, record.thread,
                         record.vertexShader, record.pixelShader, textureId(record.target0.texture),
                         textureId(record.depth.texture), record.depthRange[0], record.depthRange[1]);
        }
        if (!DumpBackBufferJpeg(swapChain, (base.wstring() + L".jpg").c_str())) {
            Log("PROBE frame image failed: %ls.jpg", base.filename().c_str());
        }
    }

    std::fprintf(file, "\n==== constant buffer samples (data in .cb.bin) ====\n");
    FILE* binary = nullptr;
    _wfopen_s(&binary, (base.wstring() + L".cb.bin").c_str(), L"wb");
    std::uint64_t offset = 0;
    for (const BufferSample& sample : samples) {
        const DrawRecord& record = records[sample.draw];
        std::fprintf(file, "draw %u VS%d slot=%u size=%u stored=%zu at=%llu RT0=T%d\n",
                     sample.draw, shaders[record.vertexShader].id, sample.slot, sample.fullSize,
                     sample.data.size(), static_cast<unsigned long long>(offset),
                     textureId(record.target0.texture));
        if (binary) {
            std::fwrite(sample.data.data(), 1, sample.data.size(), binary);
        }
        offset += sample.data.size();
    }
    if (binary) {
        std::fclose(binary);
    }
    std::fclose(file);
    Log("PROBE capture %d written: %zu draws, %d passes, %zu vertex shaders (%u without "
        "bytecode), %zu buffer samples, %zu threads, deferred draws=%u",
        g_captures, records.size(), pass, shaders.size(), withoutCode, samples.size(),
        threads.size(), deferredDraws);
}

}  // namespace

bool EnableProjectionCacheForContext(ID3D11DeviceContext* context)
{
    if (!g_projectionTracking) {
        return false;
    }
    const auto matches = [](auto* object, const auto& hooks) {
        void** table = *reinterpret_cast<void***>(object);
        return std::all_of(std::begin(hooks), std::end(hooks),
                           [&](const ProjectionHook& hook) { return table[hook.slot] == hook.address; });
    };
    ComPtr<ID3D11DeviceContext1> context1;
    const bool hasContext1 = SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context1)));
    if (!matches(context, g_projectionHooks) || hasContext1 != g_projectionContext1 ||
        (hasContext1 && (!matches(context1.Get(), g_projectionHooks) || !matches(context1.Get(), g_projectionHooks1)))) {
        return false;
    }
    EngineProjectionCache().Enable(context);
    return true;
}

bool InstallFrameProbe(ID3D11Device* anyDevice, ID3D11DeviceContext* anyContext)
{
    if (!anyDevice || !anyContext) {
        return false;
    }
    void** context = *reinterpret_cast<void***>(anyContext);
    void** device = *reinterpret_cast<void***>(anyDevice);
    bool installed = true;
    installed &= Hook(context[12], reinterpret_cast<void*>(&HookDrawIndexed),
                      reinterpret_cast<void**>(&g_drawIndexed), "DrawIndexed");
    installed &= Hook(context[13], reinterpret_cast<void*>(&HookDraw),
                      reinterpret_cast<void**>(&g_draw), "Draw");
    installed &= Hook(context[20], reinterpret_cast<void*>(&HookDrawIndexedInstanced),
                      reinterpret_cast<void**>(&g_drawIndexedInstanced), "DrawIndexedInstanced");
    installed &= Hook(context[21], reinterpret_cast<void*>(&HookDrawInstanced),
                      reinterpret_cast<void**>(&g_drawInstanced), "DrawInstanced");
    installed &= Hook(context[39], reinterpret_cast<void*>(&HookDrawIndexedInstancedIndirect),
                      reinterpret_cast<void**>(&g_drawIndexedInstancedIndirect),
                      "DrawIndexedInstancedIndirect");
    installed &= Hook(context[40], reinterpret_cast<void*>(&HookDrawInstancedIndirect),
                      reinterpret_cast<void**>(&g_drawInstancedIndirect),
                      "DrawInstancedIndirect");
    installed &= Hook(device[12], reinterpret_cast<void*>(&HookCreateVertexShader),
                      reinterpret_cast<void**>(&g_createVertexShader), "CreateVertexShader");
    if (GetConfig().detailedCapture || GetConfig().cullGodRays) {
        installed &= Hook(device[15], reinterpret_cast<void*>(&HookCreatePixelShader),
                          reinterpret_cast<void**>(&g_createPixelShader), "CreatePixelShader");
    }
    if (GetConfig().cacheStereoProjection && GetConfig().engineStereo) {
        InstallProjectionHooks(anyContext);
    }
    Log("PROBE frame probe hooks: %s. Ctrl+F2 in the game records one frame.",
        installed ? "installed" : "INCOMPLETE");
    return installed;
}

int FrameProbeCaptureNumber()
{
    return g_captures;
}

void FrameProbeOnPresent(IDXGISwapChain* swapChain)
{
    if (!GetConfig().frameProbe || !swapChain) {
        return;
    }
    ++g_presents;
    g_lastFrameDraws = g_drawsThisFrame.exchange(0);
    if (g_capturing.load()) {
        {
            std::scoped_lock lock(g_mutex);
            g_capturing.store(false);
        }
        WriteCapture(swapChain);
        return;
    }
    const int every = GetConfig().statsEveryPresents;
    if (every > 0 && g_presents % static_cast<std::uint64_t>(every) == 0) {
        std::size_t known = 0;
        {
            std::scoped_lock lock(g_mutex);
            known = g_vertexShaderCode.size();
        }
        Log("PROBE draws in the last frame=%u, vertex shaders seen at creation=%zu",
            g_lastFrameDraws, known);
    }
    // Read the key every frame so an old press does not fire later.
    const bool pressed = (GetAsyncKeyState(VK_F2) & 1) != 0;
    const bool control = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (g_armed) {
        // One frame after the key: the whole next frame is recorded.
        g_armed = false;
        std::scoped_lock lock(g_mutex);
        g_records.clear();
        g_records.reserve(8192);
        g_capturing.store(true);
        return;
    }
    if (g_captures >= kMaxCaptures) {
        return;
    }
    if (g_firstPresentTick == 0) {
        g_firstPresentTick = GetTickCount64();
    }
    const int automatic = GetConfig().frameProbeAutoSeconds;
    if (automatic > 0 && g_autoCaptures < 2 &&
        GetTickCount64() - g_firstPresentTick >=
            static_cast<ULONGLONG>(automatic) * 1000ULL * (g_autoCaptures + 1)) {
        ++g_autoCaptures;
        g_armed = true;
        Log("PROBE automatic capture %d of 2: recording the next frame", g_autoCaptures);
        return;
    }
    if (!pressed || !control) {
        return;
    }
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swapChain->GetDesc(&description)) ||
        GetAncestor(GetForegroundWindow(), GA_ROOT) != GetAncestor(description.OutputWindow, GA_ROOT)) {
        return;
    }
    g_armed = true;
    Log("PROBE Ctrl+F2: recording the next frame (capture %d)", g_captures + 1);
}

}  // namespace a3vr


namespace a3vr {

namespace {
const char* BrowseModeName(int mode)
{
    return mode == 1 ? "screen-space shaders" : (mode == 2 ? "UI draws (icons, markers)" : "off");
}
}  // namespace

void ShaderBrowseSetMode(int mode)
{
    mode = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
    if (g_browseMode.exchange(mode) != mode) {
        g_browse.store(mode != 0);
        Log("SHADER browser mode: %s (PgUp/PgDn = previous/next, Del = mark it)",
            BrowseModeName(mode));
    }
}

int ShaderBrowseMode()
{
    return g_browseMode.load();
}

void ShaderBrowseEnable(bool enabled)
{
    ShaderBrowseSetMode(enabled ? 1 : 0);
}

bool ShaderBrowseEnabled()
{
    return g_browse.load();
}

void ShaderBrowseStep(int delta)
{
    std::scoped_lock lock(g_browseMutex);
    const int mode = g_browseMode.load();
    std::vector<BrowseShader>& catalog = mode == 2 ? g_uiCatalog : g_catalog;
    int& index = mode == 2 ? g_uiIndex : g_browseIndex;
    if (catalog.empty()) {
        return;
    }
    const int count = static_cast<int>(catalog.size());
    index = ((index + delta) % count + count) % count;
    const BrowseShader& now = catalog[static_cast<size_t>(index)];
    Log("SHADER browser (%s): #%d of %d, %016llx skipped (%d %ux%u)", BrowseModeName(mode),
        index + 1, count, static_cast<unsigned long long>(now.hash), static_cast<int>(now.format),
        now.width, now.height);
}

void ShaderBrowseMark()
{
    std::string line;
    {
        std::scoped_lock lock(g_browseMutex);
        const int mode = g_browseMode.load();
        const std::vector<BrowseShader>& catalog = mode == 2 ? g_uiCatalog : g_catalog;
        const int index = mode == 2 ? g_uiIndex : g_browseIndex;
        if (index < 0 || index >= static_cast<int>(catalog.size())) {
            return;
        }
        const BrowseShader& now = catalog[static_cast<size_t>(index)];
        char text[200];
        std::snprintf(text, sizeof(text), "%s %016llx  %s %d %ux%u  (#%d of %zu)",
                      mode == 2 ? "UI" : "SHADER", static_cast<unsigned long long>(now.hash),
                      mode == 2 ? "texture" : "target", static_cast<int>(now.format), now.width,
                      now.height, index + 1, catalog.size());
        line = text;
        for (const std::string& marked : g_markedLines) {
            if (marked == line) {
                return;
            }
        }
        g_markedLines.push_back(line);
    }
    Log("BROWSER MARKED as wrong: %s", line.c_str());
    FILE* file = nullptr;
    const std::wstring path = (ModuleDirectory() / L"arma3_native_vr.shaders.txt").wstring();
    if (_wfopen_s(&file, path.c_str(), L"a") == 0 && file) {
        std::fprintf(file, "%s\n", line.c_str());
        std::fclose(file);
    }
}

std::string ShaderBrowseDescribe()
{
    std::scoped_lock lock(g_browseMutex);
    const int mode = g_browseMode.load();
    const std::vector<BrowseShader>& catalog = mode == 2 ? g_uiCatalog : g_catalog;
    const int index = mode == 2 ? g_uiIndex : g_browseIndex;
    if (catalog.empty()) {
        return mode == 2 ? "no UI draws seen yet" : "no screen-space shaders seen yet";
    }
    const BrowseShader& now = catalog[static_cast<size_t>(index)];
    char text[200];
    std::snprintf(text, sizeof(text), "%d / %zu  %016llx  %s %d %ux%u  (hidden)", index + 1,
                  catalog.size(), static_cast<unsigned long long>(now.hash),
                  mode == 2 ? "texture" : "target", static_cast<int>(now.format), now.width,
                  now.height);
    return text;
}

std::vector<std::string> ShaderBrowseMarkedLines()
{
    std::scoped_lock lock(g_browseMutex);
    return g_markedLines;
}

}  // namespace a3vr
