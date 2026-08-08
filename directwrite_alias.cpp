#include "directwrite_alias.h"

#include "directwrite_virtual_font.h"
#include "settings.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace directwrite_alias {
namespace {

struct CacheEntry
{
	CComPtr<IUnknown> factoryIdentity;
	CComPtr<IUnknown> systemSetIdentity;
	AliasFontSet aliases;
};

struct SubstitutionRule
{
	std::wstring sourceFamily;
	std::wstring replacementFamily;
	bool charsetSpecific = false;
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
	std::vector<SubstitutionRule> const& rules,
	std::wstring& sourceFamily,
	std::wstring& replacementFamily)
{
	if (familyNames == nullptr)
		return false;
	for (UINT32 nameIndex = 0; nameIndex < familyNames->GetCount(); ++nameIndex)
	{
		std::wstring family;
		std::wstring locale;
		if (!ReadLocalizedString(
				familyNames, nameIndex, family, locale) || family.empty())
			continue;

		for (SubstitutionRule const& rule : rules)
		{
			if (_wcsicmp(family.c_str(), rule.sourceFamily.c_str()) != 0)
				continue;
			sourceFamily = std::move(family);
			replacementFamily = rule.replacementFamily;
			return true;
		}
	}
	return false;
}

bool ResolveReplacementFamily(
	IDWriteFontSet* systemFontSet,
	UINT32 index,
	std::vector<SubstitutionRule> const& rules,
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
				familyNames, rules, sourceFamily, replacementFamily))
			return true;
	}
	return false;
}

std::vector<SubstitutionRule> CollectSubstitutionRules(
	CFontSubstitutesInfo const& substitutions)
{
	std::vector<SubstitutionRule> rules;
	rules.reserve(static_cast<size_t>(substitutions.GetSize()));
	for (int index = 0; index < substitutions.GetSize(); ++index)
	{
		LOGFONT source = {};
		LOGFONT replacement = {};
		bool charsetSpecific = false;
		if (!substitutions.CopyRule(
				index, source, replacement, charsetSpecific) ||
			(charsetSpecific && source.lfCharSet != DEFAULT_CHARSET) ||
			_wcsicmp(source.lfFaceName, replacement.lfFaceName) == 0)
			continue;

		auto existing = std::find_if(
			rules.begin(), rules.end(), [&](SubstitutionRule const& rule) {
				return _wcsicmp(
					rule.sourceFamily.c_str(), source.lfFaceName) == 0;
			});
		if (existing == rules.end())
		{
			rules.push_back({
				source.lfFaceName,
				replacement.lfFaceName,
				charsetSpecific,
			});
		}
		else if (charsetSpecific && !existing->charsetSpecific)
		{
			existing->replacementFamily = replacement.lfFaceName;
			existing->charsetSpecific = true;
		}
	}
	return rules;
}

HRESULT FindReplacementReference(
	IDWriteFontSet* systemFontSet,
	IDWriteFontFaceReference* sourceReference,
	WCHAR const* replacementFamily,
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

	CComPtr<IDWriteFontSet> replacementSet;
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

BuildStatus Build(
	IDWriteFactory* factory,
	IDWriteFontSet* systemFontSet,
	AliasFontSet& result)
{
	result = {};
	if (factory == nullptr || systemFontSet == nullptr)
		return BuildStatus::systemSetUnavailable;

	CGdippSettings const* settings = CGdippSettings::GetInstance();
	if (!settings->DelayedInited())
		return BuildStatus::settingsNotInitialized;
	SetLastError(ERROR_SUCCESS);
	DWORD const disabled = GetEnvironmentVariableW(
		L"MACTYPE_FONTSUBSTITUTES_ENV", nullptr, 0);
	if (disabled != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND)
		return BuildStatus::noSubstitutions;
	std::vector<SubstitutionRule> const rules =
		CollectSubstitutionRules(settings->GetFontSubstitutesInfo());
	if (rules.empty())
		return BuildStatus::noSubstitutions;

	CComPtr<IDWriteFactory3> factory3;
	if (FAILED(factory->QueryInterface(&factory3)) || factory3 == nullptr)
		return BuildStatus::unsupportedFactory;

	CComPtr<IDWriteFontSetBuilder> builder;
	if (FAILED(factory3->CreateFontSetBuilder(&builder)) || builder == nullptr)
		return BuildStatus::builderUnavailable;
	bool missingReplacement = false;
	bool resolvedRule = false;
	BuildStatus emptyResultStatus = BuildStatus::replacementUnavailable;
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
			systemFontSet, index, rules, sourceFamily, replacementFamily))
		{
			if (FAILED(builder->AddFontFaceReference(sourceReference)))
				return BuildStatus::addFontFailed;
			continue;
		}
		resolvedRule = true;

		CComPtr<IDWriteFontFaceReference> replacementReference;
		addResult = FindReplacementReference(
			systemFontSet,
			sourceReference,
			replacementFamily.c_str(), replacementReference);
		if (SUCCEEDED(addResult) && replacementReference != nullptr)
		{
			CComPtr<IDWriteFontFaceReference> virtualReference;
			directwrite_virtual_font::Identity identity;
			addResult = directwrite_virtual_font::CreateAliasedReference(
				factory3,
				replacementReference,
				sourceFamily.c_str(),
				virtualReference,
				identity);
			if (FAILED(addResult))
				emptyResultStatus = BuildStatus::virtualFontFailed;
			else
			{
				addResult = builder->AddFontFaceReference(virtualReference);
				if (FAILED(addResult))
					emptyResultStatus = BuildStatus::aliasReferenceRejected;
			}
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
		return resolvedRule ? emptyResultStatus :
			BuildStatus::noResolvedSubstitutions;
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

BuildStatus GetOrCreateUnchecked(
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

} // namespace

BuildStatus GetOrCreate(
	IDWriteFactory* factory,
	IDWriteFontSet* systemFontSet,
	AliasFontSet& result) noexcept
{
	try
	{
		return GetOrCreateUnchecked(factory, systemFontSet, result);
	}
	catch (std::bad_alloc const&)
	{
		result = {};
		return BuildStatus::outOfMemory;
	}
	catch (...)
	{
		result = {};
		return BuildStatus::unexpectedFailure;
	}
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
	case BuildStatus::settingsNotInitialized:
		return L"alias-collection-settings-not-initialized";
	case BuildStatus::noResolvedSubstitutions:
		return L"alias-collection-no-resolved-rules";
	case BuildStatus::unsupportedFactory:
		return L"alias-collection-unsupported-factory";
	case BuildStatus::systemSetUnavailable:
		return L"alias-collection-system-set-unavailable";
	case BuildStatus::builderUnavailable:
		return L"alias-collection-builder-unavailable";
	case BuildStatus::replacementUnavailable:
		return L"alias-collection-replacement-unavailable";
	case BuildStatus::virtualFontFailed:
		return L"alias-collection-virtual-font-failed";
	case BuildStatus::aliasReferenceRejected:
		return L"alias-collection-alias-reference-rejected";
	case BuildStatus::outOfMemory:
		return L"alias-collection-out-of-memory";
	case BuildStatus::unexpectedFailure:
		return L"alias-collection-unexpected-failure";
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
