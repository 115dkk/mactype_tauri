#include "virtual_font_cache.h"
#include "renderer_raii.h"

#include <aclapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bcrypt.h>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <sddl.h>
#include <utility>

#pragma comment(lib, "bcrypt.lib")

namespace renderer {
namespace virtual_font_cache {
namespace {

// CompareFile runs behind DirectWrite factory hooks on application threads,
// so its comparison chunk lives on the heap rather than in a 64 KB frame.
constexpr std::size_t kCompareChunkBytes = 64 * 1024;

struct AlgorithmProviderCloser
{
    void operator()(void* value) const noexcept
    {
        BCryptCloseAlgorithmProvider(
            static_cast<BCRYPT_ALG_HANDLE>(value), 0);
    }
};

struct HashCloser
{
    void operator()(void* value) const noexcept
    {
        BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(value));
    }
};

using UniqueAlgorithmProvider =
    std::unique_ptr<void, AlgorithmProviderCloser>;
using UniqueHash = std::unique_ptr<void, HashCloser>;

HRESULT HashBytes(
    std::vector<BYTE> const& bytes,
    std::array<BYTE, 32>& digest)
{
    BCRYPT_ALG_HANDLE rawAlgorithm = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &rawAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status))
        return HRESULT_FROM_NT(status);
    UniqueAlgorithmProvider algorithm(rawAlgorithm);

    DWORD objectSize = 0;
    DWORD copied = 0;
    status = BCryptGetProperty(
        algorithm.get(), BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &copied, 0);
    if (!BCRYPT_SUCCESS(status) || copied != sizeof(objectSize))
        return BCRYPT_SUCCESS(status) ? E_FAIL : HRESULT_FROM_NT(status);

    std::vector<BYTE> hashObject(objectSize);
    BCRYPT_HASH_HANDLE rawHash = nullptr;
    status = BCryptCreateHash(
        algorithm.get(), &rawHash,
        hashObject.empty() ? nullptr : hashObject.data(), objectSize,
        nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status))
        return HRESULT_FROM_NT(status);
    UniqueHash hash(rawHash);

    status = BCryptHashData(
        hash.get(), const_cast<PUCHAR>(bytes.data()),
        static_cast<ULONG>(bytes.size()), 0);
    if (!BCRYPT_SUCCESS(status))
        return HRESULT_FROM_NT(status);
    status = BCryptFinishHash(
        hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0);
    return BCRYPT_SUCCESS(status) ? S_OK : HRESULT_FROM_NT(status);
}

std::wstring HexDigest(std::array<BYTE, 32> const& digest)
{
    constexpr WCHAR digits[] = L"0123456789abcdef";
    std::wstring value(digest.size() * 2, L'0');
    for (size_t index = 0; index < digest.size(); ++index)
    {
        value[index * 2] = digits[digest[index] >> 4];
        value[index * 2 + 1] = digits[digest[index] & 0x0f];
    }
    return value;
}

HRESULT EnsureDirectory(std::wstring const& path)
{
    if (CreateDirectoryW(path.c_str(), nullptr))
        return S_OK;
    DWORD const error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS)
        return HRESULT_FROM_WIN32(error);
    DWORD const attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    return S_OK;
}

HRESULT ReadEnvironmentVariable(
    WCHAR const* name,
    std::wstring& value)
{
    value.clear();
    for (unsigned int attempt = 0; attempt < 3; ++attempt)
    {
        DWORD const required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0)
            return HRESULT_FROM_WIN32(GetLastError());
        std::vector<WCHAR> buffer(required);
        DWORD const copied = GetEnvironmentVariableW(
            name, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0)
            return HRESULT_FROM_WIN32(GetLastError());
        if (copied < buffer.size())
        {
            value.assign(buffer.data(), copied);
            return S_OK;
        }
    }
    return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
}

void AppendPathComponent(std::wstring& path, WCHAR const* component)
{
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
        path.push_back(L'\\');
    path.append(component);
}

HRESULT GetCacheDirectoryImpl(std::wstring& path)
{
    HRESULT result = ReadEnvironmentVariable(L"LOCALAPPDATA", path);
    if (FAILED(result) || path.empty())
    {
        std::array<WCHAR, MAX_PATH + 1> temporary = {};
        DWORD const length = GetTempPathW(
            static_cast<DWORD>(temporary.size()), temporary.data());
        if (length == 0 || length >= temporary.size())
            return HRESULT_FROM_WIN32(
                length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);
        path.assign(temporary.data(), length);
    }

    AppendPathComponent(path, L"MacType");
    result = EnsureDirectory(path);
    if (FAILED(result))
        return result;
    AppendPathComponent(path, L"FontCache");
    result = EnsureDirectory(path);
    if (FAILED(result))
        return result;
    static std::atomic<bool> sharedReadAttempted(false);
    if (!sharedReadAttempted.exchange(true))
    {
        // Sandboxed callers lack WRITE_DAC; an ordinary process can repair it later.
        GrantSharedReadAccess(path);
    }
    return S_OK;
}

