#include "directwrite_fallback.h"

#include "directwrite_alias.h"
#include "directwrite_alias_policy.h"
#include "font_substitution.h"
#include "hookCounter.h"
#include "hook_lifecycle.h"
#include "renderer_raii.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <vector>

namespace {

using FallbackMapCharactersMethod = HRESULT (WINAPI *)(
	IDWriteFontFallback*, IDWriteTextAnalysisSource*, UINT32, UINT32,
	IDWriteFontCollection*, WCHAR const*, DWRITE_FONT_WEIGHT,
	DWRITE_FONT_STYLE, DWRITE_FONT_STRETCH, UINT32*, IDWriteFont**, FLOAT*);
using Fallback1MapCharactersMethod = HRESULT (WINAPI *)(
	IDWriteFontFallback1*, IDWriteTextAnalysisSource*, UINT32, UINT32,
	IDWriteFontCollection*, WCHAR const*, DWRITE_FONT_AXIS_VALUE const*, UINT32,
	UINT32*, FLOAT*, IDWriteFontFace5**);

HRESULT WINAPI IMPL_Fallback_MapCharacters(
	IDWriteFontFallback*, IDWriteTextAnalysisSource*, UINT32, UINT32,
	IDWriteFontCollection*, WCHAR const*, DWRITE_FONT_WEIGHT,
	DWRITE_FONT_STYLE, DWRITE_FONT_STRETCH, UINT32*, IDWriteFont**, FLOAT*);
HRESULT WINAPI IMPL_Fallback1_MapCharacters(
	IDWriteFontFallback1*, IDWriteTextAnalysisSource*, UINT32, UINT32,
	IDWriteFontCollection*, WCHAR const*, DWRITE_FONT_AXIS_VALUE const*, UINT32,
	UINT32*, FLOAT*, IDWriteFontFace5**);

// dwrite_2.h lines 525-564 declare IDWriteFontFallback directly after
// IUnknown, so MapCharacters is slot 3. dwrite_3.h lines 3450-3485 derive
// IDWriteFontFallback1 and append its axis overload, so that overload is slot 4.
constexpr size_t kFallbackMapCharactersSlot = 3;
constexpr size_t kFallback1MapCharactersSlot = 4;

struct FallbackVtableHooks
{
	void** vtable = nullptr;
	FallbackMapCharactersMethod mapCharacters = nullptr;
	Fallback1MapCharactersMethod mapCharacters1 = nullptr;
	bool mapCharactersPatched = false;
	bool mapCharacters1Patched = false;
};

struct FactorySource
{
	CComPtr<IUnknown> factoryIdentity;
	CComPtr<IDWriteFactory> factory;
	CComPtr<IUnknown> systemCollectionIdentity;
};

constexpr size_t kFactorySourceLimit = 8;

struct TextSpan
{
	std::vector<WCHAR> text;
};

class HookAttemptGuard final
{
public:
	HookAttemptGuard(
		renderer::HookCoordinator& coordinator,
		renderer::HookAttempt attempt) noexcept
		: coordinator_(coordinator), attempt_(attempt)
	{
	}

	~HookAttemptGuard() noexcept
	{
		if (!completed_ && attempt_.valid())
			coordinator_.CompleteAttempt(
				attempt_, false, renderer::CapabilityReason::transactionFailed,
				ERROR_UNHANDLED_EXCEPTION);
	}

	bool valid() const noexcept { return attempt_.valid(); }

