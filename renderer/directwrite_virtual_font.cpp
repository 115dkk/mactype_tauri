#include "directwrite_virtual_font.h"
#include "sfnt_names.h"
#include "virtual_font_cache.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <strsafe.h>
#include <utility>
#include <vector>


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
constexpr UINT32 kTagOs2 = MakeTag('O', 'S', '/', '2');
constexpr UINT32 kTagFvar = MakeTag('f', 'v', 'a', 'r');
constexpr UINT32 kSfntChecksum = 0xB1B0AFBA;
constexpr size_t kOs2WeightClassOffset = 4;
constexpr size_t kOs2WidthClassOffset = 6;
constexpr size_t kOs2SelectionOffset = 62;
constexpr UINT16 kOs2SelectionItalic = 0x0001;
constexpr UINT16 kOs2SelectionBold = 0x0020;
constexpr UINT16 kOs2SelectionRegular = 0x0040;
constexpr size_t kHeadMacStyleOffset = 44;
constexpr UINT16 kHeadMacStyleBold = 0x0001;

bool IsBoldWeightClass(UINT16 weight) noexcept
{
	return weight >= 600;
}

using renderer::sfnt::CanRead;
using renderer::sfnt::DecodeName;
using renderer::sfnt::IsUnicodeNameRecord;
using renderer::sfnt::LanguageTagRecord;
using renderer::sfnt::NameRecord;
using renderer::sfnt::ReadNameTable;
using renderer::sfnt::ReadU16;
using renderer::sfnt::ReadU32;
using renderer::sfnt::TableChecksum;
using renderer::sfnt::WriteU16;
using renderer::sfnt::WriteU32;

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

struct ReplacementFileIdentity
{
	std::wstring path;
	UINT64 size = 0;
	UINT64 lastWriteTime = 0;
};

