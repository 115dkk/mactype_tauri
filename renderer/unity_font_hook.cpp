#include "unity_font_hook.h"

#include "dll.h"
#include "generated_unity_anticheat_catalog.h"
#include "hookCounter.h"
#include "hook_lifecycle.h"
#include "profile_runtime.h"
#include "renderer_raii.h"
#include "settings.h"
#include "unity_font_catalog.h"
#include "unity_font_evidence.h"
#include "unity_font_hook_core.h"
#include "unity_font_selection_context.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef USE_DETOURS
#include "detour_transaction.h"
#include <freetype/freetype.h>
#endif

extern LONG g_bHookEnabled;

namespace {

enum class UnityLifecyclePhase : unsigned char
{
	dormant,
	starting,
	active,
	failed,
	stopping,
	stopped,
};

#ifdef USE_DETOURS
using PublicRenderFunction = FT_Error (*)(FT_GlyphSlot, FT_Render_Mode);
using InternalRenderFunction = FT_Error (*)(
	FT_Library, FT_GlyphSlot, FT_Render_Mode, const FT_Vector*);
using FaceOpenFunction = FT_Error (__fastcall *)(
	FT_Library, const FT_Open_Args*, FT_Long, FT_Face*, FT_Bool);
using TextCoreFontLoadFunction = int (*)(const char*, int, int);
using LegacyCharacterLookupFunction = FT_Face (*)(
	void*, const void*, const void*, unsigned int);
using LegacyOsFaceResolverFunction = FT_Face (*)(void*, const void*);
using FreeTypeCharIndexFunction = FT_UInt (*)(FT_Face, FT_ULong);
using FontCatalogLoadFunction = void (*)(void*);
#else
using PublicRenderFunction = void*;
using InternalRenderFunction = void*;
using FaceOpenFunction = void*;
using TextCoreFontLoadFunction = void*;
using LegacyCharacterLookupFunction = void*;
using LegacyOsFaceResolverFunction = void*;
using FreeTypeCharIndexFunction = void*;
using FontCatalogLoadFunction = void*;
#endif

struct UnityLifecycleState final
{
	renderer_raii::UniqueModuleReference unityPlayer;
	renderer_raii::UniqueHandle startupWorker;
	std::mutex mutex;
	std::atomic<bool> stopping{false};
	std::atomic<bool> operational{false};
	UnityLifecyclePhase phase = UnityLifecyclePhase::dormant;
	UnityLifecyclePhase phaseBeforeStop = UnityLifecyclePhase::dormant;
	renderer::unity::RenderAbi abi = renderer::unity::RenderAbi::publicRender;
	PublicRenderFunction publicRender = nullptr;
	InternalRenderFunction internalRender = nullptr;
	FaceOpenFunction faceOpen = nullptr;
	TextCoreFontLoadFunction fontLoad = nullptr;
	LegacyCharacterLookupFunction characterLookup = nullptr;
	LegacyOsFaceResolverFunction osFaceResolver = nullptr;
	FreeTypeCharIndexFunction freeTypeCharIndex = nullptr;
	FontCatalogLoadFunction fontCatalogLoad = nullptr;
	std::shared_ptr<const renderer::unity::FontFileRedirectTable> redirects;
	renderer_raii::UniqueHandle evidenceMapping;
	renderer_raii::UniqueMappedView evidenceView;
	bool renderAttached = false;
	bool faceOpenAttached = false;
	bool fontLoadAttached = false;
	bool faceOpenFallbackRequired = false;
	bool characterLookupAttached = false;
	bool osFaceResolverAttached = false;
	bool freeTypeCharIndexAttached = false;
	bool fontCatalogLoadAttached = false;
	bool fontCatalogLoadRequired = false;
	bool osFaceResolverRequired = false;
	renderer::unity::FontSubstitutionBoundary substitutionBoundary =
		renderer::unity::FontSubstitutionBoundary::unavailable;
	bool startupSucceeded = false;
};

UnityLifecycleState& ProcessUnityLifecycle()
{
	// Explicit unload drains the contained references outside the loader lock.
	// Process termination leaves the tiny state allocation to the OS.
	static UnityLifecycleState* state = new UnityLifecycleState;
	return *state;
}

bool IsSubstitutionBoundaryOperational(
	const UnityLifecycleState& lifecycle) noexcept
{
	switch (lifecycle.substitutionBoundary)
	{
	case renderer::unity::FontSubstitutionBoundary::textCoreFontLoad:
		return lifecycle.fontLoadAttached &&
			(!lifecycle.faceOpenFallbackRequired || lifecycle.faceOpenAttached) &&
			(!lifecycle.fontCatalogLoadRequired ||
				lifecycle.fontCatalogLoadAttached) &&
			(!lifecycle.osFaceResolverRequired ||
				lifecycle.osFaceResolverAttached);
	case renderer::unity::FontSubstitutionBoundary::freeTypeFaceOpen:
		return lifecycle.faceOpenAttached &&
			(!lifecycle.fontCatalogLoadRequired ||
				lifecycle.fontCatalogLoadAttached) &&
			(!lifecycle.osFaceResolverRequired ||
				lifecycle.osFaceResolverAttached);
	case renderer::unity::FontSubstitutionBoundary::unavailable:
		return true;
	}
	return false;
}

renderer::unity::UnityFontEvidenceV1* UnityEvidence() noexcept
{
	return static_cast<renderer::unity::UnityFontEvidenceV1*>(
		ProcessUnityLifecycle().evidenceView.get());
}

void InitializeUnityEvidenceIfRequested(UnityLifecycleState& lifecycle) noexcept
{
	wchar_t enabled[2]{};
	if (GetEnvironmentVariableW(
		L"MACTYPE_UNITY_EVIDENCE", enabled, _countof(enabled)) != 1 ||
		enabled[0] != L'1')
		return;
	wchar_t mappingName[96]{};
	if (!renderer::unity::FormatUnityFontEvidenceMappingName(
		GetCurrentProcessId(), mappingName, _countof(mappingName)))
		return;
	renderer_raii::UniqueHandle mapping(CreateFileMappingW(
		INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
		static_cast<DWORD>(sizeof(renderer::unity::UnityFontEvidenceV1)),
		mappingName));
	if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS)
		return;
	renderer_raii::UniqueMappedView view(MapViewOfFile(
		mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
		sizeof(renderer::unity::UnityFontEvidenceV1)));
	if (!view)
		return;
	renderer::unity::InitializeUnityFontEvidence(
		*static_cast<renderer::unity::UnityFontEvidenceV1*>(view.get()),
		GetCurrentProcessId());
	lifecycle.evidenceMapping = std::move(mapping);
	lifecycle.evidenceView = std::move(view);
}

