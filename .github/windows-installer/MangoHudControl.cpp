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
constexpr int ID_TARGET = 1101, ID_AUTO = 1102, ID_ENABLED = 1103, ID_STATUS = 1104;
constexpr int MENU_OPEN = 1, MENU_TOGGLE = 2, MENU_EXIT = 3;

struct AppWindow {
    DWORD pid{};
    std::wstring exe;
    std::wstring title;
};

HWND g_main{}, g_targetCombo{}, g_autoCheck{}, g_enabledCheck{}, g_status{};
HANDLE g_overlay{};
HANDLE g_overlayEvent{};
DWORD g_overlayPid{}, g_targetPid{};
std::wstring g_targetExe, g_targetTitle;
std::vector<AppWindow> g_apps;
NOTIFYICONDATAW g_nid{};
bool g_tray{}, g_autoSwitch{true}, g_enabled{true}, g_updatingCombo{};
HFONT g_titleFont{}, g_font{}, g_smallFont{};
HBRUSH g_bgBrush{};
HICON g_appIcon{};

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

BOOL CALLBACK collect_windows(HWND hwnd, LPARAM parameter) {
    if (!IsWindowVisible(hwnd) || (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
        return TRUE;

    wchar_t title[512]{};
    if (GetWindowTextW(hwnd, title, 512) <= 0)
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId())
        return TRUE;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return TRUE;

    std::vector<wchar_t> image(32768);
    DWORD length = static_cast<DWORD>(image.size());
    const BOOL gotImage = QueryFullProcessImageNameW(process, 0, image.data(), &length);
    CloseHandle(process);
    if (!gotImage || !length) return TRUE;

    const std::wstring exe = basename(std::wstring(image.data(), length));
    if (ignored_process(exe)) return TRUE;

    auto& found = *reinterpret_cast<std::vector<AppWindow>*>(parameter);
    for (auto& existing : found) {
        if (existing.pid == pid) {
            if (existing.title.empty()) existing.title = title;
            return TRUE;
        }
    }
    found.push_back({pid, exe, title});
    return TRUE;
}

void set_status(const std::wstring& message) {
    if (g_status) SetWindowTextW(g_status, message.c_str());
}

std::wstring target_label(const AppWindow& app) {
    std::wstring label = app.title.empty() ? app.exe : app.title;
    label += L"  (";
    label += app.exe;
    label += L")";
    return label;
}

void close_overlay_handles() {
    if (g_overlayEvent) {
        ResetEvent(g_overlayEvent);
        CloseHandle(g_overlayEvent);
        g_overlayEvent = nullptr;
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

bool start_overlay() {
    if (!g_enabled) return false;
    if (!g_targetPid || g_targetExe.empty()) {
        set_status(L"Abre Firestorm u otra aplicación compatible");
        return false;
    }

    if (g_overlay && g_overlayPid == g_targetPid &&
        WaitForSingleObject(g_overlay, 0) == WAIT_TIMEOUT) {
        if (g_overlayEvent && SetEvent(g_overlayEvent)) {
            set_status(L"HUD conectado · " + g_targetExe +
                L" (PID " + std::to_wstring(g_targetPid) + L")");
            return true;
        }
    }

    if (g_overlay || g_overlayEvent) stop_overlay(false);

    constexpr DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE;
    HANDLE process = OpenProcess(access, FALSE, g_targetPid);
    if (!process) {
        const DWORD error = GetLastError();
        std::wstring message = L"No se pudo acceder al proceso (error " +
            std::to_wstring(error) + L").";
        if (error == ERROR_ACCESS_DENIED)
            message += L" Ejecuta Control con el mismo nivel de permisos que Firestorm.";
        set_status(message);
        return false;
    }

    if (!is_x64_process(process)) {
        CloseHandle(process);
        set_status(L"OpenGL de MangoHud requiere que la aplicación sea x64.");
        return false;
    }

    const std::wstring dllPath = app_dir() + L"\\bin\\MangoHud.dll";
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CloseHandle(process);
        set_status(L"No se encuentra bin\\MangoHud.dll en la instalación.");
        return false;
    }

    wchar_t enabledName[96]{};
    wchar_t readyName[96]{};
    wchar_t loadedName[96]{};
    swprintf_s(enabledName, L"Local\\MangoHudOpenGLEnabled_%lu", g_targetPid);
    swprintf_s(readyName, L"Local\\MangoHudOpenGLReady_%lu", g_targetPid);
    swprintf_s(loadedName, L"Local\\MangoHudOpenGLLoaded_%lu", g_targetPid);

    HANDLE enabledEvent = CreateEventW(nullptr, TRUE, FALSE, enabledName);
    if (!enabledEvent) {
        const DWORD error = GetLastError();
        CloseHandle(process);
        set_status(L"No se pudo crear el canal de control (error " +
            std::to_wstring(error) + L").");
        return false;
    }
    ResetEvent(enabledEvent);

    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, readyName);
    if (!readyEvent) {
        const DWORD error = GetLastError();
        CloseHandle(enabledEvent);
        CloseHandle(process);
        set_status(L"No se pudo preparar la señal de inicialización (error " +
            std::to_wstring(error) + L").");
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
        set_status(L"No se pudo consultar la lista de módulos de Firestorm (error " +
            std::to_wstring(GetLastError()) + L").");
        return false;
    }

    DWORD injectionError = ERROR_SUCCESS;
    if (moduleLoaded) {
        // Never inject a second copy. Reuse only a DLL that exposes this PID protocol.
        HANDLE loadedEvent = OpenEventW(SYNCHRONIZE, FALSE, loadedName);
        const bool compatible = loadedEvent &&
            WaitForSingleObject(loadedEvent, 0) == WAIT_OBJECT_0;
        if (loadedEvent) CloseHandle(loadedEvent);
        if (!compatible) {
            CloseHandle(readyEvent);
            ResetEvent(enabledEvent);
            CloseHandle(enabledEvent);
            CloseHandle(process);
            set_status(L"Firestorm ya tiene una MangoHud.dll anterior cargada. "
                L"Cierra Firestorm por completo y vuelve a abrirlo para actualizar el hook.");
            return false;
        }
    } else {
        if (!inject_mangohud_dll(process, g_targetPid, dllPath, injectionError)) {
            CloseHandle(readyEvent);
            ResetEvent(enabledEvent);
            CloseHandle(enabledEvent);
            CloseHandle(process);
            std::wstring message = L"No se pudo inyectar MangoHud.dll (error " +
                std::to_wstring(injectionError) + L").";
            if (injectionError == ERROR_ACCESS_DENIED)
                message += L" Comprueba los permisos de Firestorm y MangoHud Control.";
            set_status(message);
            return false;
        }

        const DWORD ready = WaitForSingleObject(readyEvent, 20000);
        if (ready != WAIT_OBJECT_0) {
            const DWORD error = ready == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError();
            CloseHandle(readyEvent);
            ResetEvent(enabledEvent);
            CloseHandle(enabledEvent);
            CloseHandle(process);
            set_status(L"La DLL se cargó, pero el hook OpenGL no confirmó su inicio "
                L"(error " + std::to_wstring(error) +
                L"). Revisa la arquitectura x64 y el registro de MangoHud.");
            return false;
        }
    }
    CloseHandle(readyEvent);

    HANDLE loadedEvent = OpenEventW(SYNCHRONIZE, FALSE, loadedName);
    const bool hookReady = loadedEvent &&
        WaitForSingleObject(loadedEvent, 2000) == WAIT_OBJECT_0;
    if (loadedEvent) CloseHandle(loadedEvent);
    if (!hookReady) {
        ResetEvent(enabledEvent);
        CloseHandle(enabledEvent);
        CloseHandle(process);
        set_status(L"MangoHud.dll está en el proceso, pero no confirmó el protocolo "
            L"de Control. Reinicia Firestorm antes de volver a probar.");
        return false;
    }

    if (!SetEvent(enabledEvent)) {
        const DWORD error = GetLastError();
        ResetEvent(enabledEvent);
        CloseHandle(enabledEvent);
        CloseHandle(process);
        set_status(L"No se pudo activar el HUD (error " +
            std::to_wstring(error) + L").");
        return false;
    }

    g_overlay = process;
    g_overlayEvent = enabledEvent;
    g_overlayPid = g_targetPid;
    set_status(L"HUD conectado · " + (g_targetTitle.empty() ? g_targetExe : g_targetTitle) +
        L" (PID " + std::to_wstring(g_targetPid) + L")");
    return true;
}

void set_target(const AppWindow& app, bool automatic) {
    const bool changed = app.pid != g_targetPid;
    g_targetPid = app.pid;
    g_targetExe = app.exe;
    g_targetTitle = app.title;
    if (changed && g_enabled) start_overlay();
    else if (!g_overlay && g_enabled) start_overlay();

    if (automatic && g_overlay) set_status(L"HUD activo · " + (g_targetTitle.empty() ? g_targetExe : g_targetTitle));
}

void update_combo_selection() {
    if (!g_targetCombo) return;
    g_updatingCombo = true;
    SendMessageW(g_targetCombo, CB_RESETCONTENT, 0, 0);
    int selected = CB_ERR;
    for (const auto& app : g_apps) {
        const std::wstring label = target_label(app);
        const LRESULT index = SendMessageW(g_targetCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label.c_str()));
        if (index >= 0) {
            SendMessageW(g_targetCombo, CB_SETITEMDATA, static_cast<WPARAM>(index),
                static_cast<LPARAM>(app.pid));
            if (app.pid == g_targetPid) selected = static_cast<int>(index);
        }
    }
    if (selected != CB_ERR) SendMessageW(g_targetCombo, CB_SETCURSEL, selected, 0);
    else if (!g_apps.empty()) SendMessageW(g_targetCombo, CB_SETCURSEL, 0, 0);
    g_updatingCombo = false;
}

