#include "../../../renderer/module_name.h"

#include <cstdlib>
#include <iostream>

#include <windows.h>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct SmallStackProbe
{
    HMODULE module = nullptr;
    bool matched = false;
    bool rejectedOtherName = false;
    bool rejectedNullModule = false;
};

DWORD WINAPI CompareOnSmallStack(LPVOID parameter)
{
    auto* probe = static_cast<SmallStackProbe*>(parameter);
    probe->matched =
        renderer::module_name::BaseNameEquals(probe->module, L"kernel32.dll");
    probe->rejectedOtherName =
        !renderer::module_name::BaseNameEquals(probe->module, L"ntdll.dll");
    probe->rejectedNullModule =
        !renderer::module_name::BaseNameEquals(nullptr, L"kernel32.dll");
    return 0;
}

} // namespace

int main()
{
    using renderer::module_name::BaseNameEquals;

    HMODULE const kernel32 = GetModuleHandleW(L"kernel32.dll");
    Require(kernel32 != nullptr, "kernel32.dll must be loaded");
    HMODULE const self = GetModuleHandleW(nullptr);
    Require(self != nullptr, "the test image must be loaded");

    Require(BaseNameEquals(kernel32, L"kernel32.dll"),
            "the exact base name must match");
    Require(BaseNameEquals(kernel32, L"KERNEL32.DLL"),
            "the comparison must ignore case");
    Require(!BaseNameEquals(kernel32, L"kernel32"),
            "a missing extension must not match");
    Require(!BaseNameEquals(kernel32, L"ntdll.dll"),
            "a different module name must not match");
    Require(!BaseNameEquals(kernel32, L"System32\\kernel32.dll"),
            "a directory prefix must not match the base name");
    Require(!BaseNameEquals(self, L"kernel32.dll"),
            "the test image must not match another module's name");
    Require(!BaseNameEquals(nullptr, L"kernel32.dll"),
            "a null module must be rejected");
    Require(!BaseNameEquals(kernel32, nullptr),
            "a null expected name must be rejected");
    Require(!BaseNameEquals(kernel32, L""),
            "an empty expected name must be rejected");

    // Loader hooks observe module loads from foreign threads whose stacks can
    // be as small as 64 KB. The comparison must complete on such a thread
    // without exhausting it.
    SmallStackProbe probe;
    probe.module = kernel32;
    HANDLE const thread = CreateThread(
        nullptr, 64 * 1024, CompareOnSmallStack, &probe,
        STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    Require(thread != nullptr, "the 64 KB stack thread must start");
    Require(WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0,
            "the 64 KB stack thread must finish");
    DWORD exitCode = 1;
    Require(GetExitCodeThread(thread, &exitCode) && exitCode == 0,
            "the 64 KB stack thread must exit cleanly");
    CloseHandle(thread);
    Require(probe.matched && probe.rejectedOtherName && probe.rejectedNullModule,
            "the comparison must succeed on a 64 KB thread stack");

    std::cout << "module name tests passed\n";
    return 0;
}
