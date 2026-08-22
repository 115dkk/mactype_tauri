#include <Windows.h>

#include <cwchar>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3 ||
        (std::wcscmp(argv[2], L"load") != 0 &&
         std::wcscmp(argv[2], L"reject") != 0)) {
        return 64;
    }

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    HMODULE const module = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    bool const loaded = module != nullptr;
    bool const expectLoad = std::wcscmp(argv[2], L"load") == 0;
    // Process termination owns successful-test cleanup. Calling FreeLibrary
    // here would race the renderer's deliberately asynchronous startup probe.
    return loaded == expectLoad ? 0 : (loaded ? 2 : 3);
}
