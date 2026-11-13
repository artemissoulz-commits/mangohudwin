$ErrorActionPreference = 'Stop'
$root = (Get-Location).Path
$nl = [Environment]::NewLine

function Write-Utf8File([string]$RelativePath, [string]$Content) {
    $full = Join-Path $root $RelativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $full) -Force | Out-Null
    [System.IO.File]::WriteAllText($full, $Content, [System.Text.UTF8Encoding]::new($false))
}

# Enable ImGui OpenGL3 and link the Windows OpenGL/GDI libraries.
$rootPath = Join-Path $root 'meson.build'
$rootMeson = [System.IO.File]::ReadAllText($rootPath).Replace("'opengl=disabled'", "'opengl=enabled'")
if (-not $rootMeson.Contains("'opengl=enabled'")) { throw 'ImGui OpenGL3 backend could not be enabled.' }
$anchor = "dep_iphlpapi = cc.find_library('iphlpapi', required: false)"
if (-not $rootMeson.Contains('dep_opengl32 =')) {
    if (-not $rootMeson.Contains($anchor)) { throw 'Windows library declaration anchor missing in meson.build.' }
    $rootMeson = $rootMeson.Replace($anchor, $anchor + $nl + "dep_opengl32 = cc.find_library('opengl32', required: true)" + $nl + "dep_gdi32 = cc.find_library('gdi32', required: true)")
}
[System.IO.File]::WriteAllText($rootPath, $rootMeson, [System.Text.UTF8Encoding]::new($false))

# Add the WGL hook to the MangoHud DLL and add a launcher executable.
$srcPath = Join-Path $root 'src/meson.build'
$srcMeson = [System.IO.File]::ReadAllText($srcPath)
$anchor = "  'win/d3d_shared.cpp',"
if (-not $srcMeson.Contains("win/opengl_hook.cpp")) {
    if (-not $srcMeson.Contains($anchor)) { throw 'Windows source list anchor missing in src/meson.build.' }
    $srcMeson = $srcMeson.Replace($anchor, $anchor + $nl + "  'win/opengl_hook.cpp',")
}
$idx = $srcMeson.IndexOf("    dep_d3d12,")
if ($idx -lt 0) { throw 'MangoHud library dependency anchor missing.' }
if (-not $srcMeson.Contains('    dep_opengl32,')) {
    $idx += "    dep_d3d12,".Length
    $srcMeson = $srcMeson.Insert($idx, $nl + '    dep_opengl32,' + $nl + '    dep_gdi32,')
}
$anchor = '  objects: mangohud_static_lib.extract_all_objects(),'
if (-not $srcMeson.Contains("sources: files('win/opengl_bootstrap.cpp')")) {
    if (-not $srcMeson.Contains($anchor)) { throw 'MangoHud.dll target anchor missing.' }
    $srcMeson = $srcMeson.Replace($anchor, $anchor + $nl + "  sources: files('win/opengl_bootstrap.cpp'),")
}
$anchor = '# MangoHud standalone overlay (windowed mode, default)'
if (-not $srcMeson.Contains('MangoHudLauncher')) {
    if (-not $srcMeson.Contains($anchor)) { throw 'Launcher insertion anchor missing.' }
    $target = @'
# Launcher injects MangoHud.dll before the game starts.
mangohud_launcher = executable(
  'MangoHudLauncher',
  files('app/mangohud_launcher.cpp'),
  install: true,
)

'@
    $srcMeson = $srcMeson.Replace($anchor, $target + $anchor)
}
[System.IO.File]::WriteAllText($srcPath, $srcMeson, [System.Text.UTF8Encoding]::new($false))

