#include "directwrite_virtual_font.h"
#include "renderer_raii.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bcrypt.h>
#include <cstring>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace directwrite_virtual_font {
namespace {

constexpr UINT32 MakeTag(char a, char b, char c, char d) noexcept
{
	return (static_cast<UINT32>(static_cast<unsigned char>(a)) << 24) |
		(static_cast<UINT32>(static_cast<unsigned char>(b)) << 16) |
		(static_cast<UINT32>(static_cast<unsigned char>(c)) << 8) |
		static_cast<UINT32>(static_cast<unsigned char>(d));
}

constexpr UINT32 kTagCollection = MakeTag('t', 't', 'c', 'f');
constexpr UINT32 kTagCff = MakeTag('O', 'T', 'T', 'O');
constexpr UINT32 kTagTrueType = 0x00010000;
constexpr UINT32 kTagTrue = MakeTag('t', 'r', 'u', 'e');
constexpr UINT32 kTagType1 = MakeTag('t', 'y', 'p', '1');
constexpr UINT32 kTagName = MakeTag('n', 'a', 'm', 'e');
constexpr UINT32 kTagHead = MakeTag('h', 'e', 'a', 'd');
constexpr UINT32 kTagDsig = MakeTag('D', 'S', 'I', 'G');
constexpr UINT32 kSfntChecksum = 0xB1B0AFBA;

bool CanRead(size_t offset, size_t length, size_t size) noexcept
{
	return offset <= size && length <= size - offset;
}

UINT16 ReadU16(std::vector<BYTE> const& bytes, size_t offset)
{
	return static_cast<UINT16>(
		(static_cast<UINT16>(bytes[offset]) << 8) |
		static_cast<UINT16>(bytes[offset + 1]));
}

UINT32 ReadU32(std::vector<BYTE> const& bytes, size_t offset)
{
	return (static_cast<UINT32>(bytes[offset]) << 24) |
		(static_cast<UINT32>(bytes[offset + 1]) << 16) |
		(static_cast<UINT32>(bytes[offset + 2]) << 8) |
		static_cast<UINT32>(bytes[offset + 3]);
}

void WriteU16(std::vector<BYTE>& bytes, size_t offset, UINT16 value)
{
	bytes[offset] = static_cast<BYTE>(value >> 8);
	bytes[offset + 1] = static_cast<BYTE>(value);
}

void WriteU32(std::vector<BYTE>& bytes, size_t offset, UINT32 value)
{
	bytes[offset] = static_cast<BYTE>(value >> 24);
	bytes[offset + 1] = static_cast<BYTE>(value >> 16);
	bytes[offset + 2] = static_cast<BYTE>(value >> 8);
	bytes[offset + 3] = static_cast<BYTE>(value);
}

UINT32 TableChecksum(BYTE const* bytes, size_t size) noexcept
{
	UINT32 checksum = 0;
	for (size_t offset = 0; offset < size; offset += 4)
	{
		UINT32 word = 0;
		for (size_t byteIndex = 0; byteIndex < 4; ++byteIndex)
		{
			word <<= 8;
			if (offset + byteIndex < size)
				word |= bytes[offset + byteIndex];
		}
		checksum += word;
	}
	return checksum;
}

class FontFileFragment
{
public:
	explicit FontFileFragment(IDWriteFontFileStream* stream) noexcept :
		stream_(stream)
	{
	}

	~FontFileFragment()
	{
		if (context_ != nullptr)
			stream_->ReleaseFileFragment(context_);
	}

	FontFileFragment(FontFileFragment const&) = delete;
	FontFileFragment& operator=(FontFileFragment const&) = delete;

	HRESULT Read(UINT64 offset, UINT64 size) noexcept
	{
		return stream_->ReadFileFragment(&start_, offset, size, &context_);
	}

