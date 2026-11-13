#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>

namespace {
constexpr wchar_t kClass[] = L"MangoHudControl";
constexpr UINT kTrayMessage = WM_APP + 7;
constexpr UINT_PTR kTimerId = 1;
constexpr int ID_ENABLED = 1103;
constexpr int MENU_OPEN = 1, MENU_TOGGLE = 2, MENU_EXIT = 3;
constexpr wchar_t kSharedStateName[] = L"Local\\MangoHudControlState";

struct AppWindow {
    DWORD pid{};
    std::wstring exe;
    std::wstring title;
};

struct SharedHudState {
    volatile LONG enabled;
    volatile LONG activePid;
};

HWND g_main{}, g_enabledCheck{}, g_status{};
HANDLE g_overlay{};
DWORD g_overlayPid{}, g_targetPid{};
std::wstring g_targetExe, g_targetTitle;
NOTIFYICONDATAW g_nid{};
bool g_tray{}, g_enabled{true}, g_targetOpenGL{};
ULONGLONG g_nextRetry{};
HFONT g_titleFont{}, g_font{}, g_smallFont{};
HBRUSH g_bgBrush{};
HICON g_appIcon{}, g_trayIcon{};
HANDLE g_stateMapping{};
SharedHudState* g_sharedState{};

std::wstring app_dir() {
    std::vector<wchar_t> b(32768);
    DWORD n = GetModuleFileNameW(nullptr, b.data(), static_cast<DWORD>(b.size()));
    if (!n || n >= b.size()) return L".";
    std::wstring p(b.data(), n);
    const size_t s = p.find_last_of(L"\\/");
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

void set_status(const std::wstring& message) {
    if (g_status) SetWindowTextW(g_status, message.c_str());
}

void set_shared_enabled(bool enabled) {
    g_enabled = enabled;
    if (g_sharedState)
        InterlockedExchange(&g_sharedState->enabled, enabled ? 1 : 0);
    if (g_enabledCheck)
        SendMessageW(g_enabledCheck, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
}

void set_shared_target(DWORD pid) {
    if (g_sharedState)
        InterlockedExchange(&g_sharedState->activePid, static_cast<LONG>(pid));
}

void init_shared_state() {
    g_stateMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(SharedHudState), kSharedStateName);
    if (!g_stateMapping) return;
    g_sharedState = static_cast<SharedHudState*>(
        MapViewOfFile(g_stateMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedHudState)));
    if (!g_sharedState) {
        CloseHandle(g_stateMapping);
        g_stateMapping = nullptr;
        return;
    }
    InterlockedExchange(&g_sharedState->enabled, 1);
    InterlockedExchange(&g_sharedState->activePid, 0);
}

bool query_process(DWORD pid, AppWindow& app) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    std::vector<wchar_t> image(32768);
    DWORD length = static_cast<DWORD>(image.size());
    const BOOL ok = QueryFullProcessImageNameW(process, 0, image.data(), &length);
    CloseHandle(process);
    if (!ok || !length) return false;

    app.pid = pid;
    app.exe = basename(std::wstring(image.data(), length));
    return !app.exe.empty();
}

bool process_has_module(DWORD pid, const wchar_t* moduleName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, moduleName) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool known_opengl_app(const std::wstring& exeName) {
    const std::wstring e = lower(exeName);
    return e.find(L"firestorm") != std::wstring::npos ||
        e.find(L"secondlife") != std::wstring::npos ||
        e.find(L"alchemy") != std::wstring::npos ||
        e.find(L"singularity") != std::wstring::npos ||
        e.find(L"kokua") != std::wstring::npos ||
        e.find(L"coolvlviewer") != std::wstring::npos;
}

bool inject_mangohud_dll(DWORD pid) {
    if (process_has_module(pid, L"MangoHud.dll")) return true;

    const std::wstring dllPath = app_dir() + L"\\bin\\MangoHud.dll";
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        set_status(L"No se encuentra bin\\MangoHud.dll");
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        const DWORD error = GetLastError();
        wchar_t message[160]{};
        swprintf_s(message, L"No se pudo acceder a la aplicación (error %lu)", error);
        set_status(message);
        return false;
    }

    BOOL wow64 = FALSE;
    if (IsWow64Process(process, &wow64) && wow64) {
        CloseHandle(process);
        set_status(L"La aplicación es de 32 bits; esta versión es x64");
        return false;
    }

    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, bytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        const DWORD error = GetLastError();
        CloseHandle(process);
        wchar_t message[160]{};
        swprintf_s(message, L"No se pudo preparar la conexión (error %lu)", error);
        set_status(message);
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, dllPath.c_str(), bytes, &written) ||
        written != bytes) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        wchar_t message[160]{};
        swprintf_s(message, L"No se pudo enviar MangoHud (error %lu)", error);
        set_status(message);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW"));
    HANDLE thread = loadLibrary ? CreateRemoteThread(process, nullptr, 0,
        loadLibrary, remotePath, 0, nullptr) : nullptr;
    if (!thread) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        wchar_t message[160]{};
        swprintf_s(message, L"No se pudo iniciar el hook (error %lu)", error);
        set_status(message);
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(thread, 15000);
    DWORD moduleHandle = 0;
    const bool loaded = waitResult == WAIT_OBJECT_0 &&
        GetExitCodeThread(thread, &moduleHandle) && moduleHandle != 0;
    const DWORD error = GetLastError();
    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);

    if (!loaded) {
        wchar_t message[160]{};
        swprintf_s(message, L"No se pudo cargar MangoHud (error %lu)", error);
        set_status(message);
        return false;
    }
    return true;
}

