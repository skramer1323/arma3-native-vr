#pragma once

#include <vector>

namespace a3vr {

// Measures how the engine answers FreeTrack input, instead of guessing gains.
// It sends a fixed list of yaw, pitch and sideways values, holds each for a few
// frames and reads the resulting camera from the engine's own view matrix
// (RVGetProjectionViewTransform). The player must stand still and not touch
// the mouse for about 5 seconds.
//
// Engine matrix layout (PROVEN in headset test 3 logs, build 2.22.0.154089):
// floats 16..31 map world to camera with row vectors; column j of the 3x3 part
// is camera axis j in the world (0 = right, 1 = up, 2 = forward), floats
// 28..30 are the translation.
class TrackerCalibration final {
public:
    void Start();
    bool Active() const { return m_active; }
    bool Valid() const { return m_valid; }

    // Once per published frame while active: what to send (radians, radians, mm).
    void NextCommand(float& yaw, float& pitch, float& shiftMillimeters, float& liftMillimeters,
                     float& depthMillimeters);
    // Once per presented frame while active.
    void Sample(const float* matrices);

    // After a valid run: value to send so that the engine camera does `desired`;
    // `achieved` is what the engine will really reach (it has limits).
    float YawToSend(float desiredRightRadians, float& achievedRadians) const;
    float PitchToSend(float desiredUpRadians, float& achievedRadians) const;
    float ShiftToSend(float desiredRightMeters, float& achievedMeters) const;
    // Head position (lean): metres the camera moves per millimetre sent on X (right), Y (up),
    // Z (forward), signed. Valid when the engine answered all three.
    bool PositionValid() const { return m_positionValid; }
    float PositionGain(int axis) const { return m_gain[axis]; }

private:
    enum class Axis { None, Yaw, Pitch, Shift, Lift, Depth, ShiftYaw };
    struct Step {
        Axis axis = Axis::None;
        float sent = 0.0F;  // degrees or millimetres
        double sum = 0.0;   // measured degrees or metres on the same axis
        double sum2 = 0.0;  // ShiftYaw: forward metres (sum = right metres)
        int count = 0;
    };
    // One side of an axis: |sent| -> |measured|, rising.
    struct Branch {
        std::vector<float> sent;
        std::vector<float> measured;
        float sentSign = 1.0F;
        float measuredSign = 1.0F;
    };

    void Finish();
    Branch MakeBranch(Axis axis, bool positive) const;
    static float Invert(const Branch& positive, const Branch& negative, float desired,
                        float& achieved);

    bool m_active = false;
    bool m_valid = false;
    bool m_positionValid = false;
    float m_gain[3]{};
    int m_frame = 0;
    std::vector<Step> m_steps;
    bool m_haveBase = false;
    double m_baseRotation[3][3]{};
    double m_basePosition[3]{};
    std::vector<float> m_yawTrace;    // per-frame answer to the first yaw step
    std::vector<float> m_shiftTrace;  // per-frame answer to the first shift step
    double m_driftYaw = 0.0;
    double m_driftShift = 0.0;
    Branch m_yaw[2];
    Branch m_pitch[2];
    Branch m_shift[2];
};

}  // namespace a3vr
