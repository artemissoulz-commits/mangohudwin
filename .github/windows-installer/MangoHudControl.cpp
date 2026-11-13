#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <iterator>
#include <utility>

namespace {
constexpr wchar_t kClass[] = L"MangoHudControl";
constexpr UINT kTrayMessage = WM_APP + 7;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kExitMessage = WM_APP + 8;
constexpr int ID_ENABLED = 1103, ID_STATUS = 1104, ID_SKIN = 1105;
constexpr int MENU_OPEN = 1, MENU_TOGGLE = 2, MENU_EXIT = 3;

struct AppWindow {
    DWORD pid{};
    std::wstring exe;
    std::wstring title;
};

HWND g_main{}, g_enabledCheck{}, g_status{}, g_skinCombo{};
HANDLE g_overlay{};
HANDLE g_overlayEvent{};
HANDLE g_dxgiOverlayEvent{};
HANDLE g_skinReloadEvent{};
DWORD g_skinReloadPid{};
DWORD g_overlayPid{}, g_targetPid{}, g_lastAttemptPid{};
ULONGLONG g_lastAttemptTick{};
std::wstring g_targetExe, g_targetTitle;
NOTIFYICONDATAW g_nid{};
bool g_tray{}, g_enabled{true};
HFONT g_titleFont{}, g_font{}, g_smallFont{};
HBRUSH g_bgBrush{};
HBRUSH g_cardBrush{};
HICON g_appIcon{};
COLORREF g_statusColor = RGB(87, 105, 128);

std::wstring app_dir() {
    std::vector<wchar_t> b(32768);
    DWORD n = GetModuleFileNameW(nullptr, b.data(), static_cast<DWORD>(b.size()));
    if (!n || n >= b.size()) return L".";
    std::wstring p(b.data(), n);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

std::wstring quote(const std::wstring& s) {
    if (s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : s) {
        if (c == L'\\') ++bs;
        else if (c == L'"') { r.append(bs * 2 + 1, L'\\'); r += L'"'; bs = 0; }
        else { r.append(bs, L'\\'); bs = 0; r += c; }
    }
    r.append(bs * 2, L'\\');
    r += L'"';
    return r;
}

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

std::wstring basename(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

bool ignored_process(const std::wstring& exeName) {
    const std::wstring e = lower(exeName);
    static const wchar_t* ignored[] = {
        L"mangohud.exe", L"mangohudcontrol.exe", L"mangohudlauncher.exe",
        L"explorer.exe", L"firefox.exe", L"chrome.exe", L"msedge.exe",
        L"brave.exe", L"opera.exe", L"vivaldi.exe", L"waterfox.exe",
        L"discord.exe", L"steam.exe", L"steamwebhelper.exe",
        L"epicgameslauncher.exe", L"battle.net.exe", L"obs64.exe",
        L"taskmgr.exe", L"applicationframehost.exe", L"shellexperiencehost.exe",
        L"searchhost.exe", L"startmenuexperiencehost.exe", L"textinputhost.exe",
        L"runtimebroker.exe", L"systemsettings.exe", L"widgets.exe",
        L"lockapp.exe", L"dwm.exe", L"ctfmon.exe"
    };
    for (const wchar_t* item : ignored) if (e == item) return true;
    return e.find(L"mangohud") != std::wstring::npos;
}

bool get_foreground_app(AppWindow& app) {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    if (!pid || pid == GetCurrentProcessId()) return false;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;

    std::vector<wchar_t> image(32768);
    DWORD length = static_cast<DWORD>(image.size());
    const BOOL gotImage = QueryFullProcessImageNameW(process, 0, image.data(), &length);
    CloseHandle(process);
    if (!gotImage || !length) return false;

    const std::wstring exe = basename(std::wstring(image.data(), length));
    if (ignored_process(exe)) return false;

    wchar_t title[512]{};
    GetWindowTextW(foreground, title, static_cast<int>(sizeof(title) / sizeof(title[0])));
    app.pid = pid;
    app.exe = exe;
    app.title = title;
    return true;
}

void set_status(const std::wstring& message) {
    if (message.find(L"HUD conectado") != std::wstring::npos ||
        message.find(L"HUD activo") != std::wstring::npos) {
        g_statusColor = RGB(30, 145, 91);
    } else if (message.find(L"error") != std::wstring::npos ||
               message.find(L"No se pudo") != std::wstring::npos ||
               message.find(L"Falta ") != std::wstring::npos ||
               message.find(L"no confirmó") != std::wstring::npos ||
               message.find(L"No se encuentra") != std::wstring::npos) {
        g_statusColor = RGB(190, 72, 65);
    } else {
        g_statusColor = RGB(87, 105, 128);
    }
    if (g_status) SetWindowTextW(g_status, message.c_str());
    if (g_main) InvalidateRect(g_main, nullptr, FALSE);
}

void close_overlay_handles() {
    if (g_skinReloadEvent) {
        CloseHandle(g_skinReloadEvent);
        g_skinReloadEvent = nullptr;
        g_skinReloadPid = 0;
    }
    if (g_overlayEvent) {
        ResetEvent(g_overlayEvent);
        CloseHandle(g_overlayEvent);
        g_overlayEvent = nullptr;
    }
    if (g_dxgiOverlayEvent) {
        ResetEvent(g_dxgiOverlayEvent);
        CloseHandle(g_dxgiOverlayEvent);
        g_dxgiOverlayEvent = nullptr;
    }
    if (g_overlay) {
        CloseHandle(g_overlay);
        g_overlay = nullptr;
    }
    g_overlayPid = 0;
}

void stop_overlay(bool announce = true) {
    // This is a handle to the selected app, not a helper process.
    // Never close or terminate Firestorm: reset its event to hide the HUD.
    close_overlay_handles();
    if (announce && g_enabled) set_status(L"HUD pausado");
}

void pause_overlay() {
    if (g_overlayEvent) ResetEvent(g_overlayEvent);
    if (g_dxgiOverlayEvent) ResetEvent(g_dxgiOverlayEvent);
    if (g_enabled) set_status(L"HUD pausado");
}

bool is_x64_process(HANDLE process) {
    using IsWow64Process2Fn = BOOL (WINAPI *)(HANDLE, USHORT*, USHORT*);
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto isWow64Process2 = kernel32
        ? reinterpret_cast<IsWow64Process2Fn>(GetProcAddress(kernel32, "IsWow64Process2"))
        : nullptr;

    if (isWow64Process2) {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (!isWow64Process2(process, &processMachine, &nativeMachine))
            return false;
        return nativeMachine == IMAGE_FILE_MACHINE_AMD64 &&
            (processMachine == IMAGE_FILE_MACHINE_UNKNOWN ||
             processMachine == IMAGE_FILE_MACHINE_AMD64);
    }

    BOOL wow64 = FALSE;
    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);
    return systemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 &&
        IsWow64Process(process, &wow64) && !wow64;
}

HANDLE create_module_snapshot(DWORD pid) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        HANDLE snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot != INVALID_HANDLE_VALUE) return snapshot;
        if (GetLastError() != ERROR_BAD_LENGTH) break;
        Sleep(50);
    }
    return INVALID_HANDLE_VALUE;
}