BOOL CALLBACK close_overlay_window(HWND hwnd, LPARAM parameter) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == static_cast<DWORD>(parameter)) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return FALSE;
    }
    return TRUE;
}

void stop_overlay() {
    if (!g_overlay) return;
    if (g_overlayPid) EnumWindows(close_overlay_window, static_cast<LPARAM>(g_overlayPid));
    if (WaitForSingleObject(g_overlay, 1000) == WAIT_TIMEOUT) {
        TerminateProcess(g_overlay, 0);
        WaitForSingleObject(g_overlay, 800);
    }
    CloseHandle(g_overlay);
    g_overlay = nullptr;
    g_overlayPid = 0;
}

bool start_overlay() {
    if (!g_enabled || g_targetExe.empty()) return false;
    if (g_overlay) stop_overlay();

    const std::wstring exe = app_dir() + L"\\tools\\MangoHud.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        set_status(L"No se encuentra tools\\MangoHud.exe");
        return false;
    }

    const std::wstring command = quote(exe) + L" " + quote(g_targetExe);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};

    const std::wstring configPath = app_dir() + L"\\config\\MangoHud.conf";
    const bool useConfig = GetFileAttributesW(configPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    DWORD oldLength = GetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", nullptr, 0);
    std::vector<wchar_t> oldValue(oldLength ? oldLength : 1, L'\0');
    const bool hadOld = oldLength > 0 &&
        GetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", oldValue.data(), oldLength) > 0;

    if (useConfig && !SetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", configPath.c_str())) {
        set_status(L"No se pudo cargar la configuración");
        return false;
    }

    const std::wstring workingDirectory = app_dir();
    const BOOL created = CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, workingDirectory.c_str(), &startup, &process);

    if (useConfig) {
        if (hadOld) SetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", oldValue.data());
        else SetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", nullptr);
    }

    if (!created) {
        const DWORD error = GetLastError();
        wchar_t message[160]{};
        swprintf_s(message, L"No se pudo iniciar el HUD (error %lu)", error);
        set_status(message);
        return false;
    }

    CloseHandle(process.hThread);
    g_overlay = process.hProcess;
    g_overlayPid = process.dwProcessId;
    g_nextRetry = GetTickCount64() + 4000;
    return true;
}

void clear_target() {
    stop_overlay();
    g_targetPid = 0;
    g_targetExe.clear();
    g_targetTitle.clear();
    g_targetOpenGL = false;
    set_shared_target(0);
}

void refresh_foreground() {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    // Opening the control window must not steal the target from the game.
    if (!pid || pid == GetCurrentProcessId()) return;

    AppWindow app{};
    if (!query_process(pid, app)) return;
    wchar_t title[512]{};
    GetWindowTextW(foreground, title, 512);
    app.title = title;

    if (ignored_process(app.exe)) {
        if (g_targetPid) clear_target();
        set_status(L"Esperando una aplicación compatible");
        return;
    }

    const bool changed = app.pid != g_targetPid;
    if (changed) {
        stop_overlay();
        g_targetPid = app.pid;
        g_targetExe = app.exe;
        g_targetTitle = app.title;
        g_targetOpenGL = false;
        g_nextRetry = 0;
    } else {
        g_targetTitle = app.title;
    }

    set_shared_target(g_enabled ? app.pid : 0);
    if (!g_enabled) {
        set_status(L"HUD pausado");
        return;
    }

    const bool hasOpenGL = process_has_module(app.pid, L"opengl32.dll");
    if (hasOpenGL || known_opengl_app(app.exe))
        g_targetOpenGL = true;

    const std::wstring displayName = app.title.empty() ? app.exe : app.title;
    if (g_targetOpenGL) {
        if (g_overlay) stop_overlay();
        if (!hasOpenGL) {
            set_status(L"Esperando a que " + app.exe + L" inicie OpenGL");
            return;
        }

        if (process_has_module(app.pid, L"MangoHud.dll")) {
            set_status(L"OpenGL conectado · " + displayName);
            return;
        }

        if (GetTickCount64() < g_nextRetry) return;
        g_nextRetry = GetTickCount64() + 2500;
        if (inject_mangohud_dll(app.pid)) {
            set_status(L"Conectado a OpenGL · " + displayName);
        }
        return;
    }

    if (g_overlay && WaitForSingleObject(g_overlay, 0) == WAIT_OBJECT_0) {
        CloseHandle(g_overlay);
        g_overlay = nullptr;
        g_overlayPid = 0;
        g_nextRetry = GetTickCount64() + 2500;
    }
    if (!g_overlay && GetTickCount64() >= g_nextRetry) {
        if (start_overlay())
            set_status(L"HUD activo · " + displayName);
    } else if (g_overlay) {
        set_status(L"HUD activo · " + displayName);
    }
}

