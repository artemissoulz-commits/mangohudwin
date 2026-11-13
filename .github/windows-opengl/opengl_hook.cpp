#include <windows.h>
#include <GL/gl.h>
#include "MinHook.h"
#include <imgui.h>
#include <implot.h>
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"
#include <mutex>

#include "../overlay.h"
#include "../hud_elements.h"
#include "d3d_shared.h"

namespace {
using SwapBuffersFn = BOOL (WINAPI *)(HDC);
SwapBuffersFn realSwapBuffers = nullptr;
std::mutex hookMutex;
std::mutex renderMutex;
bool hookInstalled = false;
bool imguiReady = false;
HWND overlayWindow = nullptr;

BOOL WINAPI hookedSwapBuffers(HDC dc)
{
    std::lock_guard<std::mutex> renderLock(renderMutex);

    if (wglGetCurrentContext()) {
        dx_version = kiero::RenderType::OpenGL;

        if (!imguiReady) {
            HWND hwnd = WindowFromDC(dc);
            if (hwnd) {
                overlayWindow = hwnd;
                IMGUI_CHECKVERSION();
                ImGui::CreateContext();
                ImPlot::CreateContext();
                ImGui::GetIO().IniFilename = nullptr;

                init_d3d_shared();
                ImGui::StyleColorsDark();
                HUDElements.convert_colors(false, params);

                const bool platformReady = ImGui_ImplWin32_Init(hwnd);
                const bool rendererReady =
                    platformReady && ImGui_ImplOpenGL3_Init("#version 130");

                if (platformReady && rendererReady) {
                    RECT rect{};
                    GetClientRect(hwnd, &rect);
                    ImGui::GetIO().DisplaySize = ImVec2(
                        static_cast<float>(rect.right - rect.left),
                        static_cast<float>(rect.bottom - rect.top));

                    create_fonts(nullptr, params, sw_stats.font_small,
                                 sw_stats.font_text, sw_stats.font_secondary);
                    sw_stats.font_params_hash = params.font_params_hash;
                    imguiReady = true;
                } else {
                    if (rendererReady)
                        ImGui_ImplOpenGL3_Shutdown();
                    if (platformReady)
                        ImGui_ImplWin32_Shutdown();
                    ImPlot::DestroyContext();
                    ImGui::DestroyContext();
                    overlayWindow = nullptr;
                }
            }
        }

        if (imguiReady) {
            RECT rect{};
            if (overlayWindow && GetClientRect(overlayWindow, &rect)) {
                ImGuiIO& io = ImGui::GetIO();
                io.DisplaySize = ImVec2(
                    static_cast<float>(rect.right - rect.left),
                    static_cast<float>(rect.bottom - rect.top));
            }

            d3d_run();
            if (HUDElements.colors.update)
                HUDElements.convert_colors(params);

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            overlay_new_frame(params);
            position_layer(sw_stats, params, window_size);
            render_imgui(sw_stats, params, window_size, false);
            overlay_end_frame();

            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            if (sw_stats.font_params_hash != params.font_params_hash) {
                sw_stats.font_params_hash = params.font_params_hash;
                create_fonts(nullptr, params, sw_stats.font_small,
                             sw_stats.font_text, sw_stats.font_secondary);
            }
        }
    }

    return realSwapBuffers ? realSwapBuffers(dc) : FALSE;
}
} // namespace

bool init_opengl_hook()
{
    std::lock_guard<std::mutex> lock(hookMutex);
    if (hookInstalled && realSwapBuffers)
        return true;

    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi)
        gdi = LoadLibraryW(L"gdi32.dll");
    if (!gdi)
        return false;

    void* target = reinterpret_cast<void*>(GetProcAddress(gdi, "SwapBuffers"));
    if (!target)
        return false;

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
        return false;

    const MH_STATUS createStatus =
        MH_CreateHook(target, &hookedSwapBuffers,
                      reinterpret_cast<void**>(&realSwapBuffers));
    if (createStatus != MH_OK)
        return false;

    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        realSwapBuffers = nullptr;
        return false;
    }

    hookInstalled = true;
    return true;
}
