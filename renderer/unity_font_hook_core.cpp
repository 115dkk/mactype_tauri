#include "unity_font_hook_core.h"

#include "freetype_runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <set>

namespace renderer { namespace unity {
namespace {

constexpr unsigned char kPixelModeGray = 2;
constexpr unsigned char kPixelModeLcd = 5;
constexpr unsigned char kPixelModeLcdVertical = 6;
constexpr std::uint64_t kMaximumBitmapBytes = 64ULL * 1024ULL * 1024ULL;

bool EqualOrdinalIgnoreCase(
	const std::wstring& left,
	const std::wstring& right) noexcept
{
	if (left.size() != right.size() ||
		left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
		return false;
	return CompareStringOrdinal(
		left.data(), static_cast<int>(left.size()),
		right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool NormalizeAbsolutePath(
	const wchar_t* value,
	std::wstring& normalized)
{
	normalized.clear();
	if (value == nullptr || *value == L'\0')
		return false;
	normalized.assign(value);
	std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
	if (normalized.compare(0, 4, L"\\\\?\\") == 0)
	{
		if (normalized.compare(4, 4, L"UNC\\") == 0)
			normalized.replace(0, 8, L"\\\\");
		else
			normalized.erase(0, 4);
	}
	bool const driveAbsolute = normalized.size() >= 3 &&
		((normalized[0] >= L'A' && normalized[0] <= L'Z') ||
		 (normalized[0] >= L'a' && normalized[0] <= L'z')) &&
		normalized[1] == L':' && normalized[2] == L'\\';
	bool const uncAbsolute = normalized.size() >= 3 &&
		normalized[0] == L'\\' && normalized[1] == L'\\' &&
		normalized[2] != L'\\';
	return driveAbsolute || uncAbsolute;
}

bool Utf8PathToWide(const char* value, std::wstring& converted)
{
	converted.clear();
	if (value == nullptr)
		return false;
	constexpr std::size_t kMaximumPathBytes = 32768;
	std::size_t const length = strnlen_s(value, kMaximumPathBytes);
	if (length == 0 || length == kMaximumPathBytes ||
		length > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
		return false;
	int const required = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, value, static_cast<int>(length),
		nullptr, 0);
	if (required <= 0)
		return false;
	converted.resize(static_cast<std::size_t>(required));
	return MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, value, static_cast<int>(length),
		&converted[0], required) == required;
}

bool WidePathToUtf8(const std::wstring& value, std::string& converted)
{
	converted.clear();
	if (value.empty() ||
		value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
		return false;
	int const required = WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (required <= 0)
		return false;
	converted.resize(static_cast<std::size_t>(required));
	return WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), &converted[0], required,
		nullptr, nullptr) == required;
}

std::vector<InstalledFontFace> PathsForFamily(
	const std::vector<InstalledFontFace>& installedFonts,
	const std::wstring& family)
{
	std::vector<InstalledFontFace> paths;
	for (const InstalledFontFace& font : installedFonts)
	{
		if (!EqualOrdinalIgnoreCase(font.family, family))
			continue;
		std::wstring normalized;
		if (!NormalizeAbsolutePath(font.filePath.c_str(), normalized))
			continue;
		if (std::none_of(paths.begin(), paths.end(), [&](const InstalledFontFace& path) {
				return path.faceIndex == font.faceIndex &&
					EqualOrdinalIgnoreCase(path.filePath, normalized);
			}))
			paths.push_back({font.family, std::move(normalized), font.faceIndex});
	}
	return paths;
}

std::vector<std::wstring> FamiliesForFace(
	const std::vector<InstalledFontFace>& installedFonts,
	const InstalledFontFace& requestedFace)
{
	std::vector<std::wstring> families;
	for (const InstalledFontFace& font : installedFonts)
	{
		if (font.faceIndex != requestedFace.faceIndex)
			continue;
		std::wstring normalized;
		if (!NormalizeAbsolutePath(font.filePath.c_str(), normalized) ||
			!EqualOrdinalIgnoreCase(normalized, requestedFace.filePath))
			continue;
		if (std::none_of(
			families.begin(), families.end(), [&](const std::wstring& existing) {
				return EqualOrdinalIgnoreCase(existing, font.family);
			}))
			families.push_back(font.family);
	}
	return families;
}

constexpr std::array<unsigned char, 32> kPublicPrefixLegacy{{
	0x48,0x89,0x6C,0x24,0x18,0x56,0x48,0x83,0xEC,0x20,0x8B,0xEA,0x48,0x8B,0xF1,0x48,
	0x85,0xC9,0x0F,0x84,0x0F,0x01,0x00,0x00,0x48,0x8B,0x41,0x08,0x48,0x85,0xC0,0x0F,
}};
constexpr std::array<unsigned char, 32> kPublicPrefix2020{{
	0x48,0x89,0x6C,0x24,0x18,0x56,0x48,0x83,0xEC,0x20,0x8B,0xEA,0x48,0x8B,0xF1,0x48,
	0x85,0xC9,0x0F,0x84,0x22,0x01,0x00,0x00,0x48,0x8B,0x41,0x08,0x48,0x85,0xC0,0x0F,
}};
constexpr std::array<unsigned char, 32> kPublicPrefixWrapper{{
	0x48,0x85,0xC9,0x74,0x1F,0x48,0x8B,0x41,0x08,0x48,0x85,0xC0,0x74,0x16,0x48,0x8B,
	0x80,0x90,0x00,0x00,0x00,0x44,0x8B,0xC2,0x48,0x8B,0xD1,0x48,0x8B,0x48,0x08,0xE9,
}};
constexpr std::array<unsigned char, 32> kInternalPrefixX64{{
	0x4C,0x8B,0xDC,0x49,0x89,0x5B,0x08,0x49,0x89,0x6B,0x18,0x56,0x57,0x41,0x54,0x41,
	0x56,0x41,0x57,0x48,0x83,0xEC,0x40,0x48,0x8B,0x82,0xF0,0x00,0x00,0x00,0x45,0x33,
}};
constexpr std::array<unsigned char, 32> kInternalPrefixX86{{
	0x55,0x8B,0xEC,0x83,0xEC,0x1C,0x53,0x56,0x57,0x8B,0xFA,0x8B,0xD1,0x89,0x55,0xFC,
	0x8B,0x87,0x9C,0x00,0x00,0x00,0x8B,0x77,0x04,0xF7,0x40,0x28,0x00,0x00,0x10,0x00,
}};
constexpr std::array<unsigned char, 32> kFaceOpenPrefix2019X64{{
	0x4C,0x89,0x4C,0x24,0x20,0x48,0x89,0x4C,0x24,0x08,0x55,0x53,0x57,0x41,0x54,0x41,
	0x55,0x41,0x56,0x48,0x8D,0x6C,0x24,0xD9,0x48,0x81,0xEC,0xF8,0x00,0x00,0x00,0x4C,
}};
constexpr std::array<unsigned char, 32> kFaceOpenPrefix2020_3_34X64{{
	0x4C,0x89,0x4C,0x24,0x20,0x48,0x89,0x4C,0x24,0x08,0x55,0x57,0x41,0x54,0x41,0x55,
	0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,0x24,0xF8,0x48,0x81,0xEC,0x08,0x01,0x00,0x00,
}};
constexpr std::array<unsigned char, 32> kFaceOpenPrefix2021_3_38X64{{
	0x4C,0x89,0x4C,0x24,0x20,0x48,0x89,0x4C,0x24,0x08,0x55,0x56,0x57,0x41,0x55,0x41,
	0x56,0x48,0x8D,0x6C,0x24,0xD1,0x48,0x81,0xEC,0x00,0x01,0x00,0x00,0x48,0x8B,0xF9,
}};
constexpr std::array<unsigned char, 32> kFaceOpenPrefix2020_3_40X64{{
	0x4C,0x89,0x4C,0x24,0x20,0x48,0x89,0x4C,0x24,0x08,0x55,0x57,0x41,0x55,0x41,0x56,
	0x41,0x57,0x48,0x8D,0x6C,0x24,0xD1,0x48,0x81,0xEC,0x00,0x01,0x00,0x00,0x4C,0x8B,
}};
constexpr std::array<unsigned char, 32> kFaceOpenPrefix2022EarlyX64{{
	0x4C,0x89,0x4C,0x24,0x20,0x48,0x89,0x4C,0x24,0x08,0x55,0x56,0x57,0x41,0x56,0x48,
	0x8D,0x6C,0x24,0xF8,0x48,0x81,0xEC,0x08,0x01,0x00,0x00,0x48,0x8B,0xF9,0x49,0x8B,
}};
constexpr std::array<unsigned char, 32> kFaceOpenPrefixLateX64{{
	0x4C,0x89,0x4C,0x24,0x20,0x48,0x89,0x4C,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,
	0x56,0x41,0x57,0x48,0x8D,0x6C,0x24,0xD9,0x48,0x81,0xEC,0xF8,0x00,0x00,0x00,0x33,
}};
constexpr std::array<unsigned char, 32> kFaceOpenPrefix2022_3_62f3X86{{
	0x55,0x8B,0xEC,0x83,0xE4,0xF8,0x81,0xEC,0xA4,0x00,0x00,0x00,0x53,0x56,0x57,0x8B,
	0x7D,0x08,0x8B,0xF2,0x89,0x74,0x24,0x28,0x89,0x4C,0x24,0x10,0xC7,0x44,0x24,0x18,
}};
constexpr std::array<unsigned char, 32> kTextCoreFontLoadPrefix2022_3_62f3X64{{
	0x48,0x89,0x74,0x24,0x18,0x89,0x54,0x24,0x10,0x55,0x57,0x41,0x56,0x48,0x8D,0x6C,
	0x24,0xB9,0x48,0x81,0xEC,0xA0,0x00,0x00,0x00,0x48,0x83,0x3D,0xA7,0x07,0xFD,0x00,
}};
constexpr std::array<unsigned char, 32> kLegacyCharacterLookupPrefix2022_3_62f3X64{{
	0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x56,0x57,0x41,0x56,0x48,0x83,
	0xEC,0x70,0x45,0x8B,0xF1,0x49,0x8B,0xF0,0x48,0x8B,0xDA,0x48,0x8B,0xE9,0xE8,0xBD,
}};
constexpr std::array<unsigned char, 32> kOsFaceResolverPrefix2022_3_62f3X64{{
	0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7C,0x24,0x20,0x55,
	0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,0x24,0xC9,0x48,0x81,0xEC,
}};

constexpr AdapterDescriptor kProductionDescriptors[] = {
	{"unity-2018.4.32-x64",0x8664,0x60240965,0x01731000,
	 {{0xD5,0xC2,0x9A,0x59,0xCE,0x81,0x6E,0x4C,0xBA,0xA8,0x20,0x30,0xB6,0xAC,0xFB,0x9B}},1,0x00C633D0,RenderAbi::publicRender,&kPublicPrefixLegacy,
	 0x00C64630,&kFaceOpenPrefix2019X64,FaceOpenAbi::unityInternal},
	{"unity-2019.4.9-x64",0x8664,0x5F3B44F7,0x01996000,
	 {{0x6F,0x2D,0xF0,0x51,0x37,0x70,0xC3,0x48,0x97,0x01,0xAF,0x4F,0xD6,0x8A,0xDC,0x70}},1,0x00E2AD80,RenderAbi::publicRender,&kPublicPrefixLegacy,
	 0x00E2BEC0,&kFaceOpenPrefix2019X64,FaceOpenAbi::unityInternal},
	{"unity-2019.4.41-x64",0x8664,0x68ED0247,0x019DB000,
	 {{0xF2,0x4E,0x36,0x83,0xDD,0x88,0x07,0x4D,0xA6,0x7A,0xDF,0xF6,0xDF,0x4B,0x04,0xBF}},1,0x00E49DE0,RenderAbi::publicRender,&kPublicPrefixLegacy,
	 0x00E4B040,&kFaceOpenPrefix2019X64,FaceOpenAbi::unityInternal},
	{"unity-2020.3.34-x64",0x8664,0x626E7CA6,0x01BE3000,
	 {{0x7E,0x65,0x5B,0x3F,0x92,0xED,0x2D,0x4B,0xBA,0x7D,0xCC,0xE5,0x01,0x35,0x19,0x82}},1,0x00F5C220,RenderAbi::publicRender,&kPublicPrefix2020,
	 0x00F5D4A0,&kFaceOpenPrefix2020_3_34X64,FaceOpenAbi::unityInternal},
	{"unity-2020.3.37-x64",0x8664,0x62C42C65,0x01BEE000,
	 {{0xEC,0xCD,0x3D,0x29,0xE0,0x5C,0x9E,0x45,0xA6,0x83,0x39,0x12,0x6C,0xCA,0x0D,0xCC}},1,0x00F62AB0,RenderAbi::publicRender,&kPublicPrefix2020,
	 0x00F63D30,&kFaceOpenPrefix2020_3_34X64,FaceOpenAbi::unityInternal},
	{"unity-2020.3.40-x64",0x8664,0x63219AD9,0x01C07000,
	 {{0x3E,0x66,0xD2,0xB0,0x5A,0xB7,0xB5,0x45,0x85,0x13,0x2E,0xE9,0x7A,0x7D,0x48,0x47}},1,0x00F6B430,RenderAbi::publicRender,&kPublicPrefixWrapper,
	 0x00F6CB20,&kFaceOpenPrefix2020_3_40X64,FaceOpenAbi::unityInternal},
	{"unity-2021.1.29-x64",0x8664,0x68AC7825,0x01BB9000,
	 {{0xAC,0x3B,0x71,0x1B,0xAD,0x45,0x55,0x49,0x8C,0x03,0xC0,0x0A,0x27,0x34,0x9C,0x64}},1,0x00F33EF0,RenderAbi::publicRender,&kPublicPrefix2020,
	 0x00F35170,&kFaceOpenPrefix2020_3_34X64,FaceOpenAbi::unityInternal},
	{"unity-2021.3.38-x64",0x8664,0x6630DCDF,0x01D04000,
	 {{0x1A,0xDF,0x64,0x56,0xAA,0xB6,0xC6,0x45,0xA4,0x0F,0xE4,0xD5,0xD6,0x3E,0x9B,0xEB}},1,0x0101F790,RenderAbi::publicRender,&kPublicPrefixWrapper,
	 0x010226D0,&kFaceOpenPrefix2021_3_38X64,FaceOpenAbi::unityInternal},
	{"unity-2022.3.2-x64",0x8664,0x647FA560,0x01E24000,
	 {{0x40,0x65,0x3A,0x2B,0x29,0x82,0xEF,0x45,0xBF,0xB0,0x1C,0x66,0xA4,0xED,0x63,0x33}},1,0x012AD950,RenderAbi::internalRender,&kInternalPrefixX64,
	 0x012AC360,&kFaceOpenPrefix2022EarlyX64,FaceOpenAbi::unityInternal},
	{"unity-2022.3.4-x64",0x8664,0x6493710F,0x01E25000,
	 {{0x06,0xC5,0x32,0x5F,0x14,0xEC,0xEC,0x48,0xAC,0xB8,0x00,0xE9,0x58,0xA4,0x3D,0x36}},1,0x012AE960,RenderAbi::internalRender,&kInternalPrefixX64,
	 0x012AD370,&kFaceOpenPrefix2022EarlyX64,FaceOpenAbi::unityInternal},
	{"unity-2022.3.10-x64",0x8664,0x6501108B,0x01E2F000,
	 {{0x99,0x51,0x84,0x61,0x3D,0x0F,0x67,0x41,0x84,0x8C,0x2B,0x83,0x03,0x15,0x02,0x03}},1,0x012AA210,RenderAbi::internalRender,&kInternalPrefixX64,
	 0x012A8C20,&kFaceOpenPrefix2022EarlyX64,FaceOpenAbi::unityInternal},
	{"unity-2022.3.62f2-x64",0x8664,0x68D67E4D,0x01E7C000,
	 {{0xBE,0xBF,0x1E,0xA6,0xCD,0x39,0x79,0x4C,0xB4,0x64,0xF3,0x88,0x53,0xDB,0x30,0xB5}},1,0x012E2CE0,RenderAbi::internalRender,&kInternalPrefixX64,
	 0x012E16B0,&kFaceOpenPrefixLateX64,FaceOpenAbi::unityInternal},
	{"unity-2022.3.62f3-x86",0x014C,0x68FB5DE8,0x016A8000,
	 {{0x80,0x71,0x7F,0xE2,0x81,0x6B,0x30,0x40,0xB6,0x34,0x2E,0x0C,0xD5,0xE5,0x76,0x33}},1,0x00DFC1E0,RenderAbi::internalRender,&kInternalPrefixX86,
	 0x00DFADB0,&kFaceOpenPrefix2022_3_62f3X86,FaceOpenAbi::unityInternal},
	{"unity-2022.3.62f3-x64",0x8664,0x68FB626F,0x01E65000,
	 {{0x85,0x43,0x52,0xC6,0x31,0x89,0x9F,0x4F,0x94,0x93,0xD4,0xDD,0xBC,0x52,0x6B,0xCB}},1,0x012CAAE0,RenderAbi::internalRender,&kInternalPrefixX64,
	 0x012C94B0,&kFaceOpenPrefixLateX64,FaceOpenAbi::unityInternal,
	 0x00CDC210,&kTextCoreFontLoadPrefix2022_3_62f3X64,
	 FontLoadAbi::textCorePathSizeFace,
	 0x00CC9540,&kLegacyCharacterLookupPrefix2022_3_62f3X64,
	 CharacterLookupAbi::legacyDynamicFont,
	 0x00CC9130,&kOsFaceResolverPrefix2022_3_62f3X64,
	 CharacterLookupAbi::legacyDynamicFont},
	{"unity-6000.0.50-x64",0x8664,0x68DE7994,0x0210F000,
	 {{0x50,0xCB,0xE4,0x59,0x8B,0x21,0xFA,0x41,0xBA,0xB5,0xFD,0x6B,0xE0,0x8A,0x52,0xB3}},1,0x014622C0,RenderAbi::internalRender,&kInternalPrefixX64,
	 0x01460D00,&kFaceOpenPrefixLateX64,FaceOpenAbi::unityInternal},
	{"unity-6000.0.61-x64",0x8664,0x68F891E5,0x0211E000,
	 {{0xF3,0xE9,0xCA,0x21,0xF7,0x18,0x37,0x4D,0x8F,0x8A,0xF7,0x84,0x15,0x6A,0x20,0xF8}},1,0x0146D9D0,RenderAbi::internalRender,&kInternalPrefixX64,
	 0x0146C410,&kFaceOpenPrefixLateX64,FaceOpenAbi::unityInternal},
	{"unity-6000.0.62-x64",0x8664,0x6902F1F8,0x0210D000,
	 {{0x01,0x5D,0x60,0x95,0x0D,0xCD,0xDF,0x4F,0x98,0x1E,0xC2,0x4B,0x13,0x06,0x48,0xE2}},1,0x01464760,RenderAbi::internalRender,&kInternalPrefixX64,
	 0x014631A0,&kFaceOpenPrefixLateX64,FaceOpenAbi::unityInternal},
	{"unity-6000.3.21-x64",0x8664,0x6A607C81,0x02457000,
	 {{0x64,0xA7,0x50,0xE1,0xE7,0xC9,0xF0,0x44,0x89,0x07,0xDE,0x2D,0xB3,0x87,0x2D,0x5D}},1,0x0166D120,RenderAbi::internalRender,&kInternalPrefixX64,
	 0x0166B910,&kFaceOpenPrefixLateX64,FaceOpenAbi::unityInternal},
};

bool CheckedAdd(std::size_t left, std::size_t right, std::size_t* result) noexcept
{
	if (result == nullptr || right > std::numeric_limits<std::size_t>::max() - left)
		return false;
	*result = left + right;
	return true;
}

bool IsReadableProtection(DWORD protection) noexcept
{
	switch (protection & 0xffu)
	{
	case PAGE_READONLY:
	case PAGE_READWRITE:
	case PAGE_WRITECOPY:
	case PAGE_EXECUTE_READ:
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
		return true;
	default:
		return false;
	}
}

bool IsWritableProtection(DWORD protection) noexcept
{
	switch (protection & 0xffu)
	{
	case PAGE_READWRITE:
	case PAGE_WRITECOPY:
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
		return true;
	default:
		return false;
	}
}

bool IsExecutableProtection(DWORD protection) noexcept
{
	switch (protection & 0xffu)
	{
	case PAGE_EXECUTE:
	case PAGE_EXECUTE_READ:
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
		return true;
	default:
		return false;
	}
}

template <typename Predicate>
bool IsMemoryRange(
	const void* address,
	std::size_t length,
	Predicate predicate) noexcept
{
	if (address == nullptr || length == 0)
		return false;
	std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(address);
	if (length > std::numeric_limits<std::uintptr_t>::max() - cursor)
		return false;
	std::uintptr_t const end = cursor + length;
	while (cursor < end)
	{
		MEMORY_BASIC_INFORMATION memory{};
		if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) == 0 ||
			memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) != 0 ||
			!predicate(memory.Protect))
			return false;
		std::uintptr_t const base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
		if (memory.RegionSize == 0 ||
			memory.RegionSize > std::numeric_limits<std::uintptr_t>::max() - base)
			return false;
		std::uintptr_t const regionEnd = base + memory.RegionSize;
		if (regionEnd <= cursor)
			return false;
		cursor = (std::min)(regionEnd, end);
	}
	return true;
}

class MappedImageView final
{
public:
	MappedImageView(const void* bytes, std::size_t size) noexcept
		: bytes_(static_cast<const unsigned char*>(bytes)), size_(size)
	{
		valid_ = Initialize();
	}

