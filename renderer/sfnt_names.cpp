#include "sfnt_names.h"

#include <algorithm>
#include <array>
#include <utility>

namespace renderer {
namespace sfnt {

bool CanRead(std::size_t offset, std::size_t length, std::size_t size) noexcept
{
	return offset <= size && length <= size - offset;
}

UINT16 ReadU16(std::vector<BYTE> const& bytes, std::size_t offset)
{
	return static_cast<UINT16>(
		(static_cast<UINT16>(bytes[offset]) << 8) |
		static_cast<UINT16>(bytes[offset + 1]));
}

UINT32 ReadU32(std::vector<BYTE> const& bytes, std::size_t offset)
{
	return (static_cast<UINT32>(bytes[offset]) << 24) |
		(static_cast<UINT32>(bytes[offset + 1]) << 16) |
		(static_cast<UINT32>(bytes[offset + 2]) << 8) |
		static_cast<UINT32>(bytes[offset + 3]);
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

UINT32 TableChecksum(BYTE const* bytes, std::size_t size) noexcept
{
	UINT32 checksum = 0;
	for (std::size_t offset = 0; offset < size; offset += 4)
	{
		UINT32 word = 0;
		for (std::size_t byteIndex = 0; byteIndex < 4; ++byteIndex)
		{
			word <<= 8;
			if (offset + byteIndex < size)
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

bool DecodeName(NameRecord const& record, std::wstring& value)
{
	value.clear();
	if (IsUnicodeNameRecord(record))
	{
		if ((record.value.size() & 1) != 0)
			return false;
		value.reserve(record.value.size() / 2);
		for (std::size_t offset = 0; offset < record.value.size(); offset += 2)
		{
			value.push_back(static_cast<WCHAR>(
				(static_cast<UINT16>(record.value[offset]) << 8) |
				static_cast<UINT16>(record.value[offset + 1])));
		}
		return true;
	}
	if (record.platform != 1 || record.value.empty())
		return false;

	int const required = MultiByteToWideChar(
		CP_MACCP, 0,
		reinterpret_cast<char const*>(record.value.data()),
		static_cast<int>(record.value.size()), nullptr, 0);
	if (required <= 0)
		return false;
	value.resize(static_cast<std::size_t>(required));
	return MultiByteToWideChar(
		CP_MACCP, 0,
		reinterpret_cast<char const*>(record.value.data()),
		static_cast<int>(record.value.size()), &value[0], required) == required;
}

bool ReadNameTable(
	std::vector<BYTE> const& table,
	UINT16& format,
	std::vector<NameRecord>& records,
	std::vector<LanguageTagRecord>& languageTags)
{
	if (!CanRead(0, 6, table.size()))
		return false;
	format = ReadU16(table, 0);
	if (format > 1)
		return false;
	UINT16 const count = ReadU16(table, 2);
	UINT16 const stringOffset = ReadU16(table, 4);
	std::size_t const recordsEnd = 6 + static_cast<std::size_t>(count) * 12;
	if (!CanRead(6, static_cast<std::size_t>(count) * 12, table.size()) ||
		stringOffset < recordsEnd || stringOffset > table.size())
		return false;

	records.reserve(count);
	for (UINT16 index = 0; index < count; ++index)
	{
		std::size_t const offset = 6 + static_cast<std::size_t>(index) * 12;
		NameRecord record;
		record.platform = ReadU16(table, offset);
		record.encoding = ReadU16(table, offset + 2);
		record.language = ReadU16(table, offset + 4);
		record.id = ReadU16(table, offset + 6);
		UINT16 const length = ReadU16(table, offset + 8);
		UINT16 const valueOffset = ReadU16(table, offset + 10);
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
		UINT16 const countTags = ReadU16(table, recordsEnd);
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
			UINT16 const length = ReadU16(table, offset);
			UINT16 const valueOffset = ReadU16(table, offset + 2);
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

bool ReadTypographicFamilyNames(
	std::vector<BYTE> const& table,
	std::vector<std::wstring>& families)
{
	families.clear();
	UINT16 format = 0;
	std::vector<NameRecord> records;
	std::vector<LanguageTagRecord> languageTags;
	if (!ReadNameTable(table, format, records, languageTags))
		return false;
	for (UINT16 const wanted : std::array<UINT16, 2>{{16, 1}})
	{
		for (NameRecord const& record : records)
		{
			std::wstring family;
			if (record.id != wanted || !DecodeName(record, family) ||
				family.empty())
				continue;
			bool const known = std::any_of(
				families.begin(), families.end(),
				[&](std::wstring const& existing) {
					return CompareStringOrdinal(
						existing.c_str(), static_cast<int>(existing.size()),
						family.c_str(), static_cast<int>(family.size()),
						TRUE) == CSTR_EQUAL;
				});
			if (!known)
				families.push_back(std::move(family));
		}
		if (!families.empty())
			return true;
	}
	return false;
}

} // namespace sfnt
} // namespace renderer
