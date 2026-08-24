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
#else
using PublicRenderFunction = void*;
using InternalRenderFunction = void*;
#endif

using CreateFileAFunction = HANDLE (WINAPI*)(
	LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileWFunction = HANDLE (WINAPI*)(
	LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

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
	renderer::unity::FileImportSlots fileImports;
	CreateFileAFunction createFileA = nullptr;
	CreateFileWFunction createFileW = nullptr;
	std::shared_ptr<const renderer::unity::FontFileRedirectTable> redirects;
	renderer_raii::UniqueHandle evidenceMapping;
	renderer_raii::UniqueMappedView evidenceView;
	bool renderAttached = false;
	bool createFileAAttached = false;
	bool createFileWAttached = false;
	bool startupSucceeded = false;
};

UnityLifecycleState& ProcessUnityLifecycle()
{
	// Explicit unload drains the contained references outside the loader lock.
	// Process termination leaves the tiny state allocation to the OS.
	static UnityLifecycleState* state = new UnityLifecycleState;
	return *state;
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

bool LooksLikeFontFile(const wchar_t* path) noexcept
{
	if (path == nullptr)
		return false;
	const wchar_t* const extension = wcsrchr(path, L'.');
	return extension != nullptr &&
		(_wcsicmp(extension, L".ttf") == 0 ||
		 _wcsicmp(extension, L".ttc") == 0 ||
		 _wcsicmp(extension, L".otf") == 0);
}

bool PatchImportSlot(
	void** slot,
	void* expected,
	void* replacement) noexcept
{
	if (slot == nullptr || expected == nullptr || replacement == nullptr)
		return false;
	DWORD oldProtection = 0;
	if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection))
		return false;
	void* const previous = InterlockedCompareExchangePointer(
		reinterpret_cast<PVOID volatile*>(slot), replacement, expected);
	DWORD ignored = 0;
	if (previous != expected)
	{
		VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
		return false;
	}
	if (VirtualProtect(slot, sizeof(void*), oldProtection, &ignored))
		return true;
	InterlockedCompareExchangePointer(
		reinterpret_cast<PVOID volatile*>(slot), expected, replacement);
	VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
	return false;
}

void* ReadImportSlot(void** slot) noexcept
{
	void* target = nullptr;
	return renderer::unity::ReadFileImportTarget(slot, &target)
		? target
		: nullptr;
}

