#include "directwrite_alias.h"

#include "bold_face_selection.h"
#include "directwrite_alias_policy.h"
#include "directwrite_virtual_font.h"
#include "font_substitution.h"
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
	std::uint64_t generation = 0;
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

bool IsSubstitutionDisabled() noexcept
{
	SetLastError(ERROR_SUCCESS);
	DWORD const length = GetEnvironmentVariableW(
		L"MACTYPE_FONTSUBSTITUTES_ENV", nullptr, 0);
	return length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
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

void AppendLocalizedFamilies(
	IDWriteLocalizedStrings* familyNames,
	std::vector<std::wstring>& families)
{
	if (familyNames == nullptr)
		return;
	for (UINT32 nameIndex = 0; nameIndex < familyNames->GetCount(); ++nameIndex)
	{
		std::wstring family;
		std::wstring locale;
		if (!ReadLocalizedString(
				familyNames, nameIndex, family, locale) || family.empty())
			continue;
		families.push_back(std::move(family));
	}
}

void ReadSourceFamilyNames(
	IDWriteFontSet* systemFontSet,
	UINT32 index,
	std::vector<std::wstring>& names)
{
	names.clear();
	for (DWRITE_FONT_PROPERTY_ID const familyProperty : kFamilyPropertyIds)
	{
		BOOL exists = FALSE;
		CComPtr<IDWriteLocalizedStrings> familyNames;
		if (FAILED(systemFontSet->GetPropertyValues(
				index, familyProperty, &exists, &familyNames)) ||
			!exists || familyNames == nullptr)
			continue;
		AppendLocalizedFamilies(familyNames, names);
	}
}

struct FaceTraits
{
	DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
	DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
	DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
};

bool ReadFaceTraits(IDWriteFontFaceReference* reference, FaceTraits& traits)
{
	CComPtr<IDWriteFontFace3> face;
	if (reference == nullptr || FAILED(reference->CreateFontFace(&face)) ||
		face == nullptr)
		return false;
	traits.weight = face->GetWeight();
	traits.stretch = face->GetStretch();
	traits.style = face->GetStyle();
	return true;
}

HRESULT FindFamilySet(
	IDWriteFontSet* systemFontSet,
	WCHAR const* family,
	FaceTraits const& traits,
	CComPtr<IDWriteFontSet>& matched)
{
	matched.Release();
	HRESULT result = systemFontSet->GetMatchingFonts(
		family, traits.weight, traits.stretch, traits.style, &matched);
	if (SUCCEEDED(result) && matched != nullptr && matched->GetFontCount() != 0)
		return S_OK;

	for (DWRITE_FONT_PROPERTY_ID const familyProperty : kFamilyPropertyIds)
	{
		DWRITE_FONT_PROPERTY const property = {
			familyProperty,
			family,
			nullptr,
		};
		matched.Release();
		result = systemFontSet->GetMatchingFonts(&property, 1, &matched);
		if (SUCCEEDED(result) && matched != nullptr && matched->GetFontCount() != 0)
			return S_OK;
	}
	matched.Release();
	return FAILED(result) ? result : DWRITE_E_NOFONT;
}

struct FaceCandidates
{
	std::vector<renderer::bold_face_selection::FaceCandidate> faces;
	std::vector<CComPtr<IDWriteFontFaceReference>> references;
};

void CollectCandidates(
	IDWriteFontSet* fontSet,
	DWRITE_FONT_STRETCH const* requiredStretch,
	FaceCandidates& candidates)
{
	candidates.faces.clear();
	candidates.references.clear();
	UINT32 const count = fontSet->GetFontCount();
	for (UINT32 index = 0; index < count; ++index)
	{
		CComPtr<IDWriteFontFaceReference> reference;
		FaceTraits traits;
		if (FAILED(fontSet->GetFontFaceReference(index, &reference)) ||
			reference == nullptr || !ReadFaceTraits(reference, traits))
			continue;
		if (requiredStretch != nullptr && traits.stretch != *requiredStretch)
			continue;
		renderer::bold_face_selection::FaceCandidate face;
		face.weight = static_cast<int>(traits.weight);
		face.italic = traits.style != DWRITE_FONT_STYLE_NORMAL;
		candidates.faces.push_back(face);
		candidates.references.push_back(reference);
	}
}

CComPtr<IDWriteFontFaceReference> CandidateReference(
	FaceCandidates const& candidates,
	renderer::bold_face_selection::FaceCandidate const* chosen)
{
	if (chosen == nullptr)
		return nullptr;
	std::size_t const index =
		static_cast<std::size_t>(chosen - candidates.faces.data());
	return index < candidates.references.size() ?
		candidates.references[index] : nullptr;
}

bool ReadFirstPropertyValue(
	IDWriteFontSet* fontSet,
	DWRITE_FONT_PROPERTY_ID propertyId,
	std::wstring& value)
{
	BOOL exists = FALSE;
	CComPtr<IDWriteLocalizedStrings> values;
	std::wstring locale;
	return SUCCEEDED(fontSet->GetPropertyValues(0, propertyId, &exists, &values)) &&
		exists && values != nullptr && values->GetCount() != 0 &&
		ReadLocalizedString(values, 0, value, locale) && !value.empty();
}

// Mode 2: the lightest face of the base face's own typographic family that is
// heavier than the base and reaches the requested weight, else the heaviest.
CComPtr<IDWriteFontFaceReference> SelectSameFamilyBoldFace(
	IDWriteFontSet* systemFontSet,
	IDWriteFontSet* baseSet,
	IDWriteFontFaceReference* baseReference,
	FaceTraits const& source)
{
	FaceTraits base;
	if (!ReadFaceTraits(baseReference, base))
		return nullptr;
	DWRITE_FONT_PROPERTY_ID propertyId =
		DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME;
	std::wstring family;
	if (!ReadFirstPropertyValue(baseSet, propertyId, family))
	{
		propertyId = DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME;
		if (!ReadFirstPropertyValue(baseSet, propertyId, family))
			return nullptr;
	}
	DWRITE_FONT_PROPERTY const property = {propertyId, family.c_str(), nullptr};
	CComPtr<IDWriteFontSet> familySet;
	if (FAILED(systemFontSet->GetMatchingFonts(&property, 1, &familySet)) ||
		familySet == nullptr)
		return nullptr;
	FaceCandidates candidates;
	CollectCandidates(familySet, &source.stretch, candidates);
	return CandidateReference(
		candidates,
		renderer::bold_face_selection::SelectBoldFace(
			candidates.faces,
			static_cast<int>(base.weight),
			static_cast<int>(source.weight),
			source.style != DWRITE_FONT_STYLE_NORMAL));
}

// Mode 3: the face of the paired family nearest to the source weight.
CComPtr<IDWriteFontFaceReference> SelectPairedFace(
	IDWriteFontSet* systemFontSet,
	std::wstring const& pairedFamily,
	FaceTraits const& source)
{
	CComPtr<IDWriteFontSet> pairedSet;
	if (FAILED(FindFamilySet(
			systemFontSet, pairedFamily.c_str(), source, pairedSet)))
		return nullptr;
	FaceCandidates candidates;
	CollectCandidates(pairedSet, &source.stretch, candidates);
	if (candidates.faces.empty())
		CollectCandidates(pairedSet, nullptr, candidates);
	return CandidateReference(
		candidates,
		renderer::bold_face_selection::SelectNearestWeight(
			candidates.faces,
			static_cast<int>(source.weight),
			source.style != DWRITE_FONT_STYLE_NORMAL));
}

struct ReplacementChoice
{
	CComPtr<IDWriteFontFaceReference> reference;
	directwrite_virtual_font::AliasOptions options;
};

// Bold-class traits yield an alias that advertises that weight, so the alias
// family keeps the source family's weight axis and DirectWrite neither picks
// another slot nor simulates bold on its own. Synthetic bold is the exception:
// the font set builder drops a simulation attached to an aliased reference, so
// that alias keeps the backing's own weight and DirectWrite simulates bold
// for the missing slot.
HRESULT FindReplacementReference(
	IDWriteFontSet* systemFontSet,
	FaceTraits const& source,
	renderer::font_substitution::Snapshot const& snapshot,
	std::wstring const& replacementFamily,
	ReplacementChoice& choice)
{
	namespace substitution = renderer::font_substitution;
	choice = {};

	CComPtr<IDWriteFontSet> replacementSet;
	if (!renderer::bold_face_selection::IsBoldClassWeight(
			static_cast<int>(source.weight)))
	{
		HRESULT const result = FindFamilySet(
			systemFontSet, replacementFamily.c_str(), source, replacementSet);
		if (FAILED(result))
			return result;
		return replacementSet->GetFontFaceReference(0, &choice.reference);
	}

	FaceTraits regular = source;
	regular.weight = DWRITE_FONT_WEIGHT_NORMAL;
	HRESULT result = FindFamilySet(
		systemFontSet, replacementFamily.c_str(), regular, replacementSet);
	if (FAILED(result))
		return result;
	CComPtr<IDWriteFontFaceReference> baseReference;
	result = replacementSet->GetFontFaceReference(0, &baseReference);
	if (FAILED(result) || baseReference == nullptr)
		return FAILED(result) ? result : DWRITE_E_NOFONT;

	bool synthetic = false;
	substitution::BoldPlan const plan = snapshot.PlanBold(
		replacementFamily, static_cast<int>(source.weight));
	switch (plan.action)
	{
	case substitution::BoldAction::sameFamilyFace:
		choice.reference = SelectSameFamilyBoldFace(
			systemFontSet, replacementSet, baseReference, source);
		synthetic = choice.reference == nullptr;
		break;
	case substitution::BoldAction::pairedFamily:
		choice.reference = SelectPairedFace(
			systemFontSet, plan.pairedFamily, source);
		break;
	case substitution::BoldAction::keepWeight:
		synthetic = true;
		break;
	case substitution::BoldAction::unchanged:
	case substitution::BoldAction::dropWeight:
		break;
	}
	if (!synthetic)
	{
		choice.options.overrideWeight = true;
		choice.options.weight = static_cast<UINT16>(source.weight);
	}
	if (choice.reference == nullptr)
		choice.reference = baseReference;
	return S_OK;
}

struct NativeFace
{
	UINT32 index = 0;
	std::vector<std::wstring> familyNames;
};

struct BuildObservations
{
	std::vector<SourceFaceObservation> faces;
	std::vector<FaceTraits> traits;
	std::vector<NativeFace> unresolved;
};

void Observe(
	BuildObservations& observations,
	std::vector<std::wstring> familyNames,
	FaceTraits const& traits,
	bool aliased,
	std::wstring const& replacementFamily)
{
	SourceFaceObservation face;
	face.familyNames = std::move(familyNames);
	face.weight = static_cast<int>(traits.weight);
	face.italic = traits.style != DWRITE_FONT_STYLE_NORMAL;
	face.aliased = aliased;
	if (aliased)
		face.replacementFamily = replacementFamily;
	observations.faces.push_back(std::move(face));
	observations.traits.push_back(traits);
}

// Only an unresolved face that shares a name with an aliased face can hold that
// name's bold slot, so only those faces pay for reading their traits.
void ObserveUnresolvedFaces(
	IDWriteFontSet* systemFontSet,
	BuildObservations& observations)
{
	std::vector<std::wstring> aliasedNames;
	for (SourceFaceObservation const& face : observations.faces)
	{
		if (face.aliased)
			aliasedNames.insert(
				aliasedNames.end(), face.familyNames.begin(), face.familyNames.end());
	}
	for (NativeFace& native : observations.unresolved)
	{
		if (!SharesFamilyName(native.familyNames, aliasedNames))
			continue;
		CComPtr<IDWriteFontFaceReference> reference;
		FaceTraits traits;
		// An unreadable face counts as bold so its names never gain a twin.
		if (FAILED(systemFontSet->GetFontFaceReference(native.index, &reference)) ||
			!ReadFaceTraits(reference, traits))
			traits.weight = DWRITE_FONT_WEIGHT_BOLD;
		Observe(observations, std::move(native.familyNames), traits, false, {});
	}
	observations.unresolved.clear();
}

void AddSyntheticBoldSlots(
	IDWriteFactory3* factory3,
	IDWriteFontSetBuilder* builder,
	IDWriteFontSet* systemFontSet,
	renderer::font_substitution::Snapshot const& snapshot,
	BuildObservations const& observations,
	AliasFontSet& result,
	bool& missingReplacement)
{
	std::vector<SyntheticBoldSlot> slots;
	if (!PlanSyntheticBoldSlots(observations.faces, slots))
		return;
	for (SyntheticBoldSlot const& slot : slots)
	{
		FaceTraits traits = observations.traits[slot.templateFace];
		traits.weight = DWRITE_FONT_WEIGHT_BOLD;
		ReplacementChoice replacement;
		HRESULT const found = FindReplacementReference(
			systemFontSet,
			traits,
			snapshot,
			observations.faces[slot.templateFace].replacementFamily,
			replacement);
		if (FAILED(found) || replacement.reference == nullptr)
		{
			missingReplacement = true;
			continue;
		}
		if (!replacement.options.overrideWeight)
			continue;
		std::vector<CComPtr<IDWriteFontFaceReference>> boldReferences;
		for (std::wstring const& familyName : slot.familyNames)
		{
			CComPtr<IDWriteFontFaceReference> boldReference;
			directwrite_virtual_font::Identity identity;
			if (FAILED(directwrite_virtual_font::CreateAliasedReference(
					factory3,
					replacement.reference,
					familyName.c_str(),
					boldReference,
					identity,
					replacement.options)))
			{
				boldReferences.clear();
				break;
			}
			boldReferences.push_back(boldReference);
		}
		if (boldReferences.empty())
		{
			missingReplacement = true;
			continue;
		}
		bool added = true;
		for (CComPtr<IDWriteFontFaceReference> const& boldReference : boldReferences)
		{
			if (FAILED(builder->AddFontFaceReference(boldReference)))
			{
				added = false;
				break;
			}
		}
		if (!added)
		{
			missingReplacement = true;
			continue;
		}
		++result.substitutionCount;
	}
}

// A family name that an alias carries but that no source face of that name
// fills at a bold-class weight gets one synthesized weight-700 slot, backed by
// the same FontSubstitutesBold decision a real bold source face would get. A
// name whose bold face stayed native, whose aliases disagree on the
// replacement, or whose decision is synthetic bold keeps only the slots its
// source faces produced.
BuildStatus Build(
	IDWriteFactory* factory,
	IDWriteFontSet* systemFontSet,
	std::shared_ptr<const renderer::font_substitution::Snapshot> const& snapshot,
	AliasFontSet& result)
{
	result = {};
	if (factory == nullptr || systemFontSet == nullptr)
		return BuildStatus::systemSetUnavailable;

	if (IsSubstitutionDisabled())
		return BuildStatus::noSubstitutions;
	if (!snapshot || snapshot->rules().empty())
		return BuildStatus::noSubstitutions;
	result.substitutionGeneration = snapshot->generation();
	result.substitutionDigest = snapshot->digest();

	CComPtr<IDWriteFactory3> factory3;
	if (FAILED(factory->QueryInterface(&factory3)) || factory3 == nullptr)
		return BuildStatus::unsupportedFactory;

	CComPtr<IDWriteFontSetBuilder> builder;
	if (FAILED(factory3->CreateFontSetBuilder(&builder)) || builder == nullptr)
		return BuildStatus::builderUnavailable;
	bool missingReplacement = false;
	bool resolvedRule = false;
	BuildStatus emptyResultStatus = BuildStatus::replacementUnavailable;
	BuildObservations observations;
	UINT32 const fontCount = systemFontSet->GetFontCount();
	for (UINT32 index = 0; index < fontCount; ++index)
	{
		CComPtr<IDWriteFontFaceReference> sourceReference;
		HRESULT addResult = systemFontSet->GetFontFaceReference(
			index, &sourceReference);
		if (FAILED(addResult) || sourceReference == nullptr)
			return BuildStatus::addFontFailed;

		std::vector<std::wstring> sourceNames;
		ReadSourceFamilyNames(systemFontSet, index, sourceNames);
		FamilyAliasResolution familyResolution;
		if (!ResolveFamilyAliases(sourceNames, *snapshot, familyResolution))
		{
			if (FAILED(builder->AddFontFaceReference(sourceReference)))
				return BuildStatus::addFontFailed;
			if (!sourceNames.empty())
				observations.unresolved.push_back({index, std::move(sourceNames)});
			continue;
		}
		resolvedRule = true;

		FaceTraits sourceTraits;
		ReadFaceTraits(sourceReference, sourceTraits);
		ReplacementChoice replacement;
		HRESULT substitutionResult = FindReplacementReference(
			systemFontSet,
			sourceTraits,
			*snapshot,
			familyResolution.replacementFamily,
			replacement);
		bool complete = SUCCEEDED(substitutionResult) &&
			replacement.reference != nullptr;
		if (complete)
		{
			for (const std::wstring& sourceAlias :
				familyResolution.sourceAliases)
			{
				CComPtr<IDWriteFontFaceReference> virtualReference;
				directwrite_virtual_font::Identity identity;
				substitutionResult =
					directwrite_virtual_font::CreateAliasedReference(
						factory3,
						replacement.reference,
						sourceAlias.c_str(),
						virtualReference,
						identity,
						replacement.options);
				if (FAILED(substitutionResult))
				{
					emptyResultStatus = BuildStatus::virtualFontFailed;
					complete = false;
					break;
				}
				substitutionResult =
					builder->AddFontFaceReference(virtualReference);
				if (FAILED(substitutionResult))
				{
					emptyResultStatus = BuildStatus::aliasReferenceRejected;
					complete = false;
					break;
				}
			}
		}
		Observe(
			observations,
			std::move(familyResolution.sourceAliases),
			sourceTraits,
			complete,
			familyResolution.replacementFamily);
		if (!complete)
		{
			missingReplacement = true;
			if (FAILED(builder->AddFontFaceReference(sourceReference)))
				return BuildStatus::addFontFailed;
			continue;
		}
		++result.substitutionCount;
	}

	if (result.substitutionCount != 0)
	{
		ObserveUnresolvedFaces(systemFontSet, observations);
		AddSyntheticBoldSlots(
			factory3,
			builder,
			systemFontSet,
			*snapshot,
			observations,
			result,
			missingReplacement);
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
	result = {};
	// The enablement state selects the immutable generation. A cached alias
	// must never escape while the caller explicitly requests the stock set.
	if (IsSubstitutionDisabled())
		return BuildStatus::noSubstitutions;
	CGdippSettings const* settings = CGdippSettings::GetInstance();
	if (!settings->DelayedInited())
		return BuildStatus::settingsNotInitialized;
	std::shared_ptr<const renderer::font_substitution::Snapshot> const snapshot =
		renderer::font_substitution::ProcessRegistry().Load();
	if (!snapshot || snapshot->rules().empty())
		return BuildStatus::noSubstitutions;
	std::uint64_t const generation = snapshot->generation();

	CComPtr<IUnknown> const factoryIdentity = GetIdentity(factory);
	CComPtr<IUnknown> const systemSetIdentity = GetIdentity(systemFontSet);
	if (factoryIdentity == nullptr || systemSetIdentity == nullptr)
		return BuildStatus::systemSetUnavailable;

	AliasCache& cache = GetCache();
	{
		std::lock_guard<std::mutex> lock(cache.mutex);
		cache.entries.erase(
			std::remove_if(
				cache.entries.begin(), cache.entries.end(),
				[generation](CacheEntry const& entry) {
					return entry.generation != generation;
				}),
			cache.entries.end());
		for (CacheEntry const& entry : cache.entries)
		{
			if (entry.factoryIdentity == factoryIdentity &&
				entry.systemSetIdentity == systemSetIdentity &&
				entry.generation == generation)
			{
				result = entry.aliases;
				return result.status;
			}
		}
	}

	AliasFontSet built;
	BuildStatus const status = Build(factory, systemFontSet, snapshot, built);
	built.status = status;
	if (status != BuildStatus::applied &&
		status != BuildStatus::appliedWithMissingReplacement)
		return status;

	std::lock_guard<std::mutex> lock(cache.mutex);
	for (CacheEntry const& entry : cache.entries)
	{
		if (entry.factoryIdentity == factoryIdentity &&
			entry.systemSetIdentity == systemSetIdentity &&
			entry.generation == generation)
		{
			result = entry.aliases;
			return result.status;
		}
	}
	cache.entries.push_back({
		factoryIdentity, systemSetIdentity, generation, built});
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

BuildStatus GetCachedForFactory(
	IDWriteFactory* factory,
	std::uint64_t substitutionGeneration,
	AliasFontSet& result) noexcept
{
	result = {};
	try
	{
		CComPtr<IUnknown> const identity = GetIdentity(factory);
		if (identity == nullptr)
			return BuildStatus::unsupportedFactory;
		AliasCache& cache = GetCache();
		std::lock_guard<std::mutex> lock(cache.mutex);
		for (CacheEntry const& entry : cache.entries)
		{
			if (entry.factoryIdentity == identity &&
				entry.generation == substitutionGeneration)
			{
				result = entry.aliases;
				return result.status;
			}
		}
		return BuildStatus::systemSetUnavailable;
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
