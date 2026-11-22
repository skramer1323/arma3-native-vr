#include "Menu.h"

#include "InputBlock.h"
#include "Log.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <Windows.h>

#include <algorithm>

namespace a3vr {

Menu& Menu::Instance()
{
    static Menu menu;
    return menu;
}

void Menu::SetOpen(bool open)
{
    if (m_open == open) {
        return;
    }
    m_open = open;
    InputBlockSetActive(open);
    Log("MENU %s (Insert)", open ? "opened" : "closed");
}

bool Menu::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (m_device != device) {
        if (m_initialized) {
            ImGui_ImplDX11_Shutdown();
            ImGui::DestroyContext();
        }
        m_device = device;
        m_initialized = m_failed = false;
    }
    if (m_initialized) {
        return true;
    }
    if (m_failed) {
        return false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini next to the game
    io.LogFilename = nullptr;
    io.MouseDrawCursor = true;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImFontConfig fontConfig;
    fontConfig.SizePixels = 26.0F;
    io.Fonts->AddFontDefaultVector(&fontConfig);
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0F;
    style.FrameRounding = 6.0F;
    style.GrabRounding = 6.0F;
    style.ItemSpacing = ImVec2(10.0F, 8.0F);
    style.FramePadding = ImVec2(10.0F, 6.0F);
    style.GrabMinSize = 22.0F;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.06F, 0.07F, 0.09F, 0.94F);
    if (!ImGui_ImplDX11_Init(device, context)) {
        Log("MENU ImGui DX11 backend failed to start");
        ImGui::DestroyContext();
        m_failed = true;
        return false;
    }
    m_initialized = true;
    Log("MENU ImGui %s ready", ImGui::GetVersion());
    return true;
}

bool Menu::Render(ID3D11Device* device, ID3D11DeviceContext* context,
                  ID3D11RenderTargetView* target, unsigned width, unsigned height,
                  float pixelScale, const std::function<void()>& ui, bool clearTarget)
{
    if (!device || !context || !target || !Initialize(device, context)) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DisplayFramebufferScale = ImVec2(pixelScale, pixelScale);
    io.MouseDrawCursor = m_open;  // no cursor on the small shader overlay
    const unsigned long long now = GetTickCount64();
    io.DeltaTime = m_lastTick == 0 ? 1.0F / 60.0F
                                   : std::clamp(static_cast<float>(now - m_lastTick) / 1000.0F,
                                                0.001F, 0.1F);
    m_lastTick = now;

    // The mouse the game no longer sees moves the menu cursor.
    float dx = 0.0F;
    float dy = 0.0F;
    float wheel = 0.0F;
    InputBlockTakeMouse(dx, dy, wheel);
    constexpr float kSpeed = 1.6F;
    m_cursorX = std::clamp(m_cursorX + dx * kSpeed, 0.0F, static_cast<float>(width));
    m_cursorY = std::clamp(m_cursorY + dy * kSpeed, 0.0F, static_cast<float>(height));
    io.AddMousePosEvent(m_cursorX, m_cursorY);
    io.AddMouseButtonEvent(0, InputBlockMouseButton(0));
    io.AddMouseButtonEvent(1, InputBlockMouseButton(1));
    if (wheel != 0.0F) {
        io.AddMouseWheelEvent(0.0F, wheel / 120.0F);
    }

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    ui();
    ImGui::Render();

    ID3D11RenderTargetView* savedTarget = nullptr;
    ID3D11DepthStencilView* savedDepth = nullptr;
    context->OMGetRenderTargets(1, &savedTarget, &savedDepth);
    if (clearTarget) {
        const FLOAT clear[4]{};
        context->ClearRenderTargetView(target, clear);
    }
    context->OMSetRenderTargets(1, &target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    context->OMSetRenderTargets(1, &savedTarget, savedDepth);
    if (savedTarget) {
        savedTarget->Release();
    }
    if (savedDepth) {
        savedDepth->Release();
    }
    return true;
}

}  // namespace a3vr
