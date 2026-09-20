#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <string>
#include <vector>

namespace renderer {
namespace virtual_font_cache {

HRESULT CompareFile(
    std::wstring const& path,
    std::vector<BYTE> const& expected,
    bool& exists,
    bool& matches) noexcept;

HRESULT GetCacheDirectory(std::wstring& path) noexcept;

HRESULT GrantSharedReadAccess(std::wstring const& directory) noexcept;

HRESULT PersistFont(
    std::vector<BYTE> const& bytes,
    std::wstring& path) noexcept;

} // namespace virtual_font_cache
} // namespace renderer
