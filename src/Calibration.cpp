#include "Calibration.h"

#include "Log.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace a3vr {
namespace {

constexpr int kHoldFrames = 12;
constexpr int kMeasureFromPhase = 6;  // skip the frames where the engine may still settle
constexpr double kDegrees = 57.295779513;

void ReadCamera(const float* m, double rotation[3][3], double position[3])
{
    const double r[3][3] = {{m[16], m[17], m[18]}, {m[20], m[21], m[22]}, {m[24], m[25], m[26]}};
    const double t[3] = {m[28], m[29], m[30]};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            rotation[i][j] = r[i][j];
        }
        position[i] = -(t[0] * r[i][0] + t[1] * r[i][1] + t[2] * r[i][2]);
    }
}

}  // namespace

void TrackerCalibration::Start()
{
    m_steps.clear();
    const auto add = [this](Axis axis, std::initializer_list<float> values) {
        for (const float value : values) {
            Step step;
            step.axis = axis;
            step.sent = value;
            m_steps.push_back(step);
        }
    };
    add(Axis::None, {0.0F});
    add(Axis::Yaw, {10.0F, 20.0F, 35.0F, 50.0F, 70.0F, -10.0F, -20.0F, -35.0F, -50.0F, -70.0F});
    add(Axis::Pitch, {10.0F, 25.0F, 45.0F, -10.0F, -25.0F, -45.0F});
    add(Axis::Shift,
        {20.0F, 50.0F, 100.0F, 200.0F, 400.0F, -20.0F, -50.0F, -100.0F, -200.0F, -400.0F});
    add(Axis::Lift, {50.0F, 100.0F, 200.0F, -50.0F, -100.0F, -200.0F});
    add(Axis::Depth, {50.0F, 100.0F, 200.0F, -50.0F, -100.0F, -200.0F});
    add(Axis::ShiftYaw, {100.0F});
    add(Axis::None, {0.0F});
    m_frame = 0;
    m_haveBase = false;
    m_valid = false;
    m_active = true;
    m_yawTrace.clear();
    m_shiftTrace.clear();
    Log("CALIB started: %zu steps x %d frames. Stand still, do not touch the mouse.",
        m_steps.size(), kHoldFrames);
}

void TrackerCalibration::NextCommand(float& yaw, float& pitch, float& shiftMillimeters,
                                     float& liftMillimeters, float& depthMillimeters)
{
    yaw = 0.0F;
    pitch = 0.0F;
    shiftMillimeters = 0.0F;
    liftMillimeters = 0.0F;
    depthMillimeters = 0.0F;
    if (!m_active) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(m_frame / kHoldFrames);
    if (index >= m_steps.size()) {
        Finish();
        return;
    }
    const Step& step = m_steps[index];
    if (step.axis == Axis::Yaw) {
        yaw = step.sent / static_cast<float>(kDegrees);
    } else if (step.axis == Axis::Pitch) {
        pitch = step.sent / static_cast<float>(kDegrees);
    } else if (step.axis == Axis::Shift) {
        shiftMillimeters = step.sent;
    } else if (step.axis == Axis::Lift) {
        liftMillimeters = step.sent;
    } else if (step.axis == Axis::Depth) {
        depthMillimeters = step.sent;
    } else if (step.axis == Axis::ShiftYaw) {
        shiftMillimeters = step.sent;
        yaw = 40.0F / static_cast<float>(kDegrees);
    }
    ++m_frame;
}

