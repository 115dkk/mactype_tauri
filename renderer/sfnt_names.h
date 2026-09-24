#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstddef>
#include <string>
#include <vector>

namespace renderer {
namespace sfnt {

bool CanRead(std::size_t offset, std::size_t length, std::size_t size) noexcept;
// The caller proves the range with CanRead first.
UINT16 ReadU16(std::vector<BYTE> const& bytes, std::size_t offset);
UINT32 ReadU32(std::vector<BYTE> const& bytes, std::size_t offset);
void WriteU16(std::vector<BYTE>& bytes, std::size_t offset, UINT16 value);
void WriteU32(std::vector<BYTE>& bytes, std::size_t offset, UINT32 value);
UINT32 TableChecksum(BYTE const* bytes, std::size_t size) noexcept;

struct NameRecord
{
	UINT16 platform = 0;
	UINT16 encoding = 0;
	UINT16 language = 0;
	UINT16 id = 0;
	std::vector<BYTE> value;
};

struct LanguageTagRecord
{
	std::vector<BYTE> value;
};

bool IsUnicodeNameRecord(NameRecord const& record) noexcept;
bool DecodeName(NameRecord const& record, std::wstring& value);
bool ReadNameTable(
	std::vector<BYTE> const& table,
	UINT16& format,
	std::vector<NameRecord>& records,
	std::vector<LanguageTagRecord>& languageTags);

// Every decodable typographic family name (ID 16); the family names (ID 1)
// when the table has no ID 16 record. Duplicates are removed.
bool ReadTypographicFamilyNames(
	std::vector<BYTE> const& table,
	std::vector<std::wstring>& families);

} // namespace sfnt
} // namespace renderer
