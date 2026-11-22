#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace a3vr {

// A private copy for each eye and projection layer of the currently used engine buffer.
class ProjectionCache final {
public:
    void Enable(ID3D11DeviceContext* context) { m_context.store(context); }
    bool Toggle();
    void Invalidate() { m_revision.fetch_add(1, std::memory_order_relaxed); }
    void Written(ID3D11Resource* dst);

    struct Stats { unsigned reused; unsigned built; };
    Stats TakeStats();

    template <typename Build>
    bool Bind(ID3D11DeviceContext* context, ID3D11Device* device, ID3D11Buffer* source,
              UINT slot, UINT variant, Build build)
    {
        if (!m_enabled.load(std::memory_order_relaxed) || context != m_context.load(std::memory_order_relaxed)) {
            return false;
        }
        std::scoped_lock lock(m_mutex);
        if (source != m_source.Get()) {
            D3D11_BUFFER_DESC desc{};
            source->GetDesc(&desc);
            // Shared resources can be modified outside our immediate context.
            if (desc.MiscFlags != 0 || desc.BindFlags != D3D11_BIND_CONSTANT_BUFFER) {
                return false;
            }
            m_source = source;
            m_watched.store(source);
            Invalidate();
        }

        auto& entry = m_entries[variant];
        if (!entry.buffer) {
            D3D11_BUFFER_DESC desc{};
            source->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.CPUAccessFlags = 0;
            if (FAILED(device->CreateBuffer(&desc, nullptr, entry.buffer.GetAddressOf()))) {
                return false;
            }
        }
        const auto revision = m_revision.load(std::memory_order_relaxed);
        if (entry.revision != revision) {
            build(entry.buffer.Get());
            entry.revision = revision;
            ++m_built;
        } else {
            ++m_reused;
        }
        ID3D11Buffer* target = entry.buffer.Get();
        context->VSSetConstantBuffers(slot, 1, &target);
        return true;
    }

private:
    struct Entry {
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        std::uint64_t revision = 0;
    };
    std::atomic<ID3D11DeviceContext*> m_context{nullptr};
    std::atomic<bool> m_enabled{true};
    std::atomic<ID3D11Resource*> m_watched{nullptr};
    std::atomic<std::uint64_t> m_revision{1};
    std::mutex m_mutex;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_source;
    std::array<Entry, 8> m_entries;
    unsigned m_reused = 0;
    unsigned m_built = 0;
};

ProjectionCache& EngineProjectionCache();

}  // namespace a3vr