void TrackerCalibration::Sample(const float* m)
{
    if (!m_active || m_frame < 1) {
        return;
    }
    // The frame on screen was rendered with the command of the previous present.
    const int command = m_frame - 1;
    const std::size_t index = static_cast<std::size_t>(command / kHoldFrames);
    const int phase = command % kHoldFrames;
    if (index >= m_steps.size()) {
        return;
    }
    double rotation[3][3]{};
    double position[3]{};
    ReadCamera(m, rotation, position);
    if (index == 0) {
        if (phase == kHoldFrames - 1) {
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    m_baseRotation[i][j] = rotation[i][j];
                }
                m_basePosition[i] = position[i];
            }
            m_haveBase = true;
        }
        return;
    }
    if (!m_haveBase) {
        return;
    }
    // Current forward axis in the base camera frame (x right, y up, z forward).
    double forward[3]{};
    double right = 0.0;
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            forward[j] += m_baseRotation[i][j] * rotation[i][2];
        }
    }
    double up = 0.0;
    double ahead = 0.0;
    for (int i = 0; i < 3; ++i) {
        right += (position[i] - m_basePosition[i]) * m_baseRotation[i][0];
        up += (position[i] - m_basePosition[i]) * m_baseRotation[i][1];
        ahead += (position[i] - m_basePosition[i]) * m_baseRotation[i][2];
    }
    const double yawRight = kDegrees * std::atan2(forward[0], forward[2]);
    const double pitchUp = kDegrees * std::asin(std::clamp(forward[1], -1.0, 1.0));
    Step& step = m_steps[index];
    if (index == 1) {
        m_yawTrace.push_back(static_cast<float>(yawRight));
    }
    if (step.axis == Axis::Shift && step.sent == 20.0F) {
        m_shiftTrace.push_back(static_cast<float>(right));
    }
    if (phase < kMeasureFromPhase) {
        return;
    }
    if (step.axis == Axis::Yaw) {
        step.sum += yawRight;
    } else if (step.axis == Axis::Pitch) {
        step.sum += pitchUp;
    } else if (step.axis == Axis::Shift) {
        step.sum += right;
    } else if (step.axis == Axis::Lift) {
        step.sum += up;
    } else if (step.axis == Axis::Depth) {
        step.sum += ahead;
    } else if (step.axis == Axis::ShiftYaw) {
        step.sum += right;
        step.sum2 += ahead;
    } else {
        m_driftYaw = yawRight;
        m_driftShift = right;
    }
    ++step.count;
}

TrackerCalibration::Branch TrackerCalibration::MakeBranch(Axis axis, bool positive) const
{
    Branch branch;
    branch.sentSign = positive ? 1.0F : -1.0F;
    branch.sent.push_back(0.0F);
    branch.measured.push_back(0.0F);
    double signSum = 0.0;
    float highest = 0.0F;
    for (const Step& step : m_steps) {
        if (step.axis != axis || (step.sent > 0.0F) != positive || step.count == 0) {
            continue;
        }
        const double measured = step.sum / step.count;
        signSum += measured;
        // Keep the curve rising: a flat tail means the engine hit its limit.
        highest = std::max(highest, static_cast<float>(std::fabs(measured)));
        branch.sent.push_back(std::fabs(step.sent));
        branch.measured.push_back(highest);
    }
    branch.measuredSign = signSum < 0.0 ? -1.0F : 1.0F;
    return branch;
}