	void Complete(bool succeeded, LONG status) noexcept
	{
		if (!completed_ && attempt_.valid())
		{
			coordinator_.CompleteAttempt(
				attempt_, succeeded,
				succeeded ? renderer::CapabilityReason::none
					: renderer::CapabilityReason::transactionFailed,
				status);
			completed_ = true;
		}
	}

private:
	renderer::HookCoordinator& coordinator_;
	renderer::HookAttempt attempt_;
	bool completed_ = false;
};

std::mutex& RegistryMutex()
{
	static std::mutex* mutex = new std::mutex;
	return *mutex;
}

std::vector<FallbackVtableHooks>& VtableRegistry()
{
	static std::vector<FallbackVtableHooks>* registry =
		new std::vector<FallbackVtableHooks>;
	return *registry;
}

std::vector<FactorySource>& SourceRegistry()
{
	static std::vector<FactorySource>* registry = new std::vector<FactorySource>;
	return *registry;
}

template <typename Target, typename Source>
void SetPointer(Target& target, Source source) noexcept
{
	static_assert(sizeof(Target) == sizeof(Source), "hook pointer size mismatch");
	memcpy(&target, &source, sizeof(target));
}

template <typename Method>
void* MethodAddress(Method method) noexcept
{
	void* address = nullptr;
	SetPointer(address, method);
	return address;
}

CComPtr<IUnknown> Identity(IUnknown* object)
{
	CComPtr<IUnknown> identity;
	if (object != nullptr)
		object->QueryInterface(&identity);
	return identity;
}

FallbackVtableHooks& GetOrAddVtable(void** vtable)
{
	for (FallbackVtableHooks& hooks : VtableRegistry())
	{
		if (hooks.vtable == vtable)
			return hooks;
	}
	VtableRegistry().emplace_back();
	VtableRegistry().back().vtable = vtable;
	return VtableRegistry().back();
}

template <typename Method>
bool PatchMethod(
	FallbackVtableHooks& hooks,
	size_t slot,
	Method replacement,
	Method& original,
	bool& patched)
{
	if (hooks.vtable == nullptr)
		return false;
	void** const entry = hooks.vtable + slot;
	void* const replacementAddress = MethodAddress(replacement);
	if (patched)
		return *entry == replacementAddress;
	void* const originalAddress = *entry;
	if (originalAddress == nullptr || originalAddress == replacementAddress)
		return false;
	SetPointer(original, originalAddress);
	auto protection = renderer_raii::PageProtection::TrySet(
		entry, sizeof(*entry), PAGE_READWRITE);
	if (!protection)
	{
		original = nullptr;
		return false;
	}
	InterlockedExchangePointer(reinterpret_cast<PVOID*>(entry), replacementAddress);
	if (!protection.restore())
	{
		InterlockedExchangePointer(reinterpret_cast<PVOID*>(entry), originalAddress);
		original = nullptr;
		return false;
	}
	patched = true;
	return true;
}

template <typename Method>
void RestoreMethod(
	FallbackVtableHooks& hooks,
	size_t slot,
	Method replacement,
	Method original,
	bool& patched) noexcept
{
	if (!patched || hooks.vtable == nullptr || original == nullptr)
		return;
	void** const entry = hooks.vtable + slot;
	void* const replacementAddress = MethodAddress(replacement);
	if (*entry != replacementAddress)
	{
		patched = false;
		return;
	}
	auto protection = renderer_raii::PageProtection::TrySet(
		entry, sizeof(*entry), PAGE_READWRITE);
	if (!protection)
		return;
	InterlockedExchangePointer(
		reinterpret_cast<PVOID*>(entry), MethodAddress(original));
	if (protection.restore())
		patched = false;
}

FallbackMapCharactersMethod OriginalMapCharacters(IDWriteFontFallback* fallback)
{
	void** const vtable = fallback == nullptr ? nullptr :
		*reinterpret_cast<void***>(fallback);
	std::lock_guard<std::mutex> lock(RegistryMutex());
	for (FallbackVtableHooks const& hooks : VtableRegistry())
	{
		if (hooks.vtable == vtable)
			return hooks.mapCharacters;
	}
	return nullptr;
}

Fallback1MapCharactersMethod OriginalMapCharacters1(IDWriteFontFallback1* fallback)
{
	void** const vtable = fallback == nullptr ? nullptr :
		*reinterpret_cast<void***>(fallback);
	std::lock_guard<std::mutex> lock(RegistryMutex());
	for (FallbackVtableHooks const& hooks : VtableRegistry())
	{
		if (hooks.vtable == vtable)
			return hooks.mapCharacters1;
	}
	return nullptr;
}

bool PublishSource(
	IDWriteFactory* factory,
	IDWriteFontCollection* systemCollection)
{
	CComPtr<IUnknown> const systemIdentity = Identity(systemCollection);
	CComPtr<IUnknown> const factoryIdentity = Identity(factory);
	if (factoryIdentity == nullptr || systemIdentity == nullptr)
		return false;
	// Processes such as explorer.exe create many factories over their
	// lifetime. Keep the newest ones and release evicted references after
	// the registry lock is dropped.
	FactorySource evicted;
	{
		std::lock_guard<std::mutex> lock(RegistryMutex());
		std::vector<FactorySource>& registry = SourceRegistry();
		for (FactorySource& source : registry)
		{
			if (source.factoryIdentity == factoryIdentity)
			{
				source.factory = factory;
				source.systemCollectionIdentity = systemIdentity;
				return true;
			}
		}
		if (registry.size() == kFactorySourceLimit)
		{
			evicted = std::move(registry.front());
			registry.erase(registry.begin());
		}
		registry.push_back({factoryIdentity, factory, systemIdentity});
	}
	return true;
}

size_t CopySources(std::array<FactorySource, kFactorySourceLimit>& results)
{
	size_t count = 0;
	std::lock_guard<std::mutex> lock(RegistryMutex());
	std::vector<FactorySource> const& registry = SourceRegistry();
	for (auto source = registry.rbegin();
		source != registry.rend() && count < results.size(); ++source)
		results[count++] = *source;
	return count;
}

bool IsAllowedCollection(
	IDWriteFontCollection* collection,
	FactorySource const& source,
	IDWriteFontCollection* aliasCollection)
{
	if (collection == nullptr)
		return true;
	CComPtr<IUnknown> const collectionIdentity = Identity(collection);
	if (collectionIdentity == nullptr)
		return false;
	if (collectionIdentity == source.systemCollectionIdentity)
		return true;
	CComPtr<IUnknown> const aliasIdentity = Identity(aliasCollection);
	return aliasIdentity != nullptr && collectionIdentity == aliasIdentity;
}

void AppendLocalizedNames(
	IDWriteLocalizedStrings* strings,
	std::vector<std::wstring>& names)
{
	if (strings == nullptr)
		return;
	for (UINT32 index = 0; index < strings->GetCount(); ++index)
	{
		UINT32 length = 0;
		if (FAILED(strings->GetStringLength(index, &length)))
			continue;
		std::vector<WCHAR> buffer(static_cast<size_t>(length) + 1);
		if (FAILED(strings->GetString(index, buffer.data(), length + 1)))
			continue;
		std::wstring name(buffer.data(), length);
		if (name.empty() || std::any_of(
				names.begin(), names.end(), [&](std::wstring const& existing) {
					return _wcsicmp(existing.c_str(), name.c_str()) == 0;
				}))
			continue;
		names.push_back(std::move(name));
	}
}

void AppendInformationalNames(
	IDWriteFont* font,
	DWRITE_INFORMATIONAL_STRING_ID id,
	std::vector<std::wstring>& names)
{
	CComPtr<IDWriteLocalizedStrings> strings;
	BOOL exists = FALSE;
	if (SUCCEEDED(font->GetInformationalStrings(id, &strings, &exists)) &&
		exists && strings != nullptr)
		AppendLocalizedNames(strings, names);
}

// A fallback font can expose different localized, Win32, typographic, and
// weight/stretch/style family names. Keep all of them in the policy decision.
bool ReadMappedFontNames(IDWriteFont* font, std::vector<std::wstring>& names)
{
	names.clear();
	CComPtr<IDWriteFontFamily> family;
	CComPtr<IDWriteLocalizedStrings> localized;
	if (SUCCEEDED(font->GetFontFamily(&family)) && family != nullptr &&
		SUCCEEDED(family->GetFamilyNames(&localized)) && localized != nullptr)
		AppendLocalizedNames(localized, names);
	AppendInformationalNames(
		font, DWRITE_INFORMATIONAL_STRING_TYPOGRAPHIC_FAMILY_NAMES, names);
	AppendInformationalNames(
		font, DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES, names);

	CComPtr<IDWriteFont3> font3;
	CComPtr<IDWriteFontFace3> face3;
	localized.Release();
	if (SUCCEEDED(font->QueryInterface(&font3)) && font3 != nullptr &&
		SUCCEEDED(font3->CreateFontFace(&face3)) && face3 != nullptr)
	{
		if (SUCCEEDED(face3->GetFamilyNames(&localized)) && localized != nullptr)
			AppendLocalizedNames(localized, names);
		for (DWRITE_INFORMATIONAL_STRING_ID const id : {
			DWRITE_INFORMATIONAL_STRING_TYPOGRAPHIC_FAMILY_NAMES,
			DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES})
		{
			localized.Release();
			BOOL exists = FALSE;
			if (SUCCEEDED(face3->GetInformationalStrings(
					id, &localized, &exists)) && exists && localized != nullptr)
				AppendLocalizedNames(localized, names);
		}
	}
	return !names.empty();
}

bool ReadMappedFaceNames(
	IDWriteFontFace5* face,
	std::vector<std::wstring>& names,
	DWRITE_FONT_WEIGHT& weight,
	DWRITE_FONT_STRETCH& stretch,
	DWRITE_FONT_STYLE& style)
{
	names.clear();
	CComPtr<IDWriteFontFace3> face3;
	if (FAILED(face->QueryInterface(&face3)) || face3 == nullptr)
		return false;
	weight = face3->GetWeight();
	stretch = face3->GetStretch();
	style = face3->GetStyle();
	CComPtr<IDWriteLocalizedStrings> strings;
	if (SUCCEEDED(face3->GetFamilyNames(&strings)) && strings != nullptr)
		AppendLocalizedNames(strings, names);
	for (DWRITE_INFORMATIONAL_STRING_ID const id : {
		DWRITE_INFORMATIONAL_STRING_TYPOGRAPHIC_FAMILY_NAMES,
		DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES})
	{
		strings.Release();
		BOOL exists = FALSE;
		if (SUCCEEDED(face3->GetInformationalStrings(id, &strings, &exists)) &&
			exists && strings != nullptr)
			AppendLocalizedNames(strings, names);
	}
	return !names.empty();
}

bool ReadTextSpan(
	IDWriteTextAnalysisSource* source,
	UINT32 textPosition,
	UINT32 textLength,
	TextSpan& span)
{
	span.text.clear();
	if (source == nullptr || textLength == 0)
		return false;
	span.text.reserve(textLength);
	UINT32 position = textPosition;
	UINT32 remaining = textLength;
	while (remaining != 0)
	{
		WCHAR const* block = nullptr;
		UINT32 available = 0;
		if (FAILED(source->GetTextAtPosition(position, &block, &available)) ||
			block == nullptr || available == 0)
			return false;
		UINT32 const take = (std::min)(available, remaining);
		span.text.insert(span.text.end(), block, block + take);
		position += take;
		remaining -= take;
	}
	return true;
}

bool NextCodePoint(
	std::vector<WCHAR> const& text,
	size_t& offset,
	UINT32& codePoint) noexcept
{
	if (offset >= text.size())
		return false;
	UINT32 const first = text[offset++];
	if (first >= 0xd800 && first <= 0xdbff && offset < text.size())
	{
		UINT32 const second = text[offset];
		if (second >= 0xdc00 && second <= 0xdfff)
		{
			++offset;
			codePoint = 0x10000 + ((first - 0xd800) << 10) + (second - 0xdc00);
			return true;
		}
	}
	codePoint = first;
	return true;
}

bool CoversSpan(IDWriteFont* font, TextSpan const& span)
{
	size_t offset = 0;
	UINT32 codePoint = 0;
	while (NextCodePoint(span.text, offset, codePoint))
	{
		BOOL exists = FALSE;
		if (FAILED(font->HasCharacter(codePoint, &exists)) || !exists)
			return false;
	}
	return true;
}

bool CoversSpan(IDWriteFontFace5* face, TextSpan const& span)
{
	CComPtr<IDWriteFontFace3> face3;
	if (face == nullptr || FAILED(face->QueryInterface(&face3)) || face3 == nullptr)
		return false;
	size_t offset = 0;
	UINT32 codePoint = 0;
	while (NextCodePoint(span.text, offset, codePoint))
	{
		if (!face3->HasCharacter(codePoint))
			return false;
	}
	return true;
}

bool FindAliasFont(
	IDWriteFontCollection* collection,
	std::wstring const& matchedName,
	DWRITE_FONT_WEIGHT weight,
	DWRITE_FONT_STRETCH stretch,
	DWRITE_FONT_STYLE style,
	CComPtr<IDWriteFont>& font)
{
	font.Release();
	UINT32 familyIndex = 0;
	BOOL exists = FALSE;
	if (collection == nullptr || FAILED(collection->FindFamilyName(
			matchedName.c_str(), &familyIndex, &exists)) || !exists)
		return false;
	CComPtr<IDWriteFontFamily> family;
	return SUCCEEDED(collection->GetFontFamily(familyIndex, &family)) &&
		family != nullptr && SUCCEEDED(family->GetFirstMatchingFont(
			weight, stretch, style, &font)) && font != nullptr;
}

bool ResolveAliasContext(
	IDWriteFontCollection* baseCollection,
	std::shared_ptr<const renderer::font_substitution::Snapshot>& snapshot,
	CComPtr<IDWriteFontCollection>& aliasCollection)
{
	snapshot = renderer::font_substitution::ProcessRegistry().Load();
	if (!snapshot || snapshot->rules().empty())
		return false;
	std::array<FactorySource, kFactorySourceLimit> sources;
	size_t const sourceCount = CopySources(sources);
	for (size_t index = 0; index < sourceCount; ++index)
	{
		FactorySource const& source = sources[index];
		if (source.factory == nullptr)
			continue;
		directwrite_alias::AliasFontSet aliases;
		directwrite_alias::BuildStatus const status =
			directwrite_alias::GetCachedForFactory(
				source.factory, snapshot->generation(), aliases);
		if (status != directwrite_alias::BuildStatus::applied &&
			status != directwrite_alias::BuildStatus::appliedWithMissingReplacement)
			continue;
		CComPtr<IDWriteFontCollection> candidate;
		if (FAILED(aliases.collection->QueryInterface(&candidate)) ||
			candidate == nullptr ||
			!IsAllowedCollection(baseCollection, source, candidate))
			continue;
		aliasCollection = candidate;
		return true;
	}
	return false;
}

bool TryMapAliasFont(
	IDWriteTextAnalysisSource* source,
	UINT32 textPosition,
	UINT32 mappedLength,
	IDWriteFontCollection* baseCollection,
	IDWriteFont* mappedFont,
	CComPtr<IDWriteFont>& aliasFont)
{
	std::shared_ptr<const renderer::font_substitution::Snapshot> snapshot;
	CComPtr<IDWriteFontCollection> aliasCollection;
	if (!ResolveAliasContext(
			baseCollection, snapshot, aliasCollection))
		return false;
	std::vector<std::wstring> names;
	directwrite_alias::FallbackAliasResolution resolution;
	TextSpan span;
	if (!ReadMappedFontNames(mappedFont, names) ||
		!directwrite_alias::ResolveFallbackAlias(names, *snapshot, resolution) ||
		!FindAliasFont(
			aliasCollection, resolution.matchedSourceName,
			mappedFont->GetWeight(), mappedFont->GetStretch(), mappedFont->GetStyle(),
			aliasFont) ||
		!ReadTextSpan(source, textPosition, mappedLength, span) ||
		!CoversSpan(aliasFont, span))
		return false;
	return true;
}

bool TryMapAliasFace(
	IDWriteTextAnalysisSource* source,
	UINT32 textPosition,
	UINT32 mappedLength,
	IDWriteFontCollection* baseCollection,
	IDWriteFontFace5* mappedFace,
	DWRITE_FONT_AXIS_VALUE const* axisValues,
	UINT32 axisValueCount,
	CComPtr<IDWriteFontFace5>& aliasFace)
{
	std::shared_ptr<const renderer::font_substitution::Snapshot> snapshot;
	CComPtr<IDWriteFontCollection> aliasCollection;
	if (!ResolveAliasContext(
			baseCollection, snapshot, aliasCollection))
		return false;
	std::vector<std::wstring> names;
	DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
	DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
	DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
	directwrite_alias::FallbackAliasResolution resolution;
	CComPtr<IDWriteFont> aliasFont;
	TextSpan span;
	if (!ReadMappedFaceNames(mappedFace, names, weight, stretch, style) ||
		!directwrite_alias::ResolveFallbackAlias(names, *snapshot, resolution) ||
		!ReadTextSpan(source, textPosition, mappedLength, span))
		return false;
	if (axisValueCount != 0 && axisValues == nullptr)
		return false;
	for (UINT32 index = 0; index < axisValueCount; ++index)
	{
		switch (axisValues[index].axisTag)
		{
		case DWRITE_FONT_AXIS_TAG_WEIGHT:
			weight = static_cast<DWRITE_FONT_WEIGHT>((std::max)(
				1.0F, (std::min)(1000.0F, axisValues[index].value)));
			break;
		case DWRITE_FONT_AXIS_TAG_WIDTH:
		{
			float const width = axisValues[index].value;
			if (width <= 56.25F)
				stretch = DWRITE_FONT_STRETCH_ULTRA_CONDENSED;
			else if (width <= 68.75F)
				stretch = DWRITE_FONT_STRETCH_EXTRA_CONDENSED;
			else if (width <= 81.25F)
				stretch = DWRITE_FONT_STRETCH_CONDENSED;
			else if (width <= 93.75F)
				stretch = DWRITE_FONT_STRETCH_SEMI_CONDENSED;
			else if (width <= 106.25F)
				stretch = DWRITE_FONT_STRETCH_NORMAL;
			else if (width <= 131.25F)
				stretch = DWRITE_FONT_STRETCH_SEMI_EXPANDED;
			else if (width <= 162.5F)
				stretch = DWRITE_FONT_STRETCH_EXPANDED;
			else if (width <= 187.5F)
				stretch = DWRITE_FONT_STRETCH_EXTRA_EXPANDED;
			else
				stretch = DWRITE_FONT_STRETCH_ULTRA_EXPANDED;
			break;
		}
		case DWRITE_FONT_AXIS_TAG_ITALIC:
			if (axisValues[index].value != 0.0F)
				style = DWRITE_FONT_STYLE_ITALIC;
			break;
		case DWRITE_FONT_AXIS_TAG_SLANT:
			if (axisValues[index].value != 0.0F &&
				style == DWRITE_FONT_STYLE_NORMAL)
				style = DWRITE_FONT_STYLE_OBLIQUE;
			break;
		default:
			break;
		}
	}
	if (!FindAliasFont(
			aliasCollection, resolution.matchedSourceName,
			weight, stretch, style, aliasFont) || !CoversSpan(aliasFont, span))
		return false;
	CComPtr<IDWriteFontFace> legacyFace;
	CComPtr<IDWriteFontFace5> baseAliasFace;
	CComPtr<IDWriteFontResource> resource;
	if (FAILED(aliasFont->CreateFontFace(&legacyFace)) || legacyFace == nullptr ||
		FAILED(legacyFace->QueryInterface(&baseAliasFace)) || baseAliasFace == nullptr)
		return false;
	if (axisValueCount == 0)
	{
		aliasFace = baseAliasFace;
		return CoversSpan(aliasFace, span);
	}
	if (axisValues == nullptr || FAILED(baseAliasFace->GetFontResource(&resource)) ||
		resource == nullptr || FAILED(resource->CreateFontFace(
			baseAliasFace->GetSimulations(), axisValues, axisValueCount, &aliasFace)) ||
		aliasFace == nullptr)
		return false;
	return CoversSpan(aliasFace, span);
}

HRESULT WINAPI IMPL_Fallback_MapCharacters(
	IDWriteFontFallback* self,
	IDWriteTextAnalysisSource* source,
	UINT32 textPosition,
	UINT32 textLength,
	IDWriteFontCollection* baseFontCollection,
	WCHAR const* baseFamilyName,
	DWRITE_FONT_WEIGHT baseWeight,
	DWRITE_FONT_STYLE baseStyle,
	DWRITE_FONT_STRETCH baseStretch,
	UINT32* mappedLength,
	IDWriteFont** mappedFont,
	FLOAT* scale)
{
	HCounter counter;
	FallbackMapCharactersMethod const original = OriginalMapCharacters(self);
	if (original == nullptr)
		return E_UNEXPECTED;
	HRESULT const result = original(
		self, source, textPosition, textLength, baseFontCollection, baseFamilyName,
		baseWeight, baseStyle, baseStretch, mappedLength, mappedFont, scale);
	if (FAILED(result) || mappedLength == nullptr || *mappedLength == 0 ||
		mappedFont == nullptr || *mappedFont == nullptr)
		return result;
	try
	{
		CComPtr<IDWriteFont> replacement;
		if (TryMapAliasFont(
				source, textPosition, *mappedLength, baseFontCollection,
				*mappedFont, replacement))
		{
			(*mappedFont)->Release();
			*mappedFont = replacement.Detach();
		}
	}
	catch (...)
	{
	}
	return result;
}

HRESULT WINAPI IMPL_Fallback1_MapCharacters(
	IDWriteFontFallback1* self,
	IDWriteTextAnalysisSource* source,
	UINT32 textPosition,
	UINT32 textLength,
	IDWriteFontCollection* baseFontCollection,
	WCHAR const* baseFamilyName,
	DWRITE_FONT_AXIS_VALUE const* fontAxisValues,
	UINT32 fontAxisValueCount,
	UINT32* mappedLength,
	FLOAT* scale,
	IDWriteFontFace5** mappedFontFace)
{
	HCounter counter;
	Fallback1MapCharactersMethod const original = OriginalMapCharacters1(self);
	if (original == nullptr)
		return E_UNEXPECTED;
	HRESULT const result = original(
		self, source, textPosition, textLength, baseFontCollection, baseFamilyName,
		fontAxisValues, fontAxisValueCount, mappedLength, scale, mappedFontFace);
	if (FAILED(result) || mappedLength == nullptr || *mappedLength == 0 ||
		mappedFontFace == nullptr || *mappedFontFace == nullptr)
		return result;
	try
	{
		CComPtr<IDWriteFontFace5> replacement;
		if (TryMapAliasFace(
				source, textPosition, *mappedLength, baseFontCollection,
				*mappedFontFace, fontAxisValues, fontAxisValueCount, replacement))
		{
			(*mappedFontFace)->Release();
			*mappedFontFace = replacement.Detach();
		}
	}
	catch (...)
	{
	}
	return result;
}

} // namespace