void PublishUnityCapability(
	bool succeeded,
	renderer::CapabilityReason reason,
	LONG status,
	std::uintptr_t target,
	bool modulePresent) noexcept
{
	renderer::HookCoordinator& coordinator = renderer::ProcessHookCoordinator();
	renderer::HookAttempt const attempt = coordinator.BeginAttempt(
		renderer::HookCapability::unityFont, target, modulePresent);
	if (attempt.valid())
		coordinator.CompleteAttempt(attempt, succeeded, reason, status);
}

enum class AntiCheatInspection : unsigned char
{
	clear,
	detected,
	unavailable,
};

bool MatchesAntiCheatName(std::wstring name)
{
	std::transform(name.begin(), name.end(), name.begin(), [](wchar_t value) {
		return static_cast<wchar_t>(std::towlower(value));
	});
	for (const wchar_t* const exact : renderer::unity::kAntiCheatTopLevelExact)
	{
		if (name == exact)
			return true;
	}
	for (const wchar_t* const prefixValue : renderer::unity::kAntiCheatTopLevelPrefixes)
	{
		std::wstring const prefix(prefixValue);
		if (name.size() >= prefix.size() &&
			name.compare(0, prefix.size(), prefix.data(), prefix.size()) == 0)
			return true;
	}
	return false;
}

AntiCheatInspection InspectCurrentInstallationForAntiCheat()
{
	try
	{
		std::wstring path(32768, L'\0');
		DWORD const length = GetModuleFileNameW(
			nullptr, &path[0], static_cast<DWORD>(path.size()));
		if (length == 0 || length >= path.size())
			return AntiCheatInspection::unavailable;
		path.resize(length);
		std::wstring::size_type const slash = path.find_last_of(L"\\/");
		if (slash == std::wstring::npos)
			return AntiCheatInspection::unavailable;
		path.resize(slash + 1);
		std::wstring const search = path + L"*";
		WIN32_FIND_DATAW entry{};
		auto find = renderer_raii::AdoptFindHandle(FindFirstFileW(search.c_str(), &entry));
		if (!find)
			return AntiCheatInspection::unavailable;
		for (unsigned int count = 0;; ++count)
		{
			if (count >= 4096)
				return AntiCheatInspection::unavailable;
			if (wcscmp(entry.cFileName, L".") != 0 &&
				wcscmp(entry.cFileName, L"..") != 0 &&
				MatchesAntiCheatName(entry.cFileName))
				return AntiCheatInspection::detected;
			if (!FindNextFileW(find.get(), &entry))
			{
				return GetLastError() == ERROR_NO_MORE_FILES
					? AntiCheatInspection::clear
					: AntiCheatInspection::unavailable;
			}
		}
	}
	catch (...)
	{
		return AntiCheatInspection::unavailable;
	}
	return AntiCheatInspection::unavailable;
}

#ifdef USE_DETOURS

thread_local bool g_skipUnityFaceOpenRedirect = false;

class ScopedFaceOpenRedirectBypass final
{
public:
	ScopedFaceOpenRedirectBypass() noexcept
		: previous_(g_skipUnityFaceOpenRedirect)
	{
		g_skipUnityFaceOpenRedirect = true;
	}

	~ScopedFaceOpenRedirectBypass()
	{
		g_skipUnityFaceOpenRedirect = previous_;
	}

	ScopedFaceOpenRedirectBypass(const ScopedFaceOpenRedirectBypass&) = delete;
	ScopedFaceOpenRedirectBypass& operator=(
		const ScopedFaceOpenRedirectBypass&) = delete;

private:
	bool previous_;
};

void HookFontCatalogLoad(void* entry) noexcept
{
	CThreadCounter callbackLease;
	FontCatalogLoadFunction const original =
		ProcessUnityLifecycle().fontCatalogLoad;
	if (original == nullptr)
		return;
	// Unity 2019 opens every installed font while building its private
	// family catalog. Redirecting those discovery opens replaces the
	// catalog identity itself (for example, Malgun becomes Pretendard), so
	// the later fallback search can no longer select the configured source.
	// Keep discovery native; pathname substitution resumes when Unity opens
	// the face it selected for actual text.
	ScopedFaceOpenRedirectBypass bypass;
	original(entry);
}

void ReadUnityFaceFamilyName(FT_Face face, std::wstring& family) noexcept
{
	family.clear();
	if (face == nullptr || face->family_name == nullptr)
		return;
	try
	{
		constexpr std::size_t kMaximumFamilyBytes = 255;
		std::size_t const length =
			strnlen_s(face->family_name, kMaximumFamilyBytes);
		if (length == 0 || length == kMaximumFamilyBytes ||
			length > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
			return;
		int const required = MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, face->family_name,
			static_cast<int>(length), nullptr, 0);
		if (required <= 0)
			return;
		family.resize(static_cast<std::size_t>(required));
		if (MultiByteToWideChar(
				CP_UTF8, MB_ERR_INVALID_CHARS, face->family_name,
				static_cast<int>(length), &family[0], required) != required)
			family.clear();
	}
	catch (...)
	{
		family.clear();
	}
}