$hook = @'
#include <windows.h>
// Use the Windows SDK WGL declarations. src/gl/gl.h is a Linux GLX compatibility shim.
#include <wingdi.h>
#include <MinHook.h>
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
        // The overlay API label reads sw_stats.engine, not dx_version.
        sw_stats.engine = EngineTypes::OPENGL;
        sw_stats.engineName = "OpenGL";
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
                const bool rendererReady = platformReady && ImGui_ImplOpenGL3_Init("#version 130");
                if (platformReady && rendererReady) {
                    RECT rect{};
                    GetClientRect(hwnd, &rect);
                    ImGui::GetIO().DisplaySize = ImVec2(
                        static_cast<float>(rect.right - rect.left),
                        static_cast<float>(rect.bottom - rect.top));
                    create_fonts(nullptr, params, sw_stats.font_small, sw_stats.font_text, sw_stats.font_secondary);
                    sw_stats.font_params_hash = params.font_params_hash;
                    imguiReady = true;
                } else {
                    if (rendererReady) ImGui_ImplOpenGL3_Shutdown();
                    if (platformReady) ImGui_ImplWin32_Shutdown();
                    ImPlot::DestroyContext();
                    ImGui::DestroyContext();
                    overlayWindow = nullptr;
                }
            }
        }
        if (imguiReady) {
            RECT rect{};
            if (overlayWindow && GetClientRect(overlayWindow, &rect)) {
                ImGui::GetIO().DisplaySize = ImVec2(
                    static_cast<float>(rect.right - rect.left),
                    static_cast<float>(rect.bottom - rect.top));
            }
            d3d_run();
            if (HUDElements.colors.update) HUDElements.convert_colors(params);
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
                create_fonts(nullptr, params, sw_stats.font_small, sw_stats.font_text, sw_stats.font_secondary);
            }
        }
    }
    return realSwapBuffers ? realSwapBuffers(dc) : FALSE;
}
} // namespace

bool init_opengl_hook()
{
    std::lock_guard<std::mutex> lock(hookMutex);
    if (hookInstalled && realSwapBuffers) return true;
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
    if (!gdi) return false;
    void* target = reinterpret_cast<void*>(GetProcAddress(gdi, "SwapBuffers"));
    if (!target) return false;
    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) return false;
    const MH_STATUS createStatus = MH_CreateHook(
        target, &hookedSwapBuffers, reinterpret_cast<void**>(&realSwapBuffers));
    if (createStatus != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        realSwapBuffers = nullptr;
        return false;
    }
    hookInstalled = true;
    return true;
}
'@

$bootstrap = @'
#include <windows.h>
#include "opengl_hook.h"

static void signalLauncherReady()
{
    // Named event: MangoHudOpenGLReady_<launcher-pid>_<tick-count>.
    wchar_t eventName[512] = {};
    const DWORD capacity = static_cast<DWORD>(sizeof(eventName) / sizeof(eventName[0]));
    const DWORD length = GetEnvironmentVariableW(L"MANGOHUD_OPENGL_READY_EVENT", eventName, capacity);
    if (length == 0 || length >= capacity) return;
    HANDLE eventHandle = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
    if (eventHandle) {
        SetEvent(eventHandle);
        CloseHandle(eventHandle);
    }
}

static DWORD WINAPI install_opengl_hook_thread(void*)
{
    if (init_opengl_hook()) signalLauncherReady();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, install_opengl_hook_thread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
'@

$header = @'
#pragma once
bool init_opengl_hook();
'@

$launcher = @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

static std::wstring quote_argument(const std::wstring& value)
{
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') ++backslashes;
        else if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result += L'"';
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result += ch;
        }
    }
    result.append(backslashes * 2, L'\\');
    result += L'"';
    return result;
}

