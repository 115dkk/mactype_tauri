#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstdint>
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
    std::wstring family;
    std::wstring subfamily;
    std::wstring fullName;
    std::wstring postScriptName;
};

HRESULT CompareFile(
    std::wstring const& path,
    std::vector<BYTE> const& expected,
    bool& exists,
    bool& matches) noexcept;

HRESULT GetCacheDirectory(std::wstring& path) noexcept;

HRESULT GrantSharedReadAccess(std::wstring const& directory) noexcept;

HRESULT LookupManifestFont(
    ManifestKey const& key,
    ManifestValue& value,
    bool& found) noexcept;

HRESULT StoreManifestFont(
    ManifestKey const& key,
    ManifestValue const& value) noexcept;

HRESULT PersistFont(
    std::vector<BYTE> const& bytes,
    std::wstring& path) noexcept;

} // namespace virtual_font_cache
} // namespace renderer