FT_Error __fastcall HookFaceOpen(
	FT_Library library,
	const FT_Open_Args* arguments,
	FT_Long faceIndex,
	FT_Face* face,
	FT_Bool testMacFonts) noexcept
{
	CThreadCounter callbackLease;
	FaceOpenFunction const original = ProcessUnityLifecycle().faceOpen;
	if (original == nullptr)
		return 0x06; // FT_Err_Invalid_Argument

	try
	{
		auto const redirects = std::atomic_load_explicit(
			&ProcessUnityLifecycle().redirects, std::memory_order_acquire);
		renderer::unity::FaceOpenPathRedirect redirect;
		if (!g_skipUnityFaceOpenRedirect &&
			renderer::unity::CurrentFontRefAllowsFaceRedirect() &&
			arguments != nullptr && redirects &&
			InterlockedCompareExchange(&g_bHookEnabled, 0, 0) != FALSE &&
			ProcessUnityLifecycle().operational.load(
				std::memory_order_acquire) &&
			renderer::unity::ResolveFaceOpenPath(
				*redirects, arguments->flags, arguments->pathname,
				faceIndex, redirect))
		{
			FT_Open_Args redirectedArguments = *arguments;
			redirectedArguments.pathname = const_cast<FT_String*>(
				redirect.replacementUtf8.c_str());
			renderer::unity::UnityFontEvidenceV1* const evidence =
				UnityEvidence();
			if (evidence != nullptr)
				renderer::unity::RecordUnityFontFileOpen(
					*evidence, redirect.sourcePath.c_str());
			FT_Error const redirectedResult = original(
				library, &redirectedArguments,
				static_cast<FT_Long>(redirect.replacementFaceIndex),
				face, testMacFonts);
			bool const redirected = redirectedResult == 0 &&
				face != nullptr && *face != nullptr;
			if (evidence != nullptr)
			{
				renderer::unity::RecordUnityFontRedirect(
					*evidence, redirect.sourcePath.c_str(),
					redirect.replacementPath.c_str(), redirected);
				if (redirected)
				{
					FT_Face const redirectedFace = *face;
					renderer::unity::RecordUnityFontFaceDetails(
						*evidence,
						redirectedFace->charmap != nullptr,
						static_cast<LONG>(redirectedFace->num_glyphs),
						FT_Get_Char_Index(redirectedFace, 0xC124));
				}
			}
			if (redirected || face == nullptr || *face != nullptr)
				return redirectedResult;
		}
	}
	catch (...)
	{
	}
	return original(library, arguments, faceIndex, face, testMacFonts);
}

int HookTextCoreFontLoad(
	const char* requestedPath,
	int pointSize,
	int faceIndex) noexcept
{
	CThreadCounter callbackLease;
	TextCoreFontLoadFunction const original = ProcessUnityLifecycle().fontLoad;
	if (original == nullptr)
		return 0x06; // FT_Err_Invalid_Argument

	try
	{
		auto const redirects = std::atomic_load_explicit(
			&ProcessUnityLifecycle().redirects, std::memory_order_acquire);
		renderer::unity::FaceOpenPathRedirect redirect;
		if (redirects &&
			InterlockedCompareExchange(&g_bHookEnabled, 0, 0) != FALSE &&
			ProcessUnityLifecycle().operational.load(
				std::memory_order_acquire) &&
			renderer::unity::ResolveTextCoreFontLoadPath(
				*redirects, requestedPath, faceIndex, redirect))
		{
			renderer::unity::UnityFontEvidenceV1* const evidence =
				UnityEvidence();
			if (evidence != nullptr)
				renderer::unity::RecordUnityFontFileOpen(
					*evidence, redirect.sourcePath.c_str());
			int const redirectedResult = original(
				redirect.replacementUtf8.c_str(), pointSize,
				static_cast<int>(redirect.replacementFaceIndex));
			bool const redirected = redirectedResult == 0;
			if (evidence != nullptr)
				renderer::unity::RecordUnityFontRedirect(
					*evidence, redirect.sourcePath.c_str(),
					redirect.replacementPath.c_str(), redirected);
			if (redirected)
				return redirectedResult;
		}
	}
	catch (...)
	{
	}
	ScopedFaceOpenRedirectBypass bypass;
	return original(requestedPath, pointSize, faceIndex);
}

FT_Face HookLegacyCharacterLookup(
	void* dynamicFontData,
	const void* fontRef,
	const void* fallbackFonts,
	unsigned int character) noexcept
{
	CThreadCounter callbackLease;
	LegacyCharacterLookupFunction const original =
		ProcessUnityLifecycle().characterLookup;
	if (original == nullptr)
		return nullptr;
	FT_Face face = original(
		dynamicFontData, fontRef, fallbackFonts, character);
	renderer::unity::UnityFontEvidenceV1* const evidence = UnityEvidence();
	std::wstring requestedFamily;
	bool mappedFamily = false;
	std::wstring resolvedFaceFamily;
	if (evidence != nullptr)
	{
		renderer::unity::ReadLegacyFontRefFamily(fontRef, requestedFamily);
		auto const redirects = std::atomic_load_explicit(
			&ProcessUnityLifecycle().redirects, std::memory_order_acquire);
		std::wstring replacementPath;
		long replacementFaceIndex = 0;
		mappedFamily = redirects && !requestedFamily.empty() &&
			redirects->ResolveFamilyFace(
				requestedFamily.c_str(), replacementPath,
				replacementFaceIndex);
	}
	if (evidence != nullptr)
	{
		unsigned int const glyphIndex = face != nullptr
			? FT_Get_Char_Index(face, character)
			: 0;
		unsigned int const sampleKoreanGlyph = face != nullptr
			? FT_Get_Char_Index(face, 0xC124)
			: 0;
		ReadUnityFaceFamilyName(face, resolvedFaceFamily);
		renderer::unity::RecordUnityFontFaceResolution(
			*evidence, face != nullptr, glyphIndex, sampleKoreanGlyph);
		renderer::unity::RecordUnityFontCharacterLookup(
			*evidence,
			requestedFamily.empty() ? nullptr : requestedFamily.c_str(),
			mappedFamily, character, face != nullptr,
			glyphIndex, sampleKoreanGlyph,
			resolvedFaceFamily.empty() ? nullptr : resolvedFaceFamily.c_str());
	}
	return face;
}

FT_Face HookLegacyOsFaceResolver(
	void* dynamicFontData,
	const void* fontRef) noexcept
{
	CThreadCounter callbackLease;
	LegacyOsFaceResolverFunction const original =
		ProcessUnityLifecycle().osFaceResolver;
	if (original == nullptr)
		return nullptr;
	std::wstring requestedFamily;
	renderer::unity::ReadLegacyFontRefFamily(fontRef, requestedFamily);
	bool mappedFamily = false;
	if (!requestedFamily.empty())
	{
		auto const redirects = std::atomic_load_explicit(
			&ProcessUnityLifecycle().redirects, std::memory_order_acquire);
		std::wstring replacementPath;
		long replacementFaceIndex = 0;
		mappedFamily = redirects && redirects->ResolveFamilyFace(
			requestedFamily.c_str(), replacementPath,
			replacementFaceIndex);
	}
	renderer::unity::ScopedFontRefSelectionContext familyContext(
		mappedFamily
			? renderer::unity::FontRefSelection::mappedFamily
			: renderer::unity::FontRefSelection::nativeFamily);
	FT_Face const face = original(dynamicFontData, fontRef);
	renderer::unity::UnityFontEvidenceV1* const evidence = UnityEvidence();
	if (evidence != nullptr)
	{
		std::wstring resolvedFamily;
		ReadUnityFaceFamilyName(face, resolvedFamily);
		unsigned int const sampleKoreanGlyph = face != nullptr
			? FT_Get_Char_Index(face, 0xC124)
			: 0;
		renderer::unity::RecordUnityFontOsFaceResolution(
			*evidence,
			requestedFamily.empty() ? nullptr : requestedFamily.c_str(),
			mappedFamily,
			face != nullptr,
			sampleKoreanGlyph,
			resolvedFamily.empty() ? nullptr : resolvedFamily.c_str());
	}
	return face;
}

