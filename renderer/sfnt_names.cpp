#include "sfnt_names.h"

#include "big_endian.h"

#include <algorithm>
#include <array>
#include <utility>

namespace renderer {
namespace sfnt {

bool CanRead(std::size_t offset, std::size_t length, std::size_t size) noexcept
{
	return offset <= size && length <= size - offset;
}

void WriteU16(std::vector<BYTE>& bytes, std::size_t offset, UINT16 value)
{
	bytes[offset] = static_cast<BYTE>(value >> 8);
	bytes[offset + 1] = static_cast<BYTE>(value);
}

void WriteU32(std::vector<BYTE>& bytes, std::size_t offset, UINT32 value)
{
	bytes[offset] = static_cast<BYTE>(value >> 24);
	bytes[offset + 1] = static_cast<BYTE>(value >> 16);
	bytes[offset + 2] = static_cast<BYTE>(value >> 8);
	bytes[offset + 3] = static_cast<BYTE>(value);
}

UINT32 TableChecksum(std::span<const BYTE> bytes) noexcept
{
	UINT32 checksum = 0;
	for (std::size_t offset = 0; offset < bytes.size(); offset += 4)
	{
		UINT32 word = 0;
		for (std::size_t byteIndex = 0; byteIndex < 4; ++byteIndex)
		{
			word <<= 8;
			if (offset + byteIndex < bytes.size())
				word |= bytes[offset + byteIndex];
		}
		checksum += word;
	}
	return checksum;
}

bool IsUnicodeNameRecord(NameRecord const& record) noexcept
{
	return record.platform == 0 || record.platform == 3;
}

std::optional<std::wstring> DecodeName(NameRecord const& record)
{
	std::wstring value;
	if (IsUnicodeNameRecord(record))
	{
		if ((record.value.size() & 1) != 0)
			return std::nullopt;
		value.reserve(record.value.size() / 2);
		for (std::size_t offset = 0; offset < record.value.size(); offset += 2)
		{
			value.push_back(static_cast<WCHAR>(
				(static_cast<UINT16>(record.value[offset]) << 8) |
				static_cast<UINT16>(record.value[offset + 1])));
		}
		return value;
	}
	if (record.platform != 1 || record.value.empty())
		return std::nullopt;

	int const required = MultiByteToWideChar(
		CP_MACCP, 0,
		reinterpret_cast<char const*>(record.value.data()),
		static_cast<int>(record.value.size()), nullptr, 0);
	if (required <= 0)
		return std::nullopt;
	value.resize(static_cast<std::size_t>(required));
	if (MultiByteToWideChar(
			CP_MACCP, 0,
			reinterpret_cast<char const*>(record.value.data()),
			static_cast<int>(record.value.size()), value.data(), required) != required)
		return std::nullopt;
	return value;
}

bool ReadNameTable(
	std::vector<BYTE> const& table,
	UINT16& format,
	std::vector<NameRecord>& records,
	std::vector<LanguageTagRecord>& languageTags)
{
	if (!CanRead(0, 6, table.size()))
		return false;
	format = *big_endian::Read16(table, 0);
	if (format > 1)
		return false;
	UINT16 const count = *big_endian::Read16(table, 2);
	UINT16 const stringOffset = *big_endian::Read16(table, 4);
	std::size_t const recordsEnd = 6 + static_cast<std::size_t>(count) * 12;
	if (!CanRead(6, static_cast<std::size_t>(count) * 12, table.size()) ||
		stringOffset < recordsEnd || stringOffset > table.size())
		return false;

	records.reserve(count);
	for (UINT16 index = 0; index < count; ++index)
	{
		std::size_t const offset = 6 + static_cast<std::size_t>(index) * 12;
		NameRecord record;
		record.platform = *big_endian::Read16(table, offset);
		record.encoding = *big_endian::Read16(table, offset + 2);
		record.language = *big_endian::Read16(table, offset + 4);
		record.id = *big_endian::Read16(table, offset + 6);
		UINT16 const length = *big_endian::Read16(table, offset + 8);
		UINT16 const valueOffset = *big_endian::Read16(table, offset + 10);
		std::size_t const absolute =
			static_cast<std::size_t>(stringOffset) + valueOffset;
		if (!CanRead(absolute, length, table.size()))
			return false;
		record.value.assign(
			table.begin() + absolute,
			table.begin() + absolute + length);
		records.emplace_back(std::move(record));
	}

	if (format == 1)
	{
		if (!CanRead(recordsEnd, 2, table.size()))
			return false;
		UINT16 const countTags = *big_endian::Read16(table, recordsEnd);
		std::size_t const tagsOffset = recordsEnd + 2;
		if (!CanRead(tagsOffset, static_cast<std::size_t>(countTags) * 4,
				table.size()) ||
			stringOffset < tagsOffset + static_cast<std::size_t>(countTags) * 4)
			return false;
		languageTags.reserve(countTags);
		for (UINT16 index = 0; index < countTags; ++index)
		{
			std::size_t const offset =
				tagsOffset + static_cast<std::size_t>(index) * 4;
			UINT16 const length = *big_endian::Read16(table, offset);
			UINT16 const valueOffset = *big_endian::Read16(table, offset + 2);
			std::size_t const absolute =
				static_cast<std::size_t>(stringOffset) + valueOffset;
			if (!CanRead(absolute, length, table.size()))
				return false;
			LanguageTagRecord tag;
			tag.value.assign(
				table.begin() + absolute,
				table.begin() + absolute + length);
			languageTags.emplace_back(std::move(tag));
		}
	}
	return true;
}

std::optional<std::vector<std::wstring>> ReadTypographicFamilyNames(
	std::vector<BYTE> const& table)
{
	UINT16 format = 0;
	std::vector<NameRecord> records;
	std::vector<LanguageTagRecord> languageTags;
	if (!ReadNameTable(table, format, records, languageTags))
		return std::nullopt;
	std::vector<std::wstring> families;
	for (UINT16 const wanted : std::array<UINT16, 2>{{16, 1}})
	{
		for (NameRecord const& record : records)
		{
			if (record.id != wanted)
				continue;
			auto family = DecodeName(record);
			if (!family || family->empty())
				continue;
			bool const known = std::any_of(
				families.begin(), families.end(),
				[&](std::wstring const& existing) {
					return CompareStringOrdinal(
						existing.c_str(), static_cast<int>(existing.size()),
						family->c_str(), static_cast<int>(family->size()),
						TRUE) == CSTR_EQUAL;
				});
			if (!known)
				families.push_back(std::move(*family));
		}
		if (!families.empty())
			return families;
	}
	return std::nullopt;
}

} // namespace sfnt
} // namespace renderer
