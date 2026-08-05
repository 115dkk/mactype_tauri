#include "../../../detour_transaction.h"
#include "../../../renderer_raii.h"

#include <iostream>

namespace {

int close_handle_calls = 0;
int close_find_calls = 0;
int free_module_calls = 0;
int delete_dc_calls = 0;
int release_dc_calls = 0;
int delete_object_calls = 0;
int select_object_calls = 0;
int close_key_calls = 0;
int free_local_calls = 0;
int free_global_calls = 0;
int free_sid_calls = 0;
int free_environment_calls = 0;
int unmap_calls = 0;
int virtual_free_calls = 0;
int remote_virtual_free_calls = 0;
int heap_free_calls = 0;
int page_lock_calls = 0;
int page_unlock_calls = 0;
bool page_lock_succeeds = true;
int page_protect_calls = 0;
bool page_protect_succeeds = true;
DWORD last_page_protection = 0;
HGDIOBJ selected_previous = reinterpret_cast<HGDIOBJ>(static_cast<ULONG_PTR>(0x41));

struct FakeResourceApi
{
    static void CloseKernelHandle(HANDLE) noexcept
    {
        ++close_handle_calls;
    }
    static void CloseFindHandle(HANDLE) noexcept
    {
        ++close_find_calls;
    }
    static void FreeLoadedModule(HMODULE) noexcept
    {
        ++free_module_calls;
    }
    static void DeleteDeviceContext(HDC) noexcept
    {
        ++delete_dc_calls;
    }
    static void ReleaseDeviceContext(HWND, HDC) noexcept
    {
        ++release_dc_calls;
    }
    static void DeleteGdiObject(HGDIOBJ) noexcept
    {
        ++delete_object_calls;
    }
    static HGDIOBJ SelectGdiObject(HDC, HGDIOBJ) noexcept
    {
        ++select_object_calls;
        return selected_previous;
    }
    static void CloseRegistryKey(HKEY) noexcept
    {
        ++close_key_calls;
    }
    static void FreeLocalMemory(HLOCAL) noexcept
    {
        ++free_local_calls;
    }
    static void FreeGlobalMemory(HGLOBAL) noexcept
    {
        ++free_global_calls;
    }
    static void FreeSidMemory(PSID) noexcept
    {
        ++free_sid_calls;
    }
    static void FreeEnvironmentBlock(LPWSTR) noexcept
    {
        ++free_environment_calls;
    }
    static void UnmapView(void *) noexcept
    {
        ++unmap_calls;
    }
    static void FreeVirtualMemory(void *) noexcept
    {
        ++virtual_free_calls;
    }
    static void FreeRemoteVirtualMemory(HANDLE, void *) noexcept
    {
        ++remote_virtual_free_calls;
    }
    static void FreeHeapMemory(HANDLE, void *) noexcept
    {
        ++heap_free_calls;
    }
    static bool LockPages(void *, SIZE_T) noexcept
    {
        ++page_lock_calls;
        return page_lock_succeeds;
    }
    static void UnlockPages(void *, SIZE_T) noexcept
    {
        ++page_unlock_calls;
    }
    static bool ProtectMemory(void *, SIZE_T, DWORD protection, DWORD *previous) noexcept
    {
        ++page_protect_calls;
        last_page_protection = protection;
        if (!page_protect_succeeds)
            return false;
        *previous = PAGE_READONLY;
        return true;
    }
    static DWORD LastError() noexcept
    {
        return ERROR_WORKING_SET_QUOTA;
    }
};

int detour_begin_calls = 0;
int detour_update_calls = 0;
int detour_attach_calls = 0;
int detour_detach_calls = 0;
int detour_commit_calls = 0;
int detour_abort_calls = 0;
LONG detour_begin_result = NOERROR;
LONG detour_update_result = NOERROR;
LONG detour_attach_result = NOERROR;
LONG detour_detach_result = NOERROR;
LONG detour_commit_result = NOERROR;

struct FakeDetoursApi
{
    static LONG Begin() noexcept
    {
        ++detour_begin_calls;
        return detour_begin_result;
    }
    static LONG Abort() noexcept
    {
        ++detour_abort_calls;
        return NOERROR;
    }
    static LONG Commit() noexcept
    {
        ++detour_commit_calls;
        return detour_commit_result;
    }
    static LONG UpdateThread(HANDLE) noexcept
    {
        ++detour_update_calls;
        return detour_update_result;
    }
    static LONG Attach(PVOID*, PVOID) noexcept
    {
        ++detour_attach_calls;
        return detour_attach_result;
    }
    static LONG Detach(PVOID*, PVOID) noexcept
    {
        ++detour_detach_calls;
        return detour_detach_result;
    }
};

bool Expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

template <typename T>
T FakePointer(ULONG_PTR value)
{
    return reinterpret_cast<T>(value);
}

bool TestStandardOwnersCallMatchingReleaseExactlyOnce()
{
    using namespace renderer_raii;
    {
        auto invalid = AdoptHandle<FakeResourceApi>(INVALID_HANDLE_VALUE);
        auto handle = AdoptHandle<FakeResourceApi>(FakePointer<HANDLE>(1));
        BasicUniqueFindHandle<FakeResourceApi> find(FakePointer<HANDLE>(2));
        BasicUniqueModuleReference<FakeResourceApi> module(FakePointer<HMODULE>(3));
        BasicUniqueDeviceContext<FakeResourceApi> dc(FakePointer<HDC>(4));
        auto window_dc = AdoptWindowDeviceContext<FakeResourceApi>(
            FakePointer<HWND>(5), FakePointer<HDC>(6));
        BasicUniqueGdiObject<HFONT, FakeResourceApi> font(FakePointer<HFONT>(7));
        BasicUniqueRegistryKey<FakeResourceApi> key(FakePointer<HKEY>(8));
        BasicUniqueLocalMemory<unsigned char, FakeResourceApi> local(FakePointer<unsigned char*>(9));
        BasicUniqueGlobalMemory<unsigned char, FakeResourceApi> global(FakePointer<unsigned char*>(10));
        BasicUniqueSid<FakeResourceApi> sid(FakePointer<PSID>(11));
        BasicUniqueEnvironmentBlock<FakeResourceApi> environment(FakePointer<LPWSTR>(12));
        BasicUniqueMappedView<FakeResourceApi> view(FakePointer<void*>(13));
        BasicUniqueVirtualMemory<FakeResourceApi> allocation(FakePointer<void*>(14));
        auto remote_allocation = AdoptRemoteVirtualMemory<FakeResourceApi>(
            FakePointer<HANDLE>(15), FakePointer<void*>(16));
        BasicUniqueHeapMemory<unsigned char, FakeResourceApi> heap(
            FakePointer<unsigned char*>(17),
            renderer_raii::detail::HeapMemoryDeleter<unsigned char, FakeResourceApi>{FakePointer<HANDLE>(18)});
        BasicUniqueHandle<FakeResourceApi> moved = std::move(handle);
        if (!Expect(!invalid && !handle && moved, "standard owner move or invalid-handle normalization failed")) {
            return false;
        }
    }

    return Expect(close_handle_calls == 1, "CloseHandle must run exactly once") &&
        Expect(close_find_calls == 1, "FindClose must run exactly once") &&
        Expect(free_module_calls == 1, "FreeLibrary must run exactly once") &&
        Expect(delete_dc_calls == 1, "DeleteDC must run exactly once") &&
        Expect(release_dc_calls == 1, "ReleaseDC must run exactly once") &&
        Expect(delete_object_calls == 1, "DeleteObject must run exactly once") &&
        Expect(close_key_calls == 1, "RegCloseKey must run exactly once") &&
        Expect(free_local_calls == 1, "LocalFree must run exactly once") &&
        Expect(free_global_calls == 1, "GlobalFree must run exactly once") &&
        Expect(free_sid_calls == 1, "FreeSid must run exactly once") &&
        Expect(free_environment_calls == 1, "FreeEnvironmentStrings must run exactly once") &&
        Expect(unmap_calls == 1, "UnmapViewOfFile must run exactly once") &&
        Expect(virtual_free_calls == 1, "VirtualFree must run exactly once") &&
        Expect(remote_virtual_free_calls == 1, "VirtualFreeEx must run exactly once") &&
        Expect(heap_free_calls == 1, "HeapFree must run exactly once");
}

bool TestContextualLeasesRestoreOnEveryExit()
{
    using namespace renderer_raii;
    {
        auto selected = SelectObject<HFONT, FakeResourceApi>(
            FakePointer<HDC>(20), FakePointer<HFONT>(21));
        if (!Expect(static_cast<bool>(selected), "selected GDI object did not retain the previous object"))
        {
            return false;
        }
    }
    if (!Expect(select_object_calls == 2, "selected GDI object was not restored"))
    {
        return false;
    }

    page_lock_succeeds = false;
    {
        auto failed = BasicPageLock<FakeResourceApi>::TryLock(FakePointer<void *>(30), 4096);
        if (!Expect(!failed && failed.error() == ERROR_WORKING_SET_QUOTA,
                    "failed page lock did not preserve its error"))
        {
            return false;
        }
    }
    if (!Expect(page_unlock_calls == 0, "failed page lock attempted VirtualUnlock"))
    {
        return false;
    }

    page_lock_succeeds = true;
    {
        auto locked = BasicPageLock<FakeResourceApi>::TryLock(FakePointer<void *>(31), 8192);
        auto moved = std::move(locked);
        if (!Expect(!locked && moved && moved.size() == 8192, "page lock move lost its lease"))
        {
            return false;
        }
    }
    if (!Expect(page_lock_calls == 2, "VirtualLock call count changed") ||
        !Expect(page_unlock_calls == 1, "successful page lock was not released exactly once"))
    {
        return false;
    }

    page_protect_succeeds = false;
    {
        auto failed = BasicPageProtection<FakeResourceApi>::TrySet(
            FakePointer<void *>(32), sizeof(void *), PAGE_READWRITE);
        if (!Expect(!failed && failed.error() == ERROR_WORKING_SET_QUOTA,
                    "failed page protection did not preserve its error"))
        {
            return false;
        }
    }
    if (!Expect(page_protect_calls == 1,
                "failed page protection attempted an unexpected restore"))
    {
        return false;
    }

    page_protect_succeeds = true;
    {
        auto protection = BasicPageProtection<FakeResourceApi>::TrySet(
            FakePointer<void *>(33), sizeof(void *), PAGE_READWRITE);
        auto moved = std::move(protection);
        if (!Expect(!protection && moved,
                    "page protection move lost its restoration lease") ||
            !Expect(moved.restore(), "page protection restore failed"))
        {
            return false;
        }
    }
    return Expect(page_protect_calls == 3,
                  "page protection was not acquired and restored exactly once") &&
           Expect(last_page_protection == PAGE_READONLY,
                  "page protection did not restore the original protection");
}

bool TestExplicitCriticalSectionOwnerIsIdempotent()
{
    renderer_raii::CriticalSection criticalSection;
    if (!Expect(!criticalSection.initialized(), "critical section started initialized"))
    {
        return false;
    }
    criticalSection.initialize();
    criticalSection.initialize();
    EnterCriticalSection(criticalSection.get());
    LeaveCriticalSection(criticalSection.get());
    if (!Expect(criticalSection.initialized(), "critical section initialization was not retained"))
    {
        return false;
    }
    criticalSection.reset();
    criticalSection.reset();
    return Expect(!criticalSection.initialized(), "critical section reset was not idempotent");
}

void ResetDetours()
{
    detour_begin_calls = 0;
    detour_update_calls = 0;
    detour_attach_calls = 0;
    detour_detach_calls = 0;
    detour_commit_calls = 0;
    detour_abort_calls = 0;
    detour_begin_result = NOERROR;
    detour_update_result = NOERROR;
    detour_attach_result = NOERROR;
    detour_detach_result = NOERROR;
    detour_commit_result = NOERROR;
}

bool TestDetourTransactionFaultCleanup()
{
    using Transaction = renderer_raii::BasicDetourTransaction<FakeDetoursApi>;
    PVOID target = FakePointer<PVOID>(40);
    PVOID hook = FakePointer<PVOID>(41);

    ResetDetours();
    {
        Transaction transaction;
        if (!Expect(transaction.Attach(&target, hook) == NOERROR, "DetourAttach unexpectedly failed") ||
            !Expect(transaction.Commit() == NOERROR, "Detour commit unexpectedly failed")) {
            return false;
        }
    }
    if (!Expect(detour_commit_calls == 1 && detour_abort_calls == 0,
        "committed Detours transaction was aborted")) {
        return false;
    }

    ResetDetours();
    detour_attach_result = ERROR_INVALID_OPERATION;
    {
        Transaction transaction;
        if (!Expect(transaction.Attach(&target, hook) == ERROR_INVALID_OPERATION,
            "Detour attach failure was not retained") ||
            !Expect(transaction.Commit() == ERROR_INVALID_OPERATION,
                "failed Detours transaction did not report the first error")) {
            return false;
        }
    }
    if (!Expect(detour_commit_calls == 0 && detour_abort_calls == 1,
        "failed Detours transaction was not aborted exactly once")) {
        return false;
    }

    ResetDetours();
    {
        Transaction transaction;
        if (!Expect(transaction.Detach(&target, hook) == NOERROR, "DetourDetach unexpectedly failed")) {
            return false;
        }
    }
    if (!Expect(detour_abort_calls == 1, "uncommitted Detours transaction escaped without abort")) {
        return false;
    }

    ResetDetours();
    detour_begin_result = ERROR_BUSY;
    {
        Transaction transaction;
        if (!Expect(transaction.Commit() == ERROR_BUSY, "Detour begin failure was not retained")) {
            return false;
        }
    }
    return Expect(detour_update_calls == 0 && detour_abort_calls == 0,
        "failed DetourTransactionBegin was treated as active");
}

} // namespace

int main()
{
    if (!TestStandardOwnersCallMatchingReleaseExactlyOnce() ||
        !TestContextualLeasesRestoreOnEveryExit() ||
        !TestExplicitCriticalSectionOwnerIsIdempotent() ||
        !TestDetourTransactionFaultCleanup()) {
        return 1;
    }
    std::cout << "Renderer RAII fault-injection tests passed.\n";
    return 0;
}
