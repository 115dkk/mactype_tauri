#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace renderer {
namespace sfnt {

[[nodiscard]] bool CanRead(
	std::size_t offset, std::size_t length, std::size_t size) noexcept;
void WriteU16(std::vector<BYTE>& bytes, std::size_t offset, UINT16 value);
void WriteU32(std::vector<BYTE>& bytes, std::size_t offset, UINT32 value);
[[nodiscard]] UINT32 TableChecksum(std::span<const BYTE> bytes) noexcept;

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

[[nodiscard]] bool IsUnicodeNameRecord(NameRecord const& record) noexcept;
[[nodiscard]] std::optional<std::wstring> DecodeName(NameRecord const& record);
[[nodiscard]] bool ReadNameTable(
	std::vector<BYTE> const& table,
	UINT16& format,
	std::vector<NameRecord>& records,
	std::vector<LanguageTagRecord>& languageTags);

// Every decodable typographic family name (ID 16); the family names (ID 1)
// when the table has no ID 16 record. Duplicates are removed.
[[nodiscard]] std::optional<std::vector<std::wstring>>
ReadTypographicFamilyNames(std::vector<BYTE> const& table);

} // namespace sfnt
} // namespace renderer