HRESULT CompareFileImpl(
    std::wstring const& path,
    std::vector<BYTE> const& expected,
    bool& exists,
    bool& matches)
{
    exists = false;
    matches = false;
    renderer_raii::UniqueHandle file = renderer_raii::AdoptHandle(
        CreateFileW(
            path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file)
    {
        DWORD const error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return S_OK;
        return HRESULT_FROM_WIN32(error);
    }
    exists = true;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file.get(), &size))
        return HRESULT_FROM_WIN32(GetLastError());
    if (size.QuadPart != static_cast<LONGLONG>(expected.size()))
        return S_OK;

    std::vector<BYTE> buffer(kCompareChunkBytes);
    size_t offset = 0;
    while (offset < expected.size())
    {
        DWORD const wanted = static_cast<DWORD>((std::min)(
            buffer.size(), expected.size() - offset));
        DWORD received = 0;
        if (!ReadFile(file.get(), buffer.data(), wanted, &received, nullptr))
            return HRESULT_FROM_WIN32(GetLastError());
        if (received != wanted ||
            memcmp(buffer.data(), expected.data() + offset, wanted) != 0)
            return S_OK;
        offset += received;
    }
    matches = true;
    return S_OK;
}

HRESULT WriteAll(HANDLE file, std::vector<BYTE> const& bytes)
{
    size_t offset = 0;
    while (offset < bytes.size())
    {
        DWORD const wanted = static_cast<DWORD>((std::min)(
            bytes.size() - offset,
            static_cast<size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!WriteFile(
                file, bytes.data() + offset, wanted, &written, nullptr))
            return HRESULT_FROM_WIN32(GetLastError());
        if (written == 0)
            return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        offset += written;
    }
    return S_OK;
}

class PendingCacheFile
{
public:
    explicit PendingCacheFile(std::wstring path) : path_(std::move(path)) {}
    ~PendingCacheFile()
    {
        if (!committed_)
            DeleteFileW(path_.c_str());
    }

    PendingCacheFile(PendingCacheFile const&) = delete;
    PendingCacheFile& operator=(PendingCacheFile const&) = delete;

    WCHAR const* path() const noexcept { return path_.c_str(); }
    void Commit() noexcept { committed_ = true; }

private:
    std::wstring path_;
    bool committed_ = false;
};

HRESULT PersistFontImpl(
    std::vector<BYTE> const& bytes,
    std::wstring& path)
{
    std::array<BYTE, 32> digest = {};
    HRESULT result = HashBytes(bytes, digest);
    if (FAILED(result))
        return result;

    std::wstring directory;
    result = GetCacheDirectory(directory);
    if (FAILED(result))
        return result;

    std::wstring const stem = HexDigest(digest);
    static std::atomic<ULONG> sequence(0);
    for (unsigned int attempt = 0; attempt < 32; ++attempt)
    {
        path = directory;
        AppendPathComponent(path, stem.c_str());
        if (attempt != 0)
        {
            path.push_back(L'-');
            path.append(std::to_wstring(GetCurrentProcessId()));
            path.push_back(L'-');
            path.append(std::to_wstring(++sequence));
        }
        path.append(L".ttf");

        bool exists = false;
        bool matches = false;
        result = CompareFile(path, bytes, exists, matches);
        if (FAILED(result))
            return result;
        if (matches)
            return S_OK;
        if (exists)
            continue;

        std::wstring temporaryPath = path;
        temporaryPath.append(L".tmp-");
        temporaryPath.append(std::to_wstring(GetCurrentProcessId()));
        temporaryPath.push_back(L'-');
        temporaryPath.append(std::to_wstring(GetCurrentThreadId()));
        temporaryPath.push_back(L'-');
        temporaryPath.append(std::to_wstring(++sequence));
        PendingCacheFile temporary(std::move(temporaryPath));
        renderer_raii::UniqueHandle file = renderer_raii::AdoptHandle(
            CreateFileW(
                temporary.path(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file)
        {
            if (GetLastError() == ERROR_FILE_EXISTS)
                continue;
            return HRESULT_FROM_WIN32(GetLastError());
        }
        result = WriteAll(file.get(), bytes);
        if (SUCCEEDED(result) && !FlushFileBuffers(file.get()))
            result = HRESULT_FROM_WIN32(GetLastError());
        file.reset();
        if (FAILED(result))
            return result;

        if (MoveFileExW(
                temporary.path(), path.c_str(), MOVEFILE_WRITE_THROUGH))
        {
            temporary.Commit();
            return S_OK;
        }
        DWORD const moveError = GetLastError();
        result = CompareFile(path, bytes, exists, matches);
        if (SUCCEEDED(result) && matches)
            return S_OK;
        if (FAILED(result))
            return result;
        if (moveError != ERROR_ALREADY_EXISTS && moveError != ERROR_FILE_EXISTS)
            return HRESULT_FROM_WIN32(moveError);
    }
    return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
}

} // namespace

// Sandboxed renderer-side processes (a browser GPU process under a restricted
// token, an AppContainer) open the aliased file by path. The user profile DACL
// denies them, and a failed open there is rasterised silently with a fallback
// face, so the cache tree carries the read grants of %WINDIR%\Fonts.
HRESULT GrantSharedReadAccess(std::wstring const& directory) noexcept
{
    try
    {
        std::vector<BYTE> users(SECURITY_MAX_SID_SIZE);
        std::vector<BYTE> packages(SECURITY_MAX_SID_SIZE);
        DWORD size = static_cast<DWORD>(users.size());
        if (!CreateWellKnownSid(WinBuiltinUsersSid, nullptr, users.data(), &size))
            return HRESULT_FROM_WIN32(GetLastError());
        size = static_cast<DWORD>(packages.size());
        if (!CreateWellKnownSid(WinBuiltinAnyPackageSid, nullptr, packages.data(), &size))
            return HRESULT_FROM_WIN32(GetLastError());
        PSID rawRestricted = nullptr;
        if (!ConvertStringSidToSidW(L"S-1-15-2-2", &rawRestricted))
            return HRESULT_FROM_WIN32(GetLastError());
        renderer_raii::UniqueLocalMemory<void> restricted(rawRestricted);
        std::array<PSID, 3> trustees = {{users.data(), packages.data(), restricted.get()}};

        std::wstring name = directory;
        PACL existingDacl = nullptr;
        PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
        DWORD error = GetNamedSecurityInfoW(
            &name[0], SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &existingDacl, nullptr, &rawDescriptor);
        renderer_raii::UniqueLocalMemory<void> descriptor(rawDescriptor);
        if (error != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(error);

        constexpr DWORD access = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
        constexpr BYTE inheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
        std::array<bool, 3> found = {};
        if (existingDacl != nullptr)
        {
            for (DWORD index = 0; index < existingDacl->AceCount; ++index)
            {
                void* rawAce = nullptr;
                if (!GetAce(existingDacl, index, &rawAce))
                    return HRESULT_FROM_WIN32(GetLastError());
                auto const* header = static_cast<ACE_HEADER const*>(rawAce);
                if (header->AceType != ACCESS_ALLOWED_ACE_TYPE ||
                    (header->AceFlags & inheritance) != inheritance)
                    continue;
                auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(rawAce);
                if ((ace->Mask & access) != access)
                    continue;
                for (size_t trustee = 0; trustee < trustees.size(); ++trustee)
                {
                    if (EqualSid(&ace->SidStart, trustees[trustee]))
                        found[trustee] = true;
                }
            }
        }
        if (std::all_of(found.begin(), found.end(), [](bool value) { return value; }))
            return S_FALSE;

        std::array<EXPLICIT_ACCESS_W, 3> entries = {};
        for (size_t index = 0; index < entries.size(); ++index)
        {
            entries[index].grfAccessPermissions = access;
            entries[index].grfAccessMode = GRANT_ACCESS;
            entries[index].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
            entries[index].Trustee.TrusteeForm = TRUSTEE_IS_SID;
            entries[index].Trustee.ptstrName = static_cast<LPWSTR>(trustees[index]);
        }
        PACL rawMerged = nullptr;
        error = SetEntriesInAclW(
            static_cast<ULONG>(entries.size()), entries.data(), existingDacl, &rawMerged);
        renderer_raii::UniqueLocalMemory<ACL> merged(rawMerged);
        if (error != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(error);
        error = SetNamedSecurityInfoW(
            &name[0], SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, merged.get(), nullptr);
        return HRESULT_FROM_WIN32(error);
    }
    catch (std::bad_alloc const&)
    {
        return E_OUTOFMEMORY;
    }
    catch (...)
    {
        return E_FAIL;
    }
}

HRESULT CompareFile(
    std::wstring const& path,
    std::vector<BYTE> const& expected,
    bool& exists,
    bool& matches) noexcept
{
    exists = false;
    matches = false;
    try
    {
        return CompareFileImpl(path, expected, exists, matches);
    }
    catch (std::bad_alloc const&)
    {
        return E_OUTOFMEMORY;
    }
    catch (...)
    {
        return E_FAIL;
    }
}

HRESULT GetCacheDirectory(std::wstring& path) noexcept
{
    try
    {
        return GetCacheDirectoryImpl(path);
    }
    catch (std::bad_alloc const&)
    {
        return E_OUTOFMEMORY;
    }
    catch (...)
    {
        return E_FAIL;
    }
}

HRESULT PersistFont(
    std::vector<BYTE> const& bytes,
    std::wstring& path) noexcept
{
    try
    {
        return PersistFontImpl(bytes, path);
    }
    catch (std::bad_alloc const&)
    {
        return E_OUTOFMEMORY;
    }
    catch (...)
    {
        return E_FAIL;
    }
}

} // namespace virtual_font_cache
} // namespace renderer
