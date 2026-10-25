#include "menu.h"
#include "common.h"
#include "frameConstants.h"
#include "introVideos.h"
#include "reshadeBridge.h"
#include "settings.h"

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "MinHook.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static const std::string g_modVersion = "Alias Isolation v1.3.0-fixes";

Settings g_settings;

static ID3D11Device *g_pd3dDevice = nullptr;
static ID3D11DeviceContext *g_pd3dDeviceContext = nullptr;
static IDXGISwapChain *g_pSwapChain = nullptr;
static ID3D11RenderTargetView *g_mainRenderTargetView = nullptr;
static HWND g_hWindow = nullptr;
static WNDPROC g_originalWndProcHandler = nullptr;

bool g_menuInitialised = false;
bool g_showMenu = false;
#ifdef ALIASISOLATION_ENABLE_PROFILER
std::string g_profilerStats = "";
extern bool g_shaderHooksEnabled;
#endif // ALIASISOLATION_ENABLE_PROFILER

// Forward-declarations
static void SetupDeviceAndContext(IDXGISwapChain &swapChain);

// A:I reads the mouse through Raw Input during gameplay, so the legacy WM_*BUTTON* / WM_MOUSEMOVE messages the
// menu used to rely on may never arrive, and the game keeps re-centering and re-clipping the cursor every frame.
// While the menu is open we therefore poll the mouse ourselves and stop the game from moving or clipping the cursor.
using SetCursorPos_t = BOOL(WINAPI *)(int, int);
using ClipCursor_t = BOOL(WINAPI *)(const RECT *);
static SetCursorPos_t g_SetCursorPos_orig = nullptr;
static ClipCursor_t g_ClipCursor_orig = nullptr;

static BOOL WINAPI SetCursorPos_hook(int x, int y)
{
    if (g_showMenu)
        return TRUE;
    return g_SetCursorPos_orig(x, y);
}

static BOOL WINAPI ClipCursor_hook(const RECT *rect)
{
    if (g_showMenu)
        return g_ClipCursor_orig(nullptr);
    return g_ClipCursor_orig(rect);
}

static void InstallCursorHooks()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    LPVOID setCursorPosTarget = nullptr;
    LPVOID clipCursorTarget = nullptr;
    MH_CHECK(MH_CreateHookApiEx(L"user32", "SetCursorPos", reinterpret_cast<LPVOID>(&SetCursorPos_hook),
                                reinterpret_cast<LPVOID *>(&g_SetCursorPos_orig), &setCursorPosTarget));
    MH_CHECK(MH_CreateHookApiEx(L"user32", "ClipCursor", reinterpret_cast<LPVOID>(&ClipCursor_hook),
                                reinterpret_cast<LPVOID *>(&g_ClipCursor_orig), &clipCursorTarget));
    MH_CHECK(MH_EnableHook(setCursorPosTarget));
    MH_CHECK(MH_EnableHook(clipCursorTarget));
}

static void SetMenuVisible(HWND hWnd, bool visible)
{
    g_showMenu = visible;

    ImGuiIO &io = ImGui::GetIO();
    // The game hides the OS cursor in gameplay; let ImGui draw its own so the menu is usable either way.
    io.MouseDrawCursor = visible;

    if (visible)
    {
        g_ClipCursor_orig(nullptr);
    }
    else
    {
        RECT windowRect;
        GetClientRect(hWnd, &windowRect);
        MapWindowPoints(hWnd, NULL, reinterpret_cast<LPPOINT>(&windowRect), 2);
        g_ClipCursor_orig(&windowRect);
        io.ClearInputMouse();
    }
}

// Feed the mouse to ImGui from the global cursor and button state, which works whether or not the game has
// legacy mouse messages turned off.
static void PollMouse()
{
    ImGuiIO &io = ImGui::GetIO();
    if (GetForegroundWindow() != g_hWindow)
        return;

    POINT pos;
    if (GetCursorPos(&pos) && ScreenToClient(g_hWindow, &pos))
        io.AddMousePosEvent(static_cast<float>(pos.x), static_cast<float>(pos.y));

    // Respect swapped mouse buttons: GetAsyncKeyState reports physical buttons.
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    const int buttons[3] = {swapped ? VK_RBUTTON : VK_LBUTTON, swapped ? VK_LBUTTON : VK_RBUTTON, VK_MBUTTON};
    for (int i = 0; i < 3; i++)
        io.AddMouseButtonEvent(i, (GetAsyncKeyState(buttons[i]) & 0x8000) != 0);
}

