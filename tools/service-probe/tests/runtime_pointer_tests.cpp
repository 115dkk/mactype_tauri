#include "../../../renderer/runtime_pointer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;

void Require(bool condition, char const* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class TempTree
{
public:
    TempTree()
    {
        WCHAR temporary[MAX_PATH]{};
        DWORD const length = GetTempPathW(_countof(temporary), temporary);
        Require(length != 0 && length < _countof(temporary),
            "the temporary directory must be available");
        for (unsigned int attempt = 0; attempt != 100; ++attempt)
        {
            root_ = fs::path(temporary) /
                (L"mactype-runtime-pointer-" +
                 std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()) + L"-" +
                 std::to_wstring(attempt));
            if (CreateDirectoryW(root_.c_str(), nullptr))
                return;
            Require(GetLastError() == ERROR_ALREADY_EXISTS,
                "the temporary test root must be creatable");
        }
        Require(false, "a unique temporary test root must be available");
    }

    ~TempTree()
    {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    TempTree(TempTree const&) = delete;
    TempTree& operator=(TempTree const&) = delete;

    fs::path const& root() const noexcept { return root_; }

private:
    fs::path root_;
};

void WriteFileBytes(fs::path const& path, std::string const& bytes)
{
    HANDLE const file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(file != INVALID_HANDLE_VALUE, "a fixture file must be creatable");
    DWORD written = 0;
    bool const wrote = bytes.empty() ||
        (WriteFile(
             file, bytes.data(), static_cast<DWORD>(bytes.size()),
             &written, nullptr) != FALSE &&
         written == static_cast<DWORD>(bytes.size()));
    CloseHandle(file);
    Require(wrote, "fixture bytes must be written completely");
}

std::wstring DirectoryString(fs::path const& path)
{
    std::wstring value = fs::absolute(path).lexically_normal().wstring();
    if (value.empty() || value.back() != L'\\')
        value.push_back(L'\\');
    return value;
}

bool Resolve(fs::path const& moduleDirectory, std::wstring& result)
{
    return renderer::runtime_pointer::CurrentRuntimeDirectory(
        DirectoryString(moduleDirectory), result);
}

std::string Pointer(std::string const& version, int schema = 1)
{
    return "{\"schema\":" + std::to_string(schema) +
        ",\"version\":\"" + version + "\"}";
}

} // namespace

int main()
{
    TempTree tree;
    fs::path const service = tree.root() / L"Service";
    fs::path const bin = service / L"bin";
    fs::path const retired = bin / L"retired";
    std::string const currentVersion =
        "0.2.0+ci.43103568277.ddea7f5b9fdd.current";
    fs::path const current = bin / fs::path(currentVersion);
    fs::create_directories(retired);
    fs::create_directories(current);
    WriteFileBytes(current / L"MacType.ini", "[General]\r\n");
    fs::path const pointerPath = service / L"current.json";

    WriteFileBytes(pointerPath, Pointer(currentVersion));
    std::wstring resolved;
    Require(Resolve(retired, resolved), "a valid current pointer must resolve");
    Require(_wcsicmp(resolved.c_str(), DirectoryString(current).c_str()) == 0,
        "the current pointer must resolve to its normalized runtime directory");
    Require(!resolved.empty() && resolved.back() == L'\\',
        "the resolved runtime directory must have a trailing backslash");

    fs::remove(pointerPath);
    Require(!Resolve(retired, resolved), "a missing pointer must be rejected");

    WriteFileBytes(pointerPath, Pointer(currentVersion, 2));
    Require(!Resolve(retired, resolved), "schema 2 must be rejected");

    WriteFileBytes(pointerPath, "\xEF\xBB\xBF" + Pointer(currentVersion));
    Require(!Resolve(retired, resolved), "a UTF-8 BOM must be rejected");

    WriteFileBytes(pointerPath, Pointer("..\\\\other"));
    Require(!Resolve(retired, resolved),
        "a version containing a backslash must be rejected");

    WriteFileBytes(pointerPath, Pointer("../other"));
    Require(!Resolve(retired, resolved),
        "a version containing a slash must be rejected");

    WriteFileBytes(pointerPath, Pointer(""));
    Require(!Resolve(retired, resolved), "an empty version must be rejected");

    WriteFileBytes(pointerPath, Pointer(std::string(129, 'a')));
    Require(!Resolve(retired, resolved),
        "a version longer than 128 characters must be rejected");

    WriteFileBytes(pointerPath, std::string(4097, ' '));
    Require(!Resolve(retired, resolved),
        "a pointer larger than 4096 bytes must be rejected");

    WriteFileBytes(pointerPath, Pointer("."));
    Require(!Resolve(retired, resolved), "a dot version must be rejected");

    WriteFileBytes(pointerPath, Pointer(".."));
    Require(!Resolve(retired, resolved), "a dot-dot version must be rejected");

    WriteFileBytes(retired / L"MacType.ini", "[General]\r\n");
    WriteFileBytes(pointerPath, Pointer("RETIRED"));
    Require(!Resolve(retired, resolved),
        "a pointer naming the module directory must be rejected");
    fs::remove(retired / L"MacType.ini");

    fs::remove(current / L"MacType.ini");
    WriteFileBytes(pointerPath, Pointer(currentVersion));
    Require(!Resolve(retired, resolved),
        "a target without MacType.ini must be rejected");

    return 0;
}