bool IsKoreanCharacter(FT_ULong character) noexcept
{
	return (character >= 0x1100 && character <= 0x11FF) ||
		(character >= 0x3130 && character <= 0x318F) ||
		(character >= 0xA960 && character <= 0xA97F) ||
		(character >= 0xAC00 && character <= 0xD7AF) ||
		(character >= 0xD7B0 && character <= 0xD7FF);
}

FT_UInt HookFreeTypeCharIndex(FT_Face face, FT_ULong character) noexcept
{
	CThreadCounter callbackLease;
	FreeTypeCharIndexFunction const original =
		ProcessUnityLifecycle().freeTypeCharIndex;
	if (original == nullptr)
		return 0;
	FT_UInt const glyphIndex = original(face, character);
	if (IsKoreanCharacter(character))
	{
		renderer::unity::UnityFontEvidenceV1* const evidence = UnityEvidence();
		if (evidence != nullptr)
		{
			std::wstring family;
			ReadUnityFaceFamilyName(face, family);
			renderer::unity::RecordUnityFontCharacterLookup(
				*evidence,
				family.empty() ? nullptr : family.c_str(),
				false,
				static_cast<unsigned int>(character),
				glyphIndex != 0,
				glyphIndex,
				0,
				family.empty() ? nullptr : family.c_str());
		}
	}
	return glyphIndex;
}

bool IsSupportedRenderMode(FT_Render_Mode mode) noexcept
{
	return mode == FT_RENDER_MODE_NORMAL || mode == FT_RENDER_MODE_LIGHT ||
		mode == FT_RENDER_MODE_LCD || mode == FT_RENDER_MODE_LCD_V;
}

void ApplyProfileCoverage(FT_GlyphSlot slot, FT_Render_Mode mode) noexcept
{
	if (slot == nullptr || !IsSupportedRenderMode(mode) ||
		slot->format != FT_GLYPH_FORMAT_BITMAP ||
		InterlockedCompareExchange(&g_bHookEnabled, 0, 0) == FALSE ||
		!ProcessUnityLifecycle().operational.load(std::memory_order_acquire))
		return;
	try
	{
		renderer::RendererPolicyRef const policy = renderer::CurrentRendererPolicy();
		if (!policy || !policy->hooks().unityFontEnabledForProcess)
			return;
		FT_Bitmap& bitmap = slot->bitmap;
		renderer::unity::ApplyCoverage(
			bitmap.buffer,
			bitmap.pitch,
			bitmap.rows,
			bitmap.width,
			bitmap.pixel_mode,
			policy->unity_coverage());
	}
	catch (...)
	{
	}
}

void RecordRenderEvidence(FT_Error result, FT_GlyphSlot slot) noexcept
{
	renderer::unity::UnityFontEvidenceV1* const evidence = UnityEvidence();
	if (evidence == nullptr)
		return;
	renderer::unity::RecordUnityFontRender(
		*evidence,
		static_cast<LONG>(result),
		slot != nullptr ? slot->glyph_index : 0,
		slot != nullptr ? slot->bitmap.width : 0,
		slot != nullptr ? slot->bitmap.rows : 0);
}

FT_Error HookPublicRender(FT_GlyphSlot slot, FT_Render_Mode mode) noexcept
{
	CThreadCounter callbackLease;
	PublicRenderFunction const original = ProcessUnityLifecycle().publicRender;
	if (original == nullptr)
		return 0x06; // FT_Err_Invalid_Argument
	FT_Error const result = original(slot, mode);
	RecordRenderEvidence(result, slot);
	if (result == 0)
		ApplyProfileCoverage(slot, mode);
	return result;
}

FT_Error __fastcall HookInternalRender(
	FT_Library library,
	FT_GlyphSlot slot,
	FT_Render_Mode mode,
	const FT_Vector* origin) noexcept
{
	CThreadCounter callbackLease;
	InternalRenderFunction const original = ProcessUnityLifecycle().internalRender;
	if (original == nullptr)
		return 0x06; // FT_Err_Invalid_Argument
	FT_Error const result = original(library, slot, mode, origin);
	RecordRenderEvidence(result, slot);
	if (result == 0)
		ApplyProfileCoverage(slot, mode);
	return result;
}

