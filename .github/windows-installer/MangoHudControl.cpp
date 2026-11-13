#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kClass[] = L"MangoHudControl";
constexpr UINT kTrayMessage = WM_APP + 7;
constexpr int ID_PROCESS = 1001, ID_START = 1002, ID_STOP = 1003, ID_TRAY = 1004, ID_STATUS = 1005;
constexpr COLORREF BG = RGB(19, 22, 30), PANEL = RGB(30, 36, 48);
constexpr COLORREF WHITE = RGB(238, 241, 247), MUTED = RGB(155, 165, 180);
constexpr COLORREF ORANGE = RGB(243, 145, 42), BUTTON = RGB(48, 57, 73);
HWND g_main{}, g_edit{}, g_status{};
HANDLE g_overlay{};
DWORD g_pid{};
NOTIFYICONDATAW g_nid{};
bool g_tray{};
HFONT g_titleFont{}, g_font{}, g_smallFont{};
HBRUSH g_bgBrush{}, g_editBrush{};
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
    std::wstring r = L"\""; size_t bs = 0;
    for (wchar_t c : s) {
        if (c == L'\\') ++bs;
        else if (c == L'"') { r.append(bs * 2 + 1, L'\\'); r += L'"'; bs = 0; }
        else { r.append(bs, L'\\'); bs = 0; r += c; }
    }
    r.append(bs * 2, L'\\'); r += L'"'; return r;
}
void status(const wchar_t* s) { if (g_status) SetWindowTextW(g_status, s); }

