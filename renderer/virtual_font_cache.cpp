#include "virtual_font_cache.h"
#include "renderer_raii.h"

#include <aclapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bcrypt.h>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
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

[[nodiscard]] std::expected<std::array<BYTE, 32>, HRESULT> HashBytes(
    std::vector<BYTE> const& bytes)
{
    BCRYPT_ALG_HANDLE rawAlgorithm = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &rawAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status))
        return std::unexpected(HRESULT_FROM_NT(status));
    UniqueAlgorithmProvider algorithm(rawAlgorithm);

    DWORD objectSize = 0;
    DWORD copied = 0;
    status = BCryptGetProperty(
        algorithm.get(), BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &copied, 0);
    if (!BCRYPT_SUCCESS(status) || copied != sizeof(objectSize))
        return std::unexpected(
            BCRYPT_SUCCESS(status) ? E_FAIL : HRESULT_FROM_NT(status));

    std::vector<BYTE> hashObject(objectSize);
    BCRYPT_HASH_HANDLE rawHash = nullptr;
    status = BCryptCreateHash(
        algorithm.get(), &rawHash,
        hashObject.empty() ? nullptr : hashObject.data(), objectSize,
        nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status))
        return std::unexpected(HRESULT_FROM_NT(status));
    UniqueHash hash(rawHash);

    status = BCryptHashData(
        hash.get(), const_cast<PUCHAR>(bytes.data()),
        static_cast<ULONG>(bytes.size()), 0);
    if (!BCRYPT_SUCCESS(status))
        return std::unexpected(HRESULT_FROM_NT(status));
    std::array<BYTE, 32> digest = {};
    status = BCryptFinishHash(
        hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (!BCRYPT_SUCCESS(status))
        return std::unexpected(HRESULT_FROM_NT(status));
    return digest;
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

[[nodiscard]] std::expected<std::wstring, HRESULT> ReadEnvironmentVariable(
    WCHAR const* name)
{
    for (unsigned int attempt = 0; attempt < 3; ++attempt)
    {
        DWORD const required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0)
        {
            DWORD const error = GetLastError();
            if (error == ERROR_SUCCESS)
                return std::wstring();
            return std::unexpected(HRESULT_FROM_WIN32(error));
        }
        std::vector<WCHAR> buffer(required);
        DWORD const copied = GetEnvironmentVariableW(
            name, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0)
        {
            DWORD const error = GetLastError();
            if (error == ERROR_SUCCESS)
                return std::wstring();
            return std::unexpected(HRESULT_FROM_WIN32(error));
        }
        if (copied < buffer.size())
            return std::wstring(buffer.data(), copied);
    }
    return std::unexpected(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
}

void AppendPathComponent(std::wstring& path, WCHAR const* component)
{
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
        path.push_back(L'\\');
    path.append(component);
}

[[nodiscard]] std::expected<std::wstring, HRESULT> GetCacheDirectoryImpl()
{
    auto environment = ReadEnvironmentVariable(L"LOCALAPPDATA");
    std::wstring path;
    if (environment && !environment->empty())
    {
        path = std::move(*environment);
    }
    else
    {
        std::array<WCHAR, MAX_PATH + 1> temporary = {};
        DWORD const length = GetTempPathW(
            static_cast<DWORD>(temporary.size()), temporary.data());
        if (length == 0 || length >= temporary.size())
            return std::unexpected(HRESULT_FROM_WIN32(
                length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER));
        path.assign(temporary.data(), length);
    }

    AppendPathComponent(path, L"MacType");
    HRESULT result = EnsureDirectory(path);
    if (FAILED(result))
        return std::unexpected(result);
    AppendPathComponent(path, L"FontCache");
    result = EnsureDirectory(path);
    if (FAILED(result))
        return std::unexpected(result);
    static std::atomic<bool> sharedReadAttempted(false);
    if (!sharedReadAttempted.exchange(true))
    {
        // Sandboxed callers lack WRITE_DAC; an ordinary process can repair it later.
        static_cast<void>(GrantSharedReadAccess(path));
    }
    return path;
}

[[nodiscard]] std::expected<FileComparison, HRESULT> CompareFileImpl(
    std::wstring const& path,
    std::vector<BYTE> const& expected)
{
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
            return FileComparison{.exists = false, .matches = false};
        return std::unexpected(HRESULT_FROM_WIN32(error));
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file.get(), &size))
        return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
    if (size.QuadPart != static_cast<LONGLONG>(expected.size()))
        return FileComparison{.exists = true, .matches = false};

    std::vector<BYTE> buffer(kCompareChunkBytes);
    size_t offset = 0;
    while (offset < expected.size())
    {
        DWORD const wanted = static_cast<DWORD>((std::min)(
            buffer.size(), expected.size() - offset));
        DWORD received = 0;
        if (!ReadFile(file.get(), buffer.data(), wanted, &received, nullptr))
            return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
        if (received != wanted ||
            memcmp(buffer.data(), expected.data() + offset, wanted) != 0)
            return FileComparison{.exists = true, .matches = false};
        offset += received;
    }
    return FileComparison{.exists = true, .matches = true};
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

