#include <windows.h>
#include "opengl_hook.h"

static DWORD WINAPI install_opengl_hook_thread(void*)
{
    init_opengl_hook();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, install_opengl_hook_thread,
                                     nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