bool same_apps(const std::vector<AppWindow>& a, const std::vector<AppWindow>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].pid != b[i].pid || a[i].exe != b[i].exe || a[i].title != b[i].title)
            return false;
    }
    return true;
}

void refresh_apps() {
    std::vector<AppWindow> found;
    EnumWindows(collect_windows, reinterpret_cast<LPARAM>(&found));
    std::sort(found.begin(), found.end(), [](const AppWindow& a, const AppWindow& b) {
        return lower(a.exe) < lower(b.exe);
    });
    const bool changed = !same_apps(g_apps, found);
    const DWORD previousTargetPid = g_targetPid;
    g_apps = std::move(found);

    DWORD foregroundPid = 0;
    HWND foreground = GetForegroundWindow();
    if (foreground) GetWindowThreadProcessId(foreground, &foregroundPid);

    bool targetStillRunning = false;
    for (const auto& app : g_apps) if (app.pid == g_targetPid) targetStillRunning = true;

    if (g_autoSwitch) {
        for (const auto& app : g_apps) {
            if (app.pid == foregroundPid) {
                if (app.pid != g_targetPid) set_target(app, true);
                break;
            }
        }
    }

    if (!targetStillRunning && g_targetPid) {
        bool foundTarget = false;
        for (const auto& app : g_apps) if (app.pid == g_targetPid) foundTarget = true;
        if (!foundTarget) {
            stop_overlay(false);
            g_targetPid = 0;
            g_targetExe.clear();
            g_targetTitle.clear();
        }
    }

    if (!g_targetPid && !g_autoSwitch)
        set_status(L"Selecciona una aplicación detectada");
    else if (!g_targetPid && !g_apps.empty() && g_autoSwitch)
        set_status(L"Activa una aplicación para iniciar el HUD");

    if (changed || previousTargetPid != g_targetPid ||
        (g_targetCombo && SendMessageW(g_targetCombo, CB_GETCOUNT, 0, 0) != static_cast<LRESULT>(g_apps.size())))
        update_combo_selection();

    if (g_apps.empty() && !g_targetPid)
        set_status(L"Esperando a que abras una aplicación compatible");
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
        if (LOWORD(wParam) == ID_TARGET && HIWORD(wParam) == CBN_SELCHANGE && !g_updatingCombo) {
            const LRESULT index = SendMessageW(g_targetCombo, CB_GETCURSEL, 0, 0);
            if (index != CB_ERR) {
                const DWORD pid = static_cast<DWORD>(SendMessageW(g_targetCombo, CB_GETITEMDATA, index, 0));
                for (const auto& app : g_apps) if (app.pid == pid) {
                    g_autoSwitch = false;
                    if (g_autoCheck) SendMessageW(g_autoCheck, BM_SETCHECK, BST_UNCHECKED, 0);
                    set_target(app, false);
                    break;
                }
            }
            return 0;
        }
        if (LOWORD(wParam) == ID_AUTO && HIWORD(wParam) == BN_CLICKED) {
            g_autoSwitch = SendMessageW(g_autoCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (g_autoSwitch) refresh_apps();
            return 0;
        }
        if (LOWORD(wParam) == ID_ENABLED && HIWORD(wParam) == BN_CLICKED) {
            g_enabled = SendMessageW(g_enabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (g_enabled) {
                if (g_targetPid) start_overlay();
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
        SetTextColor(dc, GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == ID_STATUS ?
            RGB(205, 112, 26) : GetSysColor(COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(g_bgBrush);
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
        if (g_appIcon) DrawIconEx(dc, 24, 16, g_appIcon, 42, 42, 0, nullptr, DI_NORMAL);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kTimerId) {
            if (g_overlay && WaitForSingleObject(g_overlay, 0) == WAIT_OBJECT_0) {
                stop_overlay(false);
                g_enabled = false;
                if (g_enabledCheck) SendMessageW(g_enabledCheck, BM_SETCHECK, BST_UNCHECKED, 0);
                set_status(L"El HUD se ha detenido");
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
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\MangoHudControlMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND previous = FindWindowW(kClass, nullptr);
        if (previous) ShowWindow(previous, SW_RESTORE);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    g_bgBrush = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
    g_titleFont = CreateFontW(-27, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    g_smallFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");

    const std::wstring iconPath = app_dir() + L"\\MangoHud.ico";
    g_appIcon = static_cast<HICON>(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON,
        0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE));

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
        CW_USEDEFAULT, CW_USEDEFAULT, 440, 280,
        nullptr, nullptr, instance, nullptr);
    if (!g_main) return 2;

    SendMessageW(g_main, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon));
    SendMessageW(g_main, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIcon));

    make_child(L"STATIC", L"MangoHud", SS_LEFT, 76, 18, 330, 34, 0);
    HWND subtitle = make_child(L"STATIC", L"Control automático", SS_LEFT, 78, 52, 330, 22, 0);
    if (subtitle) SendMessageW(subtitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_smallFont), TRUE);
    g_status = make_child(L"STATIC", L"Buscando aplicaciones…", SS_LEFT, 25, 84, 390, 23, ID_STATUS);

    make_child(L"STATIC", L"Aplicación detectada", SS_LEFT, 25, 117, 390, 20, 0);
    g_targetCombo = make_child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
        25, 140, 390, 220, ID_TARGET);
    if (g_targetCombo) SendMessageW(g_targetCombo, CB_SETDROPPEDWIDTH, 390, 0);

    g_autoCheck = make_child(L"BUTTON", L"Seguir automáticamente la aplicación que uso",
        BS_AUTOCHECKBOX | WS_TABSTOP, 25, 181, 390, 26, ID_AUTO);
    g_enabledCheck = make_child(L"BUTTON", L"HUD activado",
        BS_AUTOCHECKBOX | WS_TABSTOP, 25, 211, 390, 26, ID_ENABLED);

    
    if (g_titleFont) {
        HWND title = FindWindowExW(g_main, nullptr, L"STATIC", L"MangoHud");
        if (title) SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_titleFont), TRUE);
    }
    SendMessageW(g_autoCheck, BM_SETCHECK, BST_CHECKED, 0);
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
    if (g_appIcon) DestroyIcon(g_appIcon);
    if (mutex) CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}