struct ManifestEntry
{
    ManifestKey key;
    ManifestValue value;
    UINT64 stamp = 0;
};

struct ManifestFileIdentity
{
    UINT64 lastWriteTime = 0;
    UINT64 size = 0;
    bool exists = false;
};

struct ManifestState
{
    std::mutex mutex;
    std::wstring path;
    std::vector<ManifestEntry> entries;
    ManifestFileIdentity fileIdentity;
    bool fileIdentityKnown = false;
    bool loaded = false;
};

constexpr WCHAR kManifestMutexName[] = L"Local\\MacType.FontCache.Manifest.v2";
constexpr DWORD kManifestMutexTimeoutMilliseconds = 2000;

class ManifestProcessLock
{
public:
    ManifestProcessLock() noexcept
        : mutex_(renderer_raii::AdoptHandle(
            CreateMutexW(nullptr, FALSE, kManifestMutexName)))
    {
        if (!mutex_)
            return;
        DWORD const result = WaitForSingleObject(
            mutex_.get(), kManifestMutexTimeoutMilliseconds);
        owns_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }

    ~ManifestProcessLock() noexcept
    {
        if (owns_)
            ReleaseMutex(mutex_.get());
    }

    ManifestProcessLock(ManifestProcessLock const&) = delete;
    ManifestProcessLock& operator=(ManifestProcessLock const&) = delete;

private:
    renderer_raii::UniqueHandle mutex_;
    bool owns_ = false;
};

// manifest-v2.bin is a bounded little-endian record stream. The explicit
// version makes format changes fail closed even if a file is renamed.
constexpr std::array<BYTE, 8> kManifestMagic = {
    'M', 'T', 'F', 'M', 'A', 'N', 0x0d, 0x0a,
};
constexpr UINT32 kManifestVersion = 2;
constexpr size_t kManifestEntryLimit = 2048;
constexpr UINT32 kManifestAxisLimit = 64;
constexpr UINT32 kManifestStringLimit = 32768;
constexpr UINT64 kManifestFileLimit = 64ULL * 1024ULL * 1024ULL;

ManifestState& GetManifestState()
{
    static ManifestState* state = new ManifestState;
    return *state;
}

void AppendU32(std::vector<BYTE>& bytes, UINT32 value)
{
    for (unsigned int shift = 0; shift != 32; shift += 8)
        bytes.push_back(static_cast<BYTE>(value >> shift));
}

void AppendU64(std::vector<BYTE>& bytes, UINT64 value)
{
    for (unsigned int shift = 0; shift != 64; shift += 8)
        bytes.push_back(static_cast<BYTE>(value >> shift));
}

