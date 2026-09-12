#include "../../../renderer/renderer_raii.h"
#include "../../../renderer/json.hpp"

#include <dbghelp.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;
using renderer_raii::AdoptHandle;
using renderer_raii::UniqueHandle;

std::string Utf8(std::wstring const& value)
{
    int const size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring FilePath(HANDLE file)
{
    if (file == nullptr || file == INVALID_HANDLE_VALUE) return {};
    std::vector<wchar_t> buffer(32768);
    DWORD const length = GetFinalPathNameByHandleW(file, buffer.data(),
        static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
    if (length == 0 || length >= buffer.size()) return {};
    std::wstring path(buffer.data(), length);
    if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    return path;
}

struct Child
{
    UniqueHandle process;
    Json evidence = {{"modules", Json::array()}, {"exceptions", Json::array()},
        {"windowSeen", false}, {"responsive", false}, {"exited", false}};
    bool target = false;
    bool dumped = false;
    HWND window = nullptr;
};

struct WindowSearch { DWORD pid; HWND window = nullptr; };

BOOL CALLBACK FindWindow(HWND window, LPARAM argument)
{
    auto& search = *reinterpret_cast<WindowSearch*>(argument);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == search.pid && IsWindowVisible(window) &&
        GetWindow(window, GW_OWNER) == nullptr && GetWindowTextLengthW(window) > 0)
    {
        search.window = window;
        return FALSE;
    }
    return TRUE;
}

bool Capture(HWND window, std::filesystem::path const& path)
{
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return false;
    LONG const width = rect.right - rect.left;
    LONG const height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;
    HDC const screen = GetDC(nullptr);
    HDC const memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP const bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    bool saved = false;
    if (memory != nullptr && bitmap != nullptr)
    {
        HGDIOBJ const previous = SelectObject(memory, bitmap);
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(window, WM_PRINT, reinterpret_cast<WPARAM>(memory),
            PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN, SMTO_ABORTIFHUNG, 3000, &result))
        {
            BITMAPFILEHEADER header{};
            header.bfType = 0x4d42;
            header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
            DWORD const bytes = static_cast<DWORD>(width * height * 4);
            header.bfSize = header.bfOffBits + bytes;
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<char const*>(&header), sizeof(header));
            output.write(reinterpret_cast<char const*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
            output.write(static_cast<char const*>(pixels), bytes);
            saved = output.good();
        }
        SelectObject(memory, previous);
    }
    if (bitmap != nullptr) DeleteObject(bitmap);
    if (memory != nullptr) DeleteDC(memory);
    if (screen != nullptr) ReleaseDC(nullptr, screen);
    return saved;
}