bool AttachUnityRenderer(
	UnityLifecycleState& lifecycle,
	renderer_raii::UniqueModuleReference module,
	const renderer::unity::ResolvedAdapter& adapter,
	std::shared_ptr<const renderer::unity::FontFileRedirectTable> redirects) noexcept
{
	std::uintptr_t const base = reinterpret_cast<std::uintptr_t>(module.get());
	if (adapter.targetRva > (std::numeric_limits<std::uintptr_t>::max)() - base)
		return false;
	void* const target = reinterpret_cast<void*>(base + adapter.targetRva);
	if (!renderer::unity::IsExecutableMemoryRange(target, 32))
		return false;
	bool const substitutionRequired = redirects && !redirects->empty();
	renderer::unity::FontSubstitutionBoundary const substitutionBoundary =
		substitutionRequired
			? renderer::unity::SelectFontSubstitutionBoundary(adapter)
			: renderer::unity::FontSubstitutionBoundary::unavailable;
	void* faceOpenTarget = nullptr;
	void* fontLoadTarget = nullptr;
	void* characterLookupTarget = nullptr;
	void* osFaceResolverTarget = nullptr;
	void* freeTypeCharIndexTarget = nullptr;
	void* fontCatalogLoadTarget = nullptr;
	bool faceOpenFallbackRequired = false;
	bool const characterLookupRequired =
		UnityEvidence() != nullptr && adapter.characterLookupRva != 0 &&
		 adapter.characterLookupAbi ==
			renderer::unity::CharacterLookupAbi::legacyDynamicFont;
	bool const osFaceResolverRequired = substitutionRequired &&
		adapter.osFaceResolverRva != 0 &&
		adapter.osFaceResolverAbi ==
			renderer::unity::CharacterLookupAbi::legacyDynamicFont;
	if (substitutionRequired)
	{
		if (substitutionBoundary ==
			renderer::unity::FontSubstitutionBoundary::textCoreFontLoad)
		{
			if (adapter.fontLoadRva == 0 || adapter.fontLoadRva >
					(std::numeric_limits<std::uintptr_t>::max)() - base)
				return false;
			fontLoadTarget = reinterpret_cast<void*>(base + adapter.fontLoadRva);
			faceOpenFallbackRequired = adapter.faceOpenRva != 0 &&
				adapter.faceOpenAbi == renderer::unity::FaceOpenAbi::unityInternal;
			if (faceOpenFallbackRequired)
			{
				if (adapter.faceOpenRva >
						(std::numeric_limits<std::uintptr_t>::max)() - base)
					return false;
				faceOpenTarget =
					reinterpret_cast<void*>(base + adapter.faceOpenRva);
			}
		}
		else if (substitutionBoundary ==
			renderer::unity::FontSubstitutionBoundary::freeTypeFaceOpen)
		{
			if (adapter.faceOpenRva == 0 || adapter.faceOpenRva >
					(std::numeric_limits<std::uintptr_t>::max)() - base)
				return false;
			faceOpenTarget =
				reinterpret_cast<void*>(base + adapter.faceOpenRva);
		}
		else
			return false;
		if ((fontLoadTarget != nullptr &&
			 !renderer::unity::IsExecutableMemoryRange(fontLoadTarget, 32)) ||
			(faceOpenTarget != nullptr &&
			 !renderer::unity::IsExecutableMemoryRange(faceOpenTarget, 32)))
			return false;
	}
	if (characterLookupRequired)
	{
		if (adapter.characterLookupRva >
				(std::numeric_limits<std::uintptr_t>::max)() - base)
			return false;
		characterLookupTarget =
			reinterpret_cast<void*>(base + adapter.characterLookupRva);
		if (!renderer::unity::IsExecutableMemoryRange(characterLookupTarget, 32))
			return false;
	}
	if ((osFaceResolverRequired || UnityEvidence() != nullptr) &&
		adapter.osFaceResolverRva != 0 &&
		adapter.osFaceResolverAbi ==
			renderer::unity::CharacterLookupAbi::legacyDynamicFont)
	{
		if (adapter.osFaceResolverRva >
				(std::numeric_limits<std::uintptr_t>::max)() - base)
			return false;
		osFaceResolverTarget =
			reinterpret_cast<void*>(base + adapter.osFaceResolverRva);
		if (!renderer::unity::IsExecutableMemoryRange(osFaceResolverTarget, 32))
			return false;
		lifecycle.osFaceResolver =
			reinterpret_cast<LegacyOsFaceResolverFunction>(osFaceResolverTarget);
	}
	lifecycle.osFaceResolverRequired = osFaceResolverRequired;
	bool const fontCatalogLoadRequired = substitutionRequired &&
		adapter.fontCatalogLoadRva != 0 &&
		adapter.fontCatalogLoadAbi ==
			renderer::unity::FontCatalogLoadAbi::systemCatalogEntry;
	if (fontCatalogLoadRequired)
	{
		if (adapter.fontCatalogLoadRva >
				(std::numeric_limits<std::uintptr_t>::max)() - base)
			return false;
		fontCatalogLoadTarget = reinterpret_cast<void*>(
			base + adapter.fontCatalogLoadRva);
		if (!renderer::unity::IsExecutableMemoryRange(
			fontCatalogLoadTarget, 32))
			return false;
	}
	lifecycle.fontCatalogLoadRequired = fontCatalogLoadRequired;
	if (UnityEvidence() != nullptr &&
		adapter.freeTypeCharIndexRva != 0 &&
		adapter.freeTypeCharIndexAbi ==
			renderer::unity::FreeTypeCharIndexAbi::standard)
	{
		if (adapter.freeTypeCharIndexRva >
				(std::numeric_limits<std::uintptr_t>::max)() - base)
			return false;
		freeTypeCharIndexTarget = reinterpret_cast<void*>(
			base + adapter.freeTypeCharIndexRva);
		if (!renderer::unity::IsExecutableMemoryRange(
			freeTypeCharIndexTarget, 32))
			return false;
	}

	renderer_raii::DetourTransaction transaction;
	LONG status = transaction.status();
	if (status == NOERROR)
	{
		if (adapter.abi == renderer::unity::RenderAbi::publicRender)
		{
			lifecycle.publicRender = reinterpret_cast<PublicRenderFunction>(target);
			status = transaction.Attach(
				reinterpret_cast<PVOID*>(&lifecycle.publicRender),
				reinterpret_cast<PVOID>(&HookPublicRender));
		}
		else
		{
			lifecycle.internalRender = reinterpret_cast<InternalRenderFunction>(target);
			status = transaction.Attach(
				reinterpret_cast<PVOID*>(&lifecycle.internalRender),
				reinterpret_cast<PVOID>(&HookInternalRender));
		}
	}
	if (status == NOERROR && substitutionRequired)
	{
		if (substitutionBoundary ==
			renderer::unity::FontSubstitutionBoundary::textCoreFontLoad)
		{
			lifecycle.fontLoad =
				reinterpret_cast<TextCoreFontLoadFunction>(fontLoadTarget);
			status = transaction.Attach(
				reinterpret_cast<PVOID*>(&lifecycle.fontLoad),
				reinterpret_cast<PVOID>(&HookTextCoreFontLoad));
			if (status == NOERROR && faceOpenFallbackRequired)
			{
				lifecycle.faceOpen =
					reinterpret_cast<FaceOpenFunction>(faceOpenTarget);
				status = transaction.Attach(
					reinterpret_cast<PVOID*>(&lifecycle.faceOpen),
					reinterpret_cast<PVOID>(&HookFaceOpen));
			}
		}
		else
		{
			lifecycle.faceOpen =
				reinterpret_cast<FaceOpenFunction>(faceOpenTarget);
			status = transaction.Attach(
				reinterpret_cast<PVOID*>(&lifecycle.faceOpen),
				reinterpret_cast<PVOID>(&HookFaceOpen));
		}
	}
	if (status == NOERROR && characterLookupTarget != nullptr)
	{
		lifecycle.characterLookup =
			reinterpret_cast<LegacyCharacterLookupFunction>(characterLookupTarget);
		status = transaction.Attach(
			reinterpret_cast<PVOID*>(&lifecycle.characterLookup),
			reinterpret_cast<PVOID>(&HookLegacyCharacterLookup));
	}
	if (status == NOERROR && osFaceResolverTarget != nullptr)
	{
		status = transaction.Attach(
			reinterpret_cast<PVOID*>(&lifecycle.osFaceResolver),
			reinterpret_cast<PVOID>(&HookLegacyOsFaceResolver));
	}
	if (status == NOERROR && freeTypeCharIndexTarget != nullptr)
	{
		lifecycle.freeTypeCharIndex =
			reinterpret_cast<FreeTypeCharIndexFunction>(freeTypeCharIndexTarget);
		status = transaction.Attach(
			reinterpret_cast<PVOID*>(&lifecycle.freeTypeCharIndex),
			reinterpret_cast<PVOID>(&HookFreeTypeCharIndex));
	}
	if (status == NOERROR && fontCatalogLoadTarget != nullptr)
	{
		lifecycle.fontCatalogLoad =
			reinterpret_cast<FontCatalogLoadFunction>(fontCatalogLoadTarget);
		status = transaction.Attach(
			reinterpret_cast<PVOID*>(&lifecycle.fontCatalogLoad),
			reinterpret_cast<PVOID>(&HookFontCatalogLoad));
	}
	if (status == NOERROR)
		status = transaction.Commit();
	else
		transaction.Commit();
	if (status != NOERROR)
	{
		lifecycle.publicRender = nullptr;
		lifecycle.internalRender = nullptr;
		lifecycle.faceOpen = nullptr;
		lifecycle.fontLoad = nullptr;
		lifecycle.characterLookup = nullptr;
		lifecycle.osFaceResolver = nullptr;
		lifecycle.freeTypeCharIndex = nullptr;
		lifecycle.fontCatalogLoad = nullptr;
		lifecycle.osFaceResolverRequired = false;
		lifecycle.fontCatalogLoadRequired = false;
		PublishUnityCapability(
			false, renderer::CapabilityReason::transactionFailed, status,
			reinterpret_cast<std::uintptr_t>(target), true);
		return false;
	}
	lifecycle.renderAttached = true;
	lifecycle.faceOpenAttached = substitutionRequired &&
		(substitutionBoundary ==
			renderer::unity::FontSubstitutionBoundary::freeTypeFaceOpen ||
		 faceOpenFallbackRequired);
	lifecycle.fontLoadAttached = substitutionRequired &&
		substitutionBoundary ==
			renderer::unity::FontSubstitutionBoundary::textCoreFontLoad;
	lifecycle.substitutionBoundary = substitutionBoundary;
	lifecycle.faceOpenFallbackRequired = faceOpenFallbackRequired;
	lifecycle.characterLookupAttached = characterLookupTarget != nullptr;
	lifecycle.osFaceResolverAttached = osFaceResolverTarget != nullptr;
	lifecycle.freeTypeCharIndexAttached = freeTypeCharIndexTarget != nullptr;
	lifecycle.fontCatalogLoadAttached = fontCatalogLoadTarget != nullptr;
	std::atomic_store_explicit(
		&lifecycle.redirects, std::move(redirects), std::memory_order_release);

	lifecycle.abi = adapter.abi;
	lifecycle.unityPlayer = std::move(module);
	lifecycle.operational.store(
		!lifecycle.stopping.load(std::memory_order_acquire),
		std::memory_order_release);
	PublishUnityCapability(
		true, renderer::CapabilityReason::none, NOERROR,
		reinterpret_cast<std::uintptr_t>(target), true);
	return true;
}