bool AppendString(std::vector<BYTE>& bytes, std::wstring const& value)
{
    if (value.size() > kManifestStringLimit)
        return false;
    AppendU32(bytes, static_cast<UINT32>(value.size()));
    if (!value.empty())
    {
        BYTE const* first = reinterpret_cast<BYTE const*>(value.data());
        bytes.insert(bytes.end(), first, first + value.size() * sizeof(WCHAR));
    }
    return true;
}

class ManifestReader
{
public:
    explicit ManifestReader(std::vector<BYTE> const& bytes) : bytes_(bytes) {}

    [[nodiscard]] bool Skip(size_t count) noexcept
    {
        if (count > bytes_.size() - offset_)
            return false;
        offset_ += count;
        return true;
    }

    [[nodiscard]] std::optional<UINT32> U32() noexcept
    {
        if (bytes_.size() - offset_ < 4)
            return std::nullopt;
        UINT32 const value = static_cast<UINT32>(bytes_[offset_]) |
            (static_cast<UINT32>(bytes_[offset_ + 1]) << 8) |
            (static_cast<UINT32>(bytes_[offset_ + 2]) << 16) |
            (static_cast<UINT32>(bytes_[offset_ + 3]) << 24);
        offset_ += 4;
        return value;
    }

    [[nodiscard]] std::optional<UINT64> U64() noexcept
    {
        if (bytes_.size() - offset_ < 8)
            return std::nullopt;
        UINT64 value = 0;
        for (unsigned int shift = 0; shift != 64; shift += 8)
            value |= static_cast<UINT64>(bytes_[offset_++]) << shift;
        return value;
    }

    [[nodiscard]] std::optional<std::wstring> String()
    {
        auto const length = U32();
        if (!length || *length > kManifestStringLimit ||
            static_cast<size_t>(*length) >
                (bytes_.size() - offset_) / sizeof(WCHAR))
            return std::nullopt;
        std::wstring value(*length, L'\0');
        if (*length != 0)
        {
            memcpy(
                value.data(), bytes_.data() + offset_,
                static_cast<size_t>(*length) * sizeof(WCHAR));
        }
        offset_ += static_cast<size_t>(*length) * sizeof(WCHAR);
        return value;
    }

    [[nodiscard]] bool finished() const noexcept
    {
        return offset_ == bytes_.size();
    }

private:
    std::vector<BYTE> const& bytes_;
    size_t offset_ = 0;
};

