#include "../../../renderer/renderer_raii.h"
#include "../../../renderer/virtual_font_cache.h"

#include <aclapi.h>
#include <array>
#include <cstdlib>
#include <cstring>
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

void OverwriteFixture(
    std::filesystem::path const& path,
    std::vector<BYTE> const& bytes)
{
    auto file = renderer_raii::AdoptHandle(CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    RequireAcl(static_cast<bool>(file), "cannot replace manifest fixture");
    DWORD written = 0;
    RequireAcl(WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
        &written, nullptr) != FALSE && written == bytes.size(),
        "cannot replace complete manifest fixture");
}

std::vector<BYTE> ReadFixture(std::filesystem::path const& path)
{
    auto file = renderer_raii::AdoptHandle(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    RequireAcl(static_cast<bool>(file), "cannot open manifest fixture");
    LARGE_INTEGER size = {};
    RequireAcl(GetFileSizeEx(file.get(), &size) != FALSE && size.QuadPart >= 0 &&
        size.QuadPart <= 64 * 1024 * 1024, "invalid manifest fixture size");
    std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart));
    DWORD received = 0;
    RequireAcl(bytes.empty() || (ReadFile(file.get(), bytes.data(),
        static_cast<DWORD>(bytes.size()), &received, nullptr) != FALSE &&
        received == bytes.size()), "cannot read complete manifest fixture");
    return bytes;
}

void WriteLe32(std::vector<BYTE>& bytes, size_t offset, UINT32 value)
{
    RequireAcl(offset <= bytes.size() && bytes.size() - offset >= 4,
        "manifest mutation escapes fixture");
    for (unsigned int shift = 0; shift != 32; shift += 8)
        bytes[offset++] = static_cast<BYTE>(value >> shift);
}

void WriteLe64(std::vector<BYTE>& bytes, size_t offset, UINT64 value)
{
    RequireAcl(offset <= bytes.size() && bytes.size() - offset >= 8,
        "manifest mutation escapes fixture");
    for (unsigned int shift = 0; shift != 64; shift += 8)
        bytes[offset++] = static_cast<BYTE>(value >> shift);
}