void add_tray() {
    if (g_tray || !g_main) return;
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = kTrayMessage;
    g_nid.hIcon = g_trayIcon ? g_trayIcon : LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
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
    set_shared_enabled(!g_enabled);
    if (!g_enabled) {
        set_shared_target(0);
        stop_overlay();
        set_status(L"HUD pausado");
    } else {
        set_status(L"Buscando aplicación activa…");
        g_nextRetry = 0;
        refresh_foreground();
    }
}

void exit_application() {
    set_shared_enabled(false);
    set_shared_target(0);
    stop_overlay();
    remove_tray();
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
        if (LOWORD(wParam) == ID_ENABLED && HIWORD(wParam) == BN_CLICKED) {
            const bool enabled = SendMessageW(g_enabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            set_shared_enabled(enabled);
            if (!enabled) {
                set_shared_target(0);
                stop_overlay();
                set_status(L"HUD pausado");
            } else {
                g_nextRetry = 0;
                set_status(L"Buscando aplicación activa…");
                refresh_foreground();
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
        SetTextColor(dc, GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == 0 ?
            GetSysColor(COLOR_WINDOWTEXT) : RGB(205, 112, 26));
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
        if (g_appIcon) DrawIconEx(dc, 24, 16, g_appIcon, 48, 48, 0, nullptr, DI_NORMAL);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kTimerId) refresh_foreground();
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

    init_shared_state();
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
        48, 48, LR_LOADFROMFILE | LR_DEFAULTCOLOR));
    g_trayIcon = static_cast<HICON>(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON,
        16, 16, LR_LOADFROMFILE | LR_DEFAULTCOLOR));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = g_appIcon ? g_appIcon : LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIconSm = g_trayIcon ? g_trayIcon : wc.hIcon;
    wc.hbrBackground = g_bgBrush;
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc)) return 1;

    g_main = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"MangoHud",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 430, 230,
        nullptr, nullptr, instance, nullptr);
    if (!g_main) return 2;

    SendMessageW(g_main, WM_SETICON, ICON_BIG,
        reinterpret_cast<LPARAM>(g_appIcon ? g_appIcon : wc.hIcon));
    SendMessageW(g_main, WM_SETICON, ICON_SMALL,
        reinterpret_cast<LPARAM>(g_trayIcon ? g_trayIcon : wc.hIcon));

    HWND title = make_child(L"STATIC", L"MangoHud", SS_LEFT, 84, 18, 320, 34, 0);
    if (title && g_titleFont)
        SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_titleFont), TRUE);
    HWND subtitle = make_child(L"STATIC", L"Control automático", SS_LEFT, 86, 52, 320, 22, 0);
    if (subtitle) SendMessageW(subtitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_smallFont), TRUE);
    g_status = make_child(L"STATIC", L"Buscando aplicación activa…", SS_LEFT, 25, 98, 380, 26, 1);
    HWND hint = make_child(L"STATIC", L"Se conecta a la ventana que estás utilizando", SS_LEFT,
        25, 130, 380, 22, 0);
    if (hint) SendMessageW(hint, WM_SETFONT, reinterpret_cast<WPARAM>(g_smallFont), TRUE);
    g_enabledCheck = make_child(L"BUTTON", L"HUD activado",
        BS_AUTOCHECKBOX | WS_TABSTOP, 25, 165, 380, 26, ID_ENABLED);
    SendMessageW(g_enabledCheck, BM_SETCHECK, BST_CHECKED, 0);

    SetTimer(g_main, kTimerId, 500, nullptr);
    add_tray();
    refresh_foreground();
    hide_to_tray();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    stop_overlay();
    if (g_sharedState) {
        UnmapViewOfFile(g_sharedState);
        g_sharedState = nullptr;
    }
    if (g_stateMapping) CloseHandle(g_stateMapping);
    if (g_titleFont) DeleteObject(g_titleFont);
    if (g_font) DeleteObject(g_font);
    if (g_smallFont) DeleteObject(g_smallFont);
    if (g_bgBrush) DeleteObject(g_bgBrush);
    if (g_appIcon) DestroyIcon(g_appIcon);
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    if (mutex) CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
