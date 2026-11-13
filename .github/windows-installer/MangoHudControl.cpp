#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <iterator>

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
        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
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

BOOL CALLBACK close_overlay_window(HWND hwnd, LPARAM parameter) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == static_cast<DWORD>(parameter)) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return FALSE;
    }
    return TRUE;
}

void stop_overlay(bool announce = true) {
    if (g_overlay) {
        if (g_overlayPid) EnumWindows(close_overlay_window, static_cast<LPARAM>(g_overlayPid));
        if (WaitForSingleObject(g_overlay, 1200) == WAIT_TIMEOUT) {
            TerminateProcess(g_overlay, 0);
            WaitForSingleObject(g_overlay, 800);
        }
        CloseHandle(g_overlay);
        g_overlay = nullptr;
    }
    g_overlayPid = 0;
    if (announce && g_enabled) set_status(L"HUD pausado");
}

bool start_overlay() {
    if (!g_enabled) return false;
    if (g_targetExe.empty()) {
        set_status(L"Abre Firestorm u otra aplicación compatible");
        return false;
    }
    if (g_overlay && g_targetPid && WaitForSingleObject(g_overlay, 0) == WAIT_TIMEOUT)
        stop_overlay(false);

    const std::wstring exe = app_dir() + L"\\tools\\MangoHud.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        set_status(L"No se encuentra MangoHud.exe en la instalación");
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
        wchar_t error[128]{};
        swprintf_s(error, L"No se pudo cargar la configuración (error %lu)", GetLastError());
        set_status(error);
        return false;
    }

    const BOOL created = CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, app_dir().c_str(), &startup, &process);

    if (useConfig) {
        if (hadOld) SetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", oldValue.data());
        else SetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", nullptr);
    }

    if (!created) {
        wchar_t error[128]{};
        swprintf_s(error, L"No se pudo iniciar el HUD (error %lu)", GetLastError());
        set_status(error);
        return false;
    }

    CloseHandle(process.hThread);
    g_overlay = process.hProcess;
    g_overlayPid = process.dwProcessId;
    set_status(L"HUD activo · " + (g_targetTitle.empty() ? g_targetExe : g_targetTitle));
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

    if (!g_targetPid && !g_apps.empty() && g_autoSwitch) {
        const AppWindow* chosen = &g_apps.front();
        for (const auto& app : g_apps) if (app.pid == foregroundPid) { chosen = &app; break; }
        set_target(*chosen, g_autoSwitch);
    }

    if (changed || previousTargetPid != g_targetPid ||
        (g_targetCombo && SendMessageW(g_targetCombo, CB_GETCOUNT, 0, 0) != static_cast<LRESULT>(g_apps.size())))
        update_combo_selection();

    if (g_apps.empty() && !g_targetPid)
        set_status(L"Esperando a que abras una aplicación compatible");
    else if (!g_targetPid && !g_autoSwitch)
        set_status(L"Selecciona una aplicación detectada");
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
        stop_overlay(false);
        set_status(L"HUD pausado");
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
                stop_overlay(false);
                set_status(L"HUD pausado");
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
    case WM_TIMER:
        if (wParam == kTimerId) {
            if (g_overlay && WaitForSingleObject(g_overlay, 0) == WAIT_OBJECT_0) {
                CloseHandle(g_overlay);
                g_overlay = nullptr;
                g_overlayPid = 0;
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
        CW_USEDEFAULT, CW_USEDEFAULT, 440, 300,
        nullptr, nullptr, instance, nullptr);
    if (!g_main) return 2;

    SendMessageW(g_main, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon));
    SendMessageW(g_main, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIcon));

    make_child(L"STATIC", L"MangoHud", SS_LEFT, 24, 18, 370, 34, 0);
    HWND subtitle = make_child(L"STATIC", L"Control automático", SS_LEFT, 25, 52, 370, 22, 0);
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

    HWND hint = make_child(L"STATIC",
        L"La X oculta esta ventana. Usa el icono de la bandeja para salir.",
        SS_LEFT, 25, 245, 390, 20, 0);
    if (hint) SendMessageW(hint, WM_SETFONT, reinterpret_cast<WPARAM>(g_smallFont), TRUE);

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