HANDLE WINAPI HookUnityCreateFileW(
	LPCWSTR fileName,
	DWORD desiredAccess,
	DWORD shareMode,
	LPSECURITY_ATTRIBUTES securityAttributes,
	DWORD creationDisposition,
	DWORD flagsAndAttributes,
	HANDLE templateFile) noexcept
{
	CThreadCounter callbackLease;
	CreateFileWFunction const original = ProcessUnityLifecycle().createFileW;
	if (original == nullptr)
	{
		SetLastError(ERROR_PROC_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	try
	{
		if (InterlockedCompareExchange(&g_bHookEnabled, 0, 0) != FALSE &&
			ProcessUnityLifecycle().operational.load(
				std::memory_order_acquire))
		{
			renderer::unity::UnityFontEvidenceV1* const evidence =
				UnityEvidence();
			if (evidence != nullptr && LooksLikeFontFile(fileName))
				renderer::unity::RecordUnityFontFileOpen(*evidence, fileName);
			auto const redirects = std::atomic_load_explicit(
				&ProcessUnityLifecycle().redirects, std::memory_order_acquire);
			std::wstring replacement;
			if (redirects && redirects->Resolve(fileName, replacement))
			{
				HANDLE const redirected = original(
					replacement.c_str(), desiredAccess, shareMode,
					securityAttributes, creationDisposition,
					flagsAndAttributes, templateFile);
				if (evidence != nullptr)
					renderer::unity::RecordUnityFontRedirect(
						*evidence, fileName, replacement.c_str(),
						redirected != INVALID_HANDLE_VALUE);
				if (redirected != INVALID_HANDLE_VALUE)
					return redirected;
			}
		}
	}
	catch (...)
	{
	}
	return original(
		fileName, desiredAccess, shareMode, securityAttributes,
		creationDisposition, flagsAndAttributes, templateFile);
}

HANDLE WINAPI HookUnityCreateFileA(
	LPCSTR fileName,
	DWORD desiredAccess,
	DWORD shareMode,
	LPSECURITY_ATTRIBUTES securityAttributes,
	DWORD creationDisposition,
	DWORD flagsAndAttributes,
	HANDLE templateFile) noexcept
{
	CThreadCounter callbackLease;
	CreateFileAFunction const originalA = ProcessUnityLifecycle().createFileA;
	CreateFileWFunction const originalW = ProcessUnityLifecycle().createFileW;
	if (originalA == nullptr)
	{
		SetLastError(ERROR_PROC_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	try
	{
		if (fileName != nullptr && originalW != nullptr &&
			InterlockedCompareExchange(&g_bHookEnabled, 0, 0) != FALSE &&
			ProcessUnityLifecycle().operational.load(
				std::memory_order_acquire))
		{
			int const required = MultiByteToWideChar(
				CP_ACP, MB_ERR_INVALID_CHARS, fileName, -1, nullptr, 0);
			if (required > 1 && required <= 32768)
			{
				std::vector<wchar_t> requested(static_cast<std::size_t>(required));
				if (MultiByteToWideChar(
					CP_ACP, MB_ERR_INVALID_CHARS, fileName, -1,
					requested.data(), required) == required)
				{
					renderer::unity::UnityFontEvidenceV1* const evidence =
						UnityEvidence();
					if (evidence != nullptr &&
						LooksLikeFontFile(requested.data()))
						renderer::unity::RecordUnityFontFileOpen(
							*evidence, requested.data());
					auto const redirects = std::atomic_load_explicit(
						&ProcessUnityLifecycle().redirects,
						std::memory_order_acquire);
					std::wstring replacement;
					if (redirects && redirects->Resolve(
						requested.data(), replacement))
					{
						HANDLE const redirected = originalW(
							replacement.c_str(), desiredAccess, shareMode,
							securityAttributes, creationDisposition,
							flagsAndAttributes, templateFile);
						if (evidence != nullptr)
							renderer::unity::RecordUnityFontRedirect(
								*evidence, requested.data(), replacement.c_str(),
								redirected != INVALID_HANDLE_VALUE);
						if (redirected != INVALID_HANDLE_VALUE)
							return redirected;
					}
				}
			}
		}
	}
	catch (...)
	{
	}
	return originalA(
		fileName, desiredAccess, shareMode, securityAttributes,
		creationDisposition, flagsAndAttributes, templateFile);
}

bool AttachUnityFileImports(
	UnityLifecycleState& lifecycle,
	void* mappedImage,
	std::size_t mappedSize,
	std::shared_ptr<const renderer::unity::FontFileRedirectTable> redirects) noexcept
{
	renderer::unity::FileImportSlots slots{};
	if (!renderer::unity::ResolveFileImportSlots(
		mappedImage, mappedSize, &slots))
		return false;
	void* const originalA = ReadImportSlot(slots.createFileA);
	void* const originalW = ReadImportSlot(slots.createFileW);
	if (originalA == nullptr || originalW == nullptr)
		return false;
	lifecycle.fileImports = slots;
	std::memcpy(&lifecycle.createFileA, &originalA, sizeof(originalA));
	std::memcpy(&lifecycle.createFileW, &originalW, sizeof(originalW));
	std::atomic_store_explicit(
		&lifecycle.redirects, std::move(redirects), std::memory_order_release);
	if (!PatchImportSlot(
		slots.createFileW, originalW,
		reinterpret_cast<void*>(&HookUnityCreateFileW)))
	{
		lifecycle.fileImports = {};
		lifecycle.createFileA = nullptr;
		lifecycle.createFileW = nullptr;
		std::atomic_store_explicit(
			&lifecycle.redirects,
			std::shared_ptr<const renderer::unity::FontFileRedirectTable>{},
			std::memory_order_release);
		return false;
	}
	lifecycle.createFileWAttached = true;
	if (!PatchImportSlot(
		slots.createFileA, originalA,
		reinterpret_cast<void*>(&HookUnityCreateFileA)))
	{
		if (PatchImportSlot(
			slots.createFileW,
			reinterpret_cast<void*>(&HookUnityCreateFileW), originalW))
			lifecycle.createFileWAttached = false;
		std::atomic_store_explicit(
			&lifecycle.redirects,
			std::shared_ptr<const renderer::unity::FontFileRedirectTable>{},
			std::memory_order_release);
		return false;
	}
	lifecycle.createFileAAttached = true;
	return true;
}

bool DetachUnityFileImports(UnityLifecycleState& lifecycle) noexcept
{
	if (!lifecycle.createFileAAttached && !lifecycle.createFileWAttached)
		return true;
	void* originalA = nullptr;
	void* originalW = nullptr;
	std::memcpy(&originalA, &lifecycle.createFileA, sizeof(originalA));
	std::memcpy(&originalW, &lifecycle.createFileW, sizeof(originalW));
	if (lifecycle.createFileAAttached)
	{
		if (!PatchImportSlot(
			lifecycle.fileImports.createFileA,
			reinterpret_cast<void*>(&HookUnityCreateFileA), originalA))
			return false;
		lifecycle.createFileAAttached = false;
	}
	if (lifecycle.createFileWAttached)
	{
		if (!PatchImportSlot(
			lifecycle.fileImports.createFileW,
			reinterpret_cast<void*>(&HookUnityCreateFileW), originalW))
			return false;
		lifecycle.createFileWAttached = false;
	}
	return true;
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

FT_Error HookPublicRender(FT_GlyphSlot slot, FT_Render_Mode mode) noexcept
{
	CThreadCounter callbackLease;
	PublicRenderFunction const original = ProcessUnityLifecycle().publicRender;
	if (original == nullptr)
		return 0x06; // FT_Err_Invalid_Argument
	FT_Error const result = original(slot, mode);
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
	if (result == 0)
		ApplyProfileCoverage(slot, mode);
	return result;
}

bool AttachUnityRenderer(
	UnityLifecycleState& lifecycle,
	renderer_raii::UniqueModuleReference module,
	const renderer::unity::ResolvedAdapter& adapter,
	std::size_t mappedSize,
	std::shared_ptr<const renderer::unity::FontFileRedirectTable> redirects) noexcept
{
	std::uintptr_t const base = reinterpret_cast<std::uintptr_t>(module.get());
	if (adapter.targetRva > (std::numeric_limits<std::uintptr_t>::max)() - base)
		return false;
	void* const target = reinterpret_cast<void*>(base + adapter.targetRva);
	if (!renderer::unity::IsExecutableMemoryRange(target, 32))
		return false;

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
	if (status == NOERROR)
		status = transaction.Commit();
	else
		transaction.Commit();
	if (status != NOERROR)
	{
		lifecycle.publicRender = nullptr;
		lifecycle.internalRender = nullptr;
		PublishUnityCapability(
			false, renderer::CapabilityReason::transactionFailed, status,
			reinterpret_cast<std::uintptr_t>(target), true);
		return false;
	}
	lifecycle.renderAttached = true;
	if (!AttachUnityFileImports(
			lifecycle, module.get(), mappedSize, std::move(redirects)))
	{
		renderer_raii::DetourTransaction rollback;
		LONG rollbackStatus = rollback.status();
		if (rollbackStatus == NOERROR &&
			adapter.abi == renderer::unity::RenderAbi::publicRender)
			rollbackStatus = rollback.Detach(
				reinterpret_cast<PVOID*>(&lifecycle.publicRender),
				reinterpret_cast<PVOID>(&HookPublicRender));
		else if (rollbackStatus == NOERROR)
			rollbackStatus = rollback.Detach(
				reinterpret_cast<PVOID*>(&lifecycle.internalRender),
				reinterpret_cast<PVOID>(&HookInternalRender));
		if (rollbackStatus == NOERROR)
			rollbackStatus = rollback.Commit();
		else
			rollback.Commit();
		if (rollbackStatus == NOERROR)
		{
			lifecycle.renderAttached = false;
			lifecycle.publicRender = nullptr;
			lifecycle.internalRender = nullptr;
		}
		if (lifecycle.renderAttached || lifecycle.createFileAAttached ||
			lifecycle.createFileWAttached)
			lifecycle.unityPlayer = std::move(module);
		PublishUnityCapability(
			false, renderer::CapabilityReason::interfaceUnsupported,
			ERROR_PROC_NOT_FOUND,
			reinterpret_cast<std::uintptr_t>(target), true);
		return false;
	}

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
	InitializeUnityEvidenceIfRequested(ProcessUnityLifecycle());
	if (ProcessUnityLifecycle().stopping.load(std::memory_order_acquire))
		return false;
	return AttachUnityRenderer(
		ProcessUnityLifecycle(), std::move(module), adapter, mappedSize,
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
					lifecycle.createFileAAttached &&
					lifecycle.createFileWAttached,
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
		lifecycle.renderAttached || lifecycle.createFileAAttached ||
		lifecycle.createFileWAttached || lifecycle.unityPlayer)
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
		lifecycle.createFileAAttached && lifecycle.createFileWAttached;
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
		if (status == NOERROR)
			status = transaction.Commit();
		else
			transaction.Commit();
	}
	if (status != NOERROR)
		return false;
	lifecycle.renderAttached = false;
	if (!DetachUnityFileImports(lifecycle))
		return false;
#endif

	std::lock_guard<std::mutex> lock(lifecycle.mutex);
	lifecycle.startupWorker.reset();
	lifecycle.evidenceView.reset();
	lifecycle.evidenceMapping.reset();
	lifecycle.unityPlayer.reset();
	lifecycle.publicRender = nullptr;
	lifecycle.internalRender = nullptr;
	lifecycle.fileImports = {};
	lifecycle.createFileA = nullptr;
	lifecycle.createFileW = nullptr;
	lifecycle.renderAttached = false;
	lifecycle.createFileAAttached = false;
	lifecycle.createFileWAttached = false;
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