bool InitializeUnityFontHook()
{
	CGdippSettings::GetInstance();
	renderer::RendererPolicyRef const policy = renderer::CurrentRendererPolicy();
	if (!policy || !policy->hooks().unityFontEnabledForProcess)
	{
		PublishUnityCapability(
			false, renderer::CapabilityReason::explicitlyDisabled,
			ERROR_SUCCESS, 0, false);
		return false;
	}
	if (policy->hooks().unityFontMode == renderer::UnityFontHookMode::mostGames)
	{
		AntiCheatInspection const inspection = InspectCurrentInstallationForAntiCheat();
		if (inspection != AntiCheatInspection::clear)
		{
			PublishUnityCapability(
				false,
				inspection == AntiCheatInspection::detected
					? renderer::CapabilityReason::antiCheatDetected
					: renderer::CapabilityReason::safetyEvidenceUnavailable,
				ERROR_ACCESS_DENIED, 0, false);
			return false;
		}
	}

	HMODULE rawModule = nullptr;
	if (!GetModuleHandleExW(0, L"UnityPlayer.dll", &rawModule))
	{
		PublishUnityCapability(
			false, renderer::CapabilityReason::moduleMissing,
			static_cast<LONG>(GetLastError()), 0, false);
		return false;
	}
	renderer_raii::UniqueModuleReference module(rawModule);
	std::size_t mappedSize = 0;
	if (!renderer::QueryMappedModuleSize(module.get(), &mappedSize))
	{
		PublishUnityCapability(
			false, renderer::CapabilityReason::initializationFailed,
			ERROR_BAD_EXE_FORMAT, 0, true);
		return false;
	}
	std::size_t descriptorCount = 0;
	const renderer::unity::AdapterDescriptor* const descriptors =
		renderer::unity::ProductionAdapterDescriptors(&descriptorCount);
	renderer::unity::ResolvedAdapter adapter{};
	if (!renderer::unity::ResolveAdapter(
			module.get(), mappedSize, descriptors, descriptorCount, &adapter))
	{
		PublishUnityCapability(
			false, renderer::CapabilityReason::interfaceUnsupported,
			ERROR_NOT_SUPPORTED, 0, true);
		return false;
	}
	if (ProcessUnityLifecycle().stopping.load(std::memory_order_acquire))
		return false;
	std::shared_ptr<const renderer::unity::FontFileRedirectTable> redirects;
	if (policy->hooks().fontSubstitution && policy->substitutions_ready() &&
		policy->font_substitutions() &&
		!policy->font_substitutions()->rules().empty())
	{
		std::vector<renderer::unity::InstalledFontFace> installedFonts;
		if (renderer::unity::EnumerateInstalledFontFaces(installedFonts))
			redirects = renderer::unity::FontFileRedirectTable::Build(
				installedFonts, *policy->font_substitutions());
	}
	if (redirects && !redirects->empty() &&
		renderer::unity::SelectFontSubstitutionBoundary(adapter) ==
			renderer::unity::FontSubstitutionBoundary::unavailable)
	{
		PublishUnityCapability(
			false, renderer::CapabilityReason::interfaceUnsupported,
			ERROR_NOT_SUPPORTED, 0, true);
		return false;
	}
	InitializeUnityEvidenceIfRequested(ProcessUnityLifecycle());
	if (ProcessUnityLifecycle().stopping.load(std::memory_order_acquire))
		return false;
	return AttachUnityRenderer(
		ProcessUnityLifecycle(), std::move(module), adapter,
		std::move(redirects));
}

