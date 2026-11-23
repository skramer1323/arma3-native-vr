#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace a3vr {

enum class AerWarpMode { Off, HeadsetRotation, CameraRotation, Camera6DoF };

// Every behaviour has an INI key in arma3_native_vr.ini, section [ArmaVR].
struct Config {
    // 0 = proxy only: log the D3D11 device and swapchain, no OpenXR (rung 1).
    bool enableVR = true;
    // 0 = back buffer on a quad (rung 3), 1 = empty frames (rung 2),
    // 2 = session only, no frame loop.
    int openXrDiagnosticMode = 0;
    // 0 = head-locked quad (VIEW space), 1 = world-fixed quad (LOCAL space).
    int quadAnchor = 0;
    float quadWidthMeters = 2.4F;
    float quadDistanceMeters = 2.0F;
    float quadHeightOffsetMeters = 0.0F;
    // World view with engine stereo: HUD placement inside each eye image (viewport math),
    // central part of the wide game picture kept on the quad, pose lag.
    float hudScale = 0.6F;
    float hudShiftX = 0.0F;
    float hudShiftY = 0.0F;
    float hudDistanceMeters = 2.0F;
    float worldFovCrop = 0.75F;
    // The first-person weapon is drawn with viewport depth 0..0.01: it gets its own, smaller
    // depth separation (it sits within arm reach) and its own zero-parallax distance.
    float weaponScale = 0.3F;
    float weaponConvergenceMeters = 0.7F;
    float weaponSize = 0.6F;  // projection layer: size of the weapon layer on the picture
    // Show the frame with the head pose the engine actually received (matched by dataId).
    bool poseBySequence = true;
    // World view with engine stereo: draw each eye with the headset's own frustum and submit a
    // projection layer (like Tarkov KBM VR) instead of a window (quad) showing the game picture.
    bool projectionLayer = true;
    bool steamVrWorldQuad = true;
    bool steamVrMirrorHud = false;
    bool alternateEyeProjection = false;
    bool dibrShift = false;
    bool dibrForwardHistory = false;
    AerWarpMode aerWarpMode = AerWarpMode::Camera6DoF;
    int aerDibrPreference = 0;  // 0 off, 1 DIBR, 2 DIBR with forward history
    // AER with the engine's own projection and the whole target per eye (SnowRunner-style): every
    // screen-space effect (shadows, AO, glow) is computed for one normal camera. Needs AER on.
    // Port of the SnowRunner VR mod: the game window is made exactly this client size (the headset's
    // per-eye resolution) so the game renders one eye per frame at 1:1 pixels. 0 = off. Windowed
    // mode needed. Automatic sizing uses the runtime's recommended eye extent.
    bool autoHeadsetResolution = true;
    int forceGameWidth = 0;
    int forceGameHeight = 0;
    bool allowOversizeWindow = false;  // true: do not fit the window into the desktop
    // The window may be larger than the desktop: raw mouse movement drives a virtual cursor over all
    // of it (menus, map, inventory). Speed = client pixels per mouse count.
    bool virtualCursor = true;
    float virtualCursorSpeed = 1.0F;
    bool aerNative = false;
    // Menus, map and inventory (cursor shown) are drawn as one normal frame shown on the flat screen,
    // not as two doubled halves: the screen-space effects of the 3D menu background stay correct.
    bool monoScreen = true;
    // Use the engine's full 3D scene as the OpenXR source and keep the game window independent.
    bool virtualBackBuffer = false;
    int dibrFillMode = 1;  // DIBR holes: 0 magenta, 1 stale eye then background, 2 background only
    // Aiming (as in the Tarkov KBM VR mod): while aiming the view follows the game camera (the
    // sight line); only the head motion since aiming started turns it further. AimEye: the eye
    // that is put on the sight line (1 right, -1 left, 0 between the eyes).
    bool adsLockView = true;
    bool adsRightMouseToggle = false;
    int aimEye = 1;
    // Game zoom (scopes): effective magnification = zoom^ZoomMagnification (1 = the full game
    // zoom, 0 = none); the rest of the zoom becomes a smaller picture window.
    float zoomMagnification = 1.0F;
    // Head position (lean) through FreeTrack X/Y/Z. Gain = metres the engine camera moves per
    // millimetre sent (signed, measured by the calibration, F8); 0 = take it from the live
    // calibration of this session. Frame: 0 = body (recentre direction), 1 = looking direction.
    // Anchor the extra weapon distance to the body's aim direction. Arma supplies the pose.
    // M22/M32: depth constants of the weapon layer's projection
    // (frame probe 2026-11-17: 1.00002 / -0.01).
    bool weaponBodyLock = true;
    float headOffset[3]{0.0F, 0.0F, 0.0F};  // saved head placement (Ctrl+arrows, Ctrl+Delete)
    // Head placement inside a vehicle (the roof is lower): same keys, saved with Ctrl+Delete.
    float headOffsetVehicle[3]{0.0F, -0.06F, 0.0F};
    bool vehicleAuto = false;  // experimental: switch by "no first-person weapon on screen" (a vehicle
                               // cockpit is also drawn in that layer, so it is off; Ctrl+Insert switches)
    int headPlacementMode = 0;  // 0 configured default, 1 on foot, 2 vehicle
    bool weaponHeadPosition = false;  // Same-frame stereo head lean for inspecting on foot.
    // The 2D HUD is drawn once into its own texture and shown as an OpenXR quad fixed in the room
    // in front of the recentre direction (Tarkov KBM VR HudLayer): no doubling, no bending.
    // The sun/god-ray post effect is computed once over the whole side-by-side picture, so its glow
    // sits on the seam (left eye sees it on the right, right eye on the left). Its draws into the
    // scene colour target are skipped (frame probe 2026-11-19).
    bool cullGodRays = true;
    // Diagnostic: pixel-shader hashes (hex, comma separated, "CullHashes=") whose small screen-space
    // draws are skipped, to find which post effect breaks the side-by-side picture.
    std::vector<std::uint64_t> cullHashes;
    // Scope = lens: with a magnifying optic (game zoom > 1.03) only a round-ish window of
    // scopeLensDegrees apparent field is shown, magnified by the real zoom, the reticle drawn in
    // the picture; the surroundings are black (a scope tube) and the 2D HUD panel is hidden.
    bool scopeLens = false;  // off: the GPT full-field zoom is the default (user preference)
    float scopeLensDegrees = 30.0F;
    bool scopeReticle = true;  // with any magnifying optic the reticle is drawn at the aim axis in the picture
    // Through a magnifying optic the head does not move the view at all (rotation and position are
    // ignored, the picture is glued to the face): only the mouse aims, no magnified head shake.
    bool scopeStatic = true;
    // Through a magnifying optic the first-person weapon has no eye separation (same picture in both
    // eyes, centred), so the sight can be used with both eyes open.
    bool scopeWeaponFlat = true;
    bool hudQuad = true;
    bool hideGameplayHud = false;
    bool hudBodyLock = true;  // the 2D HUD stays where the body aims too
    bool stabilizeHudMarker = false;
    bool dlssBalancedExperimental = false;
    bool cacheStereoProjection = false;
    bool staleEyeWarp = false;
    bool stereoShadows = true;
    bool worldVrs = false;
    bool singlePassDepth = false;
    bool singlePassWorldPrepass = false;
    float weaponProjM22 = 1.00002F;
    float weaponProjM32 = -0.01F;
    float firstPersonClipCm = 0.0F;
    bool firstPersonHideTorso = false;
    bool headPosition = false;
    float headPositionGain[3]{0.0F, 0.0F, 0.0F};
    int headPositionFrame = 0;
    float headPositionScale = 1.0F;
    float headPositionLimitMeters = 0.25F;
    float worldYawOrbitForward = 0.0F;
    float worldYawOrbitRight = 0.0F;
    // Use the measured yaw/pitch response of the calibration (off: the plain gain is used).
    bool calibratedRotation = false;
    bool useSrgbSwapchain = true;
    // Ctrl+Home/End width, Ctrl+PgUp/PgDn distance, Ctrl+Del save,
    // Ctrl+F11 recentre. Only while the game window has focus.
    bool tuningHotkeys = true;
    // Present-rate statistics in the log every N presents (0 = off).
    int statsEveryPresents = 600;
    // Timing breakdown of a frame (draw hooks, Present, DLSS) in the log every 120 presents.
    bool perfLog = false;
    // 1 = show the Windows cursor as a small quad on the game screen.
    bool drawCursor = true;
    // 1 = head look: the headset pose goes to Arma's own FreeTrack input and,
    // while the mouse cursor is hidden, the image is shown as the world around
    // the player instead of a screen. Ctrl+F10 switches world view/screen.
    bool headTracking = true;
    float headYawLimitDegrees = 70.0F;
    float headPitchLimitDegrees = 85.0F;
    // Engine head angle per real head angle; signs flip an inverted axis.
    // Test 5 calibration (2.22.0.154089): the engine turns 2*pi times the angle sent.
    float headRotationGain = 0.159155F;
    int headYawSign = 1;
    int headPitchSign = 1;
    // Which published pose the presented frame was rendered with (1 = previous).
    int poseLagFrames = 1;
    bool stabilizeArmaRoll = false;
    float worldQuadDistanceMeters = 5.0F;
    // Used only when the game's projection cannot be read.
    float worldFovTanVertical = 0.75F;
    // 0 = mono world view. 1 = alternate-eye stereo: every game frame is rendered
    // for one eye, the camera is moved sideways through FreeTrack X.
    int stereoMode = 0;
    // Sideways camera move sent per eye (FreeTrack millimetres; the engine
    // rescales it, so this is tuned by eye: Ctrl+Left/Right, Ctrl+Del saves).
    float stereoShiftMillimeters = 30.0F;
    bool stereoSwapEyes = false;
    float headsetIpdMeters = 0.064F;
    // Depth strength after calibration: 1.0 = eye distance equals HeadsetIpdMeters.
    float stereoScale = 1.0F;
    // 1 = measure the engine's answer to head input once, at the first world view.
    bool autoCalibrate = false;
    // 1 = depth-based stereo on the screen: left/right images are made from the
    // game frame and its depth buffer. Ctrl+F9 3D on/off, Ctrl+Left/Right depth,
    // Ctrl+Up/Down distance of the screen plane, Ctrl+Del saves.
    bool depthStereo = true;
    // Game distance that appears exactly on the screen surface; nearer things
    // come out of the screen, farther ones go behind it.
    float convergenceMeters = 1.0F;
    // -1 = detect from the depth clear value, 0 = normal Z, 1 = reversed Z.
    int depthReversed = -1;
    // 1 = the game image is already side-by-side stereo (left half = left eye),
    // made by an external tool such as geo-11. Each half goes to its eye. Ctrl+F3 toggles.
    bool sbsInput = false;
    // Step 0 of the mod's own geometry stereo: read-only frame recorder (Ctrl+F2).
    bool frameProbe = false;
    // > 0: record a frame without a key, N and 2N seconds after the first Present
    // (lets the agent run the probe unattended).
    int frameProbeAutoSeconds = 0;
    // Own geometry stereo: every scene draw is repeated for the second eye (EngineStereo.h).
    bool engineStereo = false;
    float engineStereoSeparationMeters = 0.064F;
    float engineStereoConvergenceMeters = 5.0F;
    // > 0: save the back buffer as a JPEG at N, 2N and 3N seconds (self-check).
    int frameDumpSeconds = 0;
    bool detailedCapture = false;
};

const Config& GetConfig();
// The settings menu changes some options live (and saves them with SaveIniValue).
Config& MutableConfig();
void SaveIniValue(const wchar_t* key, double value);
void LoadConfig();
std::filesystem::path ConfigPath();
// Saves the live quad values (Ctrl+Del).
void SaveQuadTuning(float widthMeters, float distanceMeters, float heightOffsetMeters);
void SaveHeadOffset(float x, float y, float z);
void SaveHeadOffsetVehicle(float x, float y, float z);
void SaveHudTuning(float scale, float shiftX, float shiftY, float distanceMeters, float fovCrop,
                   int poseLagFrames, float weaponScale, float weaponConvergenceMeters,
                   float weaponSize);
void SaveStereoTuning(float scale, bool swapEyes, float convergenceMeters);

}  // namespace a3vr
