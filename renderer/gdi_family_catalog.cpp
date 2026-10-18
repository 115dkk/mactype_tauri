#include "settings.h"

#include "gdi_family_catalog.h"
#include "renderer_raii.h"
#include "sfnt_names.h"

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace renderer {
namespace gdi_family_catalog {
namespace {

using bold_face_selection::FaceCandidate;

// GetFontData takes the table tag in little-endian byte order.
constexpr DWORD kNameTableTag = 0x656d616e;
constexpr DWORD kMaximumNameTableBytes = 16u * 1024u * 1024u;

int CompareNoCase(
	std::wstring_view left,
	std::wstring_view right) noexcept
{
	return CompareStringOrdinal(
		left.data(), static_cast<int>(left.size()),
		right.data(), static_cast<int>(right.size()), TRUE);
}

bool EqualNoCase(std::wstring_view left, std::wstring_view right) noexcept
{
	return CompareNoCase(left, right) == CSTR_EQUAL;
}

struct NoCaseLess
{
	bool operator()(const std::wstring& left, const std::wstring& right) const noexcept
	{
		return CompareNoCase(left, right) == CSTR_LESS_THAN;
	}
};

using FaceMap = std::map<std::wstring, std::vector<FaceCandidate>, NoCaseLess>;
using NameMap = std::map<std::wstring, std::vector<std::wstring>, NoCaseLess>;

struct Catalog
{
	std::mutex mutex;
	bool familiesListed = false;
	std::vector<std::wstring> families;
	FaceMap faces;
	NameMap typographicNames;
	FaceMap typographicFaces;
};

Catalog& GetCatalog()
{
	static Catalog* catalog = new Catalog;
	return *catalog;
}

template <typename Map>
std::optional<typename Map::mapped_type> LookupCached(
	Map& map, const std::wstring& key)
{
	Catalog& catalog = GetCatalog();
	std::scoped_lock lock(catalog.mutex);
	typename Map::const_iterator const found = map.find(key);
	if (found == map.end())
		return std::nullopt;
	return found->second;
}

template <typename Map, typename Value>
void StoreCached(Map& map, const std::wstring& key, const Value& value)
{
	Catalog& catalog = GetCatalog();
	std::scoped_lock lock(catalog.mutex);
	map.emplace(key, value);
}

struct FaceEnumeration
{
	const std::wstring* family = nullptr;
	std::vector<FaceCandidate>* faces = nullptr;
	bool failed = false;
};

int CALLBACK CollectFace(
	const LOGFONTW* logFont,
	const TEXTMETRICW*,
	DWORD,
	LPARAM parameter)
{
	FaceEnumeration* const enumeration =
		reinterpret_cast<FaceEnumeration*>(parameter);
	if (logFont == nullptr || enumeration == nullptr)
		return 1;
	int const weight = static_cast<int>(logFont->lfWeight);
	bool const italic = logFont->lfItalic != 0;
	for (const FaceCandidate& face : *enumeration->faces)
	{
		if (face.weight == weight && face.italic == italic)
			return 1;
	}
	try
	{
		FaceCandidate face;
		face.gdiFamily = *enumeration->family;
		face.weight = weight;
		face.italic = italic;
		enumeration->faces->push_back(std::move(face));
	}
	catch (...)
	{
		enumeration->failed = true;
		return 0;
	}
	return 1;
}

struct FamilyEnumeration
{
	std::set<std::wstring, NoCaseLess>* families = nullptr;
	bool failed = false;
};

int CALLBACK CollectFamily(
	const LOGFONTW* logFont,
	const TEXTMETRICW*,
	DWORD,
	LPARAM parameter)
{
	FamilyEnumeration* const enumeration =
		reinterpret_cast<FamilyEnumeration*>(parameter);
	if (logFont == nullptr || enumeration == nullptr)
		return 1;
	std::size_t const length = wcsnlen(logFont->lfFaceName, LF_FACESIZE);
	if (length == 0 || length == LF_FACESIZE || logFont->lfFaceName[0] == L'@')
		return 1;
	try
	{
		enumeration->families->emplace(logFont->lfFaceName, length);
	}
	catch (...)
	{
		enumeration->failed = true;
		return 0;
	}
	return 1;
}

std::optional<std::vector<FaceCandidate>> EnumerateFaces(
	const std::wstring& family)
{
	LOGFONTW query = {};
	query.lfCharSet = DEFAULT_CHARSET;
	if (family.empty() ||
		FAILED(StringCchCopyW(query.lfFaceName, LF_FACESIZE, family.c_str())))
		return std::nullopt;
	renderer_raii::UniqueDeviceContext dc(CreateCompatibleDC(nullptr));
	if (!dc)
		return std::nullopt;
	std::vector<FaceCandidate> faces;
	FaceEnumeration enumeration;
	enumeration.family = &family;
	enumeration.faces = &faces;
	EnumFontFamiliesExW(
		dc.get(), &query, CollectFace, reinterpret_cast<LPARAM>(&enumeration), 0);
	if (enumeration.failed)
		return std::nullopt;
	return faces;
}

std::optional<std::vector<std::wstring>> EnumerateFamilies()
{
	LOGFONTW query = {};
	query.lfCharSet = DEFAULT_CHARSET;
	renderer_raii::UniqueDeviceContext dc(CreateCompatibleDC(nullptr));
	if (!dc)
		return std::nullopt;
	std::set<std::wstring, NoCaseLess> unique;
	FamilyEnumeration enumeration;
	enumeration.families = &unique;
	EnumFontFamiliesExW(
		dc.get(), &query, CollectFamily, reinterpret_cast<LPARAM>(&enumeration), 0);
	if (enumeration.failed)
		return std::nullopt;
	return std::vector<std::wstring>(unique.begin(), unique.end());
}

// The font is realised with FONT_MAGIC_NUMBER so the CreateFontIndirect hook
// passes it through unsubstituted, and the name table is read through the
// original GetFontData because the hooked one reports the substituted names.
std::optional<std::vector<std::wstring>> ReadTypographicNames(
	const std::wstring& family)
{
	LOGFONTW request = {};
	request.lfCharSet = DEFAULT_CHARSET;
	request.lfWeight = FW_NORMAL;
	request.lfClipPrecision = FONT_MAGIC_NUMBER;
	if (family.empty() ||
		FAILED(StringCchCopyW(request.lfFaceName, LF_FACESIZE, family.c_str())))
		return std::nullopt;
	renderer_raii::UniqueFont font(CreateFontIndirectW(&request));
	renderer_raii::UniqueDeviceContext dc(CreateCompatibleDC(nullptr));
	if (!font || !dc)
		return std::nullopt;
	auto selected = renderer_raii::SelectObject(
		dc.get(), static_cast<HFONT>(font.get()));
	if (!selected)
		return std::nullopt;
	DWORD const size = ORIG_GetFontData(dc.get(), kNameTableTag, 0, nullptr, 0);
	if (size == GDI_ERROR || size == 0 || size > kMaximumNameTableBytes)
		return std::nullopt;
	std::vector<BYTE> table(size);
	if (ORIG_GetFontData(dc.get(), kNameTableTag, 0, table.data(), size) != size)
		return std::nullopt;
	return sfnt::ReadTypographicFamilyNames(table);
}

std::optional<std::vector<FaceCandidate>> CachedFamilyFaces(
	const std::wstring& family)
{
	Catalog& catalog = GetCatalog();
	auto faces = LookupCached(catalog.faces, family);
	if (!faces)
	{
		faces = EnumerateFaces(family);
		if (!faces)
			return std::nullopt;
		StoreCached(catalog.faces, family, *faces);
	}
	return faces->empty() ? std::nullopt : std::move(faces);
}

std::optional<std::vector<std::wstring>> CachedTypographicNames(
	const std::wstring& family)
{
	Catalog& catalog = GetCatalog();
	auto names = LookupCached(catalog.typographicNames, family);
	if (!names)
	{
		names = ReadTypographicNames(family);
		if (!names)
		{
			std::vector<std::wstring> empty;
			StoreCached(catalog.typographicNames, family, empty);
			return std::nullopt;
		}
		StoreCached(catalog.typographicNames, family, *names);
	}
	return names->empty() ? std::nullopt : std::move(names);
}

std::optional<std::vector<std::wstring>> CachedFamilies()
{
	Catalog& catalog = GetCatalog();
	{
		std::scoped_lock lock(catalog.mutex);
		if (catalog.familiesListed)
			return catalog.families;
	}
	auto families = EnumerateFamilies();
	if (!families)
		return std::nullopt;
	std::scoped_lock lock(catalog.mutex);
	if (!catalog.familiesListed)
	{
		catalog.families = *families;
		catalog.familiesListed = true;
	}
	return families;
}

bool NamePrefixMatches(
	std::wstring_view family, std::wstring_view name) noexcept
{
	if (name.empty() || family.size() < name.size())
		return false;
	if (family.size() > name.size() && family[name.size()] != L' ')
		return false;
	return CompareNoCase(family.substr(0, name.size()), name) == CSTR_EQUAL;
}

bool SharesName(
	const std::vector<std::wstring>& left,
	const std::vector<std::wstring>& right) noexcept
{
	for (const std::wstring& leftName : left)
	{
		for (const std::wstring& rightName : right)
		{
			if (EqualNoCase(leftName, rightName))
				return true;
		}
	}
	return false;
}

std::optional<std::vector<FaceCandidate>> BuildTypographicFaces(
	const std::wstring& family)
{
	auto faces = CachedFamilyFaces(family);
	if (!faces)
		return std::nullopt;
	auto names = CachedTypographicNames(family);
	if (!names)
		return faces;
	auto families = CachedFamilies();
	if (!families)
		return faces;
	for (const std::wstring& candidate : *families)
	{
		if (EqualNoCase(candidate, family))
			continue;
		bool prefixed = false;
		for (const std::wstring& name : *names)
			prefixed = prefixed || NamePrefixMatches(candidate, name);
		if (!prefixed)
			continue;
		auto candidateNames = CachedTypographicNames(candidate);
		if (!candidateNames || !SharesName(*names, *candidateNames))
			continue;
		auto candidateFaces = CachedFamilyFaces(candidate);
		if (!candidateFaces)
			continue;
		faces->insert(
			faces->end(), candidateFaces->begin(), candidateFaces->end());
	}
	return faces;
}

} // namespace

std::optional<std::vector<FaceCandidate>> FamilyFaces(
	const wchar_t* gdiFamily) noexcept
{
	try
	{
		if (gdiFamily == nullptr || *gdiFamily == L'\0')
			return std::nullopt;
		return CachedFamilyFaces(gdiFamily);
	}
	catch (...)
	{
		return std::nullopt;
	}
}

std::optional<std::vector<FaceCandidate>> TypographicFamilyFaces(
	const wchar_t* gdiFamily) noexcept
{
	try
	{
		if (gdiFamily == nullptr || *gdiFamily == L'\0')
			return std::nullopt;
		std::wstring const family(gdiFamily);
		Catalog& catalog = GetCatalog();
		auto faces = LookupCached(catalog.typographicFaces, family);
		if (!faces)
		{
			faces = BuildTypographicFaces(family);
			if (!faces)
				return std::nullopt;
			StoreCached(catalog.typographicFaces, family, *faces);
		}
		return faces->empty() ? std::nullopt : std::move(faces);
	}
	catch (...)
	{
		return std::nullopt;
	}
}

void ClearForQuietUnload() noexcept
{
	Catalog& catalog = GetCatalog();
	std::scoped_lock lock(catalog.mutex);
	catalog.familiesListed = false;
	catalog.families.clear();
	catalog.faces.clear();
	catalog.typographicNames.clear();
	catalog.typographicFaces.clear();
}

} // namespace gdi_family_catalog
} // namespace renderer