static bool inject_dll(HANDLE process, const std::wstring& dllPath)
{
    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) return false;
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, dllPath.c_str(), bytes, &written) || written != bytes) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
    if (!loadLibrary) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }
    const DWORD waitResult = WaitForSingleObject(thread, INFINITE);
    DWORD moduleHandle = 0;
    const bool loaded = waitResult == WAIT_OBJECT_0 && GetExitCodeThread(thread, &moduleHandle) && moduleHandle != 0;
    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return loaded;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        std::wcerr << L"Usage: MangoHudLauncher.exe <game.exe> [arguments...]\n";
        return 2;
    }
    const std::filesystem::path targetPath = std::filesystem::absolute(argv[1]);
    if (!std::filesystem::exists(targetPath)) {
        std::wcerr << L"Game executable not found: " << targetPath.wstring() << L"\n";
        return 3;
    }
    wchar_t launcherPath[MAX_PATH] = {};
    const DWORD pathLength = GetModuleFileNameW(nullptr, launcherPath, MAX_PATH);
    if (!pathLength || pathLength >= MAX_PATH) return 4;
    const std::filesystem::path dllPath = std::filesystem::path(launcherPath).parent_path() / L"MangoHud.dll";
    if (!std::filesystem::exists(dllPath)) {
        std::wcerr << L"MangoHud.dll not found beside MangoHudLauncher.exe.\n";
        return 5;
    }

    const std::wstring eventName = L"Local\\MangoHudOpenGLReady_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    if (!readyEvent) return 6;

    DWORD oldLength = GetEnvironmentVariableW(L"MANGOHUD_OPENGL_READY_EVENT", nullptr, 0);
    std::vector<wchar_t> oldBuffer(oldLength ? oldLength : 1);
    const bool hadOld = oldLength > 0 &&
        GetEnvironmentVariableW(L"MANGOHUD_OPENGL_READY_EVENT", oldBuffer.data(), oldLength) > 0;
    if (!SetEnvironmentVariableW(L"MANGOHUD_OPENGL_READY_EVENT", eventName.c_str())) {
        CloseHandle(readyEvent);
        return 7;
    }

    // Pass the installed configuration to this game only, then restore the
    // launcher's environment immediately after CreateProcessW.
    const std::filesystem::path configPath =
        dllPath.parent_path().parent_path() / L"config" / L"MangoHud.conf";
    const bool useInstalledConfig = std::filesystem::exists(configPath);
    DWORD oldConfigLength = GetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", nullptr, 0);
    std::vector<wchar_t> oldConfigBuffer(oldConfigLength ? oldConfigLength : 1);
    const bool hadOldConfig = oldConfigLength > 0 &&
        GetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", oldConfigBuffer.data(), oldConfigLength) > 0;
    if (useInstalledConfig &&
        !SetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", configPath.c_str())) {
        if (hadOld) SetEnvironmentVariableW(L"MANGOHUD_OPENGL_READY_EVENT", oldBuffer.data());
        else SetEnvironmentVariableW(L"MANGOHUD_OPENGL_READY_EVENT", nullptr);
        CloseHandle(readyEvent);
        return 8;
    }

    std::wstring commandLine = quote_argument(targetPath.wstring());
    for (int i = 2; i < argc; ++i) commandLine += L" " + quote_argument(argv[i]);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    const BOOL created = CreateProcessW(
        targetPath.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED, nullptr, targetPath.parent_path().c_str(), &startup, &process);

    if (hadOld) SetEnvironmentVariableW(L"MANGOHUD_OPENGL_READY_EVENT", oldBuffer.data());
    else SetEnvironmentVariableW(L"MANGOHUD_OPENGL_READY_EVENT", nullptr);
    if (useInstalledConfig) {
        if (hadOldConfig) SetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", oldConfigBuffer.data());
        else SetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", nullptr);
    }

    if (!created) {
        CloseHandle(readyEvent);
        std::wcerr << L"CreateProcess failed: " << GetLastError() << L"\n";
        return 8;
    }
    if (!inject_dll(process.hProcess, dllPath.wstring())) {
        const DWORD error = GetLastError();
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(readyEvent);
        std::wcerr << L"Could not inject MangoHud.dll (Win32 error " << error << L").\n";
        return 9;
    }
    const DWORD ready = WaitForSingleObject(readyEvent, 20000);
    CloseHandle(readyEvent);
    if (ready != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        std::wcerr << L"MangoHud DLL loaded but its OpenGL hook did not report ready.\n";
        return 10;
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::wcout << L"OpenGL hook installed; game started.\n";
    return 0;
}
'@

Write-Utf8File 'src/win/opengl_hook.cpp' $hook
Write-Utf8File 'src/win/opengl_hook.h' $header
Write-Utf8File 'src/win/opengl_bootstrap.cpp' $bootstrap
Write-Utf8File 'src/app/mangohud_launcher.cpp' $launcher
Write-Host 'OpenGL WGL hook and launcher applied.'


# Ensure the experimental package actually contains the launcher needed to inject
# MangoHud.dll into a normal OpenGL executable.
$packageScriptPath = Join-Path $root 'dev/package_windows.ps1'
$packageScript = [System.IO.File]::ReadAllText($packageScriptPath)

$requiredDll = '    (Join-Path $buildOut ''MangoHud.dll''),'
$requiredLauncher = '    (Join-Path $buildOut ''MangoHudLauncher.exe''),'
if (-not $packageScript.Contains($requiredLauncher)) {
    if (-not $packageScript.Contains($requiredDll)) {
        throw 'Could not find the required-binaries list in dev/package_windows.ps1'
    }
    $packageScript = $packageScript.Replace($requiredDll, $requiredDll + $nl + $requiredLauncher)
}

