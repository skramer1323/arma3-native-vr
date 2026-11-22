#pragma once

#include <d3d11.h>

#include <cstdint>

namespace a3vr {

enum class PerfRepeatKind { World, FirstPerson, Overlay, Desktop };

bool PerfGpuPassStart(ID3D11Device* device, ID3D11DeviceContext* context, std::uint64_t present);
void PerfGpuPassFinish();
bool PerfGpuPassCollect(ID3D11DeviceContext* context, UINT64 start, UINT64 end, UINT64 post,
                        const D3D11_QUERY_DATA_TIMESTAMP_DISJOINT& clock);

bool PerfGpuPassWantsDraw(ID3D11DeviceContext* context);
void PerfGpuPassDraw(ID3D11DeviceContext* context, UINT width, UINT height, UINT format,
                     bool color, bool depth, float maxDepth, bool quad, std::uint64_t shader);
bool PerfGpuRepeatBegin(ID3D11DeviceContext* context, PerfRepeatKind kind);
void PerfGpuRepeatEnd(ID3D11DeviceContext* context);

}  // namespace a3vr
