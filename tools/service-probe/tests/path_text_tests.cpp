#include "../../../renderer/path_text.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, char const* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void TestReplaceFileName()
{
    using renderer::path_text::ReplaceFileName;

    std::array<wchar_t, 64> normal{};
    constexpr std::wstring_view original = L"C:\\MacType\\MacType.dll";
    std::copy(original.begin(), original.end(), normal.begin());
    normal[original.size()] = L'\0';
    Require(ReplaceFileName(normal, L"MTBootStrap64.dll"),
            "a normal replacement must succeed");
    Require(std::wstring_view(normal.data()) ==
                L"C:\\MacType\\MTBootStrap64.dll",
            "a normal replacement must preserve the directory");

    std::array<wchar_t, 13> exactFit{
        L'C', L':', L'\\', L'O', L'l', L'd', L'\\',
        L'x', L'.', L'd', L'l', L'l', L'\0'};
    Require(ReplaceFileName(exactFit, L"y.dll"),
            "a replacement that exactly fills the span must succeed");
    Require(std::wstring_view(exactFit.data()) == L"C:\\Old\\y.dll",
            "an exact-fit replacement must retain its terminator");

    std::array<wchar_t, 13> oneTooLong{
        L'C', L':', L'\\', L'O', L'l', L'd', L'\\',
        L'x', L'.', L'd', L'l', L'l', L'\0'};
    auto const oneTooLongBefore = oneTooLong;
    Require(!ReplaceFileName(oneTooLong, L"xx.dll"),
            "a replacement one character too long must fail");
    Require(oneTooLong == oneTooLongBefore,
            "a replacement that does not fit must leave the span unchanged");

    std::array<wchar_t, 16> noBackslash{
        L'f', L'i', L'l', L'e', L'.', L'd', L'l', L'l', L'\0'};
    auto const noBackslashBefore = noBackslash;
    Require(!ReplaceFileName(noBackslash, L"other.dll"),
            "a path without a backslash must fail");
    Require(noBackslash == noBackslashBefore,
            "a path without a backslash must remain unchanged");

    std::array<wchar_t, 4> unterminated{L'C', L':', L'\\', L'x'};
    auto const unterminatedBefore = unterminated;
    Require(!ReplaceFileName(unterminated, L"y"),
            "an unterminated span must fail");
    Require(unterminated == unterminatedBefore,
            "an unterminated span must remain unchanged");

    std::array<wchar_t, 32> emptyName{};
    constexpr std::wstring_view pathWithName = L"C:\\Old\\file.dll";
    std::copy(pathWithName.begin(), pathWithName.end(), emptyName.begin());
    emptyName[pathWithName.size()] = L'\0';
    Require(ReplaceFileName(emptyName, L""),
            "an empty file name must be accepted");
    Require(std::wstring_view(emptyName.data()) == L"C:\\Old\\",
            "an empty file name must leave the directory prefix");
}

void TestReplaceFileNameFromOverlappingView()
{
    using renderer::path_text::ReplaceFileName;

    std::array<wchar_t, 32> path{};
    constexpr std::wstring_view original = L"C:\\d\\abcdef";
    std::copy(original.begin(), original.end(), path.begin());
    path[original.size()] = L'\0';
    // The source "cdef" starts two characters after the destination, so the
    // two ranges overlap.
    std::wstring_view const tail(path.data() + 7, 4);
    Require(ReplaceFileName(path, tail),
            "a file name that views the same buffer must be replaced");
    Require(std::wstring_view(path.data()) == L"C:\\d\\cdef",
            "an overlapping replacement must copy the original characters");
}

std::wstring ReferenceModulePath(HMODULE module)
{
    std::vector<wchar_t> buffer(16);
    for (;;)
    {
        DWORD const capacity = static_cast<DWORD>(buffer.size());
        DWORD const length = GetModuleFileNameW(module, buffer.data(), capacity);
        Require(length != 0, "GetModuleFileNameW must read the test executable path");
        if (length < capacity)
            return std::wstring(buffer.data(), length);
        buffer.resize(buffer.size() * 2);
    }
}

void TestModuleDirectory()
{
    HMODULE const self = GetModuleHandleW(nullptr);
    Require(self != nullptr, "the test image must be loaded");

    std::optional<std::wstring> const directory =
        renderer::path_text::ModuleDirectory(self);
    Require(directory.has_value(),
            "the test executable directory must be available");

    // The reference starts with a 16-character buffer, so it also takes the
    // growth path that ModuleDirectory needs for paths longer than MAX_PATH.
    std::wstring const fullPath = ReferenceModulePath(self);
    std::size_t const separator = fullPath.find_last_of(L'\\');
    Require(separator != std::wstring::npos,
            "the test executable path must contain a directory");
    Require(*directory == fullPath.substr(0, separator),
            "ModuleDirectory must return the executable directory");
    Require(directory->empty() || directory->back() != L'\\',
            "ModuleDirectory must omit the trailing backslash");
}

} // namespace

int main()
{
    TestReplaceFileName();
    TestReplaceFileNameFromOverlappingView();
    TestModuleDirectory();
    std::cout << "path text tests passed\n";
    return 0;
}
