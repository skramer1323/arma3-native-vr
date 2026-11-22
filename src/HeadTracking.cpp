#include "HeadTracking.h"

#include "Log.h"

#include <cstring>

namespace a3vr {

bool FreeTrackFeed::Open()
{
    if (m_view) {
        return true;
    }
    m_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                   static_cast<DWORD>(kFreeTrackMappingSize), kFreeTrackMapping);
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    if (!m_mapping) {
        Log("HEAD could not create %s: error %lu", kFreeTrackMapping, GetLastError());
        return false;
    }
    m_view = static_cast<std::uint8_t*>(
        MapViewOfFile(m_mapping, FILE_MAP_WRITE, 0, 0, kFreeTrackMappingSize));
    if (!m_view) {
        // Another tracker created a smaller block; the standard part still fits.
        m_view = static_cast<std::uint8_t*>(
            MapViewOfFile(m_mapping, FILE_MAP_WRITE, 0, 0, sizeof(FreeTrackData)));
    }
    if (!m_view) {
        Log("HEAD could not map %s: error %lu", kFreeTrackMapping, GetLastError());
        CloseHandle(m_mapping);
        m_mapping = nullptr;
        return false;
    }
    m_mutex = CreateMutexA(nullptr, FALSE, kFreeTrackMutex);
    Log("HEAD FreeTrack block ready (%s)%s", kFreeTrackMapping,
        existed ? " - it already existed: another tracker (opentrack?) may fight over it" : "");
    return true;
}

void FreeTrackFeed::Publish(float yawRadians, float pitchRadians, float xMillimeters,
                            float yMillimeters, float zMillimeters)
{
    if (!m_view) {
        return;
    }
    FreeTrackData data{};
    if (++m_dataId == 0) {
        m_dataId = 1;  // 0 means "no sample" to the engine
    }
    data.dataId = m_dataId;
    data.cameraWidth = 640;
    data.cameraHeight = 480;
    data.yaw = data.rawYaw = yawRadians;
    data.pitch = data.rawPitch = pitchRadians;
    data.x = data.rawX = xMillimeters;
    data.y = data.rawY = yMillimeters;
    data.z = data.rawZ = zMillimeters;
    const bool locked = m_mutex && WaitForSingleObject(m_mutex, 1) == WAIT_OBJECT_0;
    std::memcpy(m_view, &data, sizeof(data));
    if (locked) {
        ReleaseMutex(m_mutex);
    }
}

std::uint32_t FreeTrackFeed::LastReturnedId() const
{
    if (!m_view) {
        return 0;
    }
    std::uint32_t id = 0;
    std::memcpy(&id, m_view + kFreeTrackLastReturnedOffset, sizeof(id));
    return id;
}

std::uint32_t FreeTrackFeed::ClientCalls() const
{
    if (!m_view) {
        return 0;
    }
    std::uint32_t calls = 0;
    std::memcpy(&calls, m_view + kFreeTrackCallCounterOffset, sizeof(calls));
    return calls;
}

}  // namespace a3vr