static bool IsInputMessage(UINT uMsg)
{
    return (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST) || (uMsg >= WM_KEYFIRST && uMsg <= WM_KEYLAST) ||
           uMsg == WM_INPUT;
}

// Our custom window handler to override A:I's own handler.
// This lets us detect keyboard inputs and pass mouse + keyboard input control to ImGui.
LRESULT CALLBACK Menu::WndProcHandler(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    ImGuiIO &io = ImGui::GetIO();
    // Ctrl+Delete is the mod's disable hotkey, so only a plain Delete toggles the menu.
    if (uMsg == WM_KEYDOWN && wParam == VK_DELETE && (GetKeyState(VK_CONTROL) & 0x8000) == 0)
    {
        SetMenuVisible(hWnd, !g_showMenu);
        return 0;
    }

    if (g_showMenu)
    {
        switch (uMsg)
        {
        case WM_MOUSEWHEEL:
            io.AddMouseWheelEvent(0.0f, static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA);
            return 0;
        case WM_KILLFOCUS:
            io.AddFocusEvent(false);
            break;
        case WM_SETFOCUS:
            io.AddFocusEvent(true);
            break;
        case WM_CHAR:
            if (wParam > 0 && wParam < 0x10000)
                io.AddInputCharacterUTF16(static_cast<unsigned short>(wParam));
            return 0;
        case WM_INPUT:
            // Keep the game from turning the camera behind the menu; DefWindowProc frees the raw input buffer.
            return DefWindowProc(hWnd, uMsg, wParam, lParam);
        default:
            break;
        }

        // Swallow the rest of the input, but let window, focus and activation messages reach the game so that
        // Alt+Tab and resizing keep working while the menu is open.
        if (IsInputMessage(uMsg))
            return 0;
    }

    return CallWindowProc(g_originalWndProcHandler, hWnd, uMsg, wParam, lParam);
}

static void doMenuStyling()
{
    ImGuiStyle &style = ImGui::GetStyle();
    ImVec4 *colors = style.Colors;
    ImGui::StyleColorsDark(&style);

    colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.17f, 0.37f, 0.00f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.19f, 0.19f, 0.19f, 0.92f);
    colors[ImGuiCol_Border] = ImVec4(0.19f, 0.19f, 0.19f, 0.29f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.24f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.05f, 0.05f, 0.05f, 0.54f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.19f, 0.19f, 0.54f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.22f, 0.23f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.05f, 0.05f, 0.05f, 0.54f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.34f, 0.34f, 0.34f, 0.54f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 0.54f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.56f, 0.56f, 0.56f, 0.54f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.33f, 0.67f, 0.86f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.34f, 0.34f, 0.34f, 0.54f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.56f, 0.56f, 0.56f, 0.54f);
    colors[ImGuiCol_Button] = ImVec4(0.05f, 0.05f, 0.05f, 0.54f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.19f, 0.19f, 0.19f, 0.54f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.22f, 0.23f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.00f, 0.00f, 0.00f, 0.52f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.00f, 0.00f, 0.00f, 0.36f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.22f, 0.23f, 0.33f);
    colors[ImGuiCol_Separator] = ImVec4(0.28f, 0.28f, 0.28f, 0.29f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.44f, 0.44f, 0.44f, 0.29f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.40f, 0.44f, 0.47f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.28f, 0.28f, 0.28f, 0.29f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.44f, 0.44f, 0.44f, 0.29f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.40f, 0.44f, 0.47f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.00f, 0.00f, 0.00f, 0.52f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.20f, 0.20f, 0.20f, 0.36f);
    colors[ImGuiCol_TabDimmed] = ImVec4(0.00f, 0.00f, 0.00f, 0.52f);
    colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_PlotLines] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.52f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.00f, 0.00f, 0.00f, 0.52f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.28f, 0.28f, 0.28f, 0.29f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.20f, 0.22f, 0.23f, 1.00f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(0.33f, 0.67f, 0.86f, 1.00f);
    colors[ImGuiCol_NavHighlight] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 0.00f, 0.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(1.00f, 0.00f, 0.00f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(1.00f, 0.00f, 0.00f, 0.35f);

    style.WindowPadding = ImVec2(8.00f, 8.00f);
    style.FramePadding = ImVec2(5.00f, 2.00f);
    style.CellPadding = ImVec2(6.00f, 6.00f);
    style.ItemSpacing = ImVec2(6.00f, 6.00f);
    style.ItemInnerSpacing = ImVec2(6.00f, 6.00f);
    style.TouchExtraPadding = ImVec2(0.00f, 0.00f);
    style.IndentSpacing = 25;
    style.ScrollbarSize = 15;
    style.GrabMinSize = 10;
    style.WindowBorderSize = 1;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 1;
    style.TabBorderSize = 1;
    style.WindowRounding = 7;
    style.ChildRounding = 4;
    style.FrameRounding = 3;
    style.PopupRounding = 4;
    style.ScrollbarRounding = 9;
    style.GrabRounding = 3;
    style.LogSliderDeadzone = 4;
    style.TabRounding = 4;
}