	bool valid() const noexcept { return valid_; }
	WORD machine() const noexcept { return machine_; }
	DWORD timestamp() const noexcept { return timestamp_; }
	DWORD image_size() const noexcept { return imageSize_; }

	bool CopyRva(DWORD rva, void* destination, std::size_t length) const noexcept
	{
		return Read(static_cast<std::size_t>(rva), destination, length);
	}

	bool IsExecutableRva(DWORD rva, std::size_t length) const noexcept
	{
		if (!valid_ || length == 0)
			return false;
		for (WORD index = 0; index < sectionCount_; ++index)
		{
			IMAGE_SECTION_HEADER section{};
			if (!Read(
				sectionTableOffset_ + static_cast<std::size_t>(index) * sizeof(section),
				&section, sizeof(section)))
				return false;
			std::uint64_t const start = section.VirtualAddress;
			std::uint64_t const span = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
			std::uint64_t const end = start + span;
			std::uint64_t const requestedEnd = static_cast<std::uint64_t>(rva) + length;
			if (rva >= start && requestedEnd <= end)
				return (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
		}
		return false;
	}

	bool ReadCodeView(
		std::array<unsigned char, 16>* guid,
		DWORD* age) const noexcept
	{
		if (!valid_ || guid == nullptr || age == nullptr ||
			debugDirectory_.VirtualAddress == 0 ||
			debugDirectory_.Size < sizeof(IMAGE_DEBUG_DIRECTORY))
			return false;
		std::size_t const count =
			debugDirectory_.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
		for (std::size_t index = 0; index < count; ++index)
		{
			IMAGE_DEBUG_DIRECTORY entry{};
			if (!CopyRva(
				debugDirectory_.VirtualAddress +
					static_cast<DWORD>(index * sizeof(entry)),
				&entry, sizeof(entry)))
				return false;
			if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW ||
				entry.SizeOfData < 24 || entry.AddressOfRawData == 0)
				continue;
			std::array<unsigned char, 24> codeView{};
			if (!CopyRva(entry.AddressOfRawData, codeView.data(), codeView.size()) ||
				std::memcmp(codeView.data(), "RSDS", 4) != 0)
				continue;
			std::copy_n(codeView.data() + 4, guid->size(), guid->begin());
			std::memcpy(age, codeView.data() + 20, sizeof(*age));
			return true;
		}
		return false;
	}

private:
	bool Read(std::size_t offset, void* destination, std::size_t length) const noexcept
	{
		if (bytes_ == nullptr || destination == nullptr || length == 0 ||
			offset > size_ || length > size_ - offset)
			return false;
		std::uintptr_t const base = reinterpret_cast<std::uintptr_t>(bytes_);
		if (offset > std::numeric_limits<std::uintptr_t>::max() - base)
			return false;
		const void* const source = reinterpret_cast<const void*>(base + offset);
		if (!IsMemoryRange(source, length, IsReadableProtection))
			return false;
		std::memcpy(destination, source, length);
		return true;
	}

