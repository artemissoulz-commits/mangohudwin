#include <windows.h>
#include <cwchar>
#include "opengl_hook.h"

static DWORD WINAPI install_opengl_hook_thread(void*)
{
    const bool installed = init_opengl_hook();

    wchar_t eventName[96] = {};
    swprintf_s(eventName, _countof(eventName),
               L"Local\\MangoHudOpenGLReady_%lu", GetCurrentProcessId());
    HANDLE readyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
    if (readyEvent) {
        if (installed)
            SetEvent(readyEvent);
        CloseHandle(readyEvent);
    }
    return installed ? 0 : 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, install_opengl_hook_thread, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