bool Diagnostic(DWORD pid, wchar_t const* stage)
{
    wchar_t nameSpace[80]{};
    if (!GetEnvironmentVariableW(L"MACTYPE_DIRECTWRITE_DIAGNOSTICS", nameSpace, 80)) return false;
    std::wstring const name = L"Local\\MacType." + std::wstring(nameSpace) +
        L".pid-" + std::to_wstring(pid) + L"." + stage;
    UniqueHandle event = AdoptHandle(OpenEventW(SYNCHRONIZE, FALSE, name.c_str()));
    return event && WaitForSingleObject(event.get(), 0) == WAIT_OBJECT_0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 4)
    {
        std::cerr << "usage: startup-debug-probe APP LOADER_OR_stock OUTPUT_DIRECTORY\n";
        return 2;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::filesystem::path const app = std::filesystem::absolute(argv[1]);
    std::filesystem::path const output = std::filesystem::absolute(argv[3]);
    std::filesystem::create_directories(output);
    bool const stock = std::wstring(argv[2]) == L"stock";
    std::wstring command = L"\"" + app.wstring() + L"\"";
    if (!stock) command = L"\"" + std::wstring(argv[2]) + L"\" " + command;
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    UniqueHandle job = AdoptHandle(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
        &limits, sizeof(limits))) return 3;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION initial{};
    // DEBUG_PROCESS also observes the child created by the shipped MacLoader.
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
        DEBUG_PROCESS | CREATE_SUSPENDED, nullptr, app.parent_path().c_str(), &startup, &initial))
        return 4;
    UniqueHandle initialProcess = AdoptHandle(initial.hProcess);
    UniqueHandle initialThread = AdoptHandle(initial.hThread);
    if (!AssignProcessToJobObject(job.get(), initialProcess.get()))
    {
        TerminateProcess(initialProcess.get(), 3);
        return 5;
    }
    if (ResumeThread(initialThread.get()) == static_cast<DWORD>(-1)) return 6;
    std::map<DWORD, Child> children;
    Json report = {{"processes", Json::array()}, {"observationMilliseconds", 30000},
        {"debuggerError", 0}, {"targetObserved", false}, {"healthy", false}};
    ULONGLONG const deadline = GetTickCount64() + 30000;
    bool debuggerFailed = false;
    while (GetTickCount64() < deadline)
    {
        DEBUG_EVENT event{};
        bool const eventAvailable = WaitForDebugEvent(&event, 100) != FALSE;
        if (eventAvailable)
        {
            DWORD disposition = DBG_CONTINUE;
            auto& child = children[event.dwProcessId];
            if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT)
            {
                HANDLE owned = nullptr;
                if (!DuplicateHandle(GetCurrentProcess(), event.u.CreateProcessInfo.hProcess,
                    GetCurrentProcess(), &owned, 0, FALSE, DUPLICATE_SAME_ACCESS))
                    debuggerFailed = true;
                child.process = AdoptHandle(owned);
                UniqueHandle imageFile = AdoptHandle(event.u.CreateProcessInfo.hFile);
                std::wstring const imagePath = FilePath(imageFile.get());
                child.target = _wcsicmp(imagePath.c_str(), app.c_str()) == 0;
                child.evidence["pid"] = event.dwProcessId;
                child.evidence["image"] = Utf8(imagePath);
                child.evidence["target"] = child.target;
                child.evidence["imageBase"] = reinterpret_cast<uintptr_t>(event.u.CreateProcessInfo.lpBaseOfImage);
            }
            else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT)
            {
                UniqueHandle moduleFile = AdoptHandle(event.u.LoadDll.hFile);
                child.evidence["modules"].push_back({{"path", Utf8(FilePath(moduleFile.get()))},
                    {"base", reinterpret_cast<uintptr_t>(event.u.LoadDll.lpBaseOfDll)}});
            }
            else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT)
            {
                child.evidence["exited"] = true;
                child.evidence["exitCode"] = event.u.ExitProcess.dwExitCode;
            }
            else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT)
            {
                auto const& exception = event.u.Exception;
                DWORD const code = exception.ExceptionRecord.ExceptionCode;
                // Only debugger breakpoints are consumed. Application/CLR exceptions
                // retain their normal first- and second-chance handling.
                disposition = code == EXCEPTION_BREAKPOINT || code == 0x4000001f
                    ? DBG_CONTINUE : DBG_EXCEPTION_NOT_HANDLED;
                if (code == EXCEPTION_STACK_OVERFLOW || exception.dwFirstChance == 0)
                {
                    Json fault = {{"code", code}, {"firstChance", exception.dwFirstChance != 0},
                        {"thread", event.dwThreadId},
                        {"address", reinterpret_cast<uintptr_t>(exception.ExceptionRecord.ExceptionAddress)}};
                    if (!child.dumped)
                    {
                        auto const dumpPath = output / (L"crash-" + std::to_wstring(event.dwProcessId) + L".dmp");
                        UniqueHandle dump = AdoptHandle(CreateFileW(dumpPath.c_str(), GENERIC_WRITE,
                            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
                        bool const written = dump && MiniDumpWriteDump(child.process.get(),
                            event.dwProcessId, dump.get(), static_cast<MINIDUMP_TYPE>(
                                MiniDumpWithFullMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                            nullptr, nullptr, nullptr);
                        fault["dumpWritten"] = written;
                        fault["dumpError"] = written ? 0 : GetLastError();
                        child.dumped = true;
                    }
                    child.evidence["exceptions"].push_back(std::move(fault));
                }
            }
            if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition))
            {
                report["debuggerError"] = GetLastError();
                debuggerFailed = true;
                break;
            }
            bool allExited = !children.empty();
            for (auto const& [pid, tracked] : children)
            {
                (void)pid;
                allExited = allExited && tracked.evidence["exited"].get<bool>();
            }
            if (allExited) break;
        }
        else if (GetLastError() != ERROR_SEM_TIMEOUT)
        {
            report["debuggerError"] = GetLastError();
            debuggerFailed = true;
            break;
        }
        if (eventAvailable) continue;
        for (auto& [pid, child] : children)
        {
            if (!child.target || child.evidence["exited"].get<bool>()) continue;
            WindowSearch search{pid};
            EnumWindows(FindWindow, reinterpret_cast<LPARAM>(&search));
            if (search.window != nullptr)
            {
                child.window = search.window;
                child.evidence["windowSeen"] = true;
                DWORD_PTR reply = 0;
                child.evidence["responsive"] = SendMessageTimeoutW(search.window, WM_NULL,
                    0, 0, SMTO_ABORTIFHUNG, 200, &reply) != 0;
            }
        }
    }
    for (auto& [pid, child] : children)
    {
        if (child.target)
        {
            report["targetObserved"] = true;
            bool const alive = child.process && WaitForSingleObject(child.process.get(), 0) == WAIT_TIMEOUT;
            bool const healthy = alive && child.evidence["windowSeen"].get<bool>() &&
                child.evidence["responsive"].get<bool>() && child.evidence["exceptions"].empty();
            report["healthy"] = healthy && !debuggerFailed;
            child.evidence["hookReady"] = Diagnostic(pid, L"hook-ready");
            child.evidence["aliasApplied"] = Diagnostic(pid, L"alias-collection-applied") ||
                Diagnostic(pid, L"alias-collection-partial");
            if (healthy) child.evidence["captureSaved"] = Capture(child.window, output / L"window.bmp");
        }
        report["processes"].push_back(std::move(child.evidence));
    }
    std::ofstream evidence(output / L"observation.json");
    evidence << report.dump(2) << '\n';
    if (!evidence.good()) return 7;
    std::cout << report.dump() << '\n';
    // The job owns only this launch tree; closing it ends the bounded observation.
    return debuggerFailed ? 8 : 0;
}