void TrackerCalibration::Finish()
{
    m_active = false;
    std::string trace;
    for (const float value : m_yawTrace) {
        trace += std::to_string(value).substr(0, 6) + " ";
    }
    Log("CALIB first yaw step (+10 deg sent), camera yaw per frame: %s", trace.c_str());
    trace.clear();
    for (const float value : m_shiftTrace) {
        trace += std::to_string(value * 1000.0F).substr(0, 6) + " ";
    }
    Log("CALIB first shift step (+20 mm sent), camera move per frame in mm: %s", trace.c_str());
    for (const Step& step : m_steps) {
        if (step.axis == Axis::None) {
            continue;
        }
        const double measured = step.count ? step.sum / step.count : 0.0;
        if (step.axis == Axis::Shift) {
            Log("CALIB shift sent=%+.0f mm -> camera moved right %+.4f m", step.sent, measured);
        } else if (step.axis == Axis::Lift) {
            Log("CALIB lift sent=%+.0f mm -> camera moved up %+.4f m", step.sent, measured);
        } else if (step.axis == Axis::Depth) {
            Log("CALIB depth sent=%+.0f mm -> camera moved forward %+.4f m", step.sent, measured);
        } else if (step.axis == Axis::ShiftYaw) {
            const double ahead = step.count ? step.sum2 / step.count : 0.0;
            Log("CALIB shift+yaw40 sent=100 mm -> in the base frame right %+.4f m, forward "
                "%+.4f m",
                measured, ahead);
        } else {
            Log("CALIB %s sent=%+.0f deg -> camera %s %+.2f deg",
                step.axis == Axis::Yaw ? "yaw" : "pitch", step.sent,
                step.axis == Axis::Yaw ? "turned right" : "looked up", measured);
        }
    }
    m_yaw[0] = MakeBranch(Axis::Yaw, true);
    m_yaw[1] = MakeBranch(Axis::Yaw, false);
    m_pitch[0] = MakeBranch(Axis::Pitch, true);
    m_pitch[1] = MakeBranch(Axis::Pitch, false);
    m_shift[0] = MakeBranch(Axis::Shift, true);
    m_shift[1] = MakeBranch(Axis::Shift, false);
    const bool still = std::fabs(m_driftYaw) < 1.5 && std::fabs(m_driftShift) < 0.01;
    const bool answered = m_yaw[0].measured.back() > 2.0F && m_yaw[1].measured.back() > 2.0F &&
                          m_yaw[0].measuredSign != m_yaw[1].measuredSign;
    m_valid = still && answered;
    {
        // Gain from the small steps (|sent| <= 100 mm; the engine slews, so big ones are cut).
        const Axis axes[3] = {Axis::Shift, Axis::Lift, Axis::Depth};
        bool all = true;
        for (int a = 0; a < 3; ++a) {
            double sum = 0.0;
            int n = 0;
            for (const Step& step : m_steps) {
                if (step.axis == axes[a] && step.count > 0 && std::fabs(step.sent) <= 100.0F &&
                    step.sent != 20.0F && step.sent != -20.0F) {
                    sum += (step.sum / step.count) / step.sent;
                    ++n;
                }
            }
            m_gain[a] = n ? static_cast<float>(sum / n) : 0.0F;
            all = all && std::fabs(m_gain[a]) > 1e-6F;
            Log("CALIB position gain %c: %+.6f m per mm sent (%d steps)", "XYZ"[a], m_gain[a], n);
        }
        m_positionValid = still && all;
    }
    Log("CALIB %s: drift yaw=%.2f deg move=%.4f m; engine limits: yaw %.1f/%.1f deg, pitch "
        "%.1f/%.1f deg, sideways %.4f/%.4f m",
        m_valid ? "VALID, applied"
                : (still ? "INVALID (the engine did not answer the yaw input)"
                         : "INVALID (the view moved during the run), defaults are used"),
        m_driftYaw, m_driftShift, m_yaw[0].measured.back(), m_yaw[1].measured.back(),
        m_pitch[0].measured.back(), m_pitch[1].measured.back(), m_shift[0].measured.back(),
        m_shift[1].measured.back());
}

float TrackerCalibration::Invert(const Branch& positive, const Branch& negative, float desired,
                                 float& achieved)
{
    achieved = 0.0F;
    if (desired == 0.0F) {
        return 0.0F;
    }
    const float wantSign = desired < 0.0F ? -1.0F : 1.0F;
    const Branch* branch = positive.measuredSign == wantSign ? &positive : nullptr;
    if (!branch && negative.measuredSign == wantSign) {
        branch = &negative;
    }
    if (!branch || branch->sent.size() < 2) {
        return 0.0F;
    }
    const float target = std::min(std::fabs(desired), branch->measured.back());
    achieved = wantSign * target;
    for (std::size_t i = 1; i < branch->sent.size(); ++i) {
        const float low = branch->measured[i - 1];
        const float high = branch->measured[i];
        if (target <= high) {
            const float part = high > low ? (target - low) / (high - low) : 0.0F;
            return branch->sentSign *
                   (branch->sent[i - 1] + part * (branch->sent[i] - branch->sent[i - 1]));
        }
    }
    return branch->sentSign * branch->sent.back();
}

float TrackerCalibration::YawToSend(float desiredRightRadians, float& achievedRadians) const
{
    float achieved = 0.0F;
    const float sent = Invert(m_yaw[0], m_yaw[1],
                              desiredRightRadians * static_cast<float>(kDegrees), achieved);
    achievedRadians = achieved / static_cast<float>(kDegrees);
    return sent / static_cast<float>(kDegrees);
}

float TrackerCalibration::PitchToSend(float desiredUpRadians, float& achievedRadians) const
{
    float achieved = 0.0F;
    const float sent = Invert(m_pitch[0], m_pitch[1],
                              desiredUpRadians * static_cast<float>(kDegrees), achieved);
    achievedRadians = achieved / static_cast<float>(kDegrees);
    return sent / static_cast<float>(kDegrees);
}

float TrackerCalibration::ShiftToSend(float desiredRightMeters, float& achievedMeters) const
{
    return Invert(m_shift[0], m_shift[1], desiredRightMeters, achievedMeters);
}

}  // namespace a3vr