	void const* get() const noexcept { return start_; }

private:
	CComPtr<IDWriteFontFileStream> stream_;
	void const* start_ = nullptr;
	void* context_ = nullptr;
};

HRESULT ReadReplacementFile(
	IDWriteFontFaceReference* reference,
	std::vector<BYTE>& bytes,
	UINT32& faceIndex)
{
	CComPtr<IDWriteFontFile> file;
	HRESULT result = reference->GetFontFile(&file);
	if (FAILED(result) || file == nullptr)
		return FAILED(result) ? result : E_FAIL;

	void const* key = nullptr;
	UINT32 keySize = 0;
	result = file->GetReferenceKey(&key, &keySize);
	if (FAILED(result))
		return result;

	CComPtr<IDWriteFontFileLoader> fileLoader;
	result = file->GetLoader(&fileLoader);
	if (FAILED(result) || fileLoader == nullptr)
		return FAILED(result) ? result : E_FAIL;

	CComPtr<IDWriteFontFileStream> stream;
	result = fileLoader->CreateStreamFromKey(key, keySize, &stream);
	if (FAILED(result) || stream == nullptr)
		return FAILED(result) ? result : E_FAIL;

	UINT64 fileSize = 0;
	result = stream->GetFileSize(&fileSize);
	if (FAILED(result))
		return result;
	if (fileSize == 0 || fileSize > std::numeric_limits<UINT32>::max())
		return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);

	FontFileFragment fragment(stream);
	result = fragment.Read(0, fileSize);
	if (FAILED(result) || fragment.get() == nullptr)
		return FAILED(result) ? result : E_FAIL;

	BYTE const* first = static_cast<BYTE const*>(fragment.get());
	bytes.assign(first, first + static_cast<size_t>(fileSize));
	faceIndex = reference->GetFontFaceIndex();
	return S_OK;
}

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

bool IsIdentityName(UINT16 id) noexcept
{
	switch (id)
	{
	case 1:  // Font family
	case 3:  // Unique identifier
	case 4:  // Full name
	case 6:  // PostScript name
	case 16: // Typographic family
	case 18: // Compatible full name
	case 20: // PostScript CID findfont name
	case 21: // WWS family
	case 25: // Variations PostScript prefix
		return true;
	default:
		return false;
	}
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
		for (size_t offset = 0; offset < record.value.size(); offset += 2)
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
	value.resize(static_cast<size_t>(required));
	return MultiByteToWideChar(
		CP_MACCP, 0,
		reinterpret_cast<char const*>(record.value.data()),
		static_cast<int>(record.value.size()), &value[0], required) == required;
}

bool EncodeName(
	NameRecord const& record,
	std::wstring const& value,
	std::vector<BYTE>& encoded)
{
	encoded.clear();
	if (IsUnicodeNameRecord(record))
	{
		if (value.size() >
			static_cast<size_t>(std::numeric_limits<UINT16>::max()) / 2)
			return false;
		encoded.reserve(value.size() * 2);
		for (WCHAR const character : value)
		{
			encoded.push_back(static_cast<BYTE>(character >> 8));
			encoded.push_back(static_cast<BYTE>(character));
		}
		return true;
	}
	if (record.platform != 1 || value.empty())
		return false;

	BOOL usedDefault = FALSE;
	int const required = WideCharToMultiByte(
		CP_MACCP, WC_NO_BEST_FIT_CHARS, value.data(),
		static_cast<int>(value.size()), nullptr, 0, nullptr, &usedDefault);
	if (required <= 0 || usedDefault ||
		static_cast<UINT32>(required) > std::numeric_limits<UINT16>::max())
		return false;
	encoded.resize(static_cast<size_t>(required));
	usedDefault = FALSE;
	return WideCharToMultiByte(
		CP_MACCP, WC_NO_BEST_FIT_CHARS, value.data(),
		static_cast<int>(value.size()),
		reinterpret_cast<char*>(encoded.data()), required,
		nullptr, &usedDefault) == required && !usedDefault;
}

bool EqualsInsensitive(std::wstring const& left, WCHAR const* right) noexcept
{
	return _wcsicmp(left.c_str(), right) == 0;
}

bool IsRegularStyle(std::wstring const& style) noexcept
{
	return style.empty() || EqualsInsensitive(style, L"Regular") ||
		EqualsInsensitive(style, L"Normal") ||
		EqualsInsensitive(style, L"Roman");
}