bool process_has_mangohud_module(DWORD pid, bool& querySucceeded) {
    querySucceeded = false;
    HANDLE snapshot = create_module_snapshot(pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    bool found = false;
    if (Module32FirstW(snapshot, &module)) {
        querySucceeded = true;
        do {
            if (_wcsicmp(module.szModule, L"MangoHud.dll") == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
    return found;
}

bool process_has_module(DWORD pid, const std::wstring& moduleName, bool& querySucceeded) {
    querySucceeded = false;
    HANDLE snapshot = create_module_snapshot(pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    bool found = false;
    if (Module32FirstW(snapshot, &module)) {
        querySucceeded = true;
        do {
            if (_wcsicmp(module.szModule, moduleName.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
    return found;
}


bool process_has_supported_graphics_api(DWORD pid) {
    HANDLE snapshot = create_module_snapshot(pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    bool found = false;
    if (Module32FirstW(snapshot, &module)) {
        do {
            if (_wcsicmp(module.szModule, L"d3d11.dll") == 0 ||
                _wcsicmp(module.szModule, L"d3d12.dll") == 0 ||
                _wcsicmp(module.szModule, L"opengl32.dll") == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
    return found;
}

struct RunningAppSearch {
    AppWindow app{};
};

BOOL CALLBACK find_visible_graphics_app(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<RunningAppSearch*>(parameter);
    if (!search || !IsWindowVisible(window) || GetWindow(window, GW_OWNER))
        return TRUE;

    const LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extendedStyle & WS_EX_TOOLWINDOW) != 0)
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid || pid == GetCurrentProcessId())
        return TRUE;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return TRUE;

    std::vector<wchar_t> image(32768);
    DWORD length = static_cast<DWORD>(image.size());
    const BOOL gotImage = QueryFullProcessImageNameW(process, 0, image.data(), &length);
    if (!gotImage || !length) {
        CloseHandle(process);
        return TRUE;
    }

    const std::wstring exe = basename(std::wstring(image.data(), length));
    const bool eligibleProcess = !ignored_process(exe) && is_x64_process(process);
    CloseHandle(process);
    if (!eligibleProcess || !process_has_supported_graphics_api(pid))
        return TRUE;

    wchar_t title[512]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    search->app.pid = pid;
    search->app.exe = exe;
    search->app.title = title;
    // EnumWindows follows top-level z-order, so the first compatible window
    // is the most recently active visible game/application behind Control.
    return FALSE;
}

bool get_best_running_app(AppWindow& app) {
    AppWindow foreground{};
    if (get_foreground_app(foreground) &&
        process_has_supported_graphics_api(foreground.pid)) {
        app = std::move(foreground);
        return true;
    }

    RunningAppSearch search{};
    EnumWindows(find_visible_graphics_app, reinterpret_cast<LPARAM>(&search));
    if (!search.app.pid)
        return false;

    app = std::move(search.app);
    return true;
}

bool find_remote_module(DWORD pid, const std::wstring& moduleName, uintptr_t& base) {
    HANDLE snapshot = create_module_snapshot(pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    bool found = false;
    if (Module32FirstW(snapshot, &module)) {
        do {
            if (_wcsicmp(module.szModule, moduleName.c_str()) == 0) {
                base = reinterpret_cast<uintptr_t>(module.modBaseAddr);
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
    return found;
}

bool get_remote_load_library(DWORD pid, LPTHREAD_START_ROUTINE& remoteLoadLibrary) {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC localAddress = kernel32 ? GetProcAddress(kernel32, "LoadLibraryW") : nullptr;
    if (!localAddress) return false;

    HMODULE ownerModule = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(localAddress), &ownerModule))
        return false;

    wchar_t ownerPath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(ownerModule, ownerPath, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;

    const std::wstring ownerName = basename(std::wstring(ownerPath, length));
    uintptr_t remoteBase = 0;
    if (!find_remote_module(pid, ownerName, remoteBase)) return false;

    const uintptr_t rva = reinterpret_cast<uintptr_t>(localAddress) -
        reinterpret_cast<uintptr_t>(ownerModule);
    remoteLoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteBase + rva);
    return true;
}

bool inject_mangohud_dll(HANDLE process, DWORD pid,
                         const std::wstring& dllPath, DWORD& errorCode) {
    LPTHREAD_START_ROUTINE remoteLoadLibrary = nullptr;
    if (!get_remote_load_library(pid, remoteLoadLibrary)) {
        errorCode = GetLastError();
        if (!errorCode) errorCode = ERROR_MOD_NOT_FOUND;
        return false;
    }

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, bytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        errorCode = GetLastError();
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, dllPath.c_str(), bytes, &written) ||
        written != bytes) {
        errorCode = GetLastError();
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    HANDLE remoteThread = CreateRemoteThread(process, nullptr, 0,
        remoteLoadLibrary, remotePath, 0, nullptr);
    if (!remoteThread) {
        errorCode = GetLastError();
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(remoteThread, 15000);
    DWORD moduleHandle = 0;
    const bool loaded = waitResult == WAIT_OBJECT_0 &&
        GetExitCodeThread(remoteThread, &moduleHandle) && moduleHandle != 0;
    if (!loaded)
        errorCode = waitResult == WAIT_TIMEOUT ? WAIT_TIMEOUT : ERROR_DLL_INIT_FAILED;

    CloseHandle(remoteThread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return loaded;
}

bool load_remote_library(HANDLE process, DWORD pid,
                         const std::wstring& libraryPath, DWORD& errorCode) {
    LPTHREAD_START_ROUTINE remoteLoadLibrary = nullptr;
    if (!get_remote_load_library(pid, remoteLoadLibrary)) {
        errorCode = GetLastError();
        if (!errorCode) errorCode = ERROR_MOD_NOT_FOUND;
        return false;
    }

    const SIZE_T bytes = (libraryPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, bytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        errorCode = GetLastError();
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, libraryPath.c_str(), bytes, &written) ||
        written != bytes) {
        errorCode = GetLastError();
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    HANDLE remoteThread = CreateRemoteThread(process, nullptr, 0,
        remoteLoadLibrary, remotePath, 0, nullptr);
    if (!remoteThread) {
        errorCode = GetLastError();
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(remoteThread, 15000);
    DWORD moduleHandle = 0;
    const bool loaded = waitResult == WAIT_OBJECT_0 &&
        GetExitCodeThread(remoteThread, &moduleHandle) && moduleHandle != 0;
    if (!loaded)
        errorCode = waitResult == WAIT_TIMEOUT ? WAIT_TIMEOUT : ERROR_DLL_INIT_FAILED;

    CloseHandle(remoteThread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return loaded;
}

bool call_remote_export(HANDLE process, DWORD pid, const std::wstring& libraryPath,
                        const char* exportName, DWORD& errorCode) {
    HMODULE localModule = LoadLibraryW(libraryPath.c_str());
    if (!localModule) {
        errorCode = GetLastError();
        return false;
    }

    FARPROC localExport = GetProcAddress(localModule, exportName);
    if (!localExport) {
        errorCode = GetLastError();
        FreeLibrary(localModule);
        return false;
    }

    uintptr_t remoteBase = 0;
    const std::wstring moduleName = basename(libraryPath);
    if (!find_remote_module(pid, moduleName, remoteBase)) {
        errorCode = GetLastError();
        FreeLibrary(localModule);
        return false;
    }

    const uintptr_t rva = reinterpret_cast<uintptr_t>(localExport) -
        reinterpret_cast<uintptr_t>(localModule);
    auto remoteStart = reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteBase + rva);

    HANDLE remoteThread = CreateRemoteThread(process, nullptr, 0,
        remoteStart, nullptr, 0, nullptr);
    if (!remoteThread) {
        errorCode = GetLastError();
        FreeLibrary(localModule);
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(remoteThread, 8000);
    DWORD returnValue = 0;
    const bool invoked = waitResult == WAIT_OBJECT_0 &&
        GetExitCodeThread(remoteThread, &returnValue) && returnValue != 0;
    if (!invoked)
        errorCode = waitResult == WAIT_TIMEOUT ? WAIT_TIMEOUT : ERROR_DLL_INIT_FAILED;

    CloseHandle(remoteThread);
    FreeLibrary(localModule);
    return invoked;
}

bool start_dxgi_overlay(HANDLE process, DWORD pid, bool useD3D12) {
    const std::wstring dxgiPath = app_dir() + L"\\bin\\MangoHudDXGI.dll";
    if (GetFileAttributesW(dxgiPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CloseHandle(process);
        set_status(L"Falta bin\\MangoHudDXGI.dll. Reinstala el paquete completo.");
        return false;
    }

    wchar_t enabledName[96]{}, readyName[96]{};
    swprintf_s(enabledName, L"Local\\MangoHudDXGIEnabled_%lu", pid);
    swprintf_s(readyName, L"Local\\MangoHudDXGIReady_%lu", pid);
    HANDLE enabledEvent = CreateEventW(nullptr, TRUE, FALSE, enabledName);
    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, readyName);
    if (!enabledEvent || !readyEvent) {
        const DWORD error = GetLastError();
        if (enabledEvent) CloseHandle(enabledEvent);
        if (readyEvent) CloseHandle(readyEvent);
        CloseHandle(process);
        set_status(L"No se pudo crear el canal Direct3D (error " + std::to_wstring(error) + L").");
        return false;
    }
    ResetEvent(enabledEvent);
    ResetEvent(readyEvent);

    bool proxyQuerySucceeded = false;
    const bool proxyAlreadyLoaded = process_has_module(pid, L"MangoHudDXGI.dll", proxyQuerySucceeded);
    if (!proxyQuerySucceeded) {
        const DWORD error = GetLastError();
        CloseHandle(readyEvent);
        CloseHandle(enabledEvent);
        CloseHandle(process);
        set_status(L"No se pudo consultar el proxy Direct3D (error " + std::to_wstring(error) + L").");
        return false;
    }

    DWORD dxgiError = ERROR_SUCCESS;
    if (!proxyAlreadyLoaded && !load_remote_library(process, pid, dxgiPath, dxgiError)) {
        CloseHandle(readyEvent);
        ResetEvent(enabledEvent);
        CloseHandle(enabledEvent);
        CloseHandle(process);
        set_status(L"No se pudo cargar MangoHudDXGI.dll (error " + std::to_wstring(dxgiError) + L").");
        return false;
    }

    if (!call_remote_export(process, pid, dxgiPath, "MangoHudInitializeDXGIHooks", dxgiError)) {
        CloseHandle(readyEvent);
        ResetEvent(enabledEvent);
        CloseHandle(enabledEvent);
        CloseHandle(process);
        set_status(L"No se pudieron iniciar los hooks Direct3D (error " + std::to_wstring(dxgiError) + L").");
        return false;
    }

    const DWORD ready = WaitForSingleObject(readyEvent, 15000);
    CloseHandle(readyEvent);
    if (ready != WAIT_OBJECT_0) {
        const DWORD error = ready == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError();
        ResetEvent(enabledEvent);
        CloseHandle(enabledEvent);
        CloseHandle(process);
        set_status(L"El proxy DXGI se cargó, pero D3D11/D3D12 no confirmó el hook (error " +
            std::to_wstring(error) + L").");
        return false;
    }

    if (!SetEvent(enabledEvent)) {
        const DWORD error = GetLastError();
        ResetEvent(enabledEvent);
        CloseHandle(enabledEvent);
        CloseHandle(process);
        set_status(L"No se pudo activar el HUD Direct3D (error " + std::to_wstring(error) + L").");
        return false;
    }

    g_overlay = process;
    g_dxgiOverlayEvent = enabledEvent;
    g_overlayPid = pid;
    const std::wstring api = useD3D12 ? L"Direct3D 12" : L"Direct3D 11";
    set_status(L"HUD conectado · " + api + L" · " +
        (g_targetTitle.empty() ? g_targetExe : g_targetTitle) + L" (PID " + std::to_wstring(pid) + L")");
    return true;
}

bool start_overlay() {
    if (!g_enabled) return false;
    if (!g_targetPid || g_targetExe.empty()) {
        set_status(L"Abre una aplicación compatible");
        return false;
    }

    g_lastAttemptPid = g_targetPid;
    g_lastAttemptTick = GetTickCount64();

    if (g_overlay && g_overlayPid == g_targetPid &&
        WaitForSingleObject(g_overlay, 0) == WAIT_TIMEOUT) {
        bool activated = false;
        if (g_overlayEvent) activated = (SetEvent(g_overlayEvent) != FALSE) || activated;
        if (g_dxgiOverlayEvent) activated = (SetEvent(g_dxgiOverlayEvent) != FALSE) || activated;
        if (activated) {
            set_status(L"HUD conectado · " + (g_targetTitle.empty() ? g_targetExe : g_targetTitle) +
                L" (PID " + std::to_wstring(g_targetPid) + L")");
            return true;
        }
    }

    if (g_overlay || g_overlayEvent || g_dxgiOverlayEvent)
        stop_overlay(false);

    constexpr DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE;
    HANDLE process = OpenProcess(access, FALSE, g_targetPid);
    if (!process) {
        const DWORD error = GetLastError();
        std::wstring message = L"No se pudo acceder al proceso (error " + std::to_wstring(error) + L").";
        if (error == ERROR_ACCESS_DENIED)
            message += L" Ejecuta Control con los mismos permisos que el juego.";
        set_status(message);
        return false;
    }

    if (!is_x64_process(process)) {
        CloseHandle(process);
        set_status(L"MangoHud requiere que la aplicación sea x64.");
        return false;
    }

    bool d3d11QuerySucceeded = false, d3d12QuerySucceeded = false, openGLQuerySucceeded = false;
    const bool hasD3D11 = process_has_module(g_targetPid, L"d3d11.dll", d3d11QuerySucceeded);
    const bool hasD3D12 = process_has_module(g_targetPid, L"d3d12.dll", d3d12QuerySucceeded);
    const bool hasOpenGL = process_has_module(g_targetPid, L"opengl32.dll", openGLQuerySucceeded);
    if (!d3d11QuerySucceeded || !d3d12QuerySucceeded || !openGLQuerySucceeded) {
        const DWORD error = GetLastError();
        CloseHandle(process);
        set_status(L"No se pudo identificar la API gráfica (error " + std::to_wstring(error) + L").");
        return false;
    }

    if (hasD3D12 || hasD3D11)
        return start_dxgi_overlay(process, g_targetPid, hasD3D12);

    if (!hasOpenGL) {
        CloseHandle(process);
        set_status(L"Esperando a que la aplicación inicialice OpenGL o Direct3D…");
        return false;
    }

    const std::wstring dllPath = app_dir() + L"\\bin\\MangoHud.dll";
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CloseHandle(process);
        set_status(L"No se encuentra bin\\MangoHud.dll en la instalación.");
        return false;
    }

    wchar_t enabledName[96]{}, readyName[96]{}, loadedName[96]{};
    swprintf_s(enabledName, L"Local\\MangoHudOpenGLEnabled_%lu", g_targetPid);
    swprintf_s(readyName, L"Local\\MangoHudOpenGLReady_%lu", g_targetPid);
    swprintf_s(loadedName, L"Local\\MangoHudOpenGLLoaded_%lu", g_targetPid);

    HANDLE enabledEvent = CreateEventW(nullptr, TRUE, FALSE, enabledName);
    if (!enabledEvent) {
        const DWORD error = GetLastError();
        CloseHandle(process);
        set_status(L"No se pudo crear el canal OpenGL (error " + std::to_wstring(error) + L").");
        return false;
    }
    ResetEvent(enabledEvent);

    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, readyName);
    if (!readyEvent) {
        const DWORD error = GetLastError();
        CloseHandle(enabledEvent);
        CloseHandle(process);
        set_status(L"No se pudo preparar la señal OpenGL (error " + std::to_wstring(error) + L").");
        return false;
    }
    ResetEvent(readyEvent);

    bool moduleQuerySucceeded = false;
    const bool moduleLoaded = process_has_mangohud_module(g_targetPid, moduleQuerySucceeded);
    if (!moduleQuerySucceeded) {
        CloseHandle(readyEvent);
        ResetEvent(enabledEvent);
        CloseHandle(enabledEvent);
        CloseHandle(process);
        set_status(L"No se pudo consultar la lista de módulos (error " + std::to_wstring(GetLastError()) + L").");
        return false;
    }

    DWORD injectionError = ERROR_SUCCESS;
    if (moduleLoaded) {
        HANDLE loadedEvent = OpenEventW(SYNCHRONIZE, FALSE, loadedName);
        const bool compatible = loadedEvent && WaitForSingleObject(loadedEvent, 0) == WAIT_OBJECT_0;
        if (loadedEvent) CloseHandle(loadedEvent);
        if (!compatible) {
            CloseHandle(readyEvent);
            ResetEvent(enabledEvent);
            CloseHandle(enabledEvent);
            CloseHandle(process);
            set_status(L"El proceso ya tiene una MangoHud.dll antigua. Ciérralo por completo y vuelve a abrirlo tras actualizar.");
            return false;
        }
    } else {
        if (!inject_mangohud_dll(process, g_targetPid, dllPath, injectionError)) {
            CloseHandle(readyEvent);
            ResetEvent(enabledEvent);
            CloseHandle(enabledEvent);
            CloseHandle(process);
            set_status(L"No se pudo inyectar MangoHud.dll (error " + std::to_wstring(injectionError) + L").");
            return false;
        }

        const DWORD ready = WaitForSingleObject(readyEvent, 20000);
        if (ready != WAIT_OBJECT_0) {
            const DWORD error = ready == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError();
            CloseHandle(readyEvent);
            ResetEvent(enabledEvent);
            CloseHandle(enabledEvent);
            CloseHandle(process);
            set_status(L"El hook OpenGL no confirmó su inicio (error " + std::to_wstring(error) + L").");
            return false;
        }
    }
    CloseHandle(readyEvent);

    HANDLE loadedEvent = OpenEventW(SYNCHRONIZE, FALSE, loadedName);
    const bool hookReady = loadedEvent && WaitForSingleObject(loadedEvent, 0) == WAIT_OBJECT_0;
    if (loadedEvent) CloseHandle(loadedEvent);
    if (!hookReady) {
        ResetEvent(enabledEvent);
        CloseHandle(enabledEvent);
        CloseHandle(process);
        set_status(L"MangoHud.dll no confirmó el protocolo de Control. Reinicia la aplicación antes de volver a probar.");
        return false;
    }

    if (!SetEvent(enabledEvent)) {
        const DWORD error = GetLastError();
        ResetEvent(enabledEvent);
        CloseHandle(enabledEvent);
        CloseHandle(process);
        set_status(L"No se pudo activar el HUD OpenGL (error " + std::to_wstring(error) + L").");
        return false;
    }

    g_overlay = process;
    g_overlayEvent = enabledEvent;
    g_overlayPid = g_targetPid;
    set_status(L"HUD conectado · OpenGL · " +
        (g_targetTitle.empty() ? g_targetExe : g_targetTitle) +
        L" (PID " + std::to_wstring(g_targetPid) + L")");
    return true;
}

void set_target(const AppWindow& app) {
    const bool changed = app.pid != g_targetPid;
    g_targetPid = app.pid;
    g_targetExe = app.exe;
    g_targetTitle = app.title;

    if (changed) {
        set_status(L"Conectando con " + g_targetExe + L"…");
        if (g_enabled) start_overlay();
    }
}

void refresh_apps() {
    if (!g_enabled) return;

    AppWindow foreground{};
    if (!get_best_running_app(foreground)) return;

    if (foreground.pid != g_targetPid) {
        set_target(foreground);
        return;
    }

    // A failed first injection must not leave the HUD permanently detached.
    // Retry the same foreground process every 4 seconds without reinjecting
    // while a live process handle and its PID control event are already held.
    const bool hasApiEvent = g_overlayEvent || g_dxgiOverlayEvent;
    const bool disconnected = !g_overlay || !hasApiEvent ||
        WaitForSingleObject(g_overlay, 0) != WAIT_TIMEOUT;
    const bool retryWindowElapsed = g_lastAttemptPid != foreground.pid ||
        GetTickCount64() - g_lastAttemptTick >= 4000;
    if (disconnected && retryWindowElapsed)
        start_overlay();
}

void add_tray() {
    if (g_tray || !g_main) return;
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = kTrayMessage;
    g_nid.hIcon = g_appIcon ? g_appIcon : LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wcscpy_s(g_nid.szTip, L"MangoHud · seguimiento automático");
    g_tray = Shell_NotifyIconW(NIM_ADD, &g_nid) == TRUE;
}

void remove_tray() {
    if (g_tray) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_tray = false;
    }
}

void show_panel() {
    if (!g_main) return;
    ShowWindow(g_main, SW_SHOW);
    ShowWindow(g_main, SW_RESTORE);
    SetForegroundWindow(g_main);
}

void hide_to_tray() {
    add_tray();
    ShowWindow(g_main, SW_HIDE);
}

void tray_menu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MENU_OPEN, L"Abrir MangoHud");
    AppendMenuW(menu, MF_STRING, MENU_TOGGLE, g_enabled ? L"Pausar HUD" : L"Reanudar HUD");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MENU_EXIT, L"Salir completamente");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(g_main);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, point.x, point.y, 0, g_main, nullptr);
    DestroyMenu(menu);
}

void toggle_enabled() {
    g_enabled = !g_enabled;
    if (g_enabled) {
        if (g_targetPid && !g_targetExe.empty()) start_overlay();
        else set_status(L"Esperando una aplicación compatible");
    } else {
        pause_overlay();
    }
    if (g_enabledCheck) SendMessageW(g_enabledCheck, BM_SETCHECK, g_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
}

struct SkinChoice {
    const wchar_t* name;
    const wchar_t* file;
};
constexpr SkinChoice kSkins[] = {
    // Original Windows-port skins; Dark and Minimal intentionally stay unchanged.
    {L"Dark", L"Dark.conf"},
    {L"Mango", L"Mango.conf"},
    {L"Minimal", L"Minimal.conf"},
    {L"Glass", L"Glass.conf"},
    // Community palettes adapted from Catppuccin for MangoHud (MIT).
    {L"Catppuccin Latte", L"Catppuccin-Latte.conf"},
    {L"Catppuccin Frappe", L"Catppuccin-Frappe.conf"},
    {L"Catppuccin Macchiato", L"Catppuccin-Macchiato.conf"},
    {L"Catppuccin Mocha", L"Catppuccin-Mocha.conf"}
};

int load_skin_selection() {
    wchar_t selected[32]{};
    const std::wstring iniPath = app_dir() + L"\\skin.ini";
    GetPrivateProfileStringW(L"Skin", L"Name", L"Dark",
        selected, static_cast<DWORD>(std::size(selected)), iniPath.c_str());
    for (int i = 0; i < static_cast<int>(std::size(kSkins)); ++i) {
        if (_wcsicmp(selected, kSkins[i].name) == 0) return i;
    }
    return 0;
}

void request_skin_reload(DWORD pid) {
    if (!pid) return;
    if (g_skinReloadEvent && g_skinReloadPid != pid) {
        CloseHandle(g_skinReloadEvent);
        g_skinReloadEvent = nullptr;
        g_skinReloadPid = 0;
    }
    if (!g_skinReloadEvent) {
        wchar_t eventName[96]{};
        swprintf_s(eventName, L"Local\\MangoHudReloadConfig_%lu", pid);
        g_skinReloadEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
        if (!g_skinReloadEvent)
            g_skinReloadEvent = CreateEventW(nullptr, FALSE, FALSE, eventName);
        if (g_skinReloadEvent) g_skinReloadPid = pid;
    }
    // Keep our handle until the target has had time to open the event. This
    // avoids losing the first theme change immediately after connection.
    if (g_skinReloadEvent) SetEvent(g_skinReloadEvent);
}

bool apply_skin(int index) {
    if (index < 0 || index >= static_cast<int>(std::size(kSkins))) return false;
    const std::wstring root = app_dir();
    const std::wstring source = root + L"\\skins\\" + kSkins[index].file;
    const std::wstring destination = root + L"\\config\\MangoHud.conf";
    // Copy to a unique temporary file first, then replace the active config with
    // a same-directory rename. The game can therefore read either the old or
    // the new complete config, never a partially copied file.
    static LONG temporarySequence = 0;
    const std::wstring temporary = destination + L".tmp." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64()) + L"." +
        std::to_wstring(InterlockedIncrement(&temporarySequence));
    if (!CopyFileW(source.c_str(), temporary.c_str(), TRUE)) {
        const DWORD error = GetLastError();
        DeleteFileW(temporary.c_str());
        set_status(L"No se pudo preparar la skin (error " + std::to_wstring(error) + L").");
        return false;
    }

    bool replaced = false;
    DWORD replaceError = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            replaced = true;
            break;
        }
        replaceError = GetLastError();
        if (replaceError != ERROR_SHARING_VIOLATION &&
            replaceError != ERROR_ACCESS_DENIED)
            break;
        Sleep(20);
    }
    if (!replaced) {
        DeleteFileW(temporary.c_str());
        set_status(L"No se pudo instalar la skin (error " + std::to_wstring(replaceError) + L").");
        return false;
    }

    const std::wstring iniPath = root + L"\\skin.ini";
    WritePrivateProfileStringW(L"Skin", L"Name", kSkins[index].name, iniPath.c_str());

    if (g_overlayPid && g_overlay && WaitForSingleObject(g_overlay, 0) == WAIT_TIMEOUT) {
        request_skin_reload(g_overlayPid);
        set_status(L"Skin " + std::wstring(kSkins[index].name) + L" aplicada · recargando el HUD…");
    } else {
        set_status(L"Skin " + std::wstring(kSkins[index].name) + L" guardada · se aplicará al conectar");
    }
    return true;
}

void exit_application() {
    remove_tray();
    stop_overlay(false);
    if (g_main) DestroyWindow(g_main);
}

HWND make_child(const wchar_t* klass, const wchar_t* text, DWORD style,
    int x, int y, int width, int height, int id) {
    HWND control = CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, g_main,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return control;
}

LRESULT CALLBACK wndproc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_SKIN && HIWORD(wParam) == CBN_SELCHANGE) {
            const LRESULT selected = SendMessageW(g_skinCombo, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR) apply_skin(static_cast<int>(selected));
            return 0;
        }
        if (LOWORD(wParam) == ID_ENABLED && HIWORD(wParam) == BN_CLICKED) {
            g_enabled = SendMessageW(g_enabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (g_enabled) {
                if (g_targetPid && !g_targetExe.empty()) start_overlay();
                else refresh_apps();
            } else {
                pause_overlay();
            }
            return 0;
        }
        if (LOWORD(wParam) == MENU_OPEN) { show_panel(); return 0; }
        if (LOWORD(wParam) == MENU_TOGGLE) { toggle_enabled(); return 0; }
        if (LOWORD(wParam) == MENU_EXIT) { exit_application(); return 0; }
        break;

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        HWND child = reinterpret_cast<HWND>(lParam);
        const int controlId = GetDlgCtrlID(child);
        SetTextColor(dc, controlId == ID_STATUS ? g_statusColor : RGB(40, 49, 61));

        RECT childRect{};
        GetWindowRect(child, &childRect);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&childRect), 2);
        const bool insideStatusCard = childRect.top >= 116 && childRect.top < 226;
        return reinterpret_cast<LRESULT>(insideStatusCard ? g_cardBrush : g_bgBrush);
    }
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect, g_bgBrush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, g_bgBrush);

        HPEN dividerPen = CreatePen(PS_SOLID, 1, RGB(222, 227, 234));
        HGDIOBJ oldPen = SelectObject(dc, dividerPen);
        MoveToEx(dc, 24, 98, nullptr);
        LineTo(dc, client.right - 24, 98);
        SelectObject(dc, oldPen);
        DeleteObject(dividerPen);

        RECT card{24, 116, client.right - 24, 226};
        HGDIOBJ oldBrush = SelectObject(dc, g_cardBrush);
        HPEN cardPen = CreatePen(PS_SOLID, 1, RGB(229, 233, 239));
        oldPen = SelectObject(dc, cardPen);
        RoundRect(dc, card.left, card.top, card.right, card.bottom, 12, 12);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(cardPen);

        HBRUSH dotBrush = CreateSolidBrush(g_statusColor);
        oldBrush = SelectObject(dc, dotBrush);
        oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, 36, 164, 46, 174);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(dotBrush);

        if (g_appIcon) DrawIconEx(dc, 26, 18, g_appIcon, 64, 64, 0, nullptr, DI_NORMAL);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kTimerId) {
            if (g_overlay && WaitForSingleObject(g_overlay, 0) == WAIT_OBJECT_0) {
                stop_overlay(false);
                g_targetPid = 0;
                g_targetExe.clear();
                g_targetTitle.clear();
                set_status(L"La aplicación anterior se ha cerrado; buscando otra…");
            }
            refresh_apps();
        }
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) hide_to_tray();
        return 0;
    case kTrayMessage:
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) show_panel();
        else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) tray_menu();
        return 0;
    case WM_CLOSE:
        hide_to_tray();
        return 0;
    case WM_DESTROY:
        KillTimer(window, kTimerId);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\MangoHudControlMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND previous = FindWindowW(kClass, nullptr);
        if (previous) ShowWindow(previous, SW_RESTORE);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    g_bgBrush = CreateSolidBrush(RGB(243, 245, 248));
    g_cardBrush = CreateSolidBrush(RGB(255, 255, 255));
    g_titleFont = CreateFontW(-29, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    g_smallFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");

    const std::wstring iconPath = app_dir() + L"\\MangoHud.ico";
    // Request the 64x64 PNG frame directly; do not scale up a smaller system icon.
    g_appIcon = static_cast<HICON>(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON,
        64, 64, LR_LOADFROMFILE | LR_DEFAULTCOLOR));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = g_appIcon ? g_appIcon : LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = g_bgBrush;
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc)) return 1;

    g_main = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"MangoHud",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 420,
        nullptr, nullptr, instance, nullptr);
    if (!g_main) return 2;

    SendMessageW(g_main, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon));
    SendMessageW(g_main, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIcon));

    make_child(L"STATIC", L"MangoHud", SS_LEFT, 108, 26, 360, 38, 0);
    HWND subtitle = make_child(L"STATIC", L"MONITOR DE RENDIMIENTO", SS_LEFT, 110, 64, 360, 20, 0);
    if (subtitle) SendMessageW(subtitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_smallFont), TRUE);

    HWND statusCaption = make_child(L"STATIC", L"ESTADO DE CONEXIÓN", SS_LEFT, 54, 132, 410, 18, 0);
    if (statusCaption) SendMessageW(statusCaption, WM_SETFONT, reinterpret_cast<WPARAM>(g_smallFont), TRUE);
    g_status = make_child(L"STATIC", L"Esperando una aplicación compatible…", SS_LEFT, 54, 155, 410, 25, ID_STATUS);
    if (g_status) SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    HWND followHint = make_child(L"STATIC", L"El HUD seguirá automáticamente la aplicación en primer plano.",
        SS_LEFT, 54, 187, 410, 20, 0);
    if (followHint) SendMessageW(followHint, WM_SETFONT, reinterpret_cast<WPARAM>(g_smallFont), TRUE);

    HWND skinCaption = make_child(L"STATIC", L"APARIENCIA DEL HUD", SS_LEFT, 30, 242, 150, 20, 0);
    if (skinCaption) SendMessageW(skinCaption, WM_SETFONT, reinterpret_cast<WPARAM>(g_smallFont), TRUE);
    g_skinCombo = make_child(L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL,
        190, 236, 280, 170, ID_SKIN);
    if (g_skinCombo) {
        for (const auto& skin : kSkins)
            SendMessageW(g_skinCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(skin.name));
        SendMessageW(g_skinCombo, CB_SETCURSEL, load_skin_selection(), 0);
    }

    g_enabledCheck = make_child(L"BUTTON", L"HUD activado",
        BS_AUTOCHECKBOX | WS_TABSTOP, 30, 292, 450, 30, ID_ENABLED);
    HWND footer = make_child(L"STATIC", L"SEGUIMIENTO AUTOMÁTICO", SS_LEFT, 30, 330, 450, 18, 0);
    if (footer) SendMessageW(footer, WM_SETFONT, reinterpret_cast<WPARAM>(g_smallFont), TRUE);

    
    if (g_titleFont) {
        HWND title = FindWindowExW(g_main, nullptr, L"STATIC", L"MangoHud");
        if (title) SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_titleFont), TRUE);
    }
    SendMessageW(g_enabledCheck, BM_SETCHECK, BST_CHECKED, 0);

    SetTimer(g_main, kTimerId, 1200, nullptr);
    add_tray();
    refresh_apps();
    hide_to_tray();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_overlay) stop_overlay(false);
    if (g_titleFont) DeleteObject(g_titleFont);
    if (g_font) DeleteObject(g_font);
    if (g_smallFont) DeleteObject(g_smallFont);
    if (g_bgBrush) DeleteObject(g_bgBrush);
    if (g_cardBrush) DeleteObject(g_cardBrush);
    if (g_appIcon) DestroyIcon(g_appIcon);
    if (mutex) CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}