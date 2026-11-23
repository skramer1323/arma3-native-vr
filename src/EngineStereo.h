#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>

namespace a3vr {

// Own geometry stereo (EngineStereo=1). Every scene draw the engine issues is
// repeated once more: the render target is split in two halves (left eye, right
// eye), and the projection matrix of the second draw is shifted sideways.
//
// Evidence (frame probe, build 2.22.0.154089, capture of 2026-11-17):
//  - PROVEN: every scene vertex shader ends with
//      o0 = view.x*cb2[0] + view.y*cb2[1] + view.z*cb2[2] + cb2[3]
//    and cb2 (112 bytes) holds the projection matrix, equal to the export
//    RVGetProjectionViewTransform. Floats 8 and 12 (cb2[2].x, cb2[3].x) are 0.
//  - Setting float8 = s*P00*d/C and float12 = -s*P00*d moves the camera by
//    s*d sideways (s = -1 left, +1 right) and keeps distance C on the screen plane.
//  - The scene is drawn into 2048x864 targets with an R24G8 depth buffer, the
//    final image is composed into the 4096x1728 back buffer.
//
// The result is a side-by-side picture in the back buffer. The OpenXR side shows
// the left half to the left eye and the right half to the right eye (SbsInput).
//
// Screen-space passes that rebuild positions need the projection of the eye
// that wrote the depth buffer. The known cascade resolve is patched separately.
class StereoDraw final {
public:
    StereoDraw(ID3D11DeviceContext* context, UINT elementCount, bool instancedOrIndirect);
    ~StereoDraw();
    StereoDraw(const StereoDraw&) = delete;
    StereoDraw& operator=(const StereoDraw&) = delete;

    // After the first (left eye) draw: sets up the right eye. Returns true once
    // when the draw has to be issued again.
    bool NextEye();

private:
    void Begin(UINT elementCount, bool instanced);
    void SetEye(int eye);
    UINT ProjectionVariant(int eye, bool projected) const;
    ID3D11Buffer* PrepareEyeProjection(UINT variant, bool projected);
    bool BeginBodyMask(ID3D11VertexShader* shader);
    void BindStereoShadows(ID3D11RenderTargetView* target);