std::wstring FindSubfamily(
	std::vector<NameRecord> const& records,
	NameRecord const& identityRecord)
{
	for (UINT16 const wanted : std::array<UINT16, 2>{{17, 2}})
	{
		for (NameRecord const& record : records)
		{
			if (record.id != wanted ||
				record.platform != identityRecord.platform ||
				record.encoding != identityRecord.encoding ||
				record.language != identityRecord.language)
				continue;
			std::wstring style;
			if (DecodeName(record, style) && !style.empty())
				return style;
		}
	}
	return L"Regular";
}

UINT32 StableFamilyHash(std::wstring const& family) noexcept
{
	UINT32 hash = 2166136261u;
	for (WCHAR const character : family)
	{
		hash ^= static_cast<UINT16>(character);
		hash *= 16777619u;
	}
	return hash;
}

std::wstring HexHash(UINT32 value)
{
	WCHAR buffer[9] = {};
	StringCchPrintfW(buffer, ARRAYSIZE(buffer), L"%08X", value);
	return buffer;
}

std::wstring PostScriptToken(std::wstring const& value)
{
	std::wstring token;
	token.reserve(value.size());
	for (WCHAR const character : value)
	{
		if ((character >= L'A' && character <= L'Z') ||
			(character >= L'a' && character <= L'z') ||
			(character >= L'0' && character <= L'9'))
			token.push_back(character);
		else if (character == L'-' && !token.empty() && token.back() != L'-')
			token.push_back(character);
	}
	return token;
}

Identity MakeIdentity(std::wstring const& family, std::wstring const& style)
{
	Identity identity;
	identity.family = family;
	identity.subfamily = style.empty() ? L"Regular" : style;
	identity.fullName = family;
	if (!IsRegularStyle(identity.subfamily))
	{
		identity.fullName.push_back(L' ');
		identity.fullName.append(identity.subfamily);
	}

	std::wstring familyToken = PostScriptToken(family);
	if (familyToken.empty())
		familyToken = L"MacTypeAlias" + HexHash(StableFamilyHash(family));
	identity.postScriptName = familyToken;
	if (!IsRegularStyle(identity.subfamily))
	{
		std::wstring const styleToken = PostScriptToken(identity.subfamily);
		if (!styleToken.empty())
		{
			identity.postScriptName.push_back(L'-');
			identity.postScriptName.append(styleToken);
		}
	}
	if (identity.postScriptName.size() > 63)
		identity.postScriptName.resize(63);
	return identity;
}

