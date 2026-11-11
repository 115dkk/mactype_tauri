#include "profile_text.h"

#include <windows.h>

#include <climits>
#include <new>

namespace renderer {
namespace profile_text {
namespace {

bool DecodeMultiByte(
	unsigned int codePage,
	DWORD flags,
	const unsigned char* data,
	std::size_t size,
	std::wstring& text)
{
	if (size == 0)
	{
		text.clear();
		return true;
	}
	if (size > static_cast<std::size_t>(INT_MAX))
		return false;
	const char* const bytes = reinterpret_cast<const char*>(data);
	const int length = static_cast<int>(size);
	const int required = MultiByteToWideChar(codePage, flags, bytes, length, nullptr, 0);
	if (required <= 0)
		return false;
	text.resize(static_cast<std::size_t>(required));
	return MultiByteToWideChar(codePage, flags, bytes, length, &text[0], required) == required;
}

void DecodeUtf16(const unsigned char* data, std::size_t size, bool bigEndian, std::wstring& text)
{
	// An odd trailing byte cannot form a code unit; the previous loader ignored it too.
	text.resize(size / 2);
	for (std::size_t index = 0; index < text.size(); ++index)
	{
		const unsigned char first = data[index * 2];
		const unsigned char second = data[index * 2 + 1];
		text[index] = static_cast<wchar_t>(bigEndian ? (first << 8) | second : (second << 8) | first);
	}
}

} // namespace

bool Decode(
	const unsigned char* data,
	std::size_t size,
	unsigned int legacyCodePage,
	std::wstring& text)
{
	text.clear();
	if (data == nullptr || size == 0)
		return false;
	try
	{
		if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE)
		{
			DecodeUtf16(data + 2, size - 2, false, text);
			return true;
		}
		if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF)
		{
			DecodeUtf16(data + 2, size - 2, true, text);
			return true;
		}
		if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
			return DecodeMultiByte(CP_UTF8, 0, data + 3, size - 3, text);
		if (DecodeMultiByte(CP_UTF8, MB_ERR_INVALID_CHARS, data, size, text))
			return true;
		if (legacyCodePage != CP_UTF8 &&
			DecodeMultiByte(legacyCodePage, MB_ERR_INVALID_CHARS, data, size, text))
			return true;
		return DecodeMultiByte(legacyCodePage, 0, data, size, text);
	}
	catch (const std::bad_alloc&)
	{
		text.clear();
		return false;
	}
}

unsigned int SystemAnsiCodePage() noexcept
{
	DWORD codePage = 0;
	if (GetLocaleInfoEx(
			LOCALE_NAME_SYSTEM_DEFAULT,
			LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
			reinterpret_cast<LPWSTR>(&codePage),
			sizeof(codePage) / sizeof(wchar_t)) == 0 ||
		codePage == 0)
		return CP_ACP;
	return codePage;
}

} // namespace profile_text
} // namespace renderer