    ID3D11DeviceContext* m_context = nullptr;
    bool m_active = false;
    bool m_profileRepeat = false;
    bool m_secondDone = false;
    bool m_patchable = false;
    bool m_hasScissor = false;
    UINT m_targetWidth = 0;
    UINT m_targetHeight = 0;
    bool m_ui = false;  // draw into the back buffer (2D UI)
    bool m_weapon = false;  // first-person weapon layer (viewport depth 0..0.01)
    bool m_mainWorldDepth = false;
    bool m_foveatedWorld = false;
    bool m_singlePassStereo = false;
    bool m_firstPersonClip = false;
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> m_bodyMask;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_savedGeometryConstants;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_savedDepthGeometryConstants;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_originalShadowShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_savedShadowConstants;
    bool m_native = false;  // AER native: this draw fills the whole target (one eye per frame)
    D3D11_VIEWPORT m_viewport{};
    D3D11_RECT m_scissor{};
    ID3D11Buffer* m_originalBuffer = nullptr;
    // HUD quad mode: this UI draw goes once into the HUD texture instead of both eye halves.
    bool m_hudQuad = false;
    ID3D11RenderTargetView* m_savedRtv = nullptr;
    ID3D11DepthStencilView* m_savedDsv = nullptr;
    ID3D11BlendState* m_savedBlend = nullptr;
    FLOAT m_savedFactor[4]{};
    UINT m_savedMask = 0;
    bool BeginHudQuad();
    void CenterHudMarker(ID3D11VertexShader* vertexShader);
    bool m_centeredHudMarker = false;
    // The full-resolution scene composite is drawn for OpenXR and then for the game window.
    bool BeginVirtual(ID3D11RenderTargetView* windowTarget);
    void RestoreRedirectedDraw();
    bool m_virtual = false;
};

// Vertex shader created by the engine: remembered if it is a screen-space blit.
void EngineStereoNoteVertexShader(ID3D11VertexShader* shader, const void* code, SIZE_T length);
void EngineStereoNotePixelShader(ID3D11PixelShader* shader, std::uint64_t bytecodeHash);
// Before the real Present: back buffer identity, projection, frame statistics.
void EngineStereoOnPresent(IDXGISwapChain* swapChain);
// -1 renders both halves; 0 or 1 renders only that eye's scene geometry.
void EngineStereoSetRenderEye(int eye, bool captureDibrDepth = false);
struct WorldDepthProjection {
    float depthScale = 0.0F;  // native M22
    float depthBias = 0.0F;   // native M32, before translation
    float clipWOffset = 0.0F;  // physical headset and placement, in clip W
    bool valid = false;
    std::array<float, 16> view{};
    std::array<float, 3> yawOrbitCorrection{};
};
WorldDepthProjection EngineStereoLastWorldDepthProjection();
class FreeTrackFeed;
struct WorldDrawPoseTrace {
    std::uint64_t frame = 0;
    std::uint32_t returnedBefore = 0;
    std::uint32_t returnedAfter = 0;
    bool sampled = false;
    bool viewValid = false;
    std::array<float, 16> view{};
};
void EngineStereoSetPoseFeed(const FreeTrackFeed* feed);
WorldDrawPoseTrace EngineStereoLastWorldDrawPoseTrace();
struct DibrSceneDepth {
    ID3D11Texture2D* texture = nullptr;
    float worldMin = 0.0F, worldMax = 0.0F;
    float weaponMin = 0.0F, weaponMax = 0.0F;
};
DibrSceneDepth EngineStereoLastDibrDepth();
bool EngineStereoLastWorldVrs();
bool EngineStereoLastStereoShadow();
void EngineStereoReadWorldEyeRecords(float (&eyes)[2][8]);
// From the OpenXR side every frame (hotkeys): on/off, flat (menus), scales.
void EngineStereoSetTuning(bool enabled, bool flat, float separationScale, float convergenceFactor);
// World view: the 2D UI is drawn smaller and nearer the picture centre (viewport math, no new
// textures). scale 1 = unchanged; shift as a fraction of one eye image (x right, y up);
// distance = where the HUD plane appears in depth.
void EngineStereoSetHud(bool active, float scale, float shiftX, float shiftY, float distanceMeters);
// First-person weapon: own depth separation scale and zero-parallax distance (metres).
void EngineStereoSetWeapon(float separationScale, float convergenceMeters, float sizeScale);
// Headset projection mode (world view, ProjectionLayer=1): each eye is drawn with the headset's
// own frustum (tangents left/right/up/down per eye: 8 values) and eye offset, and the frame is
// submitted as a projection layer (as Tarkov KBM VR does). halfIpdMeters includes the depth scale.
// aimBias: -1 puts the right eye on the sight line while aiming, +1 the left eye, 0 neither
// (fractions blend in and out).
void EngineStereoSetHeadset(bool active, float halfIpdMeters, const float tangents[8],
                            float aimBias);
// Head position in projection mode: the camera moves right/up/forward (metres, camera axes) by
// patching the projection constants (no FreeTrack needed).
void EngineStereoSetLean(float right, float up, float forward);
// The measured half-baseline from the center to the right eye, in game camera axes.
void EngineStereoSetPhysicalEyes(bool active, float right, float up, float forward);
// Body's forward axis in the game camera (x right, y up, z forward).
// Used for the first-person standoff and HUD, while Arma owns the model orientation.
void EngineStereoSetWeaponForward(float right, float up, float forward, bool bodyLock,
                                  float aimingBlend);
void EngineStereoSetBodyAngles(float yaw, float pitch);
// Live switch (Ctrl+F6) for the HUD staying where the body aims.
void EngineStereoSetHudLock(bool enabled);
// User placement of the head against the game camera (right, up, forward in metres).
void EngineStereoSetHeadOffset(float right, float up, float forward);
// In a vehicle the first-person layer (cockpit, body, hands) moves with the head too.
void EngineStereoSetVehicle(bool vehicle);
// HUD quad: UI draws of the world view go into one texture (2D UI, drawn once). The OpenXR side
// shows it as a quad fixed in the room. HudTexture is null when nothing was drawn this frame;
// HudFrameDone clears the texture for the next frame (call after the copy).
void EngineStereoSetHudQuad(bool enabled);
void EngineStereoSetDesktopHudMirror(bool enabled);
ID3D11Texture2D* EngineStereoHudTexture(UINT& width, UINT& height);
// Game zoom (scopes) relative to the first world frame: 1 = none. Reset on recentre.
float EngineStereoZoom();
float EngineStereoDrawZoom();
// True while a magnifying optic is in use and ScopeLens=1 (window view, reticle in the picture).
bool EngineStereoScopeLens();
// True while a magnifying optic is up (lens or not): the reticle is drawn in the picture, at the
// aim axis (follows the weapon and the mouse, not the head), and the HUD panel is hidden.
bool EngineStereoScopeUi();
// Pass of the frame just drawn while the scope lens is on: 0 none, 1 world, 2 lens window.
int EngineStereoLensPass();
// Chosen per frame by the head pose publisher (0 none, 1 world, 2 lens).
void EngineStereoSetLensPass(int pass);
// One eye per engine frame drawn like a normal single-camera frame (full target, the engine's own
// projection, only the camera moved): every screen-space effect (shadows, AO, glow) stays correct.
bool EngineStereoTargetIsBackBuffer(ID3D11DeviceContext* context);
void EngineStereoSetAerNative(bool native);
// Menus, map, inventory (cursor shown): draws are not doubled, the engine draws one normal frame.
void EngineStereoSetMonoScreen(bool mono);
// A separate high-resolution scene output for OpenXR, valid only for the completed frame.
void EngineStereoSetVirtualBack(bool enabled);
ID3D11Texture2D* EngineStereoVirtualTexture();
void EngineStereoSetOpticsIntent(bool enabled);
std::uint32_t EngineStereoLastWeaponDraws();
void EngineStereoResetZoomReference();
bool EngineStereoActive();
bool EngineStereoPerfWorld();
// Size of the 3D scene render target seen last (0 = none yet).
void EngineStereoSceneSize(unsigned& width, unsigned& height);
// Distance in metres that lies on the screen plane (zero parallax).
float EngineStereoConvergenceMeters();

// Saves the back buffer as a JPEG (self-check without a headset).
bool DumpBackBufferJpeg(IDXGISwapChain* swapChain, const wchar_t* path);
bool DumpTextureJpeg(ID3D11Texture2D* texture, const wchar_t* path,
                     const D3D11_BOX* region = nullptr);

}  // namespace a3vr
