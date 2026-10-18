#include "unity_font_hook_core.h"

#include "big_endian.h"
#include "freetype_runtime.h"
#include "renderer_raii.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <set>
#include <string_view>

namespace renderer { namespace unity {
namespace {

constexpr unsigned char kPixelModeGray = 2;
constexpr unsigned char kPixelModeLcd = 5;
constexpr unsigned char kPixelModeLcdVertical = 6;
constexpr std::uint64_t kMaximumBitmapBytes = 64ULL * 1024ULL * 1024ULL;

std::optional<std::uint16_t> ReadBigEndian16(
	std::span<const unsigned char> bytes,
	std::size_t offset) noexcept
{
	return big_endian::Read16(bytes, offset);
}

std::optional<std::uint32_t> ReadBigEndian32(
	std::span<const unsigned char> bytes,
	std::size_t offset) noexcept
{
	return big_endian::Read32(bytes, offset);
}

std::optional<std::uint32_t> ReadSfntTag(
	std::span<const unsigned char> bytes,
	std::size_t offset) noexcept
{
	auto const read = big_endian::Read32(bytes, offset);
	if (!read)
		return std::nullopt;
	return ((*read & 0x000000ffU) << 24) |
		((*read & 0x0000ff00U) << 8) |
		((*read & 0x00ff0000U) >> 8) |
		((*read & 0xff000000U) >> 24);
}

bool EqualOrdinalIgnoreCase(
	std::wstring_view left,
	std::wstring_view right) noexcept
{
	if (left.size() != right.size() ||
		left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
		return false;
	return CompareStringOrdinal(
		left.data(), static_cast<int>(left.size()),
		right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::optional<std::wstring> NormalizeAbsolutePath(const wchar_t* value)
{
	if (value == nullptr || *value == L'\0')
		return std::nullopt;
	std::wstring normalized(value);
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
	if (!driveAbsolute && !uncAbsolute)
		return std::nullopt;
	return normalized;
}

std::optional<std::wstring> Utf8PathToWide(const char* value)
{
	if (value == nullptr)
		return std::nullopt;
	constexpr std::size_t kMaximumPathBytes = 32768;
	std::size_t const length = strnlen_s(value, kMaximumPathBytes);
	if (length == 0 || length == kMaximumPathBytes ||
		length > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
		return std::nullopt;
	int const required = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, value, static_cast<int>(length),
		nullptr, 0);
	if (required <= 0)
		return std::nullopt;
	std::wstring converted(static_cast<std::size_t>(required), L'\0');
	if (MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, value, static_cast<int>(length),
			converted.data(), required) != required)
		return std::nullopt;
	return converted;
}

std::optional<std::string> WidePathToUtf8(std::wstring_view value)
{
	if (value.empty() ||
		value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
		return std::nullopt;
	int const required = WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (required <= 0)
		return std::nullopt;
	std::string converted(static_cast<std::size_t>(required), '\0');
	if (WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
			static_cast<int>(value.size()), converted.data(), required,
			nullptr, nullptr) != required)
		return std::nullopt;
	return converted;
}

std::vector<InstalledFontFace> PathsForFamily(
	const std::vector<InstalledFontFace>& installedFonts,
	std::wstring_view family)
{
	std::vector<InstalledFontFace> paths;
	for (const InstalledFontFace& font : installedFonts)
	{
		if (!EqualOrdinalIgnoreCase(font.family, family))
			continue;
		auto normalized = NormalizeAbsolutePath(font.filePath.c_str());
		if (!normalized)
			continue;
		if (std::none_of(paths.begin(), paths.end(), [&](const InstalledFontFace& path) {
				return path.faceIndex == font.faceIndex &&
					EqualOrdinalIgnoreCase(path.filePath, *normalized);
			}))
			paths.push_back({
				font.family, std::move(*normalized), font.faceIndex,
				font.axisValues});
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
		auto normalized = NormalizeAbsolutePath(font.filePath.c_str());
		if (!normalized ||
			!EqualOrdinalIgnoreCase(*normalized, requestedFace.filePath))
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
constexpr std::array<unsigned char, 32> kFreeTypeCharIndexPrefix2019_4_41X64{{
	0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x33,0xFF,0x48,0x8B,0xD9,0x48,
	0x85,0xC9,0x74,0x24,0x48,0x8B,0x89,0x88,0x00,0x00,0x00,0x48,0x85,0xC9,0x74,0x18,
}};
constexpr std::array<unsigned char, 32> kFontCatalogLoadPrefix2019_4_41X64{{
	0x48,0x8B,0xC4,0x55,0x53,0x56,0x41,0x54,0x41,0x55,0x41,0x57,0x48,0x8D,0x68,0xB8,
	0x48,0x81,0xEC,0x18,0x01,0x00,0x00,0x44,0x8B,0x7D,0x58,0x33,0xF6,0x48,0x89,0x78,
}};
// Unity 6000.3.21: TextRenderingPrivate::ReadFontFileAndAddToCache(const core::string&)
constexpr std::array<unsigned char, 32> kFontCatalogLoadPrefix6000_3_21X64{{
	0x4C,0x8B,0xDC,0x55,0x53,0x57,0x49,0x8D,0xAB,0x28,0xFF,0xFF,0xFF,0x48,0x81,0xEC,
	0xC0,0x01,0x00,0x00,0x4D,0x89,0x6B,0xD8,0xB8,0x4B,0x00,0x00,0x00,0x4D,0x89,0x73,
}};
constexpr std::array<unsigned char, 32> kOsFaceResolverPrefix6000_3_21X64{{
	0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x55,0x57,0x41,0x54,0x41,0x55,
	0x41,0x57,0x48,0x8D,0x6C,0x24,0xC9,0x48,0x81,0xEC,0xC0,0x00,0x00,0x00,0x48,0x8B,
}};
constexpr std::array<unsigned char, 32> kFaceDonePrefix6000_3_21X64{{
	0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x85,0xC9,0x0F,0x84,0xD1,0x00,
	0x00,0x00,0x48,0x83,0xB9,0x90,0x00,0x00,0x00,0x00,0x0F,0x84,0xC3,0x00,0x00,0x00,
}};
// Unity 6000.3.21: DynamicFontData::GetFaceForCharacterIfAvailableInFont(
//     const FontRef&, const core::vector<PPtr<Font>>&, unsigned int)
constexpr std::array<unsigned char, 32> kLegacyCharacterLookupPrefix6000_3_21X64{{
	0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x6C,0x24,0x20,0x56,0x57,0x41,0x56,0x48,0x83,
	0xEC,0x70,0x41,0x8B,0xF1,0x49,0x8B,0xE8,0x48,0x8B,0xDA,0x4C,0x8B,0xF1,0xE8,0xED,
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
	 0x00E4B040,&kFaceOpenPrefix2019X64,FaceOpenAbi::unityInternal,
	 0,nullptr,FontLoadAbi::unavailable,
	 0,nullptr,CharacterLookupAbi::unavailable,
	 0,nullptr,CharacterLookupAbi::unavailable,
	 0x00E48FF0,&kFreeTypeCharIndexPrefix2019_4_41X64,
	 FreeTypeCharIndexAbi::standard,
	 0x00C28960,&kFontCatalogLoadPrefix2019_4_41X64,
	 FontCatalogLoadAbi::systemCatalogEntry},
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
	// Unity caches the selected face before a late attach, so this build needs
	// the character-lookup and final face-release boundaries. Its undecodable
	// FontRef leaves the opened face's family name as the safe identity. The
	// pathname catalog stays native, and TextCore::FontEngine::LoadFontFace
	// stays unhooked because this build added a fourth parameter.
	{"unity-6000.3.21-x64",0x8664,0x6A607C81,0x02457000,
	 {{0x64,0xA7,0x50,0xE1,0xE7,0xC9,0xF0,0x44,0x89,0x07,0xDE,0x2D,0xB3,0x87,0x2D,0x5D}},1,0x0166D120,RenderAbi::internalRender,&kInternalPrefixX64,
	 0x0166B910,&kFaceOpenPrefixLateX64,FaceOpenAbi::unityInternal,
	 0,nullptr,FontLoadAbi::unavailable,
	 0x00D9F110,&kLegacyCharacterLookupPrefix6000_3_21X64,
	 CharacterLookupAbi::legacyDynamicFont,
	 0x00D9EC70,&kOsFaceResolverPrefix6000_3_21X64,
	 CharacterLookupAbi::legacyDynamicFont,
	 0,nullptr,FreeTypeCharIndexAbi::unavailable,
	 0x00D9E250,&kFontCatalogLoadPrefix6000_3_21X64,
	 FontCatalogLoadAbi::systemCatalogEntry,
	 0x0166C2A0,&kFaceDonePrefix6000_3_21X64,
	 FaceDoneAbi::standard},
};

std::optional<std::size_t> CheckedAdd(
	std::size_t left, std::size_t right) noexcept
{
	if (right > std::numeric_limits<std::size_t>::max() - left)
		return std::nullopt;
	return left + right;
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

struct NamedInstanceFaceIndexCacheEntry final
{
	long baseFaceIndex = 0;
	std::vector<AxisValue> axisValues;
	long resolvedFaceIndex = 0;
};

struct NamedInstanceFileCacheEntry final
{
	std::wstring path;
	bool bytesAvailable = false;
	std::vector<unsigned char> bytes;
	std::vector<NamedInstanceFaceIndexCacheEntry> resolvedIndices;
};

std::optional<std::vector<unsigned char>> ReadFontFileBytes(
	const std::wstring& path)
{
	renderer_raii::UniqueHandle file(CreateFileW(
		path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (!file)
		return std::nullopt;
	LARGE_INTEGER fileSize{};
	constexpr LONGLONG kMaximumVariableFontBytes = 64LL * 1024LL * 1024LL;
	if (!GetFileSizeEx(file.get(), &fileSize) || fileSize.QuadPart <= 0 ||
		fileSize.QuadPart > kMaximumVariableFontBytes ||
		static_cast<ULONGLONG>(fileSize.QuadPart) >
			(std::numeric_limits<std::size_t>::max)())
		return std::nullopt;
	std::vector<unsigned char> bytes(
		static_cast<std::size_t>(fileSize.QuadPart));
	DWORD bytesRead = 0;
	if (!ReadFile(
		file.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
		&bytesRead, nullptr) ||
		bytesRead != static_cast<DWORD>(bytes.size()))
		return std::nullopt;
	return bytes;
}

bool EqualAxisValues(
	const std::vector<AxisValue>& left,
	const std::vector<AxisValue>& right) noexcept
{
	return left.size() == right.size() &&
		std::equal(
			left.begin(), left.end(), right.begin(),
			[](const AxisValue& leftValue, const AxisValue& rightValue) {
				return leftValue.tag == rightValue.tag &&
					leftValue.value == rightValue.value;
			});
}

long ResolveReplacementFaceIndex(
	const InstalledFontFace& replacement,
	std::vector<NamedInstanceFileCacheEntry>& cache)
{
	if (replacement.axisValues.empty() || replacement.faceIndex < 0 ||
		replacement.faceIndex > 0xFFFF)
		return replacement.faceIndex;

	std::vector<AxisValue> sortedAxisValues = replacement.axisValues;
	std::stable_sort(
		sortedAxisValues.begin(), sortedAxisValues.end(),
		[](const AxisValue& left, const AxisValue& right) {
			return left.tag < right.tag;
		});
	auto file = std::find_if(
		cache.begin(), cache.end(), [&](const auto& entry) {
			return EqualOrdinalIgnoreCase(entry.path, replacement.filePath);
		});
	if (file == cache.end())
	{
		cache.push_back({replacement.filePath});
		file = std::prev(cache.end());
		auto bytes = ReadFontFileBytes(file->path);
		file->bytesAvailable = bytes.has_value();
		if (bytes)
			file->bytes = std::move(*bytes);
	}
	auto const cached = std::find_if(
		file->resolvedIndices.begin(), file->resolvedIndices.end(),
		[&](const NamedInstanceFaceIndexCacheEntry& entry) {
			return entry.baseFaceIndex == replacement.faceIndex &&
				EqualAxisValues(entry.axisValues, sortedAxisValues);
		});
	if (cached != file->resolvedIndices.end())
		return cached->resolvedFaceIndex;

	long resolvedFaceIndex = replacement.faceIndex;
	std::optional<long> namedInstance;
	if (file->bytesAvailable)
		namedInstance = ParseSfntNamedInstanceIndex(
			file->bytes, replacement.faceIndex, sortedAxisValues);
	if (namedInstance && *namedInstance != 0)
	{
		unsigned long const encoded =
			static_cast<unsigned long>(replacement.faceIndex) |
			(static_cast<unsigned long>(*namedInstance) << 16);
		if (encoded <=
			static_cast<unsigned long>((std::numeric_limits<long>::max)()))
			resolvedFaceIndex = static_cast<long>(encoded);
	}
	file->resolvedIndices.push_back({
		replacement.faceIndex, std::move(sortedAxisValues), resolvedFaceIndex});
	return resolvedFaceIndex;
}

struct CodeViewIdentity final
{
	std::array<unsigned char, 16> guid{};
	DWORD age = 0;
};

class MappedImageView final
{
public:
	explicit MappedImageView(std::span<const unsigned char> bytes) noexcept
		: bytes_(bytes.data()), size_(bytes.size())
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

	std::optional<CodeViewIdentity> ReadCodeView() const noexcept
	{
		if (!valid_ || debugDirectory_.VirtualAddress == 0 ||
			debugDirectory_.Size < sizeof(IMAGE_DEBUG_DIRECTORY))
			return std::nullopt;
		std::size_t const count =
			debugDirectory_.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
		for (std::size_t index = 0; index < count; ++index)
		{
			IMAGE_DEBUG_DIRECTORY entry{};
			if (!CopyRva(
				debugDirectory_.VirtualAddress +
					static_cast<DWORD>(index * sizeof(entry)),
				&entry, sizeof(entry)))
				return std::nullopt;
			if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW ||
				entry.SizeOfData < 24 || entry.AddressOfRawData == 0)
				continue;
			std::array<unsigned char, 24> codeView{};
			if (!CopyRva(entry.AddressOfRawData, codeView.data(), codeView.size()) ||
				std::memcmp(codeView.data(), "RSDS", 4) != 0)
				continue;
			std::array<unsigned char, 16> guid{};
			std::copy_n(codeView.data() + 4, guid.size(), guid.begin());
			DWORD age = 0;
			std::memcpy(&age, codeView.data() + 20, sizeof(age));
			return CodeViewIdentity{
				.guid = guid,
				.age = age,
			};
		}
		return std::nullopt;
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

	template <typename OptionalHeader>
	std::optional<DWORD> ReadOptionalHeader(
		std::size_t optionalOffset, WORD sizeOfOptionalHeader) noexcept
	{
		if (sizeOfOptionalHeader < sizeof(OptionalHeader))
			return std::nullopt;
		OptionalHeader optional{};
		if (!Read(optionalOffset, &optional, sizeof(optional)) ||
			optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
			return std::nullopt;
		imageSize_ = optional.SizeOfImage;
		debugDirectory_ = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
		return optional.SizeOfHeaders;
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
		auto const fileHeaderOffset = CheckedAdd(ntOffset, sizeof(signature));
		if (!fileHeaderOffset)
			return false;
		IMAGE_FILE_HEADER file{};
		if (!Read(*fileHeaderOffset, &file, sizeof(file)) ||
			file.NumberOfSections == 0 ||
			(file.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == 0)
			return false;
		machine_ = file.Machine;
		timestamp_ = file.TimeDateStamp;
		auto const optionalOffset = CheckedAdd(*fileHeaderOffset, sizeof(file));
		if (!optionalOffset)
			return false;
		WORD magic = 0;
		if (!Read(*optionalOffset, &magic, sizeof(magic)))
			return false;
		std::optional<DWORD> sizeOfHeaders;
		if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
			sizeOfHeaders = ReadOptionalHeader<IMAGE_OPTIONAL_HEADER32>(
				*optionalOffset, file.SizeOfOptionalHeader);
		else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
			sizeOfHeaders = ReadOptionalHeader<IMAGE_OPTIONAL_HEADER64>(
				*optionalOffset, file.SizeOfOptionalHeader);
		if (!sizeOfHeaders)
			return false;
		if (imageSize_ == 0 || imageSize_ > size_ || *sizeOfHeaders == 0 ||
			*sizeOfHeaders > imageSize_)
			return false;
		if (debugDirectory_.VirtualAddress == 0 || debugDirectory_.Size == 0 ||
			debugDirectory_.VirtualAddress >= imageSize_ ||
			debugDirectory_.Size > imageSize_ - debugDirectory_.VirtualAddress)
			return false;
		sectionTableOffset_ = *optionalOffset + file.SizeOfOptionalHeader;
		sectionCount_ = file.NumberOfSections;
		std::size_t const sectionBytes =
			static_cast<std::size_t>(sectionCount_) * sizeof(IMAGE_SECTION_HEADER);
		if (sectionTableOffset_ > *sizeOfHeaders ||
			sectionBytes > *sizeOfHeaders - sectionTableOffset_ ||
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

FaceReplacementRegistry::LookupResult FaceReplacementRegistry::Lookup(
	const void* native) const noexcept
{
	auto const entry = replacementsByNative_.find(native);
	if (entry == replacementsByNative_.end())
		return {};
	return entry->second == nullptr
		? LookupResult{State::pending, nullptr}
		: LookupResult{State::published, entry->second};
}

bool FaceReplacementRegistry::Reserve(const void* native) noexcept
{
	if (native == nullptr || replacementsByNative_.size() >= kMaximumEntries ||
		replacementFaces_.find(native) != replacementFaces_.end())
		return false;
	try
	{
		return replacementsByNative_.emplace(native, nullptr).second;
	}
	catch (...)
	{
		return false;
	}
}

bool FaceReplacementRegistry::Publish(
	const void* native,
	const void* replacement) noexcept
{
	if (native == nullptr || replacement == nullptr)
		return false;
	auto const reservation = replacementsByNative_.find(native);
	if (reservation == replacementsByNative_.end() ||
		reservation->second != nullptr)
		return false;
	try
	{
		if (!replacementFaces_.insert(replacement).second)
		{
			replacementsByNative_.erase(reservation);
			return false;
		}
		reservation->second = replacement;
		return true;
	}
	catch (...)
	{
		replacementsByNative_.erase(reservation);
		return false;
	}
}

void FaceReplacementRegistry::Abandon(const void* native) noexcept
{
	auto const reservation = replacementsByNative_.find(native);
	if (reservation != replacementsByNative_.end() &&
		reservation->second == nullptr)
		replacementsByNative_.erase(reservation);
}

FaceReplacementRegistry::ReleaseResult FaceReplacementRegistry::Release(
	const void* face) noexcept
{
	auto const native = replacementsByNative_.find(face);
	if (native != replacementsByNative_.end())
	{
		const void* const replacement = native->second;
		if (replacement != nullptr)
			replacementFaces_.erase(replacement);
		replacementsByNative_.erase(native);
		return {true, replacement};
	}
	if (replacementFaces_.erase(face) == 0)
		return {};
	for (auto entry = replacementsByNative_.begin();
		entry != replacementsByNative_.end();)
	{
		if (entry->second == face)
			entry = replacementsByNative_.erase(entry);
		else
			++entry;
	}
	return {};
}

void FaceReplacementRegistry::Clear() noexcept
{
	replacementsByNative_.clear();
	replacementFaces_.clear();
}

std::size_t FaceReplacementRegistry::size() const noexcept
{
	return replacementsByNative_.size();
}

bool FaceReplacementRegistry::IsReplacement(const void* face) const noexcept
{
	return replacementFaces_.find(face) != replacementFaces_.end();
}

std::optional<long> ParseSfntNamedInstanceIndex(
	std::span<const unsigned char> bytes,
	long baseFaceIndex,
	std::span<const AxisValue> axisValues) noexcept
{
	try
	{
		if (bytes.size() < 12 || baseFaceIndex < 0)
			return std::nullopt;
		auto scaler = ReadBigEndian32(bytes, 0);
		if (!scaler)
			return std::nullopt;
		std::size_t sfntOffset = 0;
		if (*scaler == 0x74746366)
		{
			auto const fontCount = ReadBigEndian32(bytes, 8);
			if (!fontCount || *fontCount == 0 || *fontCount > 4096 ||
				static_cast<unsigned long>(baseFaceIndex) >= *fontCount)
				return std::nullopt;
			std::size_t const offsetsBytes =
				static_cast<std::size_t>(*fontCount) * 4;
			if (offsetsBytes > bytes.size() - 12)
				return std::nullopt;
			auto const faceOffset = ReadBigEndian32(
				bytes, 12 + static_cast<std::size_t>(baseFaceIndex) * 4);
			if (!faceOffset || *faceOffset > bytes.size() ||
				12 > bytes.size() - *faceOffset)
				return std::nullopt;
			sfntOffset = *faceOffset;
		}
		else if (baseFaceIndex != 0)
			return std::nullopt;

		scaler = ReadBigEndian32(bytes, sfntOffset);
		auto const tableCount = ReadBigEndian16(bytes, sfntOffset + 4);
		if (!scaler || !tableCount ||
			(*scaler != 0x00010000 && *scaler != 0x4F54544F &&
			 *scaler != 0x74727565 && *scaler != 0x74797031) ||
			*tableCount == 0 || *tableCount > 4096)
			return std::nullopt;
		std::size_t const tableDirectoryBytes =
			static_cast<std::size_t>(*tableCount) * 16;
		std::size_t const tableDirectoryOffset = sfntOffset + 12;
		if (tableDirectoryOffset > bytes.size() ||
			tableDirectoryBytes > bytes.size() - tableDirectoryOffset)
			return std::nullopt;

		std::size_t fvarOffset = 0;
		std::size_t fvarLength = 0;
		for (std::uint16_t table = 0; table < *tableCount; ++table)
		{
			std::size_t const record = tableDirectoryOffset +
				static_cast<std::size_t>(table) * 16;
			auto const tag = ReadBigEndian32(bytes, record);
			if (!tag)
				return std::nullopt;
			if (*tag != 0x66766172)
				continue;
			auto const offset = ReadBigEndian32(bytes, record + 8);
			auto const length = ReadBigEndian32(bytes, record + 12);
			if (!offset || !length || *length < 16 ||
				*offset > bytes.size() || *length > bytes.size() - *offset)
				return std::nullopt;
			fvarOffset = *offset;
			fvarLength = *length;
			break;
		}
		if (fvarLength == 0)
			return std::nullopt;

		auto const majorVersion = ReadBigEndian16(bytes, fvarOffset);
		auto const minorVersion = ReadBigEndian16(bytes, fvarOffset + 2);
		auto const axesArrayOffset = ReadBigEndian16(bytes, fvarOffset + 4);
		auto const reserved = ReadBigEndian16(bytes, fvarOffset + 6);
		auto const axisCount = ReadBigEndian16(bytes, fvarOffset + 8);
		auto const axisSize = ReadBigEndian16(bytes, fvarOffset + 10);
		auto const instanceCount = ReadBigEndian16(bytes, fvarOffset + 12);
		auto const instanceSize = ReadBigEndian16(bytes, fvarOffset + 14);
		if (!majorVersion || !minorVersion || !axesArrayOffset || !reserved ||
			!axisCount || !axisSize || !instanceCount || !instanceSize ||
			*majorVersion != 1 || *minorVersion > 0 || *reserved != 2 ||
			*axesArrayOffset < 16 || *axisCount == 0 || *axisSize < 20 ||
			*instanceCount == 0 || *instanceCount > 0x7FFF)
			return std::nullopt;
		std::size_t const coordinateBytes =
			static_cast<std::size_t>(*axisCount) * 4;
		if (*instanceSize != coordinateBytes + 4 &&
			*instanceSize != coordinateBytes + 6)
			return std::nullopt;
		std::size_t const axesBytes =
			static_cast<std::size_t>(*axisCount) * *axisSize;
		std::size_t const instancesBytes =
			static_cast<std::size_t>(*instanceCount) * *instanceSize;
		if (*axesArrayOffset > fvarLength ||
			axesBytes > fvarLength - *axesArrayOffset)
			return std::nullopt;
		std::size_t const instancesOffset = *axesArrayOffset + axesBytes;
		if (instancesOffset > fvarLength ||
			instancesBytes > fvarLength - instancesOffset)
			return std::nullopt;

		struct Axis final
		{
			std::uint32_t tag = 0;
			std::int32_t defaultValue = 0;
			float requestedValue = 0.0F;
		};
		std::vector<Axis> axes;
		axes.reserve(*axisCount);
		for (std::uint16_t axis = 0; axis < *axisCount; ++axis)
		{
			std::size_t const record = fvarOffset + *axesArrayOffset +
				static_cast<std::size_t>(axis) * *axisSize;
			auto const tag = ReadSfntTag(bytes, record);
			auto const minimumBits = ReadBigEndian32(bytes, record + 4);
			auto const defaultBits = ReadBigEndian32(bytes, record + 8);
			auto const maximumBits = ReadBigEndian32(bytes, record + 12);
			if (!tag || !minimumBits || !defaultBits || !maximumBits)
				return std::nullopt;
			std::int32_t const minimum = static_cast<std::int32_t>(*minimumBits);
			std::int32_t const defaultValue =
				static_cast<std::int32_t>(*defaultBits);
			std::int32_t const maximum = static_cast<std::int32_t>(*maximumBits);
			if (minimum > defaultValue || defaultValue > maximum)
				return std::nullopt;
			float requested = static_cast<float>(defaultValue) / 65536.0F;
			for (const AxisValue& value : axisValues)
			{
				if (value.tag == *tag)
				{
					requested = value.value;
					break;
				}
			}
			if (!std::isfinite(requested))
				return std::nullopt;
			axes.push_back({*tag, defaultValue, requested});
		}

		for (std::uint16_t instance = 0; instance < *instanceCount; ++instance)
		{
			std::size_t const record = fvarOffset + instancesOffset +
				static_cast<std::size_t>(instance) * *instanceSize;
			bool matches = true;
			bool defaults = true;
			for (std::uint16_t axis = 0; axis < *axisCount; ++axis)
			{
				auto const coordinateBits = ReadBigEndian32(
					bytes, record + 4 + static_cast<std::size_t>(axis) * 4);
				if (!coordinateBits)
					return std::nullopt;
				std::int32_t const coordinate =
					static_cast<std::int32_t>(*coordinateBits);
				float const coordinateValue =
					static_cast<float>(coordinate) / 65536.0F;
				if (std::fabs(coordinateValue - axes[axis].requestedValue) > 0.5F)
					matches = false;
				if (coordinate != axes[axis].defaultValue)
					defaults = false;
			}
			if (!matches)
				continue;
			return defaults ? 0 : static_cast<long>(instance) + 1;
		}
	}
	catch (...)
	{
	}
	return std::nullopt;
}

std::shared_ptr<const FontFileRedirectTable> FontFileRedirectTable::Build(
	const std::vector<InstalledFontFace>& installedFonts,
	const font_substitution::Snapshot& substitutions) noexcept
{
	try
	{
		std::vector<Redirect> redirects;
		std::vector<InstalledFontFace> ambiguousSources;
		std::vector<NamedInstanceFileCacheEntry> namedInstanceCache;
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
			long const replacementFaceIndex = ResolveReplacementFaceIndex(
				replacements.front(), namedInstanceCache);
			for (const InstalledFontFace& source : sources)
			{
				std::vector<std::wstring> sourceFamilies =
					FamiliesForFace(installedFonts, source);
				if (sourceFamilies.empty())
					sourceFamilies.push_back(rule.sourceFamily);
				if (source.faceIndex == replacementFaceIndex &&
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
						replacementFaceIndex});
				}
				else if (existing->replacementFaceIndex !=
						replacementFaceIndex ||
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

std::optional<std::wstring> FontFileRedirectTable::Resolve(
	const wchar_t* requestedPath) const noexcept
{
	try
	{
		auto normalized = NormalizeAbsolutePath(requestedPath);
		if (!normalized)
			return std::nullopt;
		std::wstring replacementPath;
		for (const auto& redirect : redirects_)
		{
			if (EqualOrdinalIgnoreCase(redirect.sourcePath, *normalized))
			{
				if (replacementPath.empty())
					replacementPath = redirect.replacementPath;
				else if (!EqualOrdinalIgnoreCase(
					replacementPath, redirect.replacementPath))
					return std::nullopt;
			}
		}
		if (replacementPath.empty())
			return std::nullopt;
		return replacementPath;
	}
	catch (...)
	{
		return std::nullopt;
	}
}

std::optional<FacePathResolution> FontFileRedirectTable::ResolveFace(
	const wchar_t* requestedPath,
	long requestedFaceIndex) const noexcept
{
	try
	{
		auto normalized = NormalizeAbsolutePath(requestedPath);
		if (!normalized)
			return std::nullopt;
		for (const Redirect& redirect : redirects_)
		{
			if (redirect.sourceFaceIndex == requestedFaceIndex &&
				EqualOrdinalIgnoreCase(redirect.sourcePath, *normalized))
			{
				return FacePathResolution{
					.replacementPath = redirect.replacementPath,
					.replacementFaceIndex = redirect.replacementFaceIndex,
				};
			}
		}
	}
	catch (...)
	{
	}
	return std::nullopt;
}

std::optional<FacePathResolution> FontFileRedirectTable::ResolveFamilyFace(
	const wchar_t* requestedFamily) const noexcept
{
	try
	{
		if (requestedFamily == nullptr || *requestedFamily == L'\0')
			return std::nullopt;
		FacePathResolution resolution;
		for (const Redirect& redirect : redirects_)
		{
			bool const matches = std::any_of(
				redirect.sourceFamilies.begin(), redirect.sourceFamilies.end(),
				[&](const std::wstring& family) {
					return EqualOrdinalIgnoreCase(family, requestedFamily);
				});
			if (!matches)
				continue;
			if (resolution.replacementPath.empty())
			{
				resolution.replacementPath = redirect.replacementPath;
				resolution.replacementFaceIndex = redirect.replacementFaceIndex;
			}
			else if (resolution.replacementFaceIndex !=
					redirect.replacementFaceIndex ||
				!EqualOrdinalIgnoreCase(
					resolution.replacementPath, redirect.replacementPath))
				return std::nullopt;
		}
		if (resolution.replacementPath.empty())
			return std::nullopt;
		return resolution;
	}
	catch (...)
	{
		return std::nullopt;
	}
}

std::optional<FaceOpenPathRedirect> ResolveTextCoreFontLoadPath(
	const FontFileRedirectTable& redirects,
	const char* requestedUtf8,
	long requestedFaceIndex) noexcept
{
	try
	{
		auto sourcePath = Utf8PathToWide(requestedUtf8);
		if (!sourcePath)
			return std::nullopt;
		auto resolution = redirects.ResolveFace(
			sourcePath->c_str(), requestedFaceIndex);
		if (!resolution)
			return std::nullopt;
		auto replacementUtf8 = WidePathToUtf8(resolution->replacementPath);
		if (!replacementUtf8)
			return std::nullopt;
		return FaceOpenPathRedirect{
			.sourcePath = std::move(*sourcePath),
			.replacementPath = std::move(resolution->replacementPath),
			.replacementUtf8 = std::move(*replacementUtf8),
			.replacementFaceIndex = resolution->replacementFaceIndex,
		};
	}
	catch (...)
	{
		return std::nullopt;
	}
}

std::optional<FaceOpenPathRedirect> ResolveFamilyFaceOpenPath(
	const FontFileRedirectTable& redirects,
	const wchar_t* requestedFamily) noexcept
{
	try
	{
		auto resolution = redirects.ResolveFamilyFace(requestedFamily);
		if (!resolution)
			return std::nullopt;
		auto replacementUtf8 = WidePathToUtf8(resolution->replacementPath);
		if (!replacementUtf8)
			return std::nullopt;
		return FaceOpenPathRedirect{
			.replacementPath = std::move(resolution->replacementPath),
			.replacementUtf8 = std::move(*replacementUtf8),
			.replacementFaceIndex = resolution->replacementFaceIndex,
		};
	}
	catch (...)
	{
		return std::nullopt;
	}
}

// A private FreeType face open only carries a pathname when its flags select
// that source; everything after that check is the same resolution the
// TextCore font load performs.
std::optional<FaceOpenPathRedirect> ResolveFaceOpenPath(
	const FontFileRedirectTable& redirects,
	unsigned int openFlags,
	const char* requestedUtf8,
	long requestedFaceIndex) noexcept
{
	constexpr unsigned int kOpenSourceMask = 0x07;
	constexpr unsigned int kOpenPathname = 0x04;
	if ((openFlags & kOpenSourceMask) != kOpenPathname)
		return std::nullopt;
	return ResolveTextCoreFontLoadPath(
		redirects, requestedUtf8, requestedFaceIndex);
}

std::optional<std::wstring> ReadLegacyFontRefFamily(
	const void* fontRef) noexcept
{
	try
	{
		constexpr std::size_t kObjectSize = 40;
		constexpr std::size_t kMaximumFamilyBytes = 32767;
		if (!IsMemoryRange(fontRef, kObjectSize, IsReadableProtection))
			return std::nullopt;
		std::array<unsigned char, kObjectSize> object{};
		std::memcpy(object.data(), fontRef, object.size());
		const char* data = nullptr;
		std::size_t length = 0;
		if (object[32] == 1)
		{
			std::int8_t const remaining =
				static_cast<std::int8_t>(object[24]);
			if (remaining < 0 || remaining > 24)
				return std::nullopt;
			length = 24 - static_cast<std::size_t>(remaining);
			data = reinterpret_cast<const char*>(object.data());
		}
		else
		{
			std::memcpy(&data, object.data(), sizeof(data));
			std::memcpy(&length, object.data() + 16, sizeof(length));
			if (data == nullptr || length > kMaximumFamilyBytes ||
				!IsMemoryRange(data, length, IsReadableProtection))
				return std::nullopt;
		}
		if (length == 0 || length > kMaximumFamilyBytes ||
			std::memchr(data, '\0', length) != nullptr)
			return std::nullopt;
		std::string utf8(data, length);
		utf8.push_back('\0');
		return Utf8PathToWide(utf8.c_str());
	}
	catch (...)
	{
		return std::nullopt;
	}
}

std::optional<std::string> ReadReadableUtf8Path(
	const char* value) noexcept
{
	try
	{
		if (value == nullptr)
			return std::nullopt;
		std::string path;
		constexpr std::size_t kMaximumPathBytes = 32767;
		std::uintptr_t const base = reinterpret_cast<std::uintptr_t>(value);
		for (std::size_t offset = 0; offset <= kMaximumPathBytes; ++offset)
		{
			if (offset > std::numeric_limits<std::uintptr_t>::max() - base)
				return std::nullopt;
			const char* const current = reinterpret_cast<const char*>(base + offset);
			if (!IsMemoryRange(current, 1, IsReadableProtection))
				return std::nullopt;
			char const character = *current;
			if (character == '\0')
				return path.empty()
					? std::nullopt
					: std::optional<std::string>(std::move(path));
			if (offset == kMaximumPathBytes)
				return std::nullopt;
			path.push_back(character);
		}
	}
	catch (...)
	{
	}
	return std::nullopt;
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

std::optional<ResolvedAdapter> ResolveAdapter(
	std::span<const unsigned char> mappedImage,
	std::span<const AdapterDescriptor> descriptors) noexcept
{
	if (mappedImage.empty() || descriptors.empty())
		return std::nullopt;
	MappedImageView const image(mappedImage);
	if (!image.valid())
		return std::nullopt;
	auto const codeView = image.ReadCodeView();
	if (!codeView)
		return std::nullopt;

	const AdapterDescriptor* match = nullptr;
	for (const AdapterDescriptor& candidate : descriptors)
	{
		if (candidate.machine != image.machine() ||
			candidate.timestamp != image.timestamp() ||
			candidate.imageSize != image.image_size() ||
			candidate.pdbGuid != codeView->guid ||
			candidate.pdbAge != codeView->age)
			continue;
		if (match != nullptr || candidate.targetPrefix == nullptr ||
			!image.IsExecutableRva(candidate.targetRva, candidate.targetPrefix->size()))
			return std::nullopt;
		std::array<unsigned char, 32> prefix{};
		if (!image.CopyRva(candidate.targetRva, prefix.data(), prefix.size()) ||
			prefix != *candidate.targetPrefix)
			return std::nullopt;
		bool const hasFaceOpen = candidate.faceOpenRva != 0;
		if (hasFaceOpen != (candidate.faceOpenPrefix != nullptr) ||
			hasFaceOpen !=
				(candidate.faceOpenAbi != FaceOpenAbi::unavailable))
			return std::nullopt;
		if (hasFaceOpen)
		{
			if (!image.IsExecutableRva(
				candidate.faceOpenRva, candidate.faceOpenPrefix->size()))
				return std::nullopt;
			std::array<unsigned char, 32> faceOpenPrefix{};
			if (!image.CopyRva(
				candidate.faceOpenRva, faceOpenPrefix.data(),
				faceOpenPrefix.size()) ||
				faceOpenPrefix != *candidate.faceOpenPrefix)
				return std::nullopt;
		}
		bool const hasFontLoad = candidate.fontLoadRva != 0;
		if (hasFontLoad != (candidate.fontLoadPrefix != nullptr) ||
			hasFontLoad !=
				(candidate.fontLoadAbi != FontLoadAbi::unavailable))
			return std::nullopt;
		if (hasFontLoad)
		{
			if (!image.IsExecutableRva(
				candidate.fontLoadRva, candidate.fontLoadPrefix->size()))
				return std::nullopt;
			std::array<unsigned char, 32> fontLoadPrefix{};
			if (!image.CopyRva(
				candidate.fontLoadRva, fontLoadPrefix.data(),
				fontLoadPrefix.size()) ||
				fontLoadPrefix != *candidate.fontLoadPrefix)
				return std::nullopt;
		}
		bool const hasCharacterLookup = candidate.characterLookupRva != 0;
		if (hasCharacterLookup != (candidate.characterLookupPrefix != nullptr) ||
			hasCharacterLookup !=
				(candidate.characterLookupAbi != CharacterLookupAbi::unavailable))
			return std::nullopt;
		if (hasCharacterLookup)
		{
			if (!image.IsExecutableRva(
				candidate.characterLookupRva,
				candidate.characterLookupPrefix->size()))
				return std::nullopt;
			std::array<unsigned char, 32> characterLookupPrefix{};
			if (!image.CopyRva(
				candidate.characterLookupRva, characterLookupPrefix.data(),
				characterLookupPrefix.size()) ||
				characterLookupPrefix != *candidate.characterLookupPrefix)
				return std::nullopt;
		}
		bool const hasOsFaceResolver = candidate.osFaceResolverRva != 0;
		if (hasOsFaceResolver != (candidate.osFaceResolverPrefix != nullptr) ||
			hasOsFaceResolver !=
				(candidate.osFaceResolverAbi != CharacterLookupAbi::unavailable))
			return std::nullopt;
		if (hasOsFaceResolver)
		{
			if (!image.IsExecutableRva(
				candidate.osFaceResolverRva,
				candidate.osFaceResolverPrefix->size()))
				return std::nullopt;
			std::array<unsigned char, 32> osFaceResolverPrefix{};
			if (!image.CopyRva(
				candidate.osFaceResolverRva, osFaceResolverPrefix.data(),
				osFaceResolverPrefix.size()) ||
				osFaceResolverPrefix != *candidate.osFaceResolverPrefix)
				return std::nullopt;
		}
		bool const hasFreeTypeCharIndex = candidate.freeTypeCharIndexRva != 0;
		if (hasFreeTypeCharIndex !=
				(candidate.freeTypeCharIndexPrefix != nullptr) ||
			hasFreeTypeCharIndex !=
				(candidate.freeTypeCharIndexAbi != FreeTypeCharIndexAbi::unavailable))
			return std::nullopt;
		if (hasFreeTypeCharIndex)
		{
			if (!image.IsExecutableRva(
				candidate.freeTypeCharIndexRva,
				candidate.freeTypeCharIndexPrefix->size()))
				return std::nullopt;
			std::array<unsigned char, 32> freeTypeCharIndexPrefix{};
			if (!image.CopyRva(
				candidate.freeTypeCharIndexRva,
				freeTypeCharIndexPrefix.data(), freeTypeCharIndexPrefix.size()) ||
				freeTypeCharIndexPrefix != *candidate.freeTypeCharIndexPrefix)
				return std::nullopt;
		}
		bool const hasFontCatalogLoad = candidate.fontCatalogLoadRva != 0;
		if (hasFontCatalogLoad !=
				(candidate.fontCatalogLoadPrefix != nullptr) ||
			hasFontCatalogLoad !=
				(candidate.fontCatalogLoadAbi != FontCatalogLoadAbi::unavailable))
			return std::nullopt;
		if (hasFontCatalogLoad)
		{
			if (!image.IsExecutableRva(
				candidate.fontCatalogLoadRva,
				candidate.fontCatalogLoadPrefix->size()))
				return std::nullopt;
			std::array<unsigned char, 32> fontCatalogLoadPrefix{};
			if (!image.CopyRva(
				candidate.fontCatalogLoadRva,
				fontCatalogLoadPrefix.data(), fontCatalogLoadPrefix.size()) ||
				fontCatalogLoadPrefix != *candidate.fontCatalogLoadPrefix)
				return std::nullopt;
		}
		bool const hasFaceDone = candidate.faceDoneRva != 0;
		if (hasFaceDone != (candidate.faceDonePrefix != nullptr) ||
			hasFaceDone != (candidate.faceDoneAbi != FaceDoneAbi::unavailable))
			return std::nullopt;
		if (hasFaceDone)
		{
			if (!image.IsExecutableRva(
				candidate.faceDoneRva, candidate.faceDonePrefix->size()))
				return std::nullopt;
			std::array<unsigned char, 32> faceDonePrefix{};
			if (!image.CopyRva(
				candidate.faceDoneRva, faceDonePrefix.data(),
				faceDonePrefix.size()) ||
				faceDonePrefix != *candidate.faceDonePrefix)
				return std::nullopt;
		}
		match = &candidate;
	}
	if (match == nullptr)
		return std::nullopt;
	return ResolvedAdapter{
		.name = match->name,
		.targetRva = match->targetRva,
		.faceOpenRva = match->faceOpenRva,
		.fontLoadRva = match->fontLoadRva,
		.characterLookupRva = match->characterLookupRva,
		.osFaceResolverRva = match->osFaceResolverRva,
		.freeTypeCharIndexRva = match->freeTypeCharIndexRva,
		.fontCatalogLoadRva = match->fontCatalogLoadRva,
		.faceDoneRva = match->faceDoneRva,
		.abi = match->abi,
		.faceOpenAbi = match->faceOpenAbi,
		.fontLoadAbi = match->fontLoadAbi,
		.characterLookupAbi = match->characterLookupAbi,
		.osFaceResolverAbi = match->osFaceResolverAbi,
		.freeTypeCharIndexAbi = match->freeTypeCharIndexAbi,
		.fontCatalogLoadAbi = match->fontCatalogLoadAbi,
		.faceDoneAbi = match->faceDoneAbi,
	};
}

std::span<const AdapterDescriptor> ProductionAdapterDescriptors() noexcept
{
	return kProductionDescriptors;
}

bool IsExecutableMemoryRange(const void* address, std::size_t length) noexcept
{
	return IsMemoryRange(address, length, IsExecutableProtection);
}

bool IsReadableMemoryRange(const void* address, std::size_t length) noexcept
{
	return IsMemoryRange(address, length, IsReadableProtection);
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
