#include "ProjectionCache.h"

#include <utility>

namespace a3vr {

bool ProjectionCache::Toggle()
{
    const bool enabled = !m_enabled.load();
    Invalidate();
    m_enabled.store(enabled);
    return enabled;
}

void ProjectionCache::Written(ID3D11Resource* dst)
{
    if (dst && dst == m_watched.load(std::memory_order_relaxed)) {
        Invalidate();
    }
}

ProjectionCache::Stats ProjectionCache::TakeStats()
{
    std::scoped_lock lock(m_mutex);
    return {std::exchange(m_reused, 0), std::exchange(m_built, 0)};
}

ProjectionCache& EngineProjectionCache()
{
    static ProjectionCache cache;
    return cache;
}

}  // namespace a3vr