HRESULT ReadReplacementFile(
	IDWriteFontFaceReference* reference,
	std::vector<BYTE>& bytes,
	UINT32& faceIndex,
	ReplacementFileIdentity& identity,
	bool readBytes)
{
	bytes.clear();
	identity = {};
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

	CComPtr<IDWriteLocalFontFileLoader> localLoader;
	if (SUCCEEDED(fileLoader->QueryInterface(&localLoader)) &&
		localLoader != nullptr)
	{
		UINT32 pathLength = 0;
		if (SUCCEEDED(localLoader->GetFilePathLengthFromKey(
				key, keySize, &pathLength)) && pathLength != 0 &&
			pathLength <= 32767)
		{
			std::vector<WCHAR> path(static_cast<size_t>(pathLength) + 1);
			if (SUCCEEDED(localLoader->GetFilePathFromKey(
					key, keySize, path.data(), pathLength + 1)))
			{
				WIN32_FILE_ATTRIBUTE_DATA attributes = {};
				if (GetFileAttributesExW(
						path.data(), GetFileExInfoStandard, &attributes) &&
					(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
				{
					ULARGE_INTEGER size = {};
					size.LowPart = attributes.nFileSizeLow;
					size.HighPart = attributes.nFileSizeHigh;
					ULARGE_INTEGER writeTime = {};
					writeTime.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
					writeTime.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
					if (size.QuadPart == fileSize)
					{
						identity.path.assign(path.data(), pathLength);
						identity.size = size.QuadPart;
						identity.lastWriteTime = writeTime.QuadPart;
					}
				}
			}
		}
	}

	faceIndex = reference->GetFontFaceIndex();
	if (!readBytes)
		return S_OK;

	FontFileFragment fragment(stream);
	result = fragment.Read(0, fileSize);
	if (FAILED(result) || fragment.get() == nullptr)
		return FAILED(result) ? result : E_FAIL;

	BYTE const* first = static_cast<BYTE const*>(fragment.get());
	bytes.assign(first, first + static_cast<size_t>(fileSize));
	return S_OK;
}

bool IsIdentityName(UINT16 id, bool rewriteSubfamily) noexcept
{
	switch (id)
	{
	case 2:  // Subfamily
	case 17: // Typographic subfamily
		return rewriteSubfamily;
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

bool ContainsInsensitive(std::wstring const& text, WCHAR const* part) noexcept
{
	size_t const partLength = wcslen(part);
	for (size_t start = 0; start + partLength <= text.size(); ++start)
	{
		if (_wcsnicmp(text.c_str() + start, part, partLength) == 0)
			return true;
	}
	return false;
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
	case 2:
	case 17:
		return identity.subfamily;
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

// A bold-advertised alias of a face that also backs a regular alias would
// otherwise share its full and PostScript names, and DirectWrite keeps only
// the first of two fonts with identical names.
std::wstring AdvertisedSubfamily(std::wstring const& backingSubfamily)
{
	return ContainsInsensitive(backingSubfamily, L"Italic") ?
		L"Bold Italic" : L"Bold";
}

bool BuildNameTable(
	std::vector<BYTE> const& original,
	std::wstring const& family,
	bool advertiseBold,
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
		if (!IsIdentityName(record.id, advertiseBold))
		{
			kept.emplace_back(std::move(record));
			continue;
		}

		std::wstring subfamily = FindSubfamily(records, record);
		if (advertiseBold)
			subfamily = AdvertisedSubfamily(subfamily);
		Identity const identity = MakeIdentity(family, subfamily);
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

UINT32 SfntAxisTag(DWRITE_FONT_AXIS_TAG tag) noexcept
{
	UINT32 const value = static_cast<UINT32>(tag);
	return ((value & 0x000000ffU) << 24) |
		((value & 0x0000ff00U) << 8) |
		((value & 0x00ff0000U) >> 8) |
		((value & 0xff000000U) >> 24);
}

bool AxisValue(
	AliasOptions const& options,
	DWRITE_FONT_AXIS_TAG wanted,
	FLOAT& value) noexcept
{
	for (DWRITE_FONT_AXIS_VALUE const& axis : options.axisValues)
	{
		if (axis.axisTag == wanted && std::isfinite(axis.value))
		{
			value = axis.value;
			return true;
		}
	}
	return false;
}

bool FixedCoordinate(FLOAT value, std::int32_t& fixed) noexcept
{
	double const scaled = static_cast<double>(value) * 65536.0;
	if (!std::isfinite(scaled) ||
		scaled < static_cast<double>((std::numeric_limits<std::int32_t>::min)()) ||
		scaled > static_cast<double>((std::numeric_limits<std::int32_t>::max)()))
		return false;
	fixed = static_cast<std::int32_t>(std::lround(scaled));
	return true;
}

bool BuildPinnedFvar(
	std::vector<BYTE> const& original,
	AliasOptions const& options,
	std::vector<BYTE>& rewritten)
{
	if (original.size() < 16)
		return false;
	UINT16 const axesOffset = ReadU16(original, 4);
	UINT16 const axisCount = ReadU16(original, 8);
	UINT16 const axisSize = ReadU16(original, 10);
	UINT16 const instanceCount = ReadU16(original, 12);
	UINT16 const instanceSize = ReadU16(original, 14);
	if (axisCount == 0 || axisSize < 20 || instanceCount == 0 ||
		instanceSize < 4 + static_cast<size_t>(axisCount) * 4 ||
		!CanRead(axesOffset, static_cast<size_t>(axisCount) * axisSize,
			original.size()))
		return false;

	size_t const instancesOffset = static_cast<size_t>(axesOffset) +
		static_cast<size_t>(axisCount) * axisSize;
	if (!CanRead(instancesOffset,
			static_cast<size_t>(instanceCount) * instanceSize, original.size()))
		return false;

	std::vector<std::int32_t> pinned(axisCount);
	for (UINT16 axisIndex = 0; axisIndex < axisCount; ++axisIndex)
	{
		size_t const axisOffset = static_cast<size_t>(axesOffset) +
			static_cast<size_t>(axisIndex) * axisSize;
		UINT32 const tag = ReadU32(original, axisOffset);
		bool found = false;
		for (DWRITE_FONT_AXIS_VALUE const& axis : options.axisValues)
		{
			if (SfntAxisTag(axis.axisTag) == tag &&
				FixedCoordinate(axis.value, pinned[axisIndex]))
			{
				found = true;
				break;
			}
		}
		if (!found)
			return false;
	}

	UINT16 pinnedInstance = instanceCount;
	for (UINT16 instanceIndex = 0; instanceIndex < instanceCount; ++instanceIndex)
	{
		size_t const instanceOffset = instancesOffset +
			static_cast<size_t>(instanceIndex) * instanceSize;
		bool matches = true;
		for (UINT16 axisIndex = 0; axisIndex < axisCount; ++axisIndex)
		{
			std::int32_t const coordinate = static_cast<std::int32_t>(
				ReadU32(original, instanceOffset + 4 +
					static_cast<size_t>(axisIndex) * 4));
			if (coordinate != pinned[axisIndex])
			{
				matches = false;
				break;
			}
		}
		if (matches && pinnedInstance == instanceCount)
			pinnedInstance = instanceIndex;
	}
	if (pinnedInstance == instanceCount)
		return false;

	size_t const pinnedOffset = instancesOffset +
		static_cast<size_t>(pinnedInstance) * instanceSize;
	rewritten.assign(original.begin(), original.begin() + instancesOffset);
	rewritten.insert(
		rewritten.end(),
		original.begin() + pinnedOffset,
		original.begin() + pinnedOffset + instanceSize);
	WriteU16(rewritten, 12, 1);
	return true;
}

UINT16 WidthClass(FLOAT width) noexcept
{
	constexpr std::array<FLOAT, 9> widths = {
		50.0f, 62.5f, 75.0f, 87.5f, 100.0f,
		112.5f, 125.0f, 150.0f, 200.0f,
	};
	size_t nearest = 0;
	FLOAT distance = std::fabs(width - widths[0]);
	for (size_t index = 1; index < widths.size(); ++index)
	{
		FLOAT const candidate = std::fabs(width - widths[index]);
		if (candidate < distance)
		{
			nearest = index;
			distance = candidate;
		}
	}
	return static_cast<UINT16>(nearest + 1);
}

bool IsScalerType(UINT32 value) noexcept
{
	return value == kTagTrueType || value == kTagCff ||
		value == kTagTrue || value == kTagType1;
}

void AdvertiseOs2Weight(std::vector<BYTE>& table, UINT16 weight)
{
	WriteU16(table, kOs2WeightClassOffset, weight);
	UINT16 selection = ReadU16(table, kOs2SelectionOffset);
	if (IsBoldWeightClass(weight))
	{
		selection = static_cast<UINT16>(
			(selection | kOs2SelectionBold) & ~kOs2SelectionRegular);
	}
	else
	{
		selection = static_cast<UINT16>(selection & ~kOs2SelectionBold);
		if ((selection & kOs2SelectionItalic) == 0)
			selection = static_cast<UINT16>(selection | kOs2SelectionRegular);
	}
	WriteU16(table, kOs2SelectionOffset, selection);
}

void AdvertiseOs2Width(std::vector<BYTE>& table, UINT16 width)
{
	WriteU16(table, kOs2WidthClassOffset, width);
}

void AdvertiseHeadWeight(std::vector<BYTE>& table, UINT16 weight)
{
	UINT16 macStyle = ReadU16(table, kHeadMacStyleOffset);
	macStyle = IsBoldWeightClass(weight) ?
		static_cast<UINT16>(macStyle | kHeadMacStyleBold) :
		static_cast<UINT16>(macStyle & ~kHeadMacStyleBold);
	WriteU16(table, kHeadMacStyleOffset, macStyle);
}

} // namespace

HRESULT BuildAliasedSfnt(
	std::vector<BYTE> const& source,
	UINT32 faceIndex,
	std::wstring const& family,
	AliasOptions const& options,
	std::vector<BYTE>& output,
	Identity& identity)
{
	output.clear();
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
		if (table.tag != kTagDsig)
			tables.emplace_back(std::move(table));
	}

	bool rewroteVariable = false;
	for (SfntTable& table : tables)
	{
		if (table.tag != kTagFvar || options.axisValues.empty())
			continue;
		std::vector<BYTE> original(
			source.begin() + table.sourceOffset,
			source.begin() + table.sourceOffset + table.sourceLength);
		if (!BuildPinnedFvar(
				original, options, table.replacement))
			return DWRITE_E_FILEFORMAT;
		rewroteVariable = true;
	}

	FLOAT pinnedWeight = 0;
	FLOAT pinnedWidth = 0;
	bool const havePinnedWeight = rewroteVariable && AxisValue(
		options, DWRITE_FONT_AXIS_TAG_WEIGHT, pinnedWeight);
	bool const havePinnedWidth = rewroteVariable && AxisValue(
		options, DWRITE_FONT_AXIS_TAG_WIDTH, pinnedWidth);
	UINT16 advertisedWeight = options.weight;
	if (havePinnedWeight && !options.overrideWeight)
	{
		pinnedWeight = (std::max)(1.0f, (std::min)(1000.0f, pinnedWeight));
		advertisedWeight = static_cast<UINT16>(std::lround(pinnedWeight));
	}
	bool const advertiseWeight = options.overrideWeight || havePinnedWeight;
	UINT16 const advertisedWidth = WidthClass(pinnedWidth);

	bool foundName = false;
	bool foundHead = false;
	bool foundOs2 = false;
	for (SfntTable& table : tables)
	{
		if (table.tag == kTagName)
		{
			std::vector<BYTE> original(
				source.begin() + table.sourceOffset,
				source.begin() + table.sourceOffset + table.sourceLength);
			if (!BuildNameTable(
					original,
					family,
					advertiseWeight && IsBoldWeightClass(advertisedWeight),
					table.replacement,
					identity))
				return DWRITE_E_FILEFORMAT;
			foundName = true;
		}
		else if (table.tag == kTagHead)
		{
			if (table.sourceLength < 12 ||
				(advertiseWeight && table.sourceLength < kHeadMacStyleOffset + 2))
				return DWRITE_E_FILEFORMAT;
			if (advertiseWeight)
			{
				table.replacement.assign(
					source.begin() + table.sourceOffset,
					source.begin() + table.sourceOffset + table.sourceLength);
				AdvertiseHeadWeight(table.replacement, advertisedWeight);
			}
			foundHead = true;
		}
		else if (table.tag == kTagOs2)
		{
			if ((advertiseWeight || havePinnedWidth) &&
				table.sourceLength < kOs2SelectionOffset + 2)
				return DWRITE_E_FILEFORMAT;
			if (advertiseWeight || havePinnedWidth)
			{
				table.replacement.assign(
					source.begin() + table.sourceOffset,
					source.begin() + table.sourceOffset + table.sourceLength);
				if (advertiseWeight)
					AdvertiseOs2Weight(table.replacement, advertisedWeight);
				if (havePinnedWidth)
					AdvertiseOs2Width(table.replacement, advertisedWidth);
			}
			foundOs2 = true;
		}
	}
	if (!foundName || !foundHead || tables.empty() ||
		((advertiseWeight || havePinnedWidth) && !foundOs2) ||
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

HRESULT CreateAliasedReference(
	IDWriteFactory3* factory,
	IDWriteFontFaceReference* replacementReference,
	WCHAR const* aliasFamily,
	CComPtr<IDWriteFontFaceReference>& reference,
	Identity& identity,
	AliasOptions const& options)
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
		ReplacementFileIdentity sourceIdentity;
		HRESULT result = ReadReplacementFile(
			replacementReference, source, faceIndex, sourceIdentity, false);
		if (FAILED(result))
			return result;

		AliasOptions effectiveOptions = options;
		CComPtr<IDWriteFontFaceReference1> replacementReference1;
		if (SUCCEEDED(replacementReference->QueryInterface(
				&replacementReference1)) && replacementReference1 != nullptr)
		{
			UINT32 const axisCount = replacementReference1->GetFontAxisValueCount();
			effectiveOptions.axisValues.resize(axisCount);
			if (axisCount != 0 && FAILED(replacementReference1->GetFontAxisValues(
					effectiveOptions.axisValues.data(), axisCount)))
				return E_FAIL;
		}

		namespace cache = renderer::virtual_font_cache;
		auto makeManifestKey = [&]() {
			cache::ManifestKey key;
			key.replacementPath = sourceIdentity.path;
			key.replacementSize = sourceIdentity.size;
			key.replacementLastWriteTime = sourceIdentity.lastWriteTime;
			key.faceIndex = faceIndex;
			key.aliasFamily = aliasFamily;
			key.overrideWeight = effectiveOptions.overrideWeight;
			key.weight = effectiveOptions.weight;
			key.profileDigest = effectiveOptions.profileDigest;
			key.axes.reserve(effectiveOptions.axisValues.size());
			for (DWRITE_FONT_AXIS_VALUE const& axis : effectiveOptions.axisValues)
				key.axes.push_back({static_cast<UINT32>(axis.axisTag), axis.value});
			return key;
		};

		cache::ManifestKey manifestKey = makeManifestKey();
		cache::ManifestValue manifestValue;
		bool manifestHit = false;
		cache::LookupManifestFont(manifestKey, manifestValue, manifestHit);

		DWRITE_FONT_SIMULATIONS const simulations =
			replacementReference->GetSimulations();
		CComPtr<IDWriteFactory6> factory6;
		if (replacementReference1 != nullptr)
			factory->QueryInterface(&factory6);
		auto analyze = [](IDWriteFontFile* file) {
			BOOL supported = FALSE;
			DWRITE_FONT_FILE_TYPE fileType = DWRITE_FONT_FILE_TYPE_UNKNOWN;
			DWRITE_FONT_FACE_TYPE faceType = DWRITE_FONT_FACE_TYPE_UNKNOWN;
			UINT32 faceCount = 0;
			HRESULT const status = file->Analyze(
				&supported, &fileType, &faceType, &faceCount);
			return FAILED(status) ? status :
				(!supported || faceCount == 0 ? DWRITE_E_FILEFORMAT : S_OK);
		};
		auto makeReference = [&](
			IDWriteFontFile* file,
			CComPtr<IDWriteFontFaceReference>& created) {
			created.Release();
			if (factory6 != nullptr)
			{
				CComPtr<IDWriteFontFaceReference1> created1;
				HRESULT const status = factory6->CreateFontFaceReference(
					file,
					0,
					simulations,
					effectiveOptions.axisValues.data(),
					static_cast<UINT32>(effectiveOptions.axisValues.size()),
					&created1);
				if (SUCCEEDED(status))
					created = created1;
				return status;
			}
			return factory->CreateFontFaceReference(
				file, 0, simulations, &created);
		};

		bool rebuiltBrokenHit = false;
		for (;;)
		{
			bool const fromManifest = manifestHit;
			std::wstring backingPath;
			CComPtr<IDWriteFontFile> fontFile;
			if (fromManifest)
			{
				backingPath = manifestValue.virtualFontPath;
				result = factory->CreateFontFileReference(
					backingPath.c_str(), nullptr, &fontFile);
				if (SUCCEEDED(result) && fontFile != nullptr)
					result = analyze(fontFile);
				if (FAILED(result) || fontFile == nullptr)
				{
					cache::DropManifestFont(manifestKey);
					manifestHit = false;
					rebuiltBrokenHit = true;
					continue;
				}
				identity.family = manifestValue.family;
				identity.subfamily = manifestValue.subfamily;
				identity.fullName = manifestValue.fullName;
				identity.postScriptName = manifestValue.postScriptName;
			}
			else
			{
				result = ReadReplacementFile(
					replacementReference, source, faceIndex, sourceIdentity, true);
				if (FAILED(result))
					return result;
				manifestKey = makeManifestKey();
				std::vector<BYTE> aliased;
				result = BuildAliasedSfnt(
					source, faceIndex, aliasFamily, effectiveOptions, aliased, identity);
				if (FAILED(result))
					return result;
				result = renderer::virtual_font_cache::PersistFont(aliased, backingPath);
				if (FAILED(result))
					return result;
				result = factory->CreateFontFileReference(
					backingPath.c_str(), nullptr, &fontFile);
				if (SUCCEEDED(result) && fontFile != nullptr)
					result = analyze(fontFile);
				if (FAILED(result) || fontFile == nullptr)
					return FAILED(result) ? result : E_FAIL;
				manifestValue.virtualFontPath = backingPath;
				manifestValue.virtualFontSize = static_cast<UINT64>(aliased.size());
				manifestValue.family = identity.family;
				manifestValue.subfamily = identity.subfamily;
				manifestValue.fullName = identity.fullName;
				manifestValue.postScriptName = identity.postScriptName;
			}

			CComPtr<IDWriteFontFaceReference> created;
			result = makeReference(fontFile, created);
			if ((FAILED(result) || created == nullptr) &&
				fromManifest && !rebuiltBrokenHit)
			{
				cache::DropManifestFont(manifestKey);
				manifestHit = false;
				rebuiltBrokenHit = true;
				continue;
			}
			if (FAILED(result) || created == nullptr)
				return FAILED(result) ? result : E_FAIL;
			if (!fromManifest)
				cache::StoreManifestFont(manifestKey, manifestValue);
			reference = created;
			return S_OK;
		}
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
