#include "../../../renderer/renderer_raii.h"
#include "../../../renderer/virtual_font_cache.h"

#include <aclapi.h>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sddl.h>
#include <stdexcept>
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
    comparison.result = renderer::virtual_font_cache::CompareFile(
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

void RequireAcl(bool condition, char const* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::wstring ReadEnvironmentVariable(WCHAR const* name)
{
    std::vector<WCHAR> buffer(32768);
    DWORD const length = GetEnvironmentVariableW(
        name, buffer.data(), static_cast<DWORD>(buffer.size()));
    RequireAcl(length != 0 && length < buffer.size(), "cannot save LOCALAPPDATA");
    return std::wstring(buffer.data(), length);
}

class CacheTestDirectory
{
public:
    CacheTestDirectory()
    {
        originalAppData_ = ReadEnvironmentVariable(L"LOCALAPPDATA");
        std::vector<WCHAR> temporary(32768);
        DWORD const length = GetTempPathW(
            static_cast<DWORD>(temporary.size()), temporary.data());
        RequireAcl(length != 0 && length < temporary.size(), "cannot locate TEMP");
        root = std::filesystem::path(std::wstring(temporary.data(), length)) /
            (L"mactype-font-cache-acl-" + std::to_wstring(GetCurrentProcessId()));
        RequireAcl(CreateDirectoryW(root.c_str(), nullptr) != FALSE,
            "cannot create a fresh cache test directory");
    }

    ~CacheTestDirectory()
    {
        SetEnvironmentVariableW(L"LOCALAPPDATA", originalAppData_.c_str());
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    CacheTestDirectory(CacheTestDirectory const&) = delete;
    CacheTestDirectory& operator=(CacheTestDirectory const&) = delete;

    std::filesystem::path root;

private:
    std::wstring originalAppData_;
};

renderer_raii::UniqueLocalMemory<void> ParseSid(WCHAR const* text)
{
    PSID rawSid = nullptr;
    RequireAcl(ConvertStringSidToSidW(text, &rawSid) != FALSE, "cannot parse test SID");
    return renderer_raii::UniqueLocalMemory<void>(rawSid);
}

void WriteFixture(std::filesystem::path const& path, std::vector<BYTE> const& bytes)
{
    auto file = renderer_raii::AdoptHandle(CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    RequireAcl(static_cast<bool>(file), "cannot create ACL fixture");
    DWORD written = 0;
    RequireAcl(WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
        &written, nullptr) != FALSE && written == bytes.size(),
        "cannot write ACL fixture");
}

void CheckReadGrants(std::wstring name, bool directory)
{
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
    DWORD const error = GetNamedSecurityInfoW(
        name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, &rawDescriptor);
    renderer_raii::UniqueLocalMemory<void> descriptor(rawDescriptor);
    RequireAcl(error == ERROR_SUCCESS && dacl != nullptr, "cannot read fixture DACL");
    for (WCHAR const* text : {L"S-1-5-32-545", L"S-1-15-2-1", L"S-1-15-2-2"})
    {
        auto sid = ParseSid(text);
        bool found = false;
        for (DWORD index = 0; index < dacl->AceCount; ++index)
        {
            void* rawAce = nullptr;
            RequireAcl(GetAce(dacl, index, &rawAce) != FALSE, "cannot read fixture ACE");
            auto const* header = static_cast<ACE_HEADER const*>(rawAce);
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
                continue;
            auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(rawAce);
            constexpr DWORD access = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
            BYTE const flags = directory ? OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE : INHERITED_ACE;
            if ((ace->Mask & access) == access &&
                (header->AceFlags & flags) == flags && EqualSid(&ace->SidStart, sid.get()))
                found = true;
        }
        RequireAcl(found, "missing shared read/execute ACE or required inheritance flags");
    }
}

void ProtectControl(std::wstring name, HANDLE token)
{
    DWORD size = 0;
    RequireAcl(!GetTokenInformation(token, TokenUser, nullptr, 0, &size) &&
        GetLastError() == ERROR_INSUFFICIENT_BUFFER, "cannot size token user");
    std::vector<BYTE> user(size);
    RequireAcl(GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE,
        "cannot read token user");
    auto system = ParseSid(L"S-1-5-18");
    std::array<PSID, 2> trustees = {{
        reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, system.get()}};
    std::array<EXPLICIT_ACCESS_W, 2> entries = {};
    for (size_t index = 0; index < entries.size(); ++index)
    {
        entries[index].grfAccessPermissions = FILE_ALL_ACCESS;
        entries[index].grfAccessMode = GRANT_ACCESS;
        entries[index].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[index].Trustee.ptstrName = static_cast<LPWSTR>(trustees[index]);
    }
    PACL rawDacl = nullptr;
    DWORD const error = SetEntriesInAclW(
        static_cast<ULONG>(entries.size()), entries.data(), nullptr, &rawDacl);
    renderer_raii::UniqueLocalMemory<ACL> dacl(rawDacl);
    RequireAcl(error == ERROR_SUCCESS, "cannot build control DACL");
    RequireAcl(SetNamedSecurityInfoW(
        name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, dacl.get(), nullptr) == ERROR_SUCCESS, "cannot protect control DACL");
}

void CheckRestrictedOpen(HANDLE token, std::wstring const& path, bool allowed)
{
    RequireAcl(ImpersonateLoggedOnUser(token) != FALSE, "cannot impersonate restricted token");
    auto file = renderer_raii::AdoptHandle(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    DWORD const error = file ? ERROR_SUCCESS : GetLastError();
    // Do not report failures or unwind while the thread still impersonates.
    Require(RevertToSelf() != FALSE, "cannot revert restricted impersonation");
    RequireAcl(allowed ? static_cast<bool>(file) : !file && error == ERROR_ACCESS_DENIED,
        allowed ? "restricted token cannot read cache fixture" : "negative control was not denied");
}

void TestCacheAcl(std::vector<BYTE> const& fixtureBytes)
{
    CacheTestDirectory temporary;
    auto const appdata = temporary.root / L"appdata";
    auto const directory = appdata / L"MacType" / L"FontCache";
    RequireAcl(std::filesystem::create_directories(directory), "cannot create cache tree");
    RequireAcl(SetEnvironmentVariableW(L"LOCALAPPDATA", appdata.c_str()) != FALSE,
        "cannot redirect LOCALAPPDATA");
    auto const preexisting = directory / L"preexisting.ttf";
    WriteFixture(preexisting, fixtureBytes);

    std::wstring path;
    RequireAcl(renderer::virtual_font_cache::GetCacheDirectory(path) == S_OK && path == directory.native(),
        "cache directory did not use the isolated fixture");
    std::wstring persistedPath;
    RequireAcl(renderer::virtual_font_cache::PersistFont(fixtureBytes, persistedPath) == S_OK,
        "cannot persist the cache fixture");
    CheckReadGrants(path, true);
    CheckReadGrants(preexisting.native(), false);
    CheckReadGrants(persistedPath, false);

    HANDLE rawToken = nullptr;
    RequireAcl(OpenProcessToken(GetCurrentProcess(),
        TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE, &rawToken) != FALSE,
        "cannot open process token");
    auto token = renderer_raii::AdoptHandle(rawToken);
    auto everyone = ParseSid(L"S-1-1-0");
    auto users = ParseSid(L"S-1-5-32-545");
    auto restrictedSid = ParseSid(L"S-1-5-12");
    std::array<SID_AND_ATTRIBUTES, 3> restricting = {{
        {everyone.get(), 0}, {users.get(), 0}, {restrictedSid.get(), 0}}};
    HANDLE rawRestricted = nullptr;
    RequireAcl(CreateRestrictedToken(token.get(), DISABLE_MAX_PRIVILEGE,
        0, nullptr, 0, nullptr, static_cast<DWORD>(restricting.size()),
        restricting.data(), &rawRestricted) != FALSE, "cannot create restricted token");
    auto restricted = renderer_raii::AdoptHandle(rawRestricted);
    CheckRestrictedOpen(restricted.get(), preexisting.native(), true);
    CheckRestrictedOpen(restricted.get(), persistedPath, true);

    auto const controlDirectory = temporary.root / L"control";
    RequireAcl(std::filesystem::create_directory(controlDirectory), "cannot create control directory");
    auto const control = controlDirectory / L"plain.bin";
    WriteFixture(control, fixtureBytes);
    ProtectControl(control.native(), token.get());
    CheckRestrictedOpen(restricted.get(), control.native(), false);
    RequireAcl(renderer::virtual_font_cache::GrantSharedReadAccess(path) == S_FALSE,
        "shared read grants were not idempotent");
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
    BYTE const lastByte = comparison.expected.back();
    comparison.expected.pop_back();
    Compare(comparison, true, false);
    comparison.path.append(L".mactype-missing-font-cache-test");
    Compare(comparison, false, false);
    std::cout << "DirectWrite font cache: 6 comparisons passed on 64 KB stacks\n";
    comparison.expected.push_back(lastByte);
    try
    {
        TestCacheAcl(comparison.expected);
    }
    catch (std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "DirectWrite font cache ACL: 3 DACL checks, 2 restricted reads, "
        "1 denied control and idempotence passed\n";
    return 0;
}