#endif // USE_DETOURS

DWORD WINAPI StartUnityFontHookWorker(LPVOID moduleReference)
{
	renderer_raii::UniqueModuleReference selfReference(
		static_cast<HMODULE>(moduleReference));
#ifdef USE_DETOURS
	bool succeeded = false;
	try
	{
		succeeded = InitializeUnityFontHook();
	}
	catch (...)
	{
		PublishUnityCapability(
			false, renderer::CapabilityReason::initializationFailed,
			ERROR_UNHANDLED_EXCEPTION, 0, false);
	}
#else
	PublishUnityCapability(
		false, renderer::CapabilityReason::interfaceUnsupported,
		ERROR_NOT_SUPPORTED, 0, false);
	bool const succeeded = false;
#endif
	UnityLifecycleState& lifecycle = ProcessUnityLifecycle();
	{
		std::lock_guard<std::mutex> lock(lifecycle.mutex);
		lifecycle.startupSucceeded = succeeded;
		if (lifecycle.phase == UnityLifecyclePhase::starting)
		{
			lifecycle.phase = succeeded
				? UnityLifecyclePhase::active
				: UnityLifecyclePhase::failed;
			lifecycle.operational.store(
				succeeded && lifecycle.renderAttached &&
					IsSubstitutionBoundaryOperational(lifecycle),
				std::memory_order_release);
		}
	}
	HMODULE const rawSelfReference = selfReference.release();
	FreeLibraryAndExitThread(
		rawSelfReference, succeeded ? ERROR_SUCCESS : ERROR_DLL_INIT_FAILED);
	return 0;
}

} // namespace

void StartUnityFontHookLifecycle()
{
	UnityLifecycleState& lifecycle = ProcessUnityLifecycle();
	std::lock_guard<std::mutex> lock(lifecycle.mutex);
	if (lifecycle.phase == UnityLifecyclePhase::starting ||
		lifecycle.phase == UnityLifecyclePhase::active ||
		lifecycle.phase == UnityLifecyclePhase::stopping ||
		lifecycle.renderAttached || lifecycle.faceOpenAttached ||
		lifecycle.fontLoadAttached || lifecycle.characterLookupAttached ||
		lifecycle.osFaceResolverAttached ||
		lifecycle.freeTypeCharIndexAttached ||
		lifecycle.fontCatalogLoadAttached ||
		lifecycle.unityPlayer)
		return;
	if (GetModuleHandleW(L"UnityPlayer.dll") == nullptr)
	{
		PublishUnityCapability(
			false, renderer::CapabilityReason::moduleMissing,
			ERROR_MOD_NOT_FOUND, 0, false);
		lifecycle.phase = UnityLifecyclePhase::failed;
		return;
	}
	lifecycle.startupWorker.reset();
	lifecycle.startupSucceeded = false;
	lifecycle.stopping.store(false, std::memory_order_release);
	lifecycle.operational.store(false, std::memory_order_release);
	lifecycle.phase = UnityLifecyclePhase::starting;

	HMODULE rawSelfReference = nullptr;
	if (!GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			reinterpret_cast<LPCWSTR>(&StartUnityFontHookWorker),
			&rawSelfReference))
	{
		PublishUnityCapability(
			false, renderer::CapabilityReason::initializationFailed,
			static_cast<LONG>(GetLastError()), 0, false);
		lifecycle.phase = UnityLifecyclePhase::failed;
		return;
	}
	renderer_raii::UniqueModuleReference selfReference(rawSelfReference);
	auto worker = renderer_raii::AdoptHandle(CreateThread(
		nullptr, 0, StartUnityFontHookWorker, selfReference.get(),
		CREATE_SUSPENDED, nullptr));
	if (!worker)
	{
		PublishUnityCapability(
			false, renderer::CapabilityReason::initializationFailed,
			static_cast<LONG>(GetLastError()), 0, false);
		lifecycle.phase = UnityLifecyclePhase::failed;
		return;
	}
	HANDLE const rawWorker = worker.get();
	HMODULE const transferredSelfReference = selfReference.release();
	lifecycle.startupWorker = std::move(worker);
	if (ResumeThread(rawWorker) == static_cast<DWORD>(-1))
	{
		LONG const status = static_cast<LONG>(GetLastError());
		lifecycle.startupWorker.reset();
		FreeLibrary(transferredSelfReference);
		PublishUnityCapability(
			false, renderer::CapabilityReason::initializationFailed,
			status, 0, false);
		lifecycle.phase = UnityLifecyclePhase::failed;
	}
}

UnityFontHookStopPreparation PrepareUnityFontHookLifecycleStop(
	DWORD timeoutMilliseconds)
{
	UnityLifecycleState& lifecycle = ProcessUnityLifecycle();
	HANDLE worker = nullptr;
	{
		std::lock_guard<std::mutex> lock(lifecycle.mutex);
		if (lifecycle.phase == UnityLifecyclePhase::dormant ||
			lifecycle.phase == UnityLifecyclePhase::stopped)
			return UnityFontHookStopPreparation::alreadyStopped;
		if (lifecycle.phase != UnityLifecyclePhase::stopping)
		{
			lifecycle.phaseBeforeStop = lifecycle.phase;
			lifecycle.phase = UnityLifecyclePhase::stopping;
			lifecycle.stopping.store(true, std::memory_order_release);
			lifecycle.operational.store(false, std::memory_order_release);
		}
		worker = lifecycle.startupWorker.get();
	}
	if (worker != nullptr &&
		WaitForSingleObject(worker, timeoutMilliseconds) != WAIT_OBJECT_0)
	{
		AbortUnityFontHookLifecycleStop();
		return UnityFontHookStopPreparation::unsafeToUnload;
	}
	return UnityFontHookStopPreparation::prepared;
}

