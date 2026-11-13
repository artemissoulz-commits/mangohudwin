#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

static std::wstring quote_argument(const std::wstring& value)
{
    if (value.find_first_of(L" \\t\\\"") == std::wstring::npos)
        return value;
    std::wstring result = L"\\\"";
    size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\\\') ++slashes;
        else if (ch == L'\\\"') {
            result.append(slashes * 2 + 1, L'\\\\');
            result += L'\\\"';
            slashes = 0;
        } else {
            result.append(slashes, L'\\\\');
            slashes = 0;
            result += ch;
        }
    }
    result.append(slashes * 2, L'\\\\');
    result += L'\\\"';
    return result;
}

static bool inject_dll(HANDLE process, const std::wstring& dllPath)
{
    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, bytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) return false;
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, dllPath.c_str(), bytes, &written)
        || written != bytes) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW"));
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
    const bool loaded = waitResult == WAIT_OBJECT_0 &&
        GetExitCodeThread(thread, &moduleHandle) && moduleHandle != 0;
    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return loaded;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        std::wcerr << L"Usage: MangoHudLauncher.exe <game.exe> [arguments...]\\n";
        return 2;
    }
    const std::filesystem::path target = std::filesystem::absolute(argv[1]);
    if (!std::filesystem::exists(target)) {
        std::wcerr << L"Game executable not found: " << target.wstring() << L"\\n";
        return 3;
    }
    wchar_t self[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (!length || length >= MAX_PATH) return 4;
    const std::filesystem::path dll =
        std::filesystem::path(self).parent_path() / L"MangoHud.dll";
    if (!std::filesystem::exists(dll)) {
        std::wcerr << L"MangoHud.dll must be beside MangoHudLauncher.exe\\n";
        return 5;
    }
    std::wstring command = quote_argument(target.wstring());
    for (int i = 2; i < argc; ++i) {
        command += L" ";
        command += quote_argument(argv[i]);
    }
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\\0');
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(target.c_str(), mutableCommand.data(), nullptr, nullptr,
        FALSE, CREATE_SUSPENDED, nullptr, target.parent_path().c_str(),
        &startup, &process)) {
        std::wcerr << L"CreateProcess failed: " << GetLastError() << L"\\n";
        return 6;
    }
    if (!inject_dll(process.hProcess, dll.wstring())) {
        const DWORD error = GetLastError();
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        std::wcerr << L"DLL injection failed (Win32 error " << error
                   << L"). Match launcher/game architecture and permissions.\\n";
        return 7;
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::wcout << L"MangoHud injected; game started.\\n";
    return 0;
}