void Menu::InitMenu(IDXGISwapChain &swapChain)
{
    loadOrCreateSettings(&g_settings);

    SetupDeviceAndContext(swapChain);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    doMenuStyling();

    // Get the swapchain description (we use this to get the handle to the game's window).
    DXGI_SWAP_CHAIN_DESC dxgiSwapChainDesc;
    DX_CHECK(g_pSwapChain->GetDesc(&dxgiSwapChainDesc));

    g_hWindow = dxgiSwapChainDesc.OutputWindow;
    InstallCursorHooks();
    // Replace the game's window handler with our own so we can intercept key events in the game.
    g_originalWndProcHandler = reinterpret_cast<WNDPROC>(
        SetWindowLongPtr(g_hWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Menu::WndProcHandler)));

    ImGui_ImplWin32_Init(g_hWindow);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    Menu::CreateRenderTarget();

    g_menuInitialised = true;
}

// Switches for ReShade's effects, such as DLSS 5 (DLSS5-Feeder) and LumeniteFX. ReShade saves them in its preset, so
// they don't need "Save Settings".
static void DrawReShadeSection()
{
    if (!ReShadeBridge::isAvailable())
    {
        return;
    }

    ImGui::SeparatorText("ReShade");

    bool effectsEnabled = ReShadeBridge::getEffectsEnabled();
    if (ImGui::Checkbox("ReShade effects", &effectsEnabled))
    {
        ReShadeBridge::setEffectsEnabled(effectsEnabled);
    }

    ImGui::BeginDisabled(!effectsEnabled);
    ImGui::Indent();
    for (const ReShadeBridge::Technique &technique : ReShadeBridge::getTechniques())
    {
        bool enabled = technique.enabled;
        const std::string id = technique.label + "##" + technique.effect + "/" + technique.name;
        if (ImGui::Checkbox(id.c_str(), &enabled))
        {
            ReShadeBridge::setTechniqueEnabled(technique, enabled);
        }
    }
    ImGui::Unindent();
    ImGui::EndDisabled();
}

static void DrawIntroVideosSection()
{
    static bool stateRead = false;
    static IntroVideos::State state;
    static bool moveFailed = false;
    if (!stateRead)
    {
        stateRead = true;
        state = IntroVideos::getState();
    }

    ImGui::SeparatorText("Startup");

    if (state == IntroVideos::State::Unknown)
    {
        ImGui::TextDisabled("Intro videos: files missing or partly moved.");
        ImGui::TextDisabled("Switching this finishes the move. The installer script can restore missing files.");
    }

    bool skipped = state == IntroVideos::State::Skipped;
    if (ImGui::Checkbox("Skip intro videos (from the next launch)", &skipped))
    {
        moveFailed = !IntroVideos::setSkipped(skipped);
        state = IntroVideos::getState();
    }
    if (moveFailed)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Couldn't move the videos. Is the game folder read-only?");
    }
}