std::wstring IdentityValue(
	UINT16 id,
	Identity const& identity,
	UINT32 familyHash)
{
	switch (id)
	{
	case 1:
	case 16:
	case 21:
		return identity.family;
	case 3:
		return identity.postScriptName + L";MacTypeAlias;" + HexHash(familyHash);
	case 4:
	case 18:
		return identity.fullName;
	case 6:
	case 20:
		return identity.postScriptName;
	case 25:
	{
		size_t const separator = identity.postScriptName.find(L'-');
		return identity.postScriptName.substr(0, separator);
	}
	default:
		return std::wstring();
	}
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
	size_t const recordsEnd = 6 + static_cast<size_t>(count) * 12;
	if (!CanRead(6, static_cast<size_t>(count) * 12, table.size()) ||
		stringOffset < recordsEnd || stringOffset > table.size())
		return false;

	records.reserve(count);
	for (UINT16 index = 0; index < count; ++index)
	{
		size_t const offset = 6 + static_cast<size_t>(index) * 12;
		NameRecord record;
		record.platform = ReadU16(table, offset);
		record.encoding = ReadU16(table, offset + 2);
		record.language = ReadU16(table, offset + 4);
		record.id = ReadU16(table, offset + 6);
		UINT16 const length = ReadU16(table, offset + 8);
		UINT16 const valueOffset = ReadU16(table, offset + 10);
		size_t const absolute = static_cast<size_t>(stringOffset) + valueOffset;
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
		size_t const tagsOffset = recordsEnd + 2;
		if (!CanRead(tagsOffset, static_cast<size_t>(countTags) * 4, table.size()) ||
			stringOffset < tagsOffset + static_cast<size_t>(countTags) * 4)
			return false;
		languageTags.reserve(countTags);
		for (UINT16 index = 0; index < countTags; ++index)
		{
			size_t const offset = tagsOffset + static_cast<size_t>(index) * 4;
			UINT16 const length = ReadU16(table, offset);
			UINT16 const valueOffset = ReadU16(table, offset + 2);
			size_t const absolute = static_cast<size_t>(stringOffset) + valueOffset;
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

bool BuildNameTable(
	std::vector<BYTE> const& original,
	std::wstring const& family,
	std::vector<BYTE>& rewritten,
	Identity& representativeIdentity)
{
	UINT16 format = 0;
	std::vector<NameRecord> records;
	std::vector<LanguageTagRecord> languageTags;
	if (!ReadNameTable(original, format, records, languageTags))
		return false;

	bool haveRepresentative = false;
	UINT32 const familyHash = StableFamilyHash(family);
	std::vector<NameRecord> kept;
	kept.reserve(records.size());
	for (NameRecord record : records)
	{
		if (!IsIdentityName(record.id))
		{
			kept.emplace_back(std::move(record));
			continue;
		}

		Identity const identity = MakeIdentity(family, FindSubfamily(records, record));
		std::vector<BYTE> value;
		if (!EncodeName(record, IdentityValue(record.id, identity, familyHash), value))
			continue;
		record.value = std::move(value);
		kept.emplace_back(std::move(record));
		if (!haveRepresentative && IsUnicodeNameRecord(kept.back()))
		{
			representativeIdentity = identity;
			haveRepresentative = true;
		}
	}
	if (!haveRepresentative)
		return false;

	size_t const headerSize = 6 + kept.size() * 12 +
		(format == 1 ? 2 + languageTags.size() * 4 : 0);
	if (kept.size() > std::numeric_limits<UINT16>::max() ||
		headerSize > std::numeric_limits<UINT16>::max())
		return false;
	rewritten.assign(headerSize, 0);
	WriteU16(rewritten, 0, format);
	WriteU16(rewritten, 2, static_cast<UINT16>(kept.size()));
	WriteU16(rewritten, 4, static_cast<UINT16>(headerSize));

	for (size_t index = 0; index < kept.size(); ++index)
	{
		NameRecord const& record = kept[index];
		if (record.value.size() > std::numeric_limits<UINT16>::max() ||
			rewritten.size() - headerSize > std::numeric_limits<UINT16>::max())
			return false;
		size_t const offset = 6 + index * 12;
		WriteU16(rewritten, offset, record.platform);
		WriteU16(rewritten, offset + 2, record.encoding);
		WriteU16(rewritten, offset + 4, record.language);
		WriteU16(rewritten, offset + 6, record.id);
		WriteU16(rewritten, offset + 8, static_cast<UINT16>(record.value.size()));
		WriteU16(rewritten, offset + 10,
			static_cast<UINT16>(rewritten.size() - headerSize));
		rewritten.insert(rewritten.end(), record.value.begin(), record.value.end());
	}

	if (format == 1)
	{
		size_t const tagsOffset = 6 + kept.size() * 12;
		if (languageTags.size() > std::numeric_limits<UINT16>::max())
			return false;
		WriteU16(rewritten, tagsOffset, static_cast<UINT16>(languageTags.size()));
		for (size_t index = 0; index < languageTags.size(); ++index)
		{
			LanguageTagRecord const& tag = languageTags[index];
			if (tag.value.size() > std::numeric_limits<UINT16>::max() ||
				rewritten.size() - headerSize > std::numeric_limits<UINT16>::max())
				return false;
			size_t const offset = tagsOffset + 2 + index * 4;
			WriteU16(rewritten, offset, static_cast<UINT16>(tag.value.size()));
			WriteU16(rewritten, offset + 2,
				static_cast<UINT16>(rewritten.size() - headerSize));
			rewritten.insert(rewritten.end(), tag.value.begin(), tag.value.end());
		}
	}
	return true;
}

struct SfntTable
{
	UINT32 tag = 0;
	UINT32 sourceOffset = 0;
	UINT32 sourceLength = 0;
	std::vector<BYTE> replacement;
};

bool IsScalerType(UINT32 value) noexcept
{
	return value == kTagTrueType || value == kTagCff ||
		value == kTagTrue || value == kTagType1;
}

HRESULT BuildAliasedSfnt(
	std::vector<BYTE> const& source,
	UINT32 faceIndex,
	std::wstring const& family,
	std::vector<BYTE>& output,
	Identity& identity)
{
	if (!CanRead(0, 12, source.size()))
		return DWRITE_E_FILEFORMAT;

	size_t faceOffset = 0;
	UINT32 scalerType = ReadU32(source, 0);
	if (scalerType == kTagCollection)
	{
		UINT32 const faceCount = ReadU32(source, 8);
		if (faceIndex >= faceCount ||
			!CanRead(12, static_cast<size_t>(faceCount) * 4, source.size()))
			return DWRITE_E_FILEFORMAT;
		faceOffset = ReadU32(source, 12 + static_cast<size_t>(faceIndex) * 4);
		if (!CanRead(faceOffset, 12, source.size()))
			return DWRITE_E_FILEFORMAT;
		scalerType = ReadU32(source, faceOffset);
	}
	else if (faceIndex != 0)
	{
		return DWRITE_E_FILEFORMAT;
	}
	if (!IsScalerType(scalerType))
		return DWRITE_E_FILEFORMAT;

	UINT16 const sourceTableCount = ReadU16(source, faceOffset + 4);
	if (sourceTableCount == 0 || sourceTableCount > 4096 ||
		!CanRead(faceOffset + 12,
			static_cast<size_t>(sourceTableCount) * 16, source.size()))
		return DWRITE_E_FILEFORMAT;

	std::vector<SfntTable> tables;
	tables.reserve(sourceTableCount);
	bool foundName = false;
	bool foundHead = false;
	for (UINT16 index = 0; index < sourceTableCount; ++index)
	{
		size_t const recordOffset = faceOffset + 12 +
			static_cast<size_t>(index) * 16;
		SfntTable table;
		table.tag = ReadU32(source, recordOffset);
		table.sourceOffset = ReadU32(source, recordOffset + 8);
		table.sourceLength = ReadU32(source, recordOffset + 12);
		if (!CanRead(table.sourceOffset, table.sourceLength, source.size()))
			return DWRITE_E_FILEFORMAT;
		if (table.tag == kTagDsig)
			continue;
		if (table.tag == kTagName)
		{
			std::vector<BYTE> original(
				source.begin() + table.sourceOffset,
				source.begin() + table.sourceOffset + table.sourceLength);
			if (!BuildNameTable(original, family, table.replacement, identity))
				return DWRITE_E_FILEFORMAT;
			foundName = true;
		}
		else if (table.tag == kTagHead)
		{
			if (table.sourceLength < 12)
				return DWRITE_E_FILEFORMAT;
			foundHead = true;
		}
		tables.emplace_back(std::move(table));
	}
	if (!foundName || !foundHead || tables.empty() ||
		tables.size() > std::numeric_limits<UINT16>::max())
		return DWRITE_E_FILEFORMAT;

	size_t const directorySize = 12 + tables.size() * 16;
	output.assign(directorySize, 0);
	WriteU32(output, 0, scalerType);
	WriteU16(output, 4, static_cast<UINT16>(tables.size()));
	UINT16 entrySelector = 0;
	UINT16 powerOfTwo = 1;
	while (static_cast<size_t>(powerOfTwo) * 2 <= tables.size())
	{
		powerOfTwo = static_cast<UINT16>(powerOfTwo * 2);
		++entrySelector;
	}
	UINT16 const searchRange = static_cast<UINT16>(powerOfTwo * 16);
	WriteU16(output, 6, searchRange);
	WriteU16(output, 8, entrySelector);
	WriteU16(output, 10,
		static_cast<UINT16>(tables.size() * 16 - searchRange));

	size_t headOutputOffset = 0;
	for (size_t index = 0; index < tables.size(); ++index)
	{
		while ((output.size() & 3) != 0)
			output.push_back(0);
		if (output.size() > std::numeric_limits<UINT32>::max())
			return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
		UINT32 const tableOffset = static_cast<UINT32>(output.size());
		SfntTable const& table = tables[index];
		if (!table.replacement.empty())
			output.insert(output.end(), table.replacement.begin(), table.replacement.end());
		else
			output.insert(
				output.end(),
				source.begin() + table.sourceOffset,
				source.begin() + table.sourceOffset + table.sourceLength);
		UINT32 const tableLength = static_cast<UINT32>(output.size() - tableOffset);
		if (table.tag == kTagHead)
		{
			WriteU32(output, static_cast<size_t>(tableOffset) + 8, 0);
			headOutputOffset = tableOffset;
		}
		size_t const recordOffset = 12 + index * 16;
		WriteU32(output, recordOffset, table.tag);
		WriteU32(output, recordOffset + 4,
			TableChecksum(output.data() + tableOffset, tableLength));
		WriteU32(output, recordOffset + 8, tableOffset);
		WriteU32(output, recordOffset + 12, tableLength);
	}
	while ((output.size() & 3) != 0)
		output.push_back(0);
	if (output.size() > std::numeric_limits<UINT32>::max())
		return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);

	UINT32 const adjustment = kSfntChecksum -
		TableChecksum(output.data(), output.size());
	WriteU32(output, headOutputOffset + 8, adjustment);
	return S_OK;
}

struct AlgorithmProviderCloser
{
	void operator()(void* value) const noexcept
	{
		BCryptCloseAlgorithmProvider(
			static_cast<BCRYPT_ALG_HANDLE>(value), 0);
	}
};

struct HashCloser
{
	void operator()(void* value) const noexcept
	{
		BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(value));
	}
};

using UniqueAlgorithmProvider =
	std::unique_ptr<void, AlgorithmProviderCloser>;
using UniqueHash = std::unique_ptr<void, HashCloser>;

HRESULT HashBytes(
	std::vector<BYTE> const& bytes,
	std::array<BYTE, 32>& digest)
{
	BCRYPT_ALG_HANDLE rawAlgorithm = nullptr;
	NTSTATUS status = BCryptOpenAlgorithmProvider(
		&rawAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
	if (!BCRYPT_SUCCESS(status))
		return HRESULT_FROM_NT(status);
	UniqueAlgorithmProvider algorithm(rawAlgorithm);

	DWORD objectSize = 0;
	DWORD copied = 0;
	status = BCryptGetProperty(
		algorithm.get(), BCRYPT_OBJECT_LENGTH,
		reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &copied, 0);
	if (!BCRYPT_SUCCESS(status) || copied != sizeof(objectSize))
		return BCRYPT_SUCCESS(status) ? E_FAIL : HRESULT_FROM_NT(status);

	std::vector<BYTE> hashObject(objectSize);
	BCRYPT_HASH_HANDLE rawHash = nullptr;
	status = BCryptCreateHash(
		algorithm.get(), &rawHash,
		hashObject.empty() ? nullptr : hashObject.data(), objectSize,
		nullptr, 0, 0);
	if (!BCRYPT_SUCCESS(status))
		return HRESULT_FROM_NT(status);
	UniqueHash hash(rawHash);

	status = BCryptHashData(
		hash.get(), const_cast<PUCHAR>(bytes.data()),
		static_cast<ULONG>(bytes.size()), 0);
	if (!BCRYPT_SUCCESS(status))
		return HRESULT_FROM_NT(status);
	status = BCryptFinishHash(
		hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0);
	return BCRYPT_SUCCESS(status) ? S_OK : HRESULT_FROM_NT(status);
}

std::wstring HexDigest(std::array<BYTE, 32> const& digest)
{
	constexpr WCHAR digits[] = L"0123456789abcdef";
	std::wstring value(digest.size() * 2, L'0');
	for (size_t index = 0; index < digest.size(); ++index)
	{
		value[index * 2] = digits[digest[index] >> 4];
		value[index * 2 + 1] = digits[digest[index] & 0x0f];
	}
	return value;
}

HRESULT EnsureDirectory(std::wstring const& path)
{
	if (CreateDirectoryW(path.c_str(), nullptr))
		return S_OK;
	DWORD const error = GetLastError();
	if (error != ERROR_ALREADY_EXISTS)
		return HRESULT_FROM_WIN32(error);
	DWORD const attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES ||
		(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
	return S_OK;
}

HRESULT ReadEnvironmentVariable(
	WCHAR const* name,
	std::wstring& value)
{
	value.clear();
	for (unsigned int attempt = 0; attempt < 3; ++attempt)
	{
		DWORD const required = GetEnvironmentVariableW(name, nullptr, 0);
		if (required == 0)
			return HRESULT_FROM_WIN32(GetLastError());
		std::vector<WCHAR> buffer(required);
		DWORD const copied = GetEnvironmentVariableW(
			name, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (copied == 0)
			return HRESULT_FROM_WIN32(GetLastError());
		if (copied < buffer.size())
		{
			value.assign(buffer.data(), copied);
			return S_OK;
		}
	}
	return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
}

void AppendPathComponent(std::wstring& path, WCHAR const* component)
{
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
		path.push_back(L'\\');
	path.append(component);
}

HRESULT GetCacheDirectory(std::wstring& path)
{
	HRESULT result = ReadEnvironmentVariable(L"LOCALAPPDATA", path);
	if (FAILED(result) || path.empty())
	{
		std::array<WCHAR, MAX_PATH + 1> temporary = {};
		DWORD const length = GetTempPathW(
			static_cast<DWORD>(temporary.size()), temporary.data());
		if (length == 0 || length >= temporary.size())
			return HRESULT_FROM_WIN32(
				length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);
		path.assign(temporary.data(), length);
	}

	AppendPathComponent(path, L"MacType");
	result = EnsureDirectory(path);
	if (FAILED(result))
		return result;
	AppendPathComponent(path, L"FontCache");
	return EnsureDirectory(path);
}

HRESULT FileMatches(
	std::wstring const& path,
	std::vector<BYTE> const& expected,
	bool& exists,
	bool& matches)
{
	exists = false;
	matches = false;
	renderer_raii::UniqueHandle file = renderer_raii::AdoptHandle(
		CreateFileW(
			path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
	if (!file)
	{
		DWORD const error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			return S_OK;
		return HRESULT_FROM_WIN32(error);
	}
	exists = true;

	LARGE_INTEGER size = {};
	if (!GetFileSizeEx(file.get(), &size))
		return HRESULT_FROM_WIN32(GetLastError());
	if (size.QuadPart != static_cast<LONGLONG>(expected.size()))
		return S_OK;

	std::array<BYTE, 64 * 1024> buffer = {};
	size_t offset = 0;
	while (offset < expected.size())
	{
		DWORD const wanted = static_cast<DWORD>((std::min)(
			buffer.size(), expected.size() - offset));
		DWORD received = 0;
		if (!ReadFile(file.get(), buffer.data(), wanted, &received, nullptr))
			return HRESULT_FROM_WIN32(GetLastError());
		if (received != wanted ||
			memcmp(buffer.data(), expected.data() + offset, wanted) != 0)
			return S_OK;
		offset += received;
	}
	matches = true;
	return S_OK;
}

HRESULT WriteAll(HANDLE file, std::vector<BYTE> const& bytes)
{
	size_t offset = 0;
	while (offset < bytes.size())
	{
		DWORD const wanted = static_cast<DWORD>((std::min)(
			bytes.size() - offset,
			static_cast<size_t>(std::numeric_limits<DWORD>::max())));
		DWORD written = 0;
		if (!WriteFile(
				file, bytes.data() + offset, wanted, &written, nullptr))
			return HRESULT_FROM_WIN32(GetLastError());
		if (written == 0)
			return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
		offset += written;
	}
	return S_OK;
}

class PendingCacheFile
{
public:
	explicit PendingCacheFile(std::wstring path) : path_(std::move(path)) {}
	~PendingCacheFile()
	{
		if (!committed_)
			DeleteFileW(path_.c_str());
	}

	PendingCacheFile(PendingCacheFile const&) = delete;
	PendingCacheFile& operator=(PendingCacheFile const&) = delete;

	WCHAR const* path() const noexcept { return path_.c_str(); }
	void Commit() noexcept { committed_ = true; }

private:
	std::wstring path_;
	bool committed_ = false;
};

HRESULT PersistFont(
	std::vector<BYTE> const& bytes,
	std::wstring& path)
{
	std::array<BYTE, 32> digest = {};
	HRESULT result = HashBytes(bytes, digest);
	if (FAILED(result))
		return result;

	std::wstring directory;
	result = GetCacheDirectory(directory);
	if (FAILED(result))
		return result;

	std::wstring const stem = HexDigest(digest);
	static std::atomic<ULONG> sequence(0);
	for (unsigned int attempt = 0; attempt < 32; ++attempt)
	{
		path = directory;
		AppendPathComponent(path, stem.c_str());
		if (attempt != 0)
		{
			path.push_back(L'-');
			path.append(std::to_wstring(GetCurrentProcessId()));
			path.push_back(L'-');
			path.append(std::to_wstring(++sequence));
		}
		path.append(L".ttf");

		bool exists = false;
		bool matches = false;
		result = FileMatches(path, bytes, exists, matches);
		if (FAILED(result))
			return result;
		if (matches)
			return S_OK;
		if (exists)
			continue;

		std::wstring temporaryPath = path;
		temporaryPath.append(L".tmp-");
		temporaryPath.append(std::to_wstring(GetCurrentProcessId()));
		temporaryPath.push_back(L'-');
		temporaryPath.append(std::to_wstring(GetCurrentThreadId()));
		temporaryPath.push_back(L'-');
		temporaryPath.append(std::to_wstring(++sequence));
		PendingCacheFile temporary(std::move(temporaryPath));
		renderer_raii::UniqueHandle file = renderer_raii::AdoptHandle(
			CreateFileW(
				temporary.path(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
		if (!file)
		{
			if (GetLastError() == ERROR_FILE_EXISTS)
				continue;
			return HRESULT_FROM_WIN32(GetLastError());
		}
		result = WriteAll(file.get(), bytes);
		if (SUCCEEDED(result) && !FlushFileBuffers(file.get()))
			result = HRESULT_FROM_WIN32(GetLastError());
		file.reset();
		if (FAILED(result))
			return result;

		if (MoveFileExW(
				temporary.path(), path.c_str(), MOVEFILE_WRITE_THROUGH))
		{
			temporary.Commit();
			return S_OK;
		}
		DWORD const moveError = GetLastError();
		result = FileMatches(path, bytes, exists, matches);
		if (SUCCEEDED(result) && matches)
			return S_OK;
		if (FAILED(result))
			return result;
		if (moveError != ERROR_ALREADY_EXISTS && moveError != ERROR_FILE_EXISTS)
			return HRESULT_FROM_WIN32(moveError);
	}
	return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
}

} // namespace

HRESULT CreateAliasedReference(
	IDWriteFactory3* factory,
	IDWriteFontFaceReference* replacementReference,
	WCHAR const* aliasFamily,
	CComPtr<IDWriteFontFaceReference>& reference,
	Identity& identity)
{
	reference.Release();
	identity = {};
	if (factory == nullptr || replacementReference == nullptr ||
		aliasFamily == nullptr ||
		*aliasFamily == L'\0')
		return E_INVALIDARG;

	try
	{
		std::vector<BYTE> source;
		UINT32 faceIndex = 0;
		HRESULT result = ReadReplacementFile(
			replacementReference, source, faceIndex);
		if (FAILED(result))
			return result;

		std::vector<BYTE> aliased;
		result = BuildAliasedSfnt(
			source, faceIndex, aliasFamily, aliased, identity);
		if (FAILED(result))
			return result;

		std::wstring backingPath;
		result = PersistFont(aliased, backingPath);
		if (FAILED(result))
			return result;

		CComPtr<IDWriteFontFile> fontFile;
		result = factory->CreateFontFileReference(
			backingPath.c_str(), nullptr, &fontFile);
		if (FAILED(result) || fontFile == nullptr)
			return FAILED(result) ? result : E_FAIL;

		return factory->CreateFontFaceReference(
			fontFile,
			0,
			replacementReference->GetSimulations(),
			&reference);
	}
	catch (std::bad_alloc const&)
	{
		return E_OUTOFMEMORY;
	}
	catch (...)
	{
		return E_FAIL;
	}
}

} // namespace directwrite_virtual_font