bool HookDirectWriteSystemFallback(
	IDWriteFactory* factory,
	IDWriteFontCollection* systemCollection) noexcept
{
	try
	{
		if (factory == nullptr || systemCollection == nullptr)
			return false;
		CComPtr<IDWriteFactory2> factory2;
		CComPtr<IDWriteFontFallback> fallback;
		if (FAILED(factory->QueryInterface(&factory2)) || factory2 == nullptr ||
			FAILED(factory2->GetSystemFontFallback(&fallback)) || fallback == nullptr)
			return false;
		CComPtr<IDWriteFontFallback1> fallback1;
		if (FAILED(fallback->QueryInterface(&fallback1)) || fallback1 == nullptr)
			return false;
		void** const fallbackVtable = *reinterpret_cast<void***>(fallback.p);
		void** const fallback1Vtable = *reinterpret_cast<void***>(fallback1.p);
		if (fallbackVtable == nullptr || fallback1Vtable == nullptr)
			return false;

		renderer::HookCoordinator& coordinator = renderer::ProcessHookCoordinator();
		HookAttemptGuard admission(
			coordinator,
			coordinator.BeginAttempt(
				renderer::HookCapability::directWrite,
				reinterpret_cast<std::uintptr_t>(fallbackVtable), true));
		bool patched = false;
		if (!PublishSource(factory, systemCollection))
		{
			admission.Complete(false, ERROR_INVALID_DATA);
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(RegistryMutex());
			FallbackVtableHooks& fallbackHooks = GetOrAddVtable(fallbackVtable);
			bool const baseWasPatched = fallbackHooks.mapCharactersPatched;
			patched = PatchMethod(
				fallbackHooks, kFallbackMapCharactersSlot,
				&IMPL_Fallback_MapCharacters, fallbackHooks.mapCharacters,
				fallbackHooks.mapCharactersPatched);
			if (patched)
			{
				FallbackVtableHooks& fallback1Hooks = GetOrAddVtable(fallback1Vtable);
				bool const modernWasPatched = fallback1Hooks.mapCharacters1Patched;
				patched = PatchMethod(
					fallback1Hooks, kFallback1MapCharactersSlot,
					&IMPL_Fallback1_MapCharacters, fallback1Hooks.mapCharacters1,
					fallback1Hooks.mapCharacters1Patched);
				if (!patched && !modernWasPatched)
					RestoreMethod(
						fallback1Hooks, kFallback1MapCharactersSlot,
						&IMPL_Fallback1_MapCharacters, fallback1Hooks.mapCharacters1,
						fallback1Hooks.mapCharacters1Patched);
			}
			if (!patched && !baseWasPatched)
				RestoreMethod(
					fallbackHooks, kFallbackMapCharactersSlot,
					&IMPL_Fallback_MapCharacters, fallbackHooks.mapCharacters,
					fallbackHooks.mapCharactersPatched);
		}
		admission.Complete(
			patched, patched ? ERROR_SUCCESS : ERROR_HOOK_NOT_INSTALLED);
		return patched;
	}
	catch (...)
	{
		return false;
	}
}

void RestoreDirectWriteFallbackVtableHooks() noexcept
{
	std::lock_guard<std::mutex> lock(RegistryMutex());
	for (FallbackVtableHooks& hooks : VtableRegistry())
	{
		RestoreMethod(
			hooks, kFallback1MapCharactersSlot,
			&IMPL_Fallback1_MapCharacters, hooks.mapCharacters1,
			hooks.mapCharacters1Patched);
		RestoreMethod(
			hooks, kFallbackMapCharactersSlot,
			&IMPL_Fallback_MapCharacters, hooks.mapCharacters,
			hooks.mapCharactersPatched);
	}
}

void ClearDirectWriteFallbackSources() noexcept
{
	std::vector<FactorySource> sources;
	{
		std::lock_guard<std::mutex> lock(RegistryMutex());
		sources.swap(SourceRegistry());
	}
	sources.clear();
}
