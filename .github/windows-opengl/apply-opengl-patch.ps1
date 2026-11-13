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

# Add Win32 shell notification support for the tray icon.
if (-not $srcMeson.Contains('dep_shell32 =')) {
    $shellAnchor = "dep_advapi32 = cc.find_library('advapi32', required: true)"
    if (-not $srcMeson.Contains($shellAnchor)) { throw 'Windows advapi32 declaration anchor missing.' }
    $srcMeson = $srcMeson.Replace($shellAnchor, $shellAnchor + $nl + "dep_shell32 = cc.find_library('shell32', required: true)")
}
$controlTargetAnchor = 'mangohud_overlay = executable('
if (-not $srcMeson.Contains('mangohud_control = executable(')) {
    if (-not $srcMeson.Contains($controlTargetAnchor)) { throw 'MangoHud overlay executable anchor missing.' }
    $controlTarget = @'
# Native GUI control panel with process targeting and a notification-area icon.
mangohud_control = executable(
  'MangoHudControl',
  files('app/mangohud_control.cpp'),
  gui_app: true,
  dependencies: [dep_dwmapi, dep_shell32],
  install: true,
)

'@
    $srcMeson = $srcMeson.Replace($controlTargetAnchor, $controlTarget + $controlTargetAnchor)
}

[System.IO.File]::WriteAllText($srcPath, $srcMeson, [System.Text.UTF8Encoding]::new($false))


# Exclude common browsers from fallback foreground-window selection.
$overlaySourcePath = Join-Path $root 'src/app/mangohud_overlay.cpp'
$overlaySource = [System.IO.File]::ReadAllText($overlaySourcePath)
$browserAnchor = 'proc == "Code.exe"'
$browserReplacement = 'proc == "Code.exe" || proc == "firefox.exe" || proc == "chrome.exe" || proc == "msedge.exe" || proc == "brave.exe" || proc == "opera.exe"'
if (-not $overlaySource.Contains('proc == "firefox.exe"')) {
    if (-not $overlaySource.Contains($browserAnchor)) { throw 'Browser exclusion anchor missing in mangohud_overlay.cpp.' }
    $overlaySource = $overlaySource.Replace($browserAnchor, $browserReplacement)
    [System.IO.File]::WriteAllText($overlaySourcePath, $overlaySource, [System.Text.UTF8Encoding]::new($false))
}

$oldEtwNotice = 'SPDLOG_WARN("ETW FPS tracing unavailable (need admin). FPS will not be shown.");'
$newEtwNotice = 'SPDLOG_INFO("ETW FPS tracing unavailable without elevation; overlay continues without ETW frame data.");'
if ($overlaySource.Contains($oldEtwNotice)) {
    $overlaySource = $overlaySource.Replace($oldEtwNotice, $newEtwNotice)
    [System.IO.File]::WriteAllText($overlaySourcePath, $overlaySource, [System.Text.UTF8Encoding]::new($false))
}


$etwSourcePath = Join-Path $root 'src/app/fps_etw.cpp'
$etwSource = [System.IO.File]::ReadAllText($etwSourcePath)
$oldEtwError = 'SPDLOG_ERROR("ETW: StartTrace failed (error {}). Need admin privileges.", status);'
$newEtwWarning = 'SPDLOG_WARN("ETW FPS tracing unavailable without elevation (StartTrace error {}). The overlay remains active.", status);'
if ($etwSource.Contains($oldEtwError)) {
    $etwSource = $etwSource.Replace($oldEtwError, $newEtwWarning)
    [System.IO.File]::WriteAllText($etwSourcePath, $etwSource, [System.Text.UTF8Encoding]::new($false))
}

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

$controlTemplatePath = Join-Path $env:GITHUB_WORKSPACE '.github\windows-installer\MangoHudControl.cpp'
if (-not (Test-Path -LiteralPath $controlTemplatePath)) {
    throw "MangoHud control-panel source not found: $controlTemplatePath"
}
$controlSource = [System.IO.File]::ReadAllText($controlTemplatePath)
Write-Utf8File 'src/app/mangohud_control.cpp' $controlSource
Write-Host 'OpenGL WGL hook, launcher, and control panel sources applied.'


# Ensure the experimental package contains all files needed by the control panel.
$packageScriptPath = Join-Path $root 'dev/package_windows.ps1'
$packageScript = [System.IO.File]::ReadAllText($packageScriptPath)

$requiredDll = '    (Join-Path $buildOut ''MangoHud.dll''),'
$requiredLauncher = '    (Join-Path $buildOut ''MangoHudLauncher.exe''),'
$requiredControl = '    (Join-Path $buildOut ''MangoHudControl.exe''),'
if (-not $packageScript.Contains($requiredLauncher)) {
    if (-not $packageScript.Contains($requiredDll)) { throw 'Required-binaries list anchor missing.' }
    $packageScript = $packageScript.Replace($requiredDll, $requiredDll + $nl + $requiredLauncher)
}
if (-not $packageScript.Contains($requiredControl)) {
    if (-not $packageScript.Contains($requiredLauncher)) { throw 'MangoHudLauncher required-binary entry missing.' }
    $packageScript = $packageScript.Replace($requiredLauncher, $requiredLauncher + $nl + $requiredControl)
}

$copyControl = 'Copy-Item -LiteralPath (Join-Path $buildOut ''MangoHudControl.exe'') -Destination (Join-Path $packageRoot ''MangoHudControl.exe'')'
if (-not $packageScript.Contains($copyControl)) {
    $inventoryMarker = '$binaryPaths = @('
    $inventoryIndex = $packageScript.IndexOf($inventoryMarker)
    if ($inventoryIndex -lt 0) { throw 'Package binary inventory section missing.' }
    $packageScript = $packageScript.Insert($inventoryIndex, $copyControl + $nl + $nl)
}

