#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace a3vr {

// Head look through Arma's own FreeTrack input (arma3_x64.exe 2.22.0.154089,
// FUN_141552740 / FUN_141552900, static evidence):
//  - the engine reads HKCU\Software\Freetrack\FreetrackClient "Path", loads
//    <Path>\FreeTrackClient64.dll and polls FTGetData;
//  - a sample is used only when its dataId is not 0;
//  - yaw/pitch/roll are radians, x/y/z millimetres.
// Our FreeTrackClient64.dll copies this block; dinput8.dll writes it.
#pragma pack(push, 1)
struct FreeTrackData {
    std::uint32_t dataId;
    std::int32_t cameraWidth;
    std::int32_t cameraHeight;
    float yaw, pitch, roll;
    float x, y, z;
    float rawYaw, rawPitch, rawRoll;
    float rawX, rawY, rawZ;
    float points[8];
};
#pragma pack(pop)
static_assert(sizeof(FreeTrackData) == 92);

constexpr const char* kFreeTrackMapping = "FT_SharedMem";
constexpr const char* kFreeTrackMutex = "FT_Mutext";
constexpr std::size_t kFreeTrackMappingSize = 256;
// Extension past the standard block: FTGetData calls seen by the client DLL.
constexpr std::size_t kFreeTrackCallCounterOffset = 192;
// Extension: dataId of the last sample the client DLL returned to the engine.
constexpr std::size_t kFreeTrackLastReturnedOffset = 196;

class FreeTrackFeed final {
public:
    bool Open();
    void Publish(float yawRadians, float pitchRadians, float xMillimeters, float yMillimeters = 0.0F,
                 float zMillimeters = 0.0F);
    // How many times the game asked our client DLL for data (0 = the engine
    // does not use FreeTrack: not enabled in the controls, or not loaded).
    std::uint32_t ClientCalls() const;
    // dataId of the newest published sample, and of the sample the engine received last.
    std::uint32_t LastPublishedId() const { return m_dataId; }
    std::uint32_t LastReturnedId() const;

private:
    HANDLE m_mapping = nullptr;
    HANDLE m_mutex = nullptr;
    std::uint8_t* m_view = nullptr;
    std::uint32_t m_dataId = 0;
};

}  // namespace a3vr