// Shown on top of the game, menu open or not, until the video settings are fixed or the mod is disabled.
static void DrawSettingsWarning()
{
    if (!g_settingsWarnings)
    {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("Alias Isolation - Video settings", NULL,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Alias Isolation needs these video settings:");
    if (g_settingsWarnings & SettingsWarning_Smaa)
    {
        ImGui::Text("- Anti-Aliasing: SMAA T1x");
    }
    if (g_settingsWarnings & SettingsWarning_MotionBlur)
    {
        ImGui::Text("- Motion Blur: Enabled");
    }
    ImGui::Text("Ctrl+Delete turns the mod off.");
    ImGui::End();
}

void Menu::DrawMenu()
{
    if (g_menuInitialised && (g_showMenu || g_settingsWarnings))
    {
        // Feed inputs to dear imgui, start new frame.
        if (g_showMenu)
        {
            PollMouse();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawSettingsWarning();

        if (g_showMenu)
        {

#ifdef ALIASISOLATION_ENABLE_PROFILER
            {
                ImGui::SetNextWindowSize(ImVec2(300, 300), ImGuiCond_FirstUseEver);
                ImGui::Begin("Alias Isolation - Profiler", NULL,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);
#ifdef ALIASISOLATION_NO_SMAA_JITTER_ADD
                ImGui::TextColored(ImVec4(255, 0, 0, 1), "No SMAA jitter calc.");
#endif
#ifdef ALIASISOLATION_NO_JITTER_REMOVE
                ImGui::TextColored(ImVec4(255, 0, 0, 1), "No jitter removal");
#endif
#ifdef ALIASISOLATION_NO_JITTER_ADD
                ImGui::TextColored(ImVec4(255, 0, 0, 1), "No jitter calculation");
#endif
#ifdef ALIASISOLATION_NO_RGBM_VS
                ImGui::TextColored(ImVec4(255, 0, 0, 1), "No RGBM vertex shader");
#endif
#ifdef ALIASISOLATION_NO_SMAA_VS
                ImGui::TextColored(ImVec4(255, 0, 0, 1), "No SMAA vertex shader");
#endif
#ifdef ALIASISOLATION_NO_VS_OVERRIDES
                ImGui::TextColored(ImVec4(255, 0, 0, 1), "No vertex shader overrides");
#endif
#ifdef ALIASISOLATION_NO_PS_OVERRIDES
                ImGui::TextColored(ImVec4(255, 0, 0, 1), "No pixel shader overrides");
#endif
#ifdef ALIASISOLATION_NO_TAA_PASS
                ImGui::TextColored(ImVec4(255, 0, 0, 1), "No TAA pass");
#endif
#ifdef ALIASISOLATION_NO_CA_PASS
                ImGui::TextColored(ImVec4(255, 0, 0, 1), "No CA pass");
#endif
                if (!g_shaderHooksEnabled)
                {
                    ImGui::TextColored(ImVec4(255, 0, 0, 1), "Hooks disabled");
                }
                else
                {
                    ImGui::TextColored(ImVec4(255, 170, 0, 1), g_profilerStats.c_str());
                }
                ImGui::End();
            }
#endif // ALIASISOLATION_ENABLE_PROFILER

            static bool aliasIsolation_menu_showAboutWindow = false;

            if (aliasIsolation_menu_showAboutWindow)
            {
                if (ImGui::Begin("Alias Isolation - About", &aliasIsolation_menu_showAboutWindow,
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
                {
                    ImGui::TextColored(ImVec4(255, 170, 0, 1), "%s - Built on %s at %s", g_modVersion.c_str(), __DATE__,
                                       __TIME__);
                    ImGui::Text("Dear ImGui %s", ImGui::GetVersion());
                    ImGui::Separator();
                    ImGui::Text("Build information:");
#ifdef _DEBUG
                    ImGui::Text("Target: Debug");
#else
                    ImGui::Text("Target: Release");
#endif
#ifdef _WIN32
                    ImGui::Text("Architecture: x86");
#elif _WIN64
                    ImGui::Text("Architecture: x64");
#endif
#ifdef __clang__
                    ImGui::Text("Compiler: Clang/LLVM");
                    ImGui::Text("Compiler Version: %s", __VERSION__);
#endif
#if defined(_MSC_VER) && !defined(__clang__)
                    ImGui::Text("MSVC Compiler Version: %d (%d)", _MSC_VER, _MSC_FULL_VER);
                    ImGui::Text("MSVC C++ Standard: %lu", _MSVC_LANG);
#endif
                    ImGui::Separator();
                    ImGui::Text("Mod credits:");
                    ImGui::Text("Alias Isolation originally by aliasIsolation.");
                    ImGui::Text("Currently maintained by RyanJGray.");
                    ImGui::Text("Thanks for making this mod a reality, aliasIsolation - whoever you are!");
                }

                ImGui::End();
            }

            // Get the center of the screen.
            ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

            {
                ImGui::Begin((g_modVersion + " - Settings").c_str(), NULL,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_MenuBar |
                                 ImGuiWindowFlags_NoSavedSettings);

                if (ImGui::BeginMenuBar())
                {
                    if (ImGui::BeginMenu("Help"))
                    {
                        if (ImGui::MenuItem("About"))
                        {
                            aliasIsolation_menu_showAboutWindow = true;
                        }

                        ImGui::EndMenu();
                    }

                    ImGui::EndMenuBar();
                }

                if (ReShadeBridge::hasHomePage())
                {
                    ImGui::TextUnformatted("Alias Isolation's switch and settings are in the ReShade overlay:");
                    ImGui::TextUnformatted("Home > Alias Isolation.");
                    ImGui::End();
                }
                else
                {
                    ImGui::Checkbox("Sharpening", &g_settings.sharpeningEnabled);
                    if (g_settings.sharpeningEnabled)
                    {
                        ImGui::SliderFloat("Sharpening Amount", &g_settings.sharpening, 0.0f, 1.0f, "%.2f");
                    }

                    ImGui::Checkbox("Chromatic Aberration", &g_settings.chromaticAberrationEnabled);
                    if (g_settings.chromaticAberrationEnabled)
                    {
                        ImGui::SliderFloat("Chromatic Aberration Amount", &g_settings.chromaticAberration, 0.0f, 1.0f,
                                           "%.2f");
                    }

                    ImGui::Checkbox("Show logo when a level loads", &g_settings.showLogo);

                    if (ImGui::Button("Save Settings"))
                    {
                        saveSettings(g_settings);
                    }

                    DrawReShadeSection();
                    DrawIntroVideosSection();

                    ImGui::End();
                }
            }
        }

        ImGui::EndFrame();

        // Render dear imgui onto the screen.
        ImGui::Render();
        if (!g_mainRenderTargetView || !g_pd3dDeviceContext)
        {
            DebugBreak();
        }
        else
        {
            g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        // A call to DXGISwapChain->Present is not needed here, as we are hooked into DirectX 11's Present function, so
        // anything we draw here will be presented by the game.
    }
}

void Menu::ShutdownMenu()
{
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    Menu::CleanupRenderTarget();
    if (g_pSwapChain)
    {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }
    if (g_pd3dDeviceContext)
    {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice)
    {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }

    g_hWindow = nullptr;
    g_originalWndProcHandler = nullptr;
    g_menuInitialised = false;
}

static void SetupDeviceAndContext(IDXGISwapChain &swapChain)
{
    g_pSwapChain = &swapChain;

    DX_CHECK(g_pSwapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void **>(&g_pd3dDevice)))
    g_pd3dDevice->GetImmediateContext(&g_pd3dDeviceContext);
}

void Menu::CreateRenderTarget()
{
    ID3D11Texture2D *pBackBuffer;
    DX_CHECK(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)));
    DX_CHECK(g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView));
    pBackBuffer->Release();
}

void Menu::CleanupRenderTarget()
{
    if (g_mainRenderTargetView)
    {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}