$readmeControl = @'
CONTROL PANEL (normal use)
1. Start MangoHudControl.exe from the Start menu or installation folder.
2. The default process is Firestorm-Releasex64.exe. Change it to another .exe name when needed.
3. Minimize to the notification area to keep the overlay active without leaving the panel on screen.
4. Double-click the tray icon to reopen the panel. Right-click it and choose Close MangoHud to stop the overlay and exit.

'@
if (-not $packageScript.Contains('CONTROL PANEL (normal use)')) {
    $readmeMarker = 'OPENGL TEST'
    $readmeIndex = $packageScript.IndexOf($readmeMarker)
    if ($readmeIndex -lt 0) { $readmeIndex = $packageScript.IndexOf('VULKAN TEST') }
    if ($readmeIndex -lt 0) { throw 'Package README test-section marker missing.' }
    $packageScript = $packageScript.Insert($readmeIndex, $readmeControl + $nl)
}

$inventoryDll = '    (Join-Path $packageRoot ''bin\MangoHud.dll''),'
$inventoryLauncher = '    (Join-Path $packageRoot ''bin\MangoHudLauncher.exe''),'
$inventoryLauncherNoComma = '    (Join-Path $packageRoot ''bin\MangoHudLauncher.exe'')'
$inventoryControl = '    (Join-Path $packageRoot ''MangoHudControl.exe'')'
if (-not $packageScript.Contains($inventoryControl)) {
    if ($packageScript.Contains($inventoryLauncher)) {
        $packageScript = $packageScript.Replace($inventoryLauncher, $inventoryLauncher + $nl + $inventoryControl)
    } elseif ($packageScript.Contains($inventoryLauncherNoComma)) {
        $packageScript = $packageScript.Replace($inventoryLauncherNoComma, $inventoryLauncherNoComma + ',' + $nl + $inventoryControl)
    } elseif ($packageScript.Contains($inventoryDll)) {
        $packageScript = $packageScript.Replace($inventoryDll, $inventoryDll + $nl + $inventoryLauncher + $nl + $inventoryControl)
    } else {
        throw 'Package binary inventory insertion point missing.'
    }
}

$controlDocs = @'
CONTROL PANEL (normal use)
1. Start MangoHudControl.exe from the Start menu or installation folder.
2. The default process is Firestorm-Releasex64.exe. Change it to another .exe name when needed.
3. Minimize to the notification area to keep the overlay active without leaving the panel on screen.
4. Double-click the tray icon to reopen the panel. Right-click it and choose Close MangoHud to stop the overlay and exit.

'@
$vulkanMarker = 'VULKAN TEST (does not write to the Windows Registry)'
if (-not $packageScript.Contains('CONTROL PANEL (normal use)')) {
    if (-not $packageScript.Contains($vulkanMarker)) { throw 'Package README Vulkan section marker missing.' }
    $packageScript = $packageScript.Replace($vulkanMarker, $controlDocs + $vulkanMarker)
}

$oldSupport = 'OpenGL: not ported in this snapshot.'
$newSupport = 'OpenGL: WGL SwapBuffers hook and launcher built; runtime validation pending.'
if ($packageScript.Contains($oldSupport)) {
    $packageScript = $packageScript.Replace($oldSupport, $newSupport)
}
$oldApiList = "    GraphicsAPIs = @('Vulkan build target', 'Direct3D 11 build target', 'Direct3D 12 MSVC build target')"
$newApiList = "    GraphicsAPIs = @('Vulkan build target', 'Direct3D 11 build target', 'Direct3D 12 MSVC build target', 'OpenGL WGL hook build target', 'Windows control panel')"
if ($packageScript.Contains($oldApiList)) {
    $packageScript = $packageScript.Replace($oldApiList, $newApiList)
}

$metadataAnchor = '    RuntimeValidated = $false'
$metadataControl = "    ControlPanel = 'Firestorm default target; process selection; minimize-to-tray; stop and exit'"
if (-not $packageScript.Contains('ControlPanel =')) {
    if (-not $packageScript.Contains($metadataAnchor)) { throw 'Build metadata RuntimeValidated marker missing.' }
    $packageScript = $packageScript.Replace($metadataAnchor, $metadataAnchor + $nl + $metadataControl)
}

[System.IO.File]::WriteAllText($packageScriptPath, $packageScript, [System.Text.UTF8Encoding]::new($false))
Write-Host 'Windows control panel added to package, inventory, and README.'


# MANGOHUD_CONFIG is an inline option string, not a filename. Vulkan's test
# launcher uses MANGOHUD_CONFIGFILE for the installed config file; unset the
# inline variable for the child process so parse_overlay_config reads it.
$vulkanTestPath = Join-Path $root 'dev/Run-Vulkan-Test.ps1'
$vulkanTest = [System.IO.File]::ReadAllText($vulkanTestPath)
$wrongConfigAssignment = '    $env:MANGOHUD_CONFIG = $configPath'
$correctConfigAssignment = '    Remove-Item Env:MANGOHUD_CONFIG -ErrorAction SilentlyContinue'
if ($vulkanTest.Contains($wrongConfigAssignment)) {
    $vulkanTest = $vulkanTest.Replace($wrongConfigAssignment, $correctConfigAssignment)
    [System.IO.File]::WriteAllText($vulkanTestPath, $vulkanTest, [System.Text.UTF8Encoding]::new($false))
}
Write-Host 'Vulkan test script now uses MANGOHUD_CONFIGFILE as a file path.'
