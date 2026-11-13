#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cwchar>
#include <iostream>
#include <string>
#include <vector>

static std::wstring quote_argument(const std::wstring& value)
{
    if (value.find_first_of(L" \\t\\\"") == std::wstring::npos)
        return value;

    std::wstring result = L"\\\"";
    size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\\\') {
            ++backslashes;
        } else if (ch == L'\\\"') {
            result.append(backslashes * 2 + 1, L'\\\\');
            result += L'\\\"';
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\\\');
            backslashes = 0;
            result += ch;
        }
    }
    result.append(backslashes * 2, L'\\\\');
    result += L'\\\"';
    return result;
}

static bool make_absolute_path(const wchar_t* input, std::wstring& output)
{
    const DWORD needed = GetFullPathNameW(input, 0, nullptr, nullptr);
    if (needed == 0)
        return false;
    std::vector<wchar_t> buffer(static_cast<size_t>(needed) + 1, L'\\0');
    const DWORD written = GetFullPathNameW(input, static_cast<DWORD>(buffer.size()),
                                            buffer.data(), nullptr);
    if (written == 0 || written >= buffer.size())
        return false;
    output.assign(buffer.data(), written);
    return true;
}

static std::wstring parent_directory(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\\\/");
    if (slash == std::wstring::npos)
        return L".";
    if (slash == 2 && path.size() >= 3 && path[1] == L':')
        return path.substr(0, 3);
    return path.substr(0, slash);
}

static bool inject_dll(HANDLE process, const std::wstring& dllPath)
{
    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, bytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath)
        return false;

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, dllPath.c_str(), bytes, &written)
        || written != bytes) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = kernel32 ? reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW")) : nullptr;
    if (!loadLibrary) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary,
                                       remotePath, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(thread, 15000);
    DWORD moduleHandle = 0;
    const bool loaded = waitResult == WAIT_OBJECT_0
        && GetExitCodeThread(thread, &moduleHandle)
        && moduleHandle != 0;

    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return loaded;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        std::wcerr << L"Usage: MangoHudLauncher.exe <game.exe> [arguments...]\\n"
                   << L"Place MangoHud.dll beside MangoHudLauncher.exe.\\n";
        return 2;
    }

    std::wstring gamePath;
    if (!make_absolute_path(argv[1], gamePath)
        || GetFileAttributesW(gamePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcerr << L"Game executable not found: " << argv[1] << L"\\n";
        return 3;
    }

    wchar_t launcherPath[MAX_PATH] = {};
    const DWORD pathLength = GetModuleFileNameW(nullptr, launcherPath, MAX_PATH);
    if (!pathLength || pathLength >= MAX_PATH) {
        std::wcerr << L"Could not determine launcher location.\\n";
        return 4;
    }

    std::wstring dllPath = parent_directory(launcherPath) + L"\\\\MangoHud.dll";
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcerr << L"MangoHud.dll not found beside the launcher: " << dllPath << L"\\n";
        return 5;
    }

    std::wstring commandLine = quote_argument(gamePath);
    for (int i = 2; i < argc; ++i) {
        commandLine += L" ";
        commandLine += quote_argument(argv[i]);
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    const std::wstring gameDirectory = parent_directory(gamePath);

    if (!CreateProcessW(gamePath.c_str(), mutableCommand.data(), nullptr, nullptr,
                        FALSE, CREATE_SUSPENDED, nullptr, gameDirectory.c_str(),
                        &startup, &process)) {
        std::wcerr << L"CreateProcess failed: " << GetLastError() << L"\\n";
        return 6;
    }

    wchar_t eventName[96] = {};
    swprintf_s(eventName, _countof(eventName),
               L"Local\\\\MangoHudOpenGLReady_%lu", process.dwProcessId);
    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, eventName);
    if (!readyEvent) {
        const DWORD error = GetLastError();
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        std::wcerr << L"Could not create startup event: " << error << L"\\n";
        return 7;
    }

    if (!inject_dll(process.hProcess, dllPath)) {
        const DWORD error = GetLastError();
        CloseHandle(readyEvent);
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        std::wcerr << L"Could not load MangoHud.dll (Win32 error " << error
                   << L"). Check that launcher and game have the same architecture.\\n";
        return 8;
    }

    const DWORD ready = WaitForSingleObject(readyEvent, 10000);
    CloseHandle(readyEvent);
    if (ready != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        std::wcerr << L"MangoHud.dll loaded, but the OpenGL hook did not become ready.\\n";
        return 9;
    }

    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::wcout << L"MangoHud OpenGL hook installed. Game started.\\n";
    return 0;
}