UINT32 ReadLe32(std::vector<BYTE> const& bytes, size_t offset)
{
    RequireAcl(offset <= bytes.size() && bytes.size() - offset >= 4,
        "manifest read escapes fixture");
    return static_cast<UINT32>(bytes[offset]) |
        (static_cast<UINT32>(bytes[offset + 1]) << 8) |
        (static_cast<UINT32>(bytes[offset + 2]) << 16) |
        (static_cast<UINT32>(bytes[offset + 3]) << 24);
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

void TestManifest(std::vector<BYTE> const&)
{
    namespace cache = renderer::virtual_font_cache;
    CacheTestDirectory temporary;
    auto const appdata = temporary.root / L"manifest-appdata";
    auto const directory = appdata / L"MacType" / L"FontCache";
    RequireAcl(std::filesystem::create_directories(directory),
        "cannot create manifest cache tree");
    RequireAcl(SetEnvironmentVariableW(L"LOCALAPPDATA", appdata.c_str()) != FALSE,
        "cannot redirect manifest LOCALAPPDATA");

    auto const manifestV1 = directory / L"manifest-v1.bin";
    WriteFixture(manifestV1, {
        'M', 'T', 'F', 'M', 'V', '1', '\r', '\n', 0, 0, 0, 0,
    });

    cache::ManifestKey key;
    key.replacementPath = L"C:\\Windows\\Fonts\\fixture.ttf";
    key.replacementSize = 1234;
    key.replacementLastWriteTime = 5678;
    key.faceIndex = 2;
    key.aliasFamily = L"Manifest Alias";
    key.overrideWeight = true;
    key.weight = 500;
    key.profileDigest = 0x1122334455667788ULL;
    key.axes = {{0x74686777, 500.0f}, {0x68746477, 100.0f}};
    cache::ManifestValue value;
    bool found = true;
    RequireAcl(cache::ReloadManifestForTests() == S_OK &&
        cache::LookupManifestFont(key, value, found) == S_OK && !found,
        "manifest-v1.bin must never be read as a v2 hit");

    auto const virtualFont = directory / L"cached.ttf";
    std::vector<BYTE> validFont = {
        0x00, 0x01, 0x00, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
    };
    WriteFixture(virtualFont, validFont);
    value.virtualFontPath = virtualFont.native();
    value.virtualFontSize = validFont.size();
    value.family = L"Manifest Alias";
    value.subfamily = L"Medium";
    value.fullName = L"Manifest Alias Medium";
    value.postScriptName = L"ManifestAlias-Medium";
    RequireAcl(cache::StoreManifestFont(key, value) == S_OK,
        "cannot atomically store a manifest entry");

    auto const manifestV2 = directory / L"manifest-v2.bin";
    std::vector<BYTE> const validManifest = ReadFixture(manifestV2);
    RequireAcl(cache::ReloadManifestForTests() == S_OK,
        "cannot parse the written manifest into a fresh state");
    cache::ManifestValue loaded;
    found = false;
    RequireAcl(cache::LookupManifestFont(key, loaded, found) == S_OK && found &&
        loaded.virtualFontPath == value.virtualFontPath &&
        loaded.virtualFontSize == value.virtualFontSize &&
        loaded.family == value.family && loaded.subfamily == value.subfamily &&
        loaded.fullName == value.fullName &&
        loaded.postScriptName == value.postScriptName,
        "a freshly parsed manifest value did not round-trip every field");

    auto expectKeyMiss = [&](cache::ManifestKey const& changed) {
        cache::ManifestValue ignored;
        bool hit = true;
        RequireAcl(cache::LookupManifestFont(changed, ignored, hit) == S_OK && !hit,
            "a manifest key field did not participate in lookup");
    };
    cache::ManifestKey changed = key;
    changed.replacementPath.push_back(L'x');
    expectKeyMiss(changed);
    changed = key;
    ++changed.replacementSize;
    expectKeyMiss(changed);
    changed = key;
    ++changed.replacementLastWriteTime;
    expectKeyMiss(changed);
    changed = key;
    ++changed.faceIndex;
    expectKeyMiss(changed);
    changed = key;
    changed.aliasFamily.push_back(L'x');
    expectKeyMiss(changed);
    changed = key;
    changed.overrideWeight = false;
    expectKeyMiss(changed);
    changed = key;
    ++changed.weight;
    expectKeyMiss(changed);
    changed = key;
    ++changed.profileDigest;
    expectKeyMiss(changed);
    changed = key;
    ++changed.axes[0].tag;
    expectKeyMiss(changed);
    changed = key;
    changed.axes[0].value = 501.0f;
    expectKeyMiss(changed);
    changed = key;
    changed.axes.pop_back();
    expectKeyMiss(changed);

    cache::ManifestKey temporaryKey = key;
    ++temporaryKey.profileDigest;
    std::vector<BYTE> temporaryManifest = validManifest;
    WriteLe64(temporaryManifest, 16 + 32, temporaryKey.profileDigest);
    auto const temporaryPath = directory / L"manifest-v2.bin.tmp-concurrent";
    WriteFixture(temporaryPath, temporaryManifest);
    RequireAcl(cache::ReloadManifestForTests() == S_OK,
        "cannot reload the manifest beside a concurrent temporary file");
    expectKeyMiss(temporaryKey);
    RequireAcl(std::filesystem::remove(temporaryPath),
        "cannot remove concurrent manifest fixture");

    std::vector<BYTE> oversizedCount = validManifest;
    WriteLe32(oversizedCount, 12, 2049);
    OverwriteFixture(manifestV2, oversizedCount);
    RequireAcl(cache::ReloadManifestForTests() == S_OK,
        "cannot reload oversized-count manifest fixture");
    found = true;
    RequireAcl(cache::LookupManifestFont(key, loaded, found) == S_OK && !found,
        "an oversized manifest record count was accepted");

    std::vector<BYTE> oversizedString = validManifest;
    UINT32 const axisCount = ReadLe32(oversizedString, 16 + 52);
    size_t const firstStringLength = 16 + 56 + static_cast<size_t>(axisCount) * 8;
    WriteLe32(oversizedString, firstStringLength, 32769);
    OverwriteFixture(manifestV2, oversizedString);
    RequireAcl(cache::ReloadManifestForTests() == S_OK,
        "cannot reload oversized-string manifest fixture");
    found = true;
    RequireAcl(cache::LookupManifestFont(key, loaded, found) == S_OK && !found,
        "an oversized manifest string was accepted");

    OverwriteFixture(manifestV2, validManifest);
    RequireAcl(cache::ReloadManifestForTests() == S_OK,
        "cannot restore the valid manifest fixture");
    int rebuilds = 0;
    auto lookupOrRebuild = [&] {
        cache::ManifestValue candidate;
        bool hit = false;
        RequireAcl(cache::LookupManifestFont(key, candidate, hit) == S_OK,
            "manifest lookup failed during rebuild test");
        if (hit)
            return;
        ++rebuilds;
        OverwriteFixture(virtualFont, validFont);
        RequireAcl(cache::StoreManifestFont(key, value) == S_OK,
            "cannot restore a rejected manifest backing");
    };
    lookupOrRebuild();
    RequireAcl(rebuilds == 0, "a valid manifest backing was rebuilt");
    std::vector<BYTE> wrongSizeFont = validFont;
    wrongSizeFont.push_back(0);
    OverwriteFixture(virtualFont, wrongSizeFont);
    lookupOrRebuild();
    RequireAcl(rebuilds == 1,
        "a size-mismatched manifest backing did not trigger one rebuild");
    std::vector<BYTE> corruptFont(validFont.size(), 0x7f);
    OverwriteFixture(virtualFont, corruptFont);
    lookupOrRebuild();
    RequireAcl(rebuilds == 2,
        "a corrupt non-empty manifest backing did not trigger one rebuild");
    found = false;
    RequireAcl(cache::LookupManifestFont(key, loaded, found) == S_OK && found,
        "the rebuilt manifest backing was not reusable");

    std::vector<BYTE> oneEntry = ReadFixture(manifestV2);
    RequireAcl(ReadLe32(oneEntry, 12) == 1 && oneEntry.size() > 16,
        "the eviction fixture requires one serialized entry");
    std::vector<BYTE> record(oneEntry.begin() + 16, oneEntry.end());
    std::vector<BYTE> full(oneEntry.begin(), oneEntry.begin() + 16);
    WriteLe32(full, 12, 2048);
    for (UINT64 index = 0; index < 2048; ++index)
    {
        std::vector<BYTE> entry = record;
        WriteLe64(entry, 0, index + 1);
        WriteLe64(entry, 32, index);
        full.insert(full.end(), entry.begin(), entry.end());
    }
    OverwriteFixture(manifestV2, full);
    RequireAcl(cache::ReloadManifestForTests() == S_OK,
        "cannot load the full eviction fixture");
    cache::ManifestKey newest = key;
    newest.profileDigest = 4096;
    RequireAcl(cache::StoreManifestFont(newest, value) == S_OK,
        "cannot append the eviction trigger");
    RequireAcl(cache::ReloadManifestForTests() == S_OK,
        "cannot reload the evicted manifest");
    cache::ManifestKey oldest = key;
    oldest.profileDigest = 0;
    found = true;
    RequireAcl(cache::LookupManifestFont(oldest, loaded, found) == S_OK && !found,
        "the oldest entry survived beyond the 2,048-entry bound");
    found = false;
    RequireAcl(cache::LookupManifestFont(newest, loaded, found) == S_OK && found &&
        ReadLe32(ReadFixture(manifestV2), 12) == 2048,
        "manifest eviction did not retain the newest bounded entry");

    for (auto const& child : std::filesystem::directory_iterator(directory))
    {
        RequireAcl(child.path().filename().native().find(L".tmp-") ==
            std::wstring::npos, "an atomic manifest temporary file leaked");
    }
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
        TestManifest(comparison.expected);
    }
    catch (std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "DirectWrite font cache ACL: 3 DACL checks, 2 restricted reads, "
        "1 denied control and idempotence passed\n";
    std::cout << "DirectWrite font manifest: v2 round-trip, bounds, eviction, "
        "atomic isolation and corrupt-backing rebuild passed" << std::endl;
    return 0;
}