void AbortUnityFontHookLifecycleStop()
{
	UnityLifecycleState& lifecycle = ProcessUnityLifecycle();
	std::lock_guard<std::mutex> lock(lifecycle.mutex);
	if (lifecycle.phase != UnityLifecyclePhase::stopping)
		return;
	bool workerStillRunning = false;
	if (lifecycle.startupWorker &&
		WaitForSingleObject(lifecycle.startupWorker.get(), 0) == WAIT_OBJECT_0)
		lifecycle.startupWorker.reset();
	else if (lifecycle.startupWorker)
		workerStillRunning = true;
	lifecycle.stopping.store(false, std::memory_order_release);
	lifecycle.phase = lifecycle.phaseBeforeStop == UnityLifecyclePhase::starting
		? (workerStillRunning
			? UnityLifecyclePhase::starting
			: lifecycle.startupSucceeded
			? UnityLifecyclePhase::active
			: UnityLifecyclePhase::failed)
		: lifecycle.phaseBeforeStop;
	bool const operational = lifecycle.phase == UnityLifecyclePhase::active &&
		lifecycle.startupSucceeded && lifecycle.renderAttached &&
		IsSubstitutionBoundaryOperational(lifecycle);
	lifecycle.operational.store(operational, std::memory_order_release);
	lifecycle.phaseBeforeStop = UnityLifecyclePhase::dormant;
}

bool CommitUnityFontHookLifecycleStop()
{
	UnityLifecycleState& lifecycle = ProcessUnityLifecycle();
	{
		std::lock_guard<std::mutex> lock(lifecycle.mutex);
		if (lifecycle.phase != UnityLifecyclePhase::stopping)
			return lifecycle.phase == UnityLifecyclePhase::dormant ||
				lifecycle.phase == UnityLifecyclePhase::stopped;
	}

#ifdef USE_DETOURS
	LONG status = NOERROR;
	if (lifecycle.renderAttached)
	{
		renderer_raii::DetourTransaction transaction;
		status = transaction.status();
		if (status == NOERROR && lifecycle.abi == renderer::unity::RenderAbi::publicRender)
			status = transaction.Detach(
				reinterpret_cast<PVOID*>(&lifecycle.publicRender),
				reinterpret_cast<PVOID>(&HookPublicRender));
		else if (status == NOERROR)
			status = transaction.Detach(
				reinterpret_cast<PVOID*>(&lifecycle.internalRender),
				reinterpret_cast<PVOID>(&HookInternalRender));
		if (status == NOERROR && lifecycle.faceOpenAttached)
			status = transaction.Detach(
				reinterpret_cast<PVOID*>(&lifecycle.faceOpen),
				reinterpret_cast<PVOID>(&HookFaceOpen));
		if (status == NOERROR && lifecycle.fontLoadAttached)
			status = transaction.Detach(
				reinterpret_cast<PVOID*>(&lifecycle.fontLoad),
				reinterpret_cast<PVOID>(&HookTextCoreFontLoad));
		if (status == NOERROR && lifecycle.characterLookupAttached)
			status = transaction.Detach(
				reinterpret_cast<PVOID*>(&lifecycle.characterLookup),
				reinterpret_cast<PVOID>(&HookLegacyCharacterLookup));
		if (status == NOERROR && lifecycle.osFaceResolverAttached)
			status = transaction.Detach(
				reinterpret_cast<PVOID*>(&lifecycle.osFaceResolver),
				reinterpret_cast<PVOID>(&HookLegacyOsFaceResolver));
		if (status == NOERROR && lifecycle.freeTypeCharIndexAttached)
			status = transaction.Detach(
				reinterpret_cast<PVOID*>(&lifecycle.freeTypeCharIndex),
				reinterpret_cast<PVOID>(&HookFreeTypeCharIndex));
		if (status == NOERROR && lifecycle.fontCatalogLoadAttached)
			status = transaction.Detach(
				reinterpret_cast<PVOID*>(&lifecycle.fontCatalogLoad),
				reinterpret_cast<PVOID>(&HookFontCatalogLoad));
		if (status == NOERROR)
			status = transaction.Commit();
		else
			transaction.Commit();
	}
	if (status != NOERROR)
		return false;
	lifecycle.renderAttached = false;
	lifecycle.faceOpenAttached = false;
	lifecycle.fontLoadAttached = false;
	lifecycle.characterLookupAttached = false;
	lifecycle.osFaceResolverAttached = false;
	lifecycle.freeTypeCharIndexAttached = false;
	lifecycle.fontCatalogLoadAttached = false;
#endif

	std::lock_guard<std::mutex> lock(lifecycle.mutex);
	lifecycle.startupWorker.reset();
	lifecycle.evidenceView.reset();
	lifecycle.evidenceMapping.reset();
	lifecycle.unityPlayer.reset();
	lifecycle.publicRender = nullptr;
	lifecycle.internalRender = nullptr;
	lifecycle.faceOpen = nullptr;
	lifecycle.fontLoad = nullptr;
	lifecycle.characterLookup = nullptr;
	lifecycle.osFaceResolver = nullptr;
	lifecycle.freeTypeCharIndex = nullptr;
	lifecycle.fontCatalogLoad = nullptr;
	lifecycle.renderAttached = false;
	lifecycle.faceOpenAttached = false;
	lifecycle.fontLoadAttached = false;
	lifecycle.faceOpenFallbackRequired = false;
	lifecycle.characterLookupAttached = false;
	lifecycle.osFaceResolverAttached = false;
	lifecycle.freeTypeCharIndexAttached = false;
	lifecycle.fontCatalogLoadAttached = false;
	lifecycle.fontCatalogLoadRequired = false;
	lifecycle.osFaceResolverRequired = false;
	lifecycle.substitutionBoundary =
		renderer::unity::FontSubstitutionBoundary::unavailable;
	std::atomic_store_explicit(
		&lifecycle.redirects,
		std::shared_ptr<const renderer::unity::FontFileRedirectTable>{},
		std::memory_order_release);
	lifecycle.stopping.store(false, std::memory_order_release);
	lifecycle.phase = UnityLifecyclePhase::stopped;
	lifecycle.phaseBeforeStop = UnityLifecyclePhase::dormant;
	lifecycle.startupSucceeded = false;
	lifecycle.operational.store(false, std::memory_order_release);
	return true;
}
