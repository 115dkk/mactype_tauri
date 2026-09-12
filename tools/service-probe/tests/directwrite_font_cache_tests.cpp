#include <cstdlib>
#include <iostream>

// Exercise the private cache reader itself without installing renderer hooks
// or writing into the user's font cache.
#include "../../../renderer/directwrite_virtual_font.cpp"

namespace {

void Require(bool condition, char const* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct Comparison
{
    std::wstring path;
    std::vector<BYTE> expected;
    HRESULT result = E_UNEXPECTED;
    bool exists = false;
    bool matches = false;
};

DWORD WINAPI CompareOnSmallStack(void* argument)
{
    auto& comparison = *static_cast<Comparison*>(argument);
    comparison.result = directwrite_virtual_font::FileMatches(
        comparison.path, comparison.expected,
        comparison.exists, comparison.matches);
    return 0;
}

void Compare(Comparison& comparison, bool exists, bool matches)
{
    // A regression must reset both outputs, not preserve a previous success.
    comparison.exists = !exists;
    comparison.matches = !matches;
    renderer_raii::UniqueHandle thread = renderer_raii::AdoptHandle(CreateThread(
        nullptr, 64 * 1024, CompareOnSmallStack, &comparison,
        STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
    Require(static_cast<bool>(thread), "cannot create the 64 KB stack thread");
    Require(WaitForSingleObject(thread.get(), 30000) == WAIT_OBJECT_0,
        "cache comparison did not finish on a 64 KB stack");
    DWORD exitCode = 1;
    Require(GetExitCodeThread(thread.get(), &exitCode) && exitCode == 0,
        "cache comparison thread failed");
    Require(comparison.result == S_OK && comparison.exists == exists &&
        comparison.matches == matches, "unexpected cache comparison result");
}

} // namespace

int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    WCHAR modulePath[MAX_PATH] = {};
    DWORD const length = GetModuleFileNameW(
        GetModuleHandleW(L"kernel32.dll"), modulePath, MAX_PATH);
    Require(length != 0 && length < MAX_PATH, "cannot locate the read-only fixture");
    Comparison comparison;
    comparison.path.assign(modulePath, length);
    renderer_raii::UniqueHandle file = renderer_raii::AdoptHandle(CreateFileW(
        modulePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Require(static_cast<bool>(file), "cannot open the read-only fixture");
    LARGE_INTEGER size = {};
    Require(GetFileSizeEx(file.get(), &size) && size.QuadPart > 128 * 1024 &&
        size.QuadPart < 16 * 1024 * 1024, "fixture must span multiple comparison chunks");
    comparison.expected.resize(static_cast<size_t>(size.QuadPart));
    DWORD received = 0;
    Require(ReadFile(file.get(), comparison.expected.data(),
        static_cast<DWORD>(comparison.expected.size()), &received, nullptr) &&
        received == comparison.expected.size(), "cannot read the complete fixture");

    Compare(comparison, true, true);
    for (size_t offset : {size_t{0}, size_t{64 * 1024}, comparison.expected.size() - 1})
    {
        comparison.expected[offset] ^= 1;
        Compare(comparison, true, false);
        comparison.expected[offset] ^= 1;
    }
    comparison.expected.pop_back();
    Compare(comparison, true, false);
    comparison.path.append(L".mactype-missing-font-cache-test");
    Compare(comparison, false, false);
    std::cout << "DirectWrite font cache: 6 comparisons passed on 64 KB stacks\n";
    return 0;
}