UINT64 CurrentFileTime() noexcept
{
    FILETIME time = {};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value = {};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

bool SameFloat(FLOAT left, FLOAT right) noexcept
{
    UINT32 leftBits = 0;
    UINT32 rightBits = 0;
    static_assert(sizeof(leftBits) == sizeof(left), "FLOAT size changed");
    memcpy(&leftBits, &left, sizeof(leftBits));
    memcpy(&rightBits, &right, sizeof(rightBits));
    return leftBits == rightBits;
}

bool SameKey(ManifestKey const& left, ManifestKey const& right) noexcept
{
    if (_wcsicmp(left.replacementPath.c_str(), right.replacementPath.c_str()) != 0 ||
        left.replacementSize != right.replacementSize ||
        left.replacementLastWriteTime != right.replacementLastWriteTime ||
        left.faceIndex != right.faceIndex ||
        left.aliasFamily != right.aliasFamily ||
        left.overrideWeight != right.overrideWeight ||
        left.weight != right.weight ||
        left.profileDigest != right.profileDigest ||
        left.axes.size() != right.axes.size())
        return false;
    for (size_t index = 0; index < left.axes.size(); ++index)
    {
        if (left.axes[index].tag != right.axes[index].tag ||
            !SameFloat(left.axes[index].value, right.axes[index].value))
            return false;
    }
    return true;
}

bool SameFileIdentity(
    ManifestFileIdentity const& left,
    ManifestFileIdentity const& right) noexcept
{
    return left.exists == right.exists &&
        (!left.exists || (left.lastWriteTime == right.lastWriteTime &&
            left.size == right.size));
}

std::optional<ManifestFileIdentity> QueryManifestFileIdentity(
    std::wstring const& path) noexcept
{
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (!GetFileAttributesExW(
            path.c_str(), GetFileExInfoStandard, &attributes))
    {
        DWORD const error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return ManifestFileIdentity{};
        return std::nullopt;
    }
    ULARGE_INTEGER size = {};
    size.LowPart = attributes.nFileSizeLow;
    size.HighPart = attributes.nFileSizeHigh;
    ULARGE_INTEGER lastWriteTime = {};
    lastWriteTime.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
    lastWriteTime.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
    return ManifestFileIdentity{
        .lastWriteTime = lastWriteTime.QuadPart,
        .size = size.QuadPart,
        .exists = true,
    };
}

bool ReadManifestFile(
    std::wstring const& path,
    std::vector<BYTE>& bytes,
    ManifestFileIdentity& identity)
{
    bytes.clear();
    identity = {};
    renderer_raii::UniqueHandle file = renderer_raii::AdoptHandle(CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file)
        return false;
    BY_HANDLE_FILE_INFORMATION information = {};
    if (!GetFileInformationByHandle(file.get(), &information))
        return false;
    ULARGE_INTEGER size = {};
    size.LowPart = information.nFileSizeLow;
    size.HighPart = information.nFileSizeHigh;
    ULARGE_INTEGER lastWriteTime = {};
    lastWriteTime.LowPart = information.ftLastWriteTime.dwLowDateTime;
    lastWriteTime.HighPart = information.ftLastWriteTime.dwHighDateTime;
    identity.size = size.QuadPart;
    identity.lastWriteTime = lastWriteTime.QuadPart;
    identity.exists = true;
    if (identity.size < 16 || identity.size > kManifestFileLimit)
        return false;
    bytes.resize(static_cast<size_t>(identity.size));
    size_t offset = 0;
    while (offset < bytes.size())
    {
        DWORD const wanted = static_cast<DWORD>((std::min)(
            bytes.size() - offset,
            static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD received = 0;
        if (!ReadFile(file.get(), bytes.data() + offset, wanted, &received, nullptr) ||
            received == 0)
            return false;
        offset += received;
    }
    return true;
}

std::optional<std::vector<ManifestEntry>> ParseManifest(
    std::vector<BYTE> const& bytes)
{
    if (bytes.size() < kManifestMagic.size() + 8 ||
        memcmp(bytes.data(), kManifestMagic.data(), kManifestMagic.size()) != 0)
        return std::nullopt;
    ManifestReader reader(bytes);
    if (!reader.Skip(kManifestMagic.size()))
        return std::nullopt;
    auto const version = reader.U32();
    auto const count = reader.U32();
    if (!version || *version != kManifestVersion ||
        !count || *count > kManifestEntryLimit)
        return std::nullopt;
    std::vector<ManifestEntry> entries;
    entries.reserve(*count);
    for (UINT32 entryIndex = 0; entryIndex < *count; ++entryIndex)
    {
        ManifestEntry entry;
        auto const stamp = reader.U64();
        auto const virtualFontSize = reader.U64();
        auto const replacementSize = reader.U64();
        auto const replacementLastWriteTime = reader.U64();
        auto const profileDigest = reader.U64();
        auto const faceIndex = reader.U32();
        auto const flags = reader.U32();
        auto const weight = reader.U32();
        auto const axisCount = reader.U32();
        if (!stamp || !virtualFontSize || !replacementSize ||
            !replacementLastWriteTime || !profileDigest || !faceIndex ||
            !flags || !weight || !axisCount || *axisCount > kManifestAxisLimit)
            return std::nullopt;
        entry.stamp = *stamp;
        entry.value.virtualFontSize = *virtualFontSize;
        entry.key.replacementSize = *replacementSize;
        entry.key.replacementLastWriteTime = *replacementLastWriteTime;
        entry.key.profileDigest = *profileDigest;
        entry.key.faceIndex = *faceIndex;
        entry.key.overrideWeight = (*flags & 1U) != 0;
        if ((*flags & ~1U) != 0 || *weight > 1000)
            return std::nullopt;
        entry.key.weight = static_cast<UINT16>(*weight);
        entry.key.axes.resize(*axisCount);
        for (ManifestAxis& axis : entry.key.axes)
        {
            auto const tag = reader.U32();
            auto const bits = reader.U32();
            if (!tag || !bits)
                return std::nullopt;
            axis.tag = *tag;
            memcpy(&axis.value, &*bits, sizeof(*bits));
            if (!std::isfinite(axis.value))
                return std::nullopt;
        }
        auto replacementPath = reader.String();
        auto aliasFamily = reader.String();
        auto virtualFontPath = reader.String();
        auto family = reader.String();
        auto subfamily = reader.String();
        auto fullName = reader.String();
        auto postScriptName = reader.String();
        if (!replacementPath || !aliasFamily || !virtualFontPath || !family ||
            !subfamily || !fullName || !postScriptName)
            return std::nullopt;
        entry.key.replacementPath = std::move(*replacementPath);
        entry.key.aliasFamily = std::move(*aliasFamily);
        entry.value.virtualFontPath = std::move(*virtualFontPath);
        entry.value.family = std::move(*family);
        entry.value.subfamily = std::move(*subfamily);
        entry.value.fullName = std::move(*fullName);
        entry.value.postScriptName = std::move(*postScriptName);
        entries.push_back(std::move(entry));
    }
    if (!reader.finished())
        return std::nullopt;
    return entries;
}

std::optional<std::vector<BYTE>> SerializeManifest(
    std::vector<ManifestEntry> const& entries)
{
    if (entries.size() > kManifestEntryLimit)
        return std::nullopt;
    std::vector<BYTE> bytes(kManifestMagic.begin(), kManifestMagic.end());
    AppendU32(bytes, kManifestVersion);
    AppendU32(bytes, static_cast<UINT32>(entries.size()));
    for (ManifestEntry const& entry : entries)
    {
        if (entry.key.axes.size() > kManifestAxisLimit)
            return std::nullopt;
        AppendU64(bytes, entry.stamp);
        AppendU64(bytes, entry.value.virtualFontSize);
        AppendU64(bytes, entry.key.replacementSize);
        AppendU64(bytes, entry.key.replacementLastWriteTime);
        AppendU64(bytes, entry.key.profileDigest);
        AppendU32(bytes, entry.key.faceIndex);
        AppendU32(bytes, entry.key.overrideWeight ? 1U : 0U);
        AppendU32(bytes, entry.key.weight);
        AppendU32(bytes, static_cast<UINT32>(entry.key.axes.size()));
        for (ManifestAxis const& axis : entry.key.axes)
        {
            UINT32 bits = 0;
            memcpy(&bits, &axis.value, sizeof(bits));
            AppendU32(bytes, axis.tag);
            AppendU32(bytes, bits);
        }
        if (!AppendString(bytes, entry.key.replacementPath) ||
            !AppendString(bytes, entry.key.aliasFamily) ||
            !AppendString(bytes, entry.value.virtualFontPath) ||
            !AppendString(bytes, entry.value.family) ||
            !AppendString(bytes, entry.value.subfamily) ||
            !AppendString(bytes, entry.value.fullName) ||
            !AppendString(bytes, entry.value.postScriptName) ||
            bytes.size() > kManifestFileLimit)
            return std::nullopt;
    }
    return bytes;
}

void LoadManifest(ManifestState& state, std::wstring const& path)
{
    auto const diskIdentity = QueryManifestFileIdentity(path);
    bool const diskIdentityKnown = diskIdentity.has_value();
    if (state.loaded && state.path == path && state.fileIdentityKnown &&
        diskIdentity && SameFileIdentity(state.fileIdentity, *diskIdentity))
        return;
    state.path = path;
    state.entries.clear();
    state.fileIdentity = diskIdentity ? *diskIdentity : ManifestFileIdentity{};
    state.fileIdentityKnown = diskIdentityKnown;
    state.loaded = true;
    if (diskIdentity && !diskIdentity->exists)
        return;
    std::vector<BYTE> bytes;
    ManifestFileIdentity loadedIdentity;
    if (!ReadManifestFile(path, bytes, loadedIdentity))
    {
        if (loadedIdentity.exists)
        {
            state.fileIdentity = loadedIdentity;
            state.fileIdentityKnown = true;
        }
        return;
    }
    state.fileIdentity = loadedIdentity;
    state.fileIdentityKnown = true;
    if (auto parsed = ParseManifest(bytes))
        state.entries = std::move(*parsed);
}

HRESULT WriteManifest(
    ManifestState& state,
    std::wstring const& path)
{
    auto bytes = SerializeManifest(state.entries);
    if (!bytes)
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    static std::atomic<ULONG> sequence(0);
    std::wstring temporaryPath = path;
    temporaryPath.append(L".tmp-");
    temporaryPath.append(std::to_wstring(GetCurrentProcessId()));
    temporaryPath.push_back(L'-');
    temporaryPath.append(std::to_wstring(GetCurrentThreadId()));
    temporaryPath.push_back(L'-');
    temporaryPath.append(std::to_wstring(++sequence));
    PendingCacheFile temporary(std::move(temporaryPath));
    renderer_raii::UniqueHandle file = renderer_raii::AdoptHandle(CreateFileW(
        temporary.path(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file)
        return HRESULT_FROM_WIN32(GetLastError());
    HRESULT result = WriteAll(file.get(), *bytes);
    if (SUCCEEDED(result) && !FlushFileBuffers(file.get()))
        result = HRESULT_FROM_WIN32(GetLastError());
    file.reset();
    if (FAILED(result))
        return result;
    if (!MoveFileExW(
            temporary.path(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return HRESULT_FROM_WIN32(GetLastError());
    temporary.Commit();
    auto const writtenIdentity = QueryManifestFileIdentity(path);
    bool const identityKnown = writtenIdentity && writtenIdentity->exists;
    state.path = path;
    state.loaded = true;
    state.fileIdentity = writtenIdentity
        ? *writtenIdentity
        : ManifestFileIdentity{};
    state.fileIdentityKnown = identityKnown;
    return S_OK;
}

bool IsUsableManifestFile(
    std::wstring const& path,
    UINT64 expectedSize) noexcept
{
    renderer_raii::UniqueHandle file = renderer_raii::AdoptHandle(CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file || expectedSize < 4)
        return false;
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 ||
        static_cast<UINT64>(size.QuadPart) != expectedSize)
        return false;
    std::array<BYTE, 4> scaler = {};
    DWORD received = 0;
    if (!ReadFile(file.get(), scaler.data(), static_cast<DWORD>(scaler.size()),
            &received, nullptr) || received != scaler.size())
        return false;
    constexpr std::array<BYTE, 4> trueType = {0x00, 0x01, 0x00, 0x00};
    constexpr std::array<BYTE, 4> cff = {'O', 'T', 'T', 'O'};
    constexpr std::array<BYTE, 4> appleTrueType = {'t', 'r', 'u', 'e'};
    constexpr std::array<BYTE, 4> collection = {'t', 't', 'c', 'f'};
    return scaler == trueType || scaler == cff ||
        scaler == appleTrueType || scaler == collection;
}

[[nodiscard]] std::expected<std::wstring, HRESULT> ManifestPath()
{
    auto path = GetCacheDirectory();
    if (!path)
        return std::unexpected(path.error());
    AppendPathComponent(*path, L"manifest-v2.bin");
    return std::move(*path);
}

[[nodiscard]] std::expected<std::optional<ManifestValue>, HRESULT>
LookupManifestFontImpl(ManifestKey const& key)
{
    if (key.replacementPath.empty() || key.aliasFamily.empty())
        return std::optional<ManifestValue>();
    auto path = ManifestPath();
    if (!path)
        return std::optional<ManifestValue>();
    ManifestProcessLock processLock;
    ManifestState& state = GetManifestState();
    std::scoped_lock lock(state.mutex);
    LoadManifest(state, *path);
    for (auto entry = state.entries.begin(); entry != state.entries.end(); ++entry)
    {
        if (!SameKey(entry->key, key))
            continue;
        if (!IsUsableManifestFile(
                entry->value.virtualFontPath, entry->value.virtualFontSize))
        {
            state.entries.erase(entry);
            HRESULT const result = WriteManifest(state, *path);
            if (FAILED(result))
                return std::unexpected(result);
            return std::optional<ManifestValue>();
        }
        entry->stamp = CurrentFileTime();
        return std::optional<ManifestValue>(entry->value);
    }
    return std::optional<ManifestValue>();
}

HRESULT StoreManifestFontImpl(
    ManifestKey const& key,
    ManifestValue const& value)
{
    if (key.replacementPath.empty() || key.aliasFamily.empty() ||
        value.virtualFontPath.empty() || value.virtualFontSize < 4 ||
        key.axes.size() > kManifestAxisLimit)
        return E_INVALIDARG;
    if (std::any_of(
            key.axes.begin(), key.axes.end(),
            [](ManifestAxis const& axis) { return !std::isfinite(axis.value); }))
        return E_INVALIDARG;
    auto path = ManifestPath();
    if (!path)
        return path.error();
    ManifestProcessLock processLock;
    ManifestState& state = GetManifestState();
    std::scoped_lock lock(state.mutex);
    LoadManifest(state, *path);
    ManifestEntry* destination = nullptr;
    for (ManifestEntry& entry : state.entries)
    {
        if (SameKey(entry.key, key))
        {
            destination = &entry;
            break;
        }
    }
    if (destination == nullptr)
    {
        state.entries.push_back({});
        destination = &state.entries.back();
        destination->key = key;
    }
    destination->value = value;
    destination->stamp = CurrentFileTime();
    while (state.entries.size() > kManifestEntryLimit)
    {
        auto const oldest = std::min_element(
            state.entries.begin(), state.entries.end(),
            [](ManifestEntry const& left, ManifestEntry const& right) {
                return left.stamp < right.stamp;
            });
        state.entries.erase(oldest);
    }
    return WriteManifest(state, *path);
}

HRESULT DropManifestFontImpl(ManifestKey const& key)
{
    if (key.replacementPath.empty() || key.aliasFamily.empty())
        return S_OK;
    auto path = ManifestPath();
    if (!path)
        return path.error();
    ManifestProcessLock processLock;
    ManifestState& state = GetManifestState();
    std::scoped_lock lock(state.mutex);
    LoadManifest(state, *path);
    size_t const before = state.entries.size();
    state.entries.erase(
        std::remove_if(
            state.entries.begin(), state.entries.end(),
            [&key](ManifestEntry const& entry) {
                return SameKey(entry.key, key);
            }),
        state.entries.end());
    return state.entries.size() == before ? S_FALSE :
        WriteManifest(state, *path);
}

HRESULT ReloadManifestForTestsImpl()
{
    auto path = ManifestPath();
    if (!path)
        return path.error();
    ManifestState fresh;
    LoadManifest(fresh, *path);
    ManifestState& state = GetManifestState();
    std::scoped_lock lock(state.mutex);
    state.path = std::move(fresh.path);
    state.entries = std::move(fresh.entries);
    state.fileIdentity = fresh.fileIdentity;
    state.fileIdentityKnown = fresh.fileIdentityKnown;
    state.loaded = fresh.loaded;
    return S_OK;
}

[[nodiscard]] std::expected<std::wstring, HRESULT> PersistFontImpl(
    std::vector<BYTE> const& bytes)
{
    auto digest = HashBytes(bytes);
    if (!digest)
        return std::unexpected(digest.error());

    auto directory = GetCacheDirectory();
    if (!directory)
        return std::unexpected(directory.error());

    std::wstring const stem = HexDigest(*digest);
    static std::atomic<ULONG> sequence(0);
    std::wstring path;
    for (unsigned int attempt = 0; attempt < 32; ++attempt)
    {
        path = *directory;
        AppendPathComponent(path, stem.c_str());
        if (attempt != 0)
        {
            path.push_back(L'-');
            path.append(std::to_wstring(GetCurrentProcessId()));
            path.push_back(L'-');
            path.append(std::to_wstring(++sequence));
        }
        path.append(L".ttf");

        auto comparison = CompareFile(path, bytes);
        if (!comparison)
            return std::unexpected(comparison.error());
        if (comparison->matches)
            return path;
        if (comparison->exists)
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
            return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
        }
        HRESULT result = WriteAll(file.get(), bytes);
        if (SUCCEEDED(result) && !FlushFileBuffers(file.get()))
            result = HRESULT_FROM_WIN32(GetLastError());
        file.reset();
        if (FAILED(result))
            return std::unexpected(result);

        if (MoveFileExW(
                temporary.path(), path.c_str(), MOVEFILE_WRITE_THROUGH))
        {
            temporary.Commit();
            return path;
        }
        DWORD const moveError = GetLastError();
        comparison = CompareFile(path, bytes);
        if (comparison && comparison->matches)
            return path;
        if (!comparison)
            return std::unexpected(comparison.error());
        if (moveError != ERROR_ALREADY_EXISTS && moveError != ERROR_FILE_EXISTS)
            return std::unexpected(HRESULT_FROM_WIN32(moveError));
    }
    return std::unexpected(HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES));
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

std::expected<FileComparison, HRESULT> CompareFile(
    std::wstring const& path,
    std::vector<BYTE> const& expected) noexcept
{
    try
    {
        return CompareFileImpl(path, expected);
    }
    catch (std::bad_alloc const&)
    {
        return std::unexpected(E_OUTOFMEMORY);
    }
    catch (...)
    {
        return std::unexpected(E_FAIL);
    }
}

std::expected<std::wstring, HRESULT> GetCacheDirectory() noexcept
{
    try
    {
        return GetCacheDirectoryImpl();
    }
    catch (std::bad_alloc const&)
    {
        return std::unexpected(E_OUTOFMEMORY);
    }
    catch (...)
    {
        return std::unexpected(E_FAIL);
    }
}

std::expected<std::optional<ManifestValue>, HRESULT>
LookupManifestFont(ManifestKey const& key) noexcept
{
    try
    {
        return LookupManifestFontImpl(key);
    }
    catch (std::bad_alloc const&)
    {
        return std::unexpected(E_OUTOFMEMORY);
    }
    catch (...)
    {
        return std::unexpected(E_FAIL);
    }
}

HRESULT StoreManifestFont(
    ManifestKey const& key,
    ManifestValue const& value) noexcept
{
    try
    {
        return StoreManifestFontImpl(key, value);
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

HRESULT DropManifestFont(ManifestKey const& key) noexcept
{
    try
    {
        return DropManifestFontImpl(key);
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

HRESULT ReloadManifestForTests() noexcept
{
    try
    {
        return ReloadManifestForTestsImpl();
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

std::expected<std::wstring, HRESULT> PersistFont(
    std::vector<BYTE> const& bytes) noexcept
{
    try
    {
        return PersistFontImpl(bytes);
    }
    catch (std::bad_alloc const&)
    {
        return std::unexpected(E_OUTOFMEMORY);
    }
    catch (...)
    {
        return std::unexpected(E_FAIL);
    }
}

} // namespace virtual_font_cache
} // namespace renderer
