#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace renderer {
namespace virtual_font_cache {

struct ManifestAxis
{
    UINT32 tag = 0;
    FLOAT value = 0;
};

struct ManifestKey
{
    std::wstring replacementPath;
    UINT64 replacementSize = 0;
    UINT64 replacementLastWriteTime = 0;
    UINT32 faceIndex = 0;
    std::wstring aliasFamily;
    bool overrideWeight = false;
    UINT16 weight = 400;
    std::uint64_t profileDigest = 0;
    std::vector<ManifestAxis> axes;
};

struct ManifestValue
{
    std::wstring virtualFontPath;
    UINT64 virtualFontSize = 0;
    std::wstring family;
    std::wstring subfamily;
    std::wstring fullName;
    std::wstring postScriptName;
};

struct FileComparison
{
    bool exists = false;
    bool matches = false;
};

[[nodiscard]] std::expected<FileComparison, HRESULT> CompareFile(
    std::wstring const& path,
    std::vector<BYTE> const& expected) noexcept;

[[nodiscard]] std::expected<std::wstring, HRESULT> GetCacheDirectory() noexcept;

[[nodiscard]] HRESULT GrantSharedReadAccess(
    std::wstring const& directory) noexcept;

[[nodiscard]] std::expected<std::optional<ManifestValue>, HRESULT>
LookupManifestFont(ManifestKey const& key) noexcept;

[[nodiscard]] HRESULT StoreManifestFont(
    ManifestKey const& key,
    ManifestValue const& value) noexcept;

[[nodiscard]] HRESULT DropManifestFont(ManifestKey const& key) noexcept;

// Forces the next test observation through a newly loaded manifest state.
[[nodiscard]] HRESULT ReloadManifestForTests() noexcept;

[[nodiscard]] std::expected<std::wstring, HRESULT> PersistFont(
    std::vector<BYTE> const& bytes) noexcept;

} // namespace virtual_font_cache
} // namespace renderer
