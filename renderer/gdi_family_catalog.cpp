#include "settings.h"

#include "gdi_family_catalog.h"
#include "renderer_raii.h"
#include "sfnt_names.h"

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace renderer {
namespace gdi_family_catalog {
namespace {

using bold_face_selection::FaceCandidate;

// GetFontData takes the table tag in little-endian byte order.
constexpr DWORD kNameTableTag = 0x656d616e;
constexpr DWORD kMaximumNameTableBytes = 16u * 1024u * 1024u;

int CompareNoCase(
	const wchar_t* left,
	std::size_t leftLength,
	const wchar_t* right,
	std::size_t rightLength) noexcept
{
	return CompareStringOrdinal(
		left, static_cast<int>(leftLength),
		right, static_cast<int>(rightLength), TRUE);
}

bool EqualNoCase(const std::wstring& left, const std::wstring& right) noexcept
{
	return CompareNoCase(left.c_str(), left.size(), right.c_str(), right.size()) ==
		CSTR_EQUAL;
}

struct NoCaseLess
{
	bool operator()(const std::wstring& left, const std::wstring& right) const noexcept
	{
		return CompareNoCase(
			left.c_str(), left.size(), right.c_str(), right.size()) == CSTR_LESS_THAN;
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

template <typename Map, typename Value>
bool LookupCached(Map& map, const std::wstring& key, Value& value)
{
	Catalog& catalog = GetCatalog();
	std::lock_guard<std::mutex> lock(catalog.mutex);
	typename Map::const_iterator const found = map.find(key);
	if (found == map.end())
		return false;
	value = found->second;
	return true;
}

template <typename Map, typename Value>
void StoreCached(Map& map, const std::wstring& key, const Value& value)
{
	Catalog& catalog = GetCatalog();
	std::lock_guard<std::mutex> lock(catalog.mutex);
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

bool EnumerateFaces(const std::wstring& family, std::vector<FaceCandidate>& faces)
{
	faces.clear();
	LOGFONTW query = {};
	query.lfCharSet = DEFAULT_CHARSET;
	if (family.empty() ||
		FAILED(StringCchCopyW(query.lfFaceName, LF_FACESIZE, family.c_str())))
		return false;
	renderer_raii::UniqueDeviceContext dc(CreateCompatibleDC(nullptr));
	if (!dc)
		return false;
	FaceEnumeration enumeration;
	enumeration.family = &family;
	enumeration.faces = &faces;
	EnumFontFamiliesExW(
		dc.get(), &query, CollectFace, reinterpret_cast<LPARAM>(&enumeration), 0);
	if (enumeration.failed)
		faces.clear();
	return !enumeration.failed;
}

bool EnumerateFamilies(std::vector<std::wstring>& families)
{
	families.clear();
	LOGFONTW query = {};
	query.lfCharSet = DEFAULT_CHARSET;
	renderer_raii::UniqueDeviceContext dc(CreateCompatibleDC(nullptr));
	if (!dc)
		return false;
	std::set<std::wstring, NoCaseLess> unique;
	FamilyEnumeration enumeration;
	enumeration.families = &unique;
	EnumFontFamiliesExW(
		dc.get(), &query, CollectFamily, reinterpret_cast<LPARAM>(&enumeration), 0);
	if (enumeration.failed)
		return false;
	families.assign(unique.begin(), unique.end());
	return true;
}

// The font is realised with FONT_MAGIC_NUMBER so the CreateFontIndirect hook
// passes it through unsubstituted, and the name table is read through the
// original GetFontData because the hooked one reports the substituted names.
bool ReadTypographicNames(
	const std::wstring& family,
	std::vector<std::wstring>& names)
{
	names.clear();
	LOGFONTW request = {};
	request.lfCharSet = DEFAULT_CHARSET;
	request.lfWeight = FW_NORMAL;
	request.lfClipPrecision = FONT_MAGIC_NUMBER;
	if (family.empty() ||
		FAILED(StringCchCopyW(request.lfFaceName, LF_FACESIZE, family.c_str())))
		return false;
	renderer_raii::UniqueFont font(CreateFontIndirectW(&request));
	renderer_raii::UniqueDeviceContext dc(CreateCompatibleDC(nullptr));
	if (!font || !dc)
		return false;
	auto selected = renderer_raii::SelectObject(
		dc.get(), static_cast<HFONT>(font.get()));
	if (!selected)
		return false;
	DWORD const size = ORIG_GetFontData(dc.get(), kNameTableTag, 0, nullptr, 0);
	if (size == GDI_ERROR || size == 0 || size > kMaximumNameTableBytes)
		return false;
	std::vector<BYTE> table(size);
	if (ORIG_GetFontData(dc.get(), kNameTableTag, 0, table.data(), size) != size)
		return false;
	return sfnt::ReadTypographicFamilyNames(table, names);
}

bool CachedFamilyFaces(const std::wstring& family, std::vector<FaceCandidate>& faces)
{
	Catalog& catalog = GetCatalog();
	if (LookupCached(catalog.faces, family, faces))
		return !faces.empty();
	if (!EnumerateFaces(family, faces))
		return false;
	StoreCached(catalog.faces, family, faces);
	return !faces.empty();
}

bool CachedTypographicNames(const std::wstring& family, std::vector<std::wstring>& names)
{
	Catalog& catalog = GetCatalog();
	if (LookupCached(catalog.typographicNames, family, names))
		return !names.empty();
	if (!ReadTypographicNames(family, names))
		names.clear();
	StoreCached(catalog.typographicNames, family, names);
	return !names.empty();
}

bool CachedFamilies(std::vector<std::wstring>& families)
{
	Catalog& catalog = GetCatalog();
	{
		std::lock_guard<std::mutex> lock(catalog.mutex);
		if (catalog.familiesListed)
		{
			families = catalog.families;
			return true;
		}
	}
	if (!EnumerateFamilies(families))
		return false;
	std::lock_guard<std::mutex> lock(catalog.mutex);
	if (!catalog.familiesListed)
	{
		catalog.families = families;
		catalog.familiesListed = true;
	}
	return true;
}

bool NamePrefixMatches(const std::wstring& family, const std::wstring& name) noexcept
{
	if (name.empty() || family.size() < name.size())
		return false;
	if (family.size() > name.size() && family[name.size()] != L' ')
		return false;
	return CompareNoCase(family.c_str(), name.size(), name.c_str(), name.size()) ==
		CSTR_EQUAL;
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

bool BuildTypographicFaces(
	const std::wstring& family,
	std::vector<FaceCandidate>& faces)
{
	faces.clear();
	if (!CachedFamilyFaces(family, faces))
		return false;
	std::vector<std::wstring> names;
	if (!CachedTypographicNames(family, names))
		return true;
	std::vector<std::wstring> families;
	if (!CachedFamilies(families))
		return true;
	std::vector<std::wstring> candidateNames;
	std::vector<FaceCandidate> candidateFaces;
	for (const std::wstring& candidate : families)
	{
		if (EqualNoCase(candidate, family))
			continue;
		bool prefixed = false;
		for (const std::wstring& name : names)
			prefixed = prefixed || NamePrefixMatches(candidate, name);
		if (!prefixed ||
			!CachedTypographicNames(candidate, candidateNames) ||
			!SharesName(names, candidateNames) ||
			!CachedFamilyFaces(candidate, candidateFaces))
			continue;
		faces.insert(faces.end(), candidateFaces.begin(), candidateFaces.end());
	}
	return true;
}

} // namespace

bool FamilyFaces(
	const wchar_t* gdiFamily,
	std::vector<FaceCandidate>& faces) noexcept
{
	try
	{
		faces.clear();
		if (gdiFamily == nullptr || *gdiFamily == L'\0')
			return false;
		return CachedFamilyFaces(gdiFamily, faces);
	}
	catch (...)
	{
		faces.clear();
		return false;
	}
}

bool TypographicFamilyFaces(
	const wchar_t* gdiFamily,
	std::vector<FaceCandidate>& faces) noexcept
{
	try
	{
		faces.clear();
		if (gdiFamily == nullptr || *gdiFamily == L'\0')
			return false;
		std::wstring const family(gdiFamily);
		Catalog& catalog = GetCatalog();
		if (LookupCached(catalog.typographicFaces, family, faces))
			return !faces.empty();
		if (!BuildTypographicFaces(family, faces))
		{
			faces.clear();
			return false;
		}
		StoreCached(catalog.typographicFaces, family, faces);
		return !faces.empty();
	}
	catch (...)
	{
		faces.clear();
		return false;
	}
}

void ClearForQuietUnload() noexcept
{
	Catalog& catalog = GetCatalog();
	std::lock_guard<std::mutex> lock(catalog.mutex);
	catalog.familiesListed = false;
	catalog.families.clear();
	catalog.faces.clear();
	catalog.typographicNames.clear();
	catalog.typographicFaces.clear();
}

} // namespace gdi_family_catalog
} // namespace renderer
