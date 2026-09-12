#include <Windows.h>

DWORD WINAPI OverflowOnSmallStack(void*)
{
    volatile BYTE buffer[64 * 1024]{};
    for (size_t index = 0; index < sizeof(buffer); ++index)
        buffer[index] = static_cast<BYTE>(index);
    return buffer[0];
}

int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    HANDLE const thread = CreateThread(nullptr, 64 * 1024, OverflowOnSmallStack,
        nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread == nullptr) return 2;
    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    return 1;
}