	bool Initialize() noexcept
	{
		IMAGE_DOS_HEADER dos{};
		if (!Read(0, &dos, sizeof(dos)) ||
			dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
			return false;
		std::size_t const ntOffset = static_cast<std::size_t>(dos.e_lfanew);
		DWORD signature = 0;
		if (!Read(ntOffset, &signature, sizeof(signature)) ||
			signature != IMAGE_NT_SIGNATURE)
			return false;
		std::size_t fileHeaderOffset = 0;
		if (!CheckedAdd(ntOffset, sizeof(signature), &fileHeaderOffset))
			return false;
		IMAGE_FILE_HEADER file{};
		if (!Read(fileHeaderOffset, &file, sizeof(file)) ||
			file.NumberOfSections == 0 ||
			(file.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == 0)
			return false;
		machine_ = file.Machine;
		timestamp_ = file.TimeDateStamp;
		std::size_t optionalOffset = 0;
		if (!CheckedAdd(fileHeaderOffset, sizeof(file), &optionalOffset))
			return false;
		WORD magic = 0;
		if (!Read(optionalOffset, &magic, sizeof(magic)))
			return false;
		DWORD sizeOfHeaders = 0;
		if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
		{
			if (file.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32))
				return false;
			IMAGE_OPTIONAL_HEADER32 optional{};
			if (!Read(optionalOffset, &optional, sizeof(optional)) ||
				optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
				return false;
			imageSize_ = optional.SizeOfImage;
			sizeOfHeaders = optional.SizeOfHeaders;
			debugDirectory_ = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
		}
		else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		{
			if (file.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
				return false;
			IMAGE_OPTIONAL_HEADER64 optional{};
			if (!Read(optionalOffset, &optional, sizeof(optional)) ||
				optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
				return false;
			imageSize_ = optional.SizeOfImage;
			sizeOfHeaders = optional.SizeOfHeaders;
			debugDirectory_ = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
		}
		else
		{
			return false;
		}
		if (imageSize_ == 0 || imageSize_ > size_ || sizeOfHeaders == 0 ||
			sizeOfHeaders > imageSize_)
			return false;
		if (debugDirectory_.VirtualAddress == 0 || debugDirectory_.Size == 0 ||
			debugDirectory_.VirtualAddress >= imageSize_ ||
			debugDirectory_.Size > imageSize_ - debugDirectory_.VirtualAddress)
			return false;
		sectionTableOffset_ = optionalOffset + file.SizeOfOptionalHeader;
		sectionCount_ = file.NumberOfSections;
		std::size_t const sectionBytes =
			static_cast<std::size_t>(sectionCount_) * sizeof(IMAGE_SECTION_HEADER);
		if (sectionTableOffset_ > sizeOfHeaders ||
			sectionBytes > sizeOfHeaders - sectionTableOffset_ ||
			sectionTableOffset_ > size_ || sectionBytes > size_ - sectionTableOffset_)
			return false;
		return true;
	}

	const unsigned char* bytes_ = nullptr;
	std::size_t size_ = 0;
	bool valid_ = false;
	WORD machine_ = 0;
	WORD sectionCount_ = 0;
	DWORD timestamp_ = 0;
	DWORD imageSize_ = 0;
	std::size_t sectionTableOffset_ = 0;
	IMAGE_DATA_DIRECTORY debugDirectory_{};
};

} // namespace

std::shared_ptr<const FontFileRedirectTable> FontFileRedirectTable::Build(
	const std::vector<InstalledFontFace>& installedFonts,
	const font_substitution::Snapshot& substitutions) noexcept
{
	try
	{
		std::vector<Redirect> redirects;
		std::vector<InstalledFontFace> ambiguousSources;
		for (const font_substitution::Rule& rule : substitutions.rules())
		{
			font_substitution::Resolution const resolution = substitutions.Resolve(
				font_substitution::Request(
					rule.sourceFamily,
					rule.charsetSpecific ? rule.charset : DEFAULT_CHARSET));
			if (!resolution.matched ||
				resolution.status != font_substitution::ResolutionStatus::applied)
				continue;
			std::vector<InstalledFontFace> const sources = PathsForFamily(
				installedFonts, rule.sourceFamily);
			std::vector<InstalledFontFace> const replacements = PathsForFamily(
				installedFonts, resolution.family);
			if (sources.empty() || replacements.size() != 1)
				continue;
			for (const InstalledFontFace& source : sources)
			{
				std::vector<std::wstring> sourceFamilies =
					FamiliesForFace(installedFonts, source);
				if (sourceFamilies.empty())
					sourceFamilies.push_back(rule.sourceFamily);
				if (source.faceIndex == replacements.front().faceIndex &&
					EqualOrdinalIgnoreCase(
						source.filePath, replacements.front().filePath))
					continue;
				auto existing = std::find_if(
					redirects.begin(), redirects.end(),
					[&](const auto& redirect) {
						return redirect.sourceFaceIndex == source.faceIndex &&
							EqualOrdinalIgnoreCase(
								redirect.sourcePath, source.filePath);
					});
				if (existing == redirects.end())
				{
					redirects.push_back({
						source.filePath,
						source.faceIndex,
						std::move(sourceFamilies),
						replacements.front().filePath,
						replacements.front().faceIndex});
				}
				else if (existing->replacementFaceIndex !=
						replacements.front().faceIndex ||
					!EqualOrdinalIgnoreCase(
						existing->replacementPath,
						replacements.front().filePath))
				{
					ambiguousSources.push_back(source);
				}
				else
				{
					for (std::wstring& family : sourceFamilies)
					{
						if (std::none_of(
							existing->sourceFamilies.begin(),
							existing->sourceFamilies.end(),
							[&](const std::wstring& candidate) {
								return EqualOrdinalIgnoreCase(candidate, family);
							}))
							existing->sourceFamilies.push_back(std::move(family));
					}
				}
			}
		}
		redirects.erase(
			std::remove_if(
				redirects.begin(), redirects.end(),
				[&](const auto& redirect) {
					return std::any_of(
						ambiguousSources.begin(), ambiguousSources.end(),
						[&](const InstalledFontFace& source) {
							return source.faceIndex == redirect.sourceFaceIndex &&
								EqualOrdinalIgnoreCase(
									source.filePath, redirect.sourcePath);
						});
				}),
			redirects.end());
		return std::shared_ptr<const FontFileRedirectTable>(
			new FontFileRedirectTable(std::move(redirects)));
	}
	catch (...)
	{
		return {};
	}
}

bool FontFileRedirectTable::Resolve(
	const wchar_t* requestedPath,
	std::wstring& replacementPath) const noexcept
{
	try
	{
		replacementPath.clear();
		std::wstring normalized;
		if (!NormalizeAbsolutePath(requestedPath, normalized))
			return false;
		for (const auto& redirect : redirects_)
		{
			if (EqualOrdinalIgnoreCase(redirect.sourcePath, normalized))
			{
				if (replacementPath.empty())
					replacementPath = redirect.replacementPath;
				else if (!EqualOrdinalIgnoreCase(
					replacementPath, redirect.replacementPath))
				{
					replacementPath.clear();
					return false;
				}
			}
		}
		return !replacementPath.empty();
	}
	catch (...)
	{
		replacementPath.clear();
	}
	return false;
}

bool FontFileRedirectTable::ResolveFace(
	const wchar_t* requestedPath,
	long requestedFaceIndex,
	std::wstring& replacementPath,
	long& replacementFaceIndex) const noexcept
{
	replacementPath.clear();
	replacementFaceIndex = 0;
	try
	{
		std::wstring normalized;
		if (!NormalizeAbsolutePath(requestedPath, normalized))
			return false;
		for (const Redirect& redirect : redirects_)
		{
			if (redirect.sourceFaceIndex == requestedFaceIndex &&
				EqualOrdinalIgnoreCase(redirect.sourcePath, normalized))
			{
				replacementPath = redirect.replacementPath;
				replacementFaceIndex = redirect.replacementFaceIndex;
				return true;
			}
		}
	}
	catch (...)
	{
		replacementPath.clear();
		replacementFaceIndex = 0;
	}
	return false;
}

bool FontFileRedirectTable::ResolveFamilyFace(
	const wchar_t* requestedFamily,
	std::wstring& replacementPath,
	long& replacementFaceIndex) const noexcept
{
	try
	{
		replacementPath.clear();
		replacementFaceIndex = 0;
		if (requestedFamily == nullptr || *requestedFamily == L'\0')
			return false;
		for (const Redirect& redirect : redirects_)
		{
			bool const matches = std::any_of(
				redirect.sourceFamilies.begin(), redirect.sourceFamilies.end(),
				[&](const std::wstring& family) {
					return EqualOrdinalIgnoreCase(family, requestedFamily);
				});
			if (!matches)
				continue;
			if (replacementPath.empty())
			{
				replacementPath = redirect.replacementPath;
				replacementFaceIndex = redirect.replacementFaceIndex;
			}
			else if (replacementFaceIndex != redirect.replacementFaceIndex ||
				!EqualOrdinalIgnoreCase(
					replacementPath, redirect.replacementPath))
			{
				replacementPath.clear();
				replacementFaceIndex = 0;
				return false;
			}
		}
		return !replacementPath.empty();
	}
	catch (...)
	{
		replacementPath.clear();
		replacementFaceIndex = 0;
		return false;
	}
}

bool ResolveFaceOpenPath(
	const FontFileRedirectTable& redirects,
	unsigned int openFlags,
	const char* requestedUtf8,
	long requestedFaceIndex,
	FaceOpenPathRedirect& redirect) noexcept
{
	try
	{
		FaceOpenPathRedirect candidate;
		constexpr unsigned int kOpenSourceMask = 0x07;
		constexpr unsigned int kOpenPathname = 0x04;
		if ((openFlags & kOpenSourceMask) != kOpenPathname ||
			!Utf8PathToWide(requestedUtf8, candidate.sourcePath) ||
			!redirects.ResolveFace(
				candidate.sourcePath.c_str(), requestedFaceIndex,
				candidate.replacementPath,
				candidate.replacementFaceIndex) ||
			!WidePathToUtf8(
				candidate.replacementPath, candidate.replacementUtf8))
		{
			redirect = {};
			return false;
		}
		redirect = std::move(candidate);
		return true;
	}
	catch (...)
	{
		redirect = {};
		return false;
	}
}

bool ResolveTextCoreFontLoadPath(
	const FontFileRedirectTable& redirects,
	const char* requestedUtf8,
	long requestedFaceIndex,
	FaceOpenPathRedirect& redirect) noexcept
{
	try
	{
		FaceOpenPathRedirect candidate;
		if (!Utf8PathToWide(requestedUtf8, candidate.sourcePath) ||
			!redirects.ResolveFace(
				candidate.sourcePath.c_str(), requestedFaceIndex,
				candidate.replacementPath,
				candidate.replacementFaceIndex) ||
			!WidePathToUtf8(
				candidate.replacementPath, candidate.replacementUtf8))
		{
			redirect = {};
			return false;
		}
		redirect = std::move(candidate);
		return true;
	}
	catch (...)
	{
		redirect = {};
		return false;
	}
}

bool ReadLegacyFontRefFamily(
	const void* fontRef,
	std::wstring& family) noexcept
{
	family.clear();
	try
	{
		constexpr std::size_t kObjectSize = 40;
		constexpr std::size_t kMaximumFamilyBytes = 32767;
		if (!IsMemoryRange(fontRef, kObjectSize, IsReadableProtection))
			return false;
		std::array<unsigned char, kObjectSize> object{};
		std::memcpy(object.data(), fontRef, object.size());
		const char* data = nullptr;
		std::size_t length = 0;
		if (object[32] == 1)
		{
			std::int8_t const remaining =
				static_cast<std::int8_t>(object[24]);
			if (remaining < 0 || remaining > 24)
				return false;
			length = 24 - static_cast<std::size_t>(remaining);
			data = reinterpret_cast<const char*>(object.data());
		}
		else
		{
			std::memcpy(&data, object.data(), sizeof(data));
			std::memcpy(&length, object.data() + 16, sizeof(length));
			if (data == nullptr || length > kMaximumFamilyBytes ||
				!IsMemoryRange(data, length, IsReadableProtection))
				return false;
		}
		if (length == 0 || length > kMaximumFamilyBytes ||
			std::memchr(data, '\0', length) != nullptr)
			return false;
		std::string utf8(data, length);
		utf8.push_back('\0');
		return Utf8PathToWide(utf8.c_str(), family);
	}
	catch (...)
	{
		family.clear();
		return false;
	}
}

FontSubstitutionBoundary SelectFontSubstitutionBoundary(
	const ResolvedAdapter& adapter) noexcept
{
	if (adapter.fontLoadRva != 0 &&
		adapter.fontLoadAbi == FontLoadAbi::textCorePathSizeFace)
		return FontSubstitutionBoundary::textCoreFontLoad;
	if (adapter.faceOpenRva != 0 &&
		adapter.faceOpenAbi == FaceOpenAbi::unityInternal)
		return FontSubstitutionBoundary::freeTypeFaceOpen;
	return FontSubstitutionBoundary::unavailable;
}

bool ResolveAdapter(
	const void* mappedImage,
	std::size_t mappedSize,
	const AdapterDescriptor* descriptors,
	std::size_t descriptorCount,
	ResolvedAdapter* resolved) noexcept
{
	if (descriptors == nullptr || descriptorCount == 0 || resolved == nullptr)
		return false;
	MappedImageView const image(mappedImage, mappedSize);
	if (!image.valid())
		return false;
	std::array<unsigned char, 16> guid{};
	DWORD age = 0;
	if (!image.ReadCodeView(&guid, &age))
		return false;

	const AdapterDescriptor* match = nullptr;
	for (std::size_t index = 0; index < descriptorCount; ++index)
	{
		const AdapterDescriptor& candidate = descriptors[index];
		if (candidate.machine != image.machine() ||
			candidate.timestamp != image.timestamp() ||
			candidate.imageSize != image.image_size() ||
			candidate.pdbGuid != guid || candidate.pdbAge != age)
			continue;
		if (match != nullptr || candidate.targetPrefix == nullptr ||
			!image.IsExecutableRva(candidate.targetRva, candidate.targetPrefix->size()))
			return false;
		std::array<unsigned char, 32> prefix{};
		if (!image.CopyRva(candidate.targetRva, prefix.data(), prefix.size()) ||
			prefix != *candidate.targetPrefix)
			return false;
		bool const hasFaceOpen = candidate.faceOpenRva != 0;
		if (hasFaceOpen != (candidate.faceOpenPrefix != nullptr) ||
			hasFaceOpen !=
				(candidate.faceOpenAbi != FaceOpenAbi::unavailable))
			return false;
		if (hasFaceOpen)
		{
			if (!image.IsExecutableRva(
				candidate.faceOpenRva, candidate.faceOpenPrefix->size()))
				return false;
			std::array<unsigned char, 32> faceOpenPrefix{};
			if (!image.CopyRva(
				candidate.faceOpenRva, faceOpenPrefix.data(),
				faceOpenPrefix.size()) ||
				faceOpenPrefix != *candidate.faceOpenPrefix)
				return false;
		}
		bool const hasFontLoad = candidate.fontLoadRva != 0;
		if (hasFontLoad != (candidate.fontLoadPrefix != nullptr) ||
			hasFontLoad !=
				(candidate.fontLoadAbi != FontLoadAbi::unavailable))
			return false;
		if (hasFontLoad)
		{
			if (!image.IsExecutableRva(
				candidate.fontLoadRva, candidate.fontLoadPrefix->size()))
				return false;
			std::array<unsigned char, 32> fontLoadPrefix{};
			if (!image.CopyRva(
				candidate.fontLoadRva, fontLoadPrefix.data(),
				fontLoadPrefix.size()) ||
				fontLoadPrefix != *candidate.fontLoadPrefix)
				return false;
		}
		bool const hasCharacterLookup = candidate.characterLookupRva != 0;
		if (hasCharacterLookup != (candidate.characterLookupPrefix != nullptr) ||
			hasCharacterLookup !=
				(candidate.characterLookupAbi != CharacterLookupAbi::unavailable))
			return false;
		if (hasCharacterLookup)
		{
			if (!image.IsExecutableRva(
				candidate.characterLookupRva,
				candidate.characterLookupPrefix->size()))
				return false;
			std::array<unsigned char, 32> characterLookupPrefix{};
			if (!image.CopyRva(
				candidate.characterLookupRva, characterLookupPrefix.data(),
				characterLookupPrefix.size()) ||
				characterLookupPrefix != *candidate.characterLookupPrefix)
				return false;
		}
		bool const hasOsFaceResolver = candidate.osFaceResolverRva != 0;
		if (hasOsFaceResolver != (candidate.osFaceResolverPrefix != nullptr) ||
			hasOsFaceResolver !=
				(candidate.osFaceResolverAbi != CharacterLookupAbi::unavailable))
			return false;
		if (hasOsFaceResolver)
		{
			if (!image.IsExecutableRva(
				candidate.osFaceResolverRva,
				candidate.osFaceResolverPrefix->size()))
				return false;
			std::array<unsigned char, 32> osFaceResolverPrefix{};
			if (!image.CopyRva(
				candidate.osFaceResolverRva, osFaceResolverPrefix.data(),
				osFaceResolverPrefix.size()) ||
				osFaceResolverPrefix != *candidate.osFaceResolverPrefix)
				return false;
		}
		match = &candidate;
	}
	if (match == nullptr)
		return false;
	resolved->name = match->name;
	resolved->targetRva = match->targetRva;
	resolved->faceOpenRva = match->faceOpenRva;
	resolved->fontLoadRva = match->fontLoadRva;
	resolved->characterLookupRva = match->characterLookupRva;
	resolved->osFaceResolverRva = match->osFaceResolverRva;
	resolved->abi = match->abi;
	resolved->faceOpenAbi = match->faceOpenAbi;
	resolved->fontLoadAbi = match->fontLoadAbi;
	resolved->characterLookupAbi = match->characterLookupAbi;
	resolved->osFaceResolverAbi = match->osFaceResolverAbi;
	return true;
}

const AdapterDescriptor* ProductionAdapterDescriptors(std::size_t* count) noexcept
{
	if (count != nullptr)
		*count = _countof(kProductionDescriptors);
	return kProductionDescriptors;
}

bool IsExecutableMemoryRange(const void* address, std::size_t length) noexcept
{
	return IsMemoryRange(address, length, IsExecutableProtection);
}

bool ApplyCoverage(
	unsigned char* buffer,
	int pitch,
	unsigned int rows,
	unsigned int width,
	unsigned char pixelMode,
	const UnityCoverageLut& lut) noexcept
{
	if (buffer == nullptr || rows == 0 || width == 0 ||
		(pixelMode != kPixelModeGray && pixelMode != kPixelModeLcd &&
		 pixelMode != kPixelModeLcdVertical))
		return false;
	std::int64_t const signedPitch = pitch;
	std::uint64_t const absolutePitch = signedPitch < 0
		? static_cast<std::uint64_t>(-signedPitch)
		: static_cast<std::uint64_t>(signedPitch);
	std::uint64_t const total = absolutePitch * rows;
	if (absolutePitch == 0 || width > absolutePitch ||
		total > kMaximumBitmapBytes ||
		total > std::numeric_limits<std::size_t>::max() ||
		!IsMemoryRange(buffer, static_cast<std::size_t>(total), IsWritableProtection))
		return false;

	for (unsigned int row = 0; row < rows; ++row)
	{
		freetype::BitmapRowView const rowView = freetype::CheckedBitmapRow(
			buffer, pitch, rows, row, width);
		if (!rowView)
			return false;
		unsigned char* const bytes = const_cast<unsigned char*>(rowView.data);
		for (unsigned int column = 0; column < width; ++column)
		{
			if (pixelMode == kPixelModeGray)
				bytes[column] = lut.gray[bytes[column]];
			else if (pixelMode == kPixelModeLcd)
				bytes[column] = lut.rgb[column % 3][bytes[column]];
			else
				bytes[column] = lut.rgb[row % 3][bytes[column]];
		}
	}
	return true;
}

}} // namespace renderer::unity