$copyDll = 'Copy-Item -LiteralPath (Join-Path $buildOut ''MangoHud.dll'') -Destination (Join-Path $packageRoot ''bin\MangoHud.dll'')'
$copyLauncher = 'Copy-Item -LiteralPath (Join-Path $buildOut ''MangoHudLauncher.exe'') -Destination (Join-Path $packageRoot ''bin\MangoHudLauncher.exe'')'
if (-not $packageScript.Contains($copyLauncher)) {
    if (-not $packageScript.Contains($copyDll)) {
        throw 'Could not find the MangoHud.dll package-copy command'
    }
    $packageScript = $packageScript.Replace($copyDll, $copyDll + $nl + $copyLauncher)
}

$readmeDll = '- bin\MangoHud.dll: Vulkan layer DLL.'
$readmeLauncher = '- bin\MangoHudLauncher.exe: starts an executable suspended, injects MangoHud.dll, then resumes it for OpenGL testing.'
if (-not $packageScript.Contains($readmeLauncher)) {
    if (-not $packageScript.Contains($readmeDll)) {
        throw 'Could not find the package README binary list'
    }
    $packageScript = $packageScript.Replace($readmeDll, $readmeDll + $nl + $readmeLauncher)
}

$inventoryDll = '    (Join-Path $packageRoot ''bin\MangoHud.dll''),'
$inventoryLauncher = '    (Join-Path $packageRoot ''bin\MangoHudLauncher.exe''),'
if (-not $packageScript.Contains($inventoryLauncher)) {
    if (-not $packageScript.Contains($inventoryDll)) {
        throw 'Could not find the package binary inventory list'
    }
    $packageScript = $packageScript.Replace($inventoryDll, $inventoryDll + $nl + $inventoryLauncher)
}

[System.IO.File]::WriteAllText($packageScriptPath, $packageScript, [System.Text.UTF8Encoding]::new($false))
Write-Host 'OpenGL launcher added to required package files, readme, and binary inventory.'


# Keep the experimental package documentation aligned with the included WGL hook.
$openGlTestMarker = 'VULKAN TEST (does not write to the Windows Registry)'
$openGlTestSection = @'
OPENGL TEST (x64 test applications only; use a disposable application)
1. Open PowerShell in the package folder.
2. Run: .\bin\MangoHudLauncher.exe "C:\Games\OpenGLTest\OpenGLTest.exe"
3. The launcher starts the game suspended, injects bin\MangoHud.dll, waits for the WGL hook, then starts the game.
Do not use this experimental injector with protected multiplayer or anti-cheat games.

'@
if (-not $packageScript.Contains('OPENGL TEST (x64 test applications only')) {
    if (-not $packageScript.Contains($openGlTestMarker)) {
        throw 'Could not find the README Vulkan test section'
    }
    $packageScript = $packageScript.Replace($openGlTestMarker, $openGlTestSection + $openGlTestMarker)
}

$oldSupport = 'OpenGL: not ported in this snapshot.'
$newSupport = 'OpenGL: WGL SwapBuffers hook and launcher built; runtime validation pending.'
if ($packageScript.Contains($oldSupport)) {
    $packageScript = $packageScript.Replace($oldSupport, $newSupport)
}

$oldApiList = "    GraphicsAPIs = @('Vulkan build target', 'Direct3D 11 build target', 'Direct3D 12 MSVC build target')"
$newApiList = "    GraphicsAPIs = @('Vulkan build target', 'Direct3D 11 build target', 'Direct3D 12 MSVC build target', 'OpenGL WGL hook build target')"
if ($packageScript.Contains($oldApiList)) {
    $packageScript = $packageScript.Replace($oldApiList, $newApiList)
}
$oldOpenGlMeta = "    OpenGL = 'Not ported'"
$newOpenGlMeta = "    OpenGL = 'WGL SwapBuffers hook and launcher built; runtime validation pending'"
if ($packageScript.Contains($oldOpenGlMeta)) {
    $packageScript = $packageScript.Replace($oldOpenGlMeta, $newOpenGlMeta)
}

[System.IO.File]::WriteAllText($packageScriptPath, $packageScript, [System.Text.UTF8Encoding]::new($false))
Write-Host 'OpenGL test instructions and build metadata updated.'
