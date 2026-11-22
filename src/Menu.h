#pragma once

#include <d3d11.h>

#include <functional>

namespace a3vr {

// In-game settings menu (Insert), Dear ImGui drawn into a texture that OpenXR shows as a panel in
// front of the player. While it is open the game does not get the mouse and keys (InputBlock); the
// mouse moves the menu cursor.
class Menu {
public:
    static Menu& Instance();
    bool IsOpen() const { return m_open; }
    void SetOpen(bool open);
    void Toggle() { SetOpen(!m_open); }

    // Draws one menu frame into `target` (width x height pixels, cleared to transparent first).
    // `ui` builds the widgets between NewFrame and Render. Returns false if ImGui could not start.
    bool Render(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
                unsigned width, unsigned height, float pixelScale, const std::function<void()>& ui,
                bool clearTarget = true);

private:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    bool m_open = false;
    bool m_initialized = false;
    bool m_failed = false;
    ID3D11Device* m_device = nullptr;  // the ImGui backend owns the device reference
    float m_cursorX = 0.0F;
    float m_cursorY = 0.0F;
    unsigned long long m_lastTick = 0;
};

}  // namespace a3vr
