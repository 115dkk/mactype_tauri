#include "directwrite_alias.h"

#include "settings.h"

#include <array>
#include <mutex>
#include <string>
#include <vector>

namespace directwrite_alias {
namespace {

struct OwnedFontProperty
{
	DWRITE_FONT_PROPERTY_ID id = DWRITE_FONT_PROPERTY_ID_NONE;
	std::wstring value;
	std::wstring locale;
};

struct CacheEntry
{
	CComPtr<IUnknown> factoryIdentity;
	CComPtr<IUnknown> systemSetIdentity;
	AliasFontSet aliases;
};

struct AliasCache
{
	std::mutex mutex;
	std::vector<CacheEntry> entries;
};

constexpr std::array<DWRITE_FONT_PROPERTY_ID, 3> kFamilyPropertyIds = {
	DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME,
	DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME,
	DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME,
};

AliasCache& GetCache()
{
	static AliasCache* cache = new AliasCache;
	return *cache;
}

std::mutex& GetResolverMutex()
{
	static std::mutex* mutex = new std::mutex;
	return *mutex;
}

bool IsFamilyProperty(DWRITE_FONT_PROPERTY_ID id) noexcept
{
	return id == DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME ||
		id == DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME ||
		id == DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME;
}

bool ReadLocalizedString(
	IDWriteLocalizedStrings* strings,
	UINT32 index,
	std::wstring& value,
	std::wstring& locale)
{
	UINT32 valueLength = 0;
	UINT32 localeLength = 0;
	if (FAILED(strings->GetStringLength(index, &valueLength)) ||
		FAILED(strings->GetLocaleNameLength(index, &localeLength)))
		return false;

	std::vector<WCHAR> valueBuffer(static_cast<size_t>(valueLength) + 1);
	std::vector<WCHAR> localeBuffer(static_cast<size_t>(localeLength) + 1);
	if (FAILED(strings->GetString(index, valueBuffer.data(), valueLength + 1)) ||
		FAILED(strings->GetLocaleName(
			index, localeBuffer.data(), localeLength + 1)))
		return false;

	value.assign(valueBuffer.data(), valueLength);
	locale.assign(localeBuffer.data(), localeLength);
	return true;
}

bool ResolveLocalizedFamily(
	IDWriteLocalizedStrings* familyNames,
	std::wstring& sourceFamily,
	std::wstring& replacementFamily)
{
	if (familyNames == nullptr)
		return false;
	CGdippSettings const* settings = CGdippSettings::GetInstance();
	for (UINT32 nameIndex = 0; nameIndex < familyNames->GetCount(); ++nameIndex)
	{
		std::wstring family;
		std::wstring locale;
		if (!ReadLocalizedString(
				familyNames, nameIndex, family, locale) || family.empty())
			continue;

		LOGFONT source = {};
		source.lfCharSet = DEFAULT_CHARSET;
		if (FAILED(StringCchCopyW(
				source.lfFaceName,
				ARRAYSIZE(source.lfFaceName),
				family.c_str())))
			continue;
		LOGFONT replacement = source;
		bool substituted = false;
		{
			std::lock_guard<std::mutex> lock(GetResolverMutex());
			substituted = settings->CopyForceFont(replacement, source);
		}
		if (!substituted ||
			_wcsicmp(source.lfFaceName, replacement.lfFaceName) == 0)
			continue;

		sourceFamily = std::move(family);
		replacementFamily = replacement.lfFaceName;
		return true;
	}
	return false;
}

bool ResolveReplacementFamily(
	IDWriteFontSet* systemFontSet,
	UINT32 index,
	std::wstring& sourceFamily,
	std::wstring& replacementFamily)
{
	for (DWRITE_FONT_PROPERTY_ID const familyProperty : kFamilyPropertyIds)
	{
		BOOL exists = FALSE;
		CComPtr<IDWriteLocalizedStrings> familyNames;
		if (FAILED(systemFontSet->GetPropertyValues(
				index, familyProperty, &exists, &familyNames)) ||
			!exists || familyNames == nullptr)
			continue;
		if (ResolveLocalizedFamily(
				familyNames, sourceFamily, replacementFamily))
			return true;
	}
	return false;
}

HRESULT FindReplacementReference(
	IDWriteFontSet* systemFontSet,
	IDWriteFontFaceReference* sourceReference,
	WCHAR const* replacementFamily,
	CComPtr<IDWriteFontSet>& replacementSet,
	CComPtr<IDWriteFontFaceReference>& replacementReference)
{
	DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
	DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
	DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
	CComPtr<IDWriteFontFace3> sourceFace;
	if (SUCCEEDED(sourceReference->CreateFontFace(&sourceFace)) && sourceFace)
	{
		weight = sourceFace->GetWeight();
		stretch = sourceFace->GetStretch();
		style = sourceFace->GetStyle();
	}

	HRESULT result = systemFontSet->GetMatchingFonts(
		replacementFamily, weight, stretch, style, &replacementSet);
	if (SUCCEEDED(result) && replacementSet != nullptr &&
		replacementSet->GetFontCount() != 0)
		return replacementSet->GetFontFaceReference(0, &replacementReference);

	for (DWRITE_FONT_PROPERTY_ID const familyProperty : kFamilyPropertyIds)
	{
		DWRITE_FONT_PROPERTY const property = {
			familyProperty,
			replacementFamily,
			nullptr,
		};
		replacementSet.Release();
		result = systemFontSet->GetMatchingFonts(
			&property, 1, &replacementSet);
		if (SUCCEEDED(result) && replacementSet != nullptr &&
			replacementSet->GetFontCount() != 0)
			return replacementSet->GetFontFaceReference(0, &replacementReference);
	}
	return FAILED(result) ? result : DWRITE_E_NOFONT;
}

bool CopyReplacementProperties(
	IDWriteFontSet* replacementSet,
	WCHAR const* sourceFamily,
	std::vector<OwnedFontProperty>& owned)
{
	std::array<bool, 3> familyProperties = {};
	for (UINT32 rawId = DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME;
		rawId < DWRITE_FONT_PROPERTY_ID_TOTAL_RS3; ++rawId)
	{
		auto const id = static_cast<DWRITE_FONT_PROPERTY_ID>(rawId);
		BOOL exists = FALSE;
		CComPtr<IDWriteLocalizedStrings> values;
		if (FAILED(replacementSet->GetPropertyValues(0, id, &exists, &values)))
			return false;
		if (!exists || values == nullptr)
			continue;

		for (UINT32 valueIndex = 0; valueIndex < values->GetCount(); ++valueIndex)
		{
			OwnedFontProperty property;
			property.id = id;
			if (!ReadLocalizedString(
					values, valueIndex, property.value, property.locale))
				return false;
			if (IsFamilyProperty(id))
				property.value = sourceFamily;
			owned.emplace_back(std::move(property));
		}

		if (values->GetCount() != 0)
		{
			if (id == DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME)
				familyProperties[0] = true;
			else if (id == DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME)
				familyProperties[1] = true;
			else if (id == DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME)
				familyProperties[2] = true;
		}
	}

	for (size_t index = 0; index < kFamilyPropertyIds.size(); ++index)
	{
		if (familyProperties[index])
			continue;
		OwnedFontProperty property;
		property.id = kFamilyPropertyIds[index];
		property.value = sourceFamily;
		property.locale = L"en-us";
		owned.emplace_back(std::move(property));
	}
	return !owned.empty();
}

HRESULT AddAliasedReference(
	IDWriteFontSetBuilder* builder,
	IDWriteFontFaceReference* replacementReference,
	IDWriteFontSet* replacementSet,
	WCHAR const* sourceFamily)
{
	std::vector<OwnedFontProperty> owned;
	if (!CopyReplacementProperties(replacementSet, sourceFamily, owned))
		return E_FAIL;

	std::vector<DWRITE_FONT_PROPERTY> properties;
	properties.reserve(owned.size());
	for (OwnedFontProperty const& property : owned)
	{
		properties.push_back({
			property.id,
			property.value.c_str(),
			property.locale.empty() ? nullptr : property.locale.c_str(),
		});
	}
	return builder->AddFontFaceReference(
		replacementReference,
		properties.data(),
		static_cast<UINT32>(properties.size()));
}

BuildStatus Build(
	IDWriteFactory* factory,
	IDWriteFontSet* systemFontSet,
	AliasFontSet& result)
{
	result = {};
	if (factory == nullptr || systemFontSet == nullptr)
		return BuildStatus::systemSetUnavailable;

	CGdippSettings const* settings = CGdippSettings::GetInstance();
	if (!settings->DelayedInited() ||
		settings->GetFontSubstitutesInfo().GetSize() == 0)
		return BuildStatus::noSubstitutions;

	CComPtr<IDWriteFactory3> factory3;
	if (FAILED(factory->QueryInterface(&factory3)) || factory3 == nullptr)
		return BuildStatus::unsupportedFactory;

	CComPtr<IDWriteFontSetBuilder> builder;
	if (FAILED(factory3->CreateFontSetBuilder(&builder)) || builder == nullptr)
		return BuildStatus::builderUnavailable;

	bool missingReplacement = false;
	UINT32 const fontCount = systemFontSet->GetFontCount();
	for (UINT32 index = 0; index < fontCount; ++index)
	{
		CComPtr<IDWriteFontFaceReference> sourceReference;
		HRESULT addResult = systemFontSet->GetFontFaceReference(
			index, &sourceReference);
		if (FAILED(addResult) || sourceReference == nullptr)
			return BuildStatus::addFontFailed;

		std::wstring sourceFamily;
		std::wstring replacementFamily;
		if (!ResolveReplacementFamily(
				systemFontSet, index, sourceFamily, replacementFamily))
		{
			if (FAILED(builder->AddFontFaceReference(sourceReference)))
				return BuildStatus::addFontFailed;
			continue;
		}

		CComPtr<IDWriteFontSet> replacementSet;
		CComPtr<IDWriteFontFaceReference> replacementReference;
		addResult = FindReplacementReference(
			systemFontSet,
			sourceReference,
			replacementFamily.c_str(),
			replacementSet,
			replacementReference);
		if (SUCCEEDED(addResult) && replacementReference != nullptr)
		{
			addResult = AddAliasedReference(
				builder,
				replacementReference,
				replacementSet,
				sourceFamily.c_str());
		}
		if (FAILED(addResult))
		{
			missingReplacement = true;
			if (FAILED(builder->AddFontFaceReference(sourceReference)))
				return BuildStatus::addFontFailed;
			continue;
		}
		++result.substitutionCount;
	}

	if (result.substitutionCount == 0)
		return BuildStatus::noSubstitutions;
	if (FAILED(builder->CreateFontSet(&result.fontSet)) ||
		result.fontSet == nullptr)
		return BuildStatus::createSetFailed;
	if (FAILED(factory3->CreateFontCollectionFromFontSet(
			result.fontSet, &result.collection)) || result.collection == nullptr)
		return BuildStatus::createCollectionFailed;

	result.status = missingReplacement ?
		BuildStatus::appliedWithMissingReplacement : BuildStatus::applied;
	return result.status;
}

CComPtr<IUnknown> GetIdentity(IUnknown* object)
{
	CComPtr<IUnknown> identity;
	if (object != nullptr)
		object->QueryInterface(&identity);
	return identity;
}

} // namespace

BuildStatus GetOrCreate(
	IDWriteFactory* factory,
	IDWriteFontSet* systemFontSet,
	AliasFontSet& result)
{
	CComPtr<IUnknown> const factoryIdentity = GetIdentity(factory);
	CComPtr<IUnknown> const systemSetIdentity = GetIdentity(systemFontSet);
	if (factoryIdentity == nullptr || systemSetIdentity == nullptr)
		return BuildStatus::systemSetUnavailable;

	AliasCache& cache = GetCache();
	{
		std::lock_guard<std::mutex> lock(cache.mutex);
		for (CacheEntry const& entry : cache.entries)
		{
			if (entry.factoryIdentity == factoryIdentity &&
				entry.systemSetIdentity == systemSetIdentity)
			{
				result = entry.aliases;
				return result.status;
			}
		}
	}

	AliasFontSet built;
	BuildStatus const status = Build(factory, systemFontSet, built);
	built.status = status;
	if (status != BuildStatus::applied &&
		status != BuildStatus::appliedWithMissingReplacement)
		return status;

	std::lock_guard<std::mutex> lock(cache.mutex);
	for (CacheEntry const& entry : cache.entries)
	{
		if (entry.factoryIdentity == factoryIdentity &&
			entry.systemSetIdentity == systemSetIdentity)
		{
			result = entry.aliases;
			return result.status;
		}
	}
	cache.entries.push_back({factoryIdentity, systemSetIdentity, built});
	result = built;
	return status;
}

void ClearCache() noexcept
{
	AliasCache& cache = GetCache();
	std::lock_guard<std::mutex> lock(cache.mutex);
	cache.entries.clear();
}

WCHAR const* StatusName(BuildStatus status) noexcept
{
	switch (status)
	{
	case BuildStatus::applied:
		return L"alias-collection-applied";
	case BuildStatus::appliedWithMissingReplacement:
		return L"alias-collection-partial";
	case BuildStatus::noSubstitutions:
		return L"alias-collection-no-rules";
	case BuildStatus::unsupportedFactory:
		return L"alias-collection-unsupported-factory";
	case BuildStatus::systemSetUnavailable:
		return L"alias-collection-system-set-unavailable";
	case BuildStatus::builderUnavailable:
		return L"alias-collection-builder-unavailable";
	case BuildStatus::addFontFailed:
		return L"alias-collection-add-font-failed";
	case BuildStatus::createSetFailed:
		return L"alias-collection-create-set-failed";
	case BuildStatus::createCollectionFailed:
		return L"alias-collection-create-collection-failed";
	}
	return L"alias-collection-unknown";
}

} // namespace directwrite_alias
