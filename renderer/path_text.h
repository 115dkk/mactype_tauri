#pragma once

#include <Windows.h>

#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace renderer::path_text {

// Replaces the file name after the last backslash of the NUL-terminated path in place.
// Returns false and leaves path unchanged when it has no backslash, is not terminated
// inside the span, or the result plus its terminator does not fit.
[[nodiscard]] inline bool ReplaceFileName(
	std::span<wchar_t> path,
	std::wstring_view fileName) noexcept
{
	std::size_t length = 0;
	while (length < path.size() && path[length] != L'\0')
		++length;
	if (length == path.size())
		return false;

	std::size_t fileNameOffset = length;
	while (fileNameOffset != 0 && path[fileNameOffset - 1] != L'\\')
		--fileNameOffset;
	if (fileNameOffset == 0)
		return false;

	if (fileName.size() > path.size() - fileNameOffset - 1)
		return false;

	if (!fileName.empty())
	{
		std::char_traits<wchar_t>::move(
			path.data() + fileNameOffset, fileName.data(), fileName.size());
	}
	path[fileNameOffset + fileName.size()] = L'\0';
	return true;
}

// Directory of module (no trailing backslash), read with a growing buffer so long paths
// work; empty when the module path cannot be read or has no backslash.
[[nodiscard]] inline std::optional<std::wstring> ModuleDirectory(HMODULE module)
{
	std::vector<wchar_t> path(MAX_PATH);
	constexpr std::size_t maxApiLength =
		static_cast<std::size_t>((std::numeric_limits<DWORD>::max)());

	while (path.size() <= maxApiLength)
	{
		DWORD const capacity = static_cast<DWORD>(path.size());
		DWORD const length = GetModuleFileNameW(module, path.data(), capacity);
		if (length == 0)
			return std::nullopt;
		if (length < capacity)
		{
			path[length] = L'\0';
			std::wstring_view const modulePath(path.data(), length);
			std::size_t const separator = modulePath.find_last_of(L'\\');
			if (separator == std::wstring_view::npos)
				return std::nullopt;
			return std::wstring(modulePath.substr(0, separator));
		}

		if (path.size() > maxApiLength / 2)
			break;
		path.resize(path.size() * 2);
	}
	return std::nullopt;
}

} // namespace renderer::path_text