BOOL CALLBACK close_overlay(HWND w, LPARAM p) {
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    if (pid == static_cast<DWORD>(p)) {
        wchar_t cls[128]{};
        GetClassNameW(w, cls, 128);
        if (wcscmp(cls, L"MangoHudOverlay") == 0) { PostMessageW(w, WM_CLOSE, 0, 0); return FALSE; }
    }
    return TRUE;
}
void stop_overlay() {
    if (!g_overlay) { status(L"Overlay detenido"); return; }
    EnumWindows(close_overlay, static_cast<LPARAM>(g_pid));
    if (WaitForSingleObject(g_overlay, 2500) == WAIT_TIMEOUT) {
        TerminateProcess(g_overlay, 0);
        WaitForSingleObject(g_overlay, 1000);
    }
    CloseHandle(g_overlay); g_overlay = nullptr; g_pid = 0;
    status(L"Overlay detenido");
}
bool start_overlay() {
    wchar_t name[260]{};
    GetWindowTextW(g_edit, name, 260);
    std::wstring target(name);
    size_t a = target.find_first_not_of(L" \t"), b = target.find_last_not_of(L" \t");
    if (a == std::wstring::npos) { status(L"Escribe el nombre de un proceso .exe"); return false; }
    target = target.substr(a, b - a + 1);
    if (target.find_first_of(L"\\/:\"") != std::wstring::npos ||
        target.size() < 5 || _wcsicmp(target.c_str() + target.size() - 4, L".exe") != 0) {
        status(L"Escribe solo el nombre, por ejemplo Firestorm-Releasex64.exe"); return false;
    }
    stop_overlay();
    std::wstring exe = app_dir() + L"\\tools\\MangoHud.exe";
    std::wstring cmd = quote(exe) + L" " + quote(target);
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end()); mutableCmd.push_back(0);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    // Pass the installed config only to the overlay child process; restore the
    // controller's environment immediately after CreateProcessW.
    const std::wstring configPath = app_dir() + L"\\config\\MangoHud.conf";
    const bool useInstalledConfig = GetFileAttributesW(configPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    DWORD oldConfigLength = GetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", nullptr, 0);
    std::vector<wchar_t> oldConfig(oldConfigLength ? oldConfigLength : 1, L'\\0');
    const bool hadOldConfig = oldConfigLength > 0 &&
        GetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", oldConfig.data(), oldConfigLength) > 0;
    if (useInstalledConfig && !SetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", configPath.c_str())) {
        wchar_t msg[160]{}; swprintf_s(msg, L"No se pudo aplicar MangoHud.conf (error %lu)", GetLastError());
        status(msg); return false;
    }

    const BOOL created = CreateProcessW(exe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, app_dir().c_str(), &si, &pi);
    if (useInstalledConfig) {
        if (hadOldConfig) SetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", oldConfig.data());
        else SetEnvironmentVariableW(L"MANGOHUD_CONFIGFILE", nullptr);
    }
    if (!created) {
        wchar_t msg[128]{}; swprintf_s(msg, L"No se pudo iniciar MangoHud.exe (error %lu)", GetLastError());
        status(msg); return false;
    }
    CloseHandle(pi.hThread); g_overlay = pi.hProcess; g_pid = pi.dwProcessId;
    std::wstring msg = L"Siguiendo únicamente " + target + L"…"; status(msg.c_str()); return true;
}
void add_tray() {
    if (g_tray) return;
    g_nid = {}; g_nid.cbSize = sizeof(g_nid); g_nid.hWnd = g_main; g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; g_nid.uCallbackMessage = kTrayMessage;
    g_nid.hIcon = g_appIcon ? g_appIcon : LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wcscpy_s(g_nid.szTip, L"MangoHud para Windows");
    g_tray = Shell_NotifyIconW(NIM_ADD, &g_nid) == TRUE;
}
void remove_tray() { if (g_tray) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_tray = false; } }
void restore() { ShowWindow(g_main, SW_SHOW); ShowWindow(g_main, SW_RESTORE); SetForegroundWindow(g_main); }
void tray_menu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Abrir panel"); AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 2, L"Cerrar MangoHud");
    POINT p{}; GetCursorPos(&p); SetForegroundWindow(g_main);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_main, nullptr); DestroyMenu(m);
}
void button_draw(const DRAWITEMSTRUCT* d) {
    COLORREF fill = d->CtlID == ID_START ? ORANGE : (d->CtlID == ID_STOP ? RGB(125, 52, 59) : BUTTON);
    HBRUSH br = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ ob = SelectObject(d->hDC, br), op = SelectObject(d->hDC, pen);
    RoundRect(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right, d->rcItem.bottom, 10, 10);
    SelectObject(d->hDC, ob); SelectObject(d->hDC, op); DeleteObject(br); DeleteObject(pen);
    wchar_t t[128]{}; GetWindowTextW(d->hwndItem, t, 128);
    SetBkMode(d->hDC, TRANSPARENT); SetTextColor(d->hDC, d->CtlID == ID_START ? RGB(34, 25, 15) : WHITE);
    SelectObject(d->hDC, g_font);
    RECT r = d->rcItem; DrawTextW(d->hDC, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(wp) == ID_START) { start_overlay(); return 0; }
        if (LOWORD(wp) == ID_STOP) { stop_overlay(); return 0; }
        if (LOWORD(wp) == ID_TRAY) { ShowWindow(w, SW_MINIMIZE); return 0; }
        if (LOWORD(wp) == 1) { restore(); return 0; }
        if (LOWORD(wp) == 2) { PostMessageW(w, WM_CLOSE, 0, 0); return 0; }
        break;
    case WM_DRAWITEM:
        if (wp == ID_START || wp == ID_STOP || wp == ID_TRAY) { button_draw(reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE; }
        break;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp); SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetDlgCtrlID(reinterpret_cast<HWND>(lp)) == ID_STATUS ? ORANGE : MUTED);
        return reinterpret_cast<LRESULT>(g_bgBrush);
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp); SetTextColor(dc, WHITE); SetBkColor(dc, PANEL);
        return reinterpret_cast<LRESULT>(g_editBrush);
    }
    case WM_ERASEBKGND: {
        RECT r{}; GetClientRect(w, &r); FillRect(reinterpret_cast<HDC>(wp), &r, g_bgBrush); return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(w, &ps); RECT r{}; GetClientRect(w, &r);
        RECT h{0,0,r.right,104}; HBRUSH hb = CreateSolidBrush(RGB(25,30,41)); FillRect(dc,&h,hb); DeleteObject(hb);
        RECT stripe{0,0,5,104}; HBRUSH ab = CreateSolidBrush(ORANGE); FillRect(dc,&stripe,ab); DeleteObject(ab);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, WHITE); SelectObject(dc,g_titleFont);
        RECT tr{26,18,r.right-20,58}; DrawTextW(dc,L"MangoHud",-1,&tr,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        SetTextColor(dc,MUTED); SelectObject(dc,g_smallFont);
        RECT sr{28,61,r.right-20,88}; DrawTextW(dc,L"CONTROL DEL OVERLAY  ·  WINDOWS",-1,&sr,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        EndPaint(w,&ps); return 0;
    }
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) { add_tray(); ShowWindow(w, SW_HIDE); } return 0;
    case kTrayMessage:
        if (lp == WM_LBUTTONDBLCLK) restore();
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) tray_menu();
        return 0;
    case WM_TIMER:
        if (g_overlay && WaitForSingleObject(g_overlay,0) == WAIT_OBJECT_0) {
            CloseHandle(g_overlay); g_overlay=nullptr; g_pid=0; status(L"El overlay se ha detenido");
        } return 0;
    case WM_CLOSE:
        remove_tray(); stop_overlay(); DestroyWindow(w); return 0;
    case WM_DESTROY: KillTimer(w,1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w,m,wp,lp);
}
HWND child(const wchar_t* klass,const wchar_t* text,DWORD style,int x,int y,int cx,int cy,int id) {
    HWND h=CreateWindowExW(0,klass,text,WS_CHILD|WS_VISIBLE|style,x,y,cx,cy,g_main,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    if(h) SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(g_font),TRUE); return h;
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    HANDLE mutex=CreateMutexW(nullptr,TRUE,L"Local\\MangoHudControlMutex");
    if(GetLastError()==ERROR_ALREADY_EXISTS) { HWND old=FindWindowW(kClass,nullptr); if(old) { ShowWindow(old,SW_RESTORE); SetForegroundWindow(old); } if(mutex) CloseHandle(mutex); return 0; }
    g_bgBrush=CreateSolidBrush(BG); g_editBrush=CreateSolidBrush(PANEL);
    g_titleFont=CreateFontW(-30,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    g_font=CreateFontW(-16,0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    g_smallFont=CreateFontW(-13,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    const std::wstring iconPath = app_dir() + L"\\MangoHud.ico";
    g_appIcon = static_cast<HICON>(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE));
    WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=wndproc; wc.hInstance=instance;
    wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512)); wc.hIcon=g_appIcon ? g_appIcon : LoadIconW(nullptr,MAKEINTRESOURCEW(32512)); wc.hIconSm=wc.hIcon; wc.hbrBackground=g_bgBrush; wc.lpszClassName=kClass;
    if(!RegisterClassExW(&wc)) return 1;
    g_main=CreateWindowExW(WS_EX_APPWINDOW,kClass,L"MangoHud para Windows",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,480,420,nullptr,nullptr,instance,nullptr);
    if(!g_main) return 2;
    if (g_appIcon) { SendMessageW(g_main, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon)); SendMessageW(g_main, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIcon)); }
    const BOOL dark=TRUE; DwmSetWindowAttribute(g_main,20,&dark,sizeof(dark));
    child(L"STATIC",L"PROCESO OBJETIVO",0,28,124,200,22,0);
    child(L"STATIC",L"El HUD sigue solo este proceso, no cambia al navegador.",0,28,149,410,20,0);
    g_edit=child(L"EDIT",L"Firestorm-Releasex64.exe",WS_BORDER|ES_AUTOHSCROLL,28,178,408,34,ID_PROCESS);
    child(L"BUTTON",L"Iniciar / reiniciar",BS_OWNERDRAW,28,235,188,42,ID_START);
    child(L"BUTTON",L"Detener",BS_OWNERDRAW,226,235,100,42,ID_STOP);
    child(L"BUTTON",L"Minimizar a la bandeja",BS_OWNERDRAW,28,288,298,36,ID_TRAY);
    g_status=child(L"STATIC",L"Iniciando…",SS_LEFT,28,338,408,22,ID_STATUS);
    EnumChildWindows(g_main,[](HWND h,LPARAM)->BOOL{ SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(g_font),TRUE); return TRUE; },0);
    SetTimer(g_main,1,750,nullptr); start_overlay();
    ShowWindow(g_main,show==SW_HIDE?SW_SHOWNORMAL:show); UpdateWindow(g_main);
    MSG msg{}; while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    if(g_titleFont)DeleteObject(g_titleFont); if(g_font)DeleteObject(g_font); if(g_smallFont)DeleteObject(g_smallFont);
    if(g_bgBrush)DeleteObject(g_bgBrush); if(g_editBrush)DeleteObject(g_editBrush); if(g_appIcon)DestroyIcon(g_appIcon); if(mutex)CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}