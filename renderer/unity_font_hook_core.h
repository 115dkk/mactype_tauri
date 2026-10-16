#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "renderer_policy.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace renderer { namespace unity {

enum class RenderAbi : unsigned char
{
	publicRender,
	internalRender,
};

enum class FaceOpenAbi : unsigned char
{
	unavailable,
	unityInternal,
};

enum class FaceDoneAbi : unsigned char
{
	unavailable,
	standard,
};

enum class FontLoadAbi : unsigned char
{
	unavailable,
	textCorePathSizeFace,
};

enum class CharacterLookupAbi : unsigned char
{
	unavailable,
	legacyDynamicFont,
};

enum class FreeTypeCharIndexAbi : unsigned char
{
	unavailable,
	standard,
};

enum class FontCatalogLoadAbi : unsigned char
{
	unavailable,
	systemCatalogEntry,
};

enum class FontSubstitutionBoundary : unsigned char
{
	unavailable,
	textCoreFontLoad,
	freeTypeFaceOpen,
};

struct AdapterDescriptor final
{
	const char* name = nullptr;
	WORD machine = 0;
	DWORD timestamp = 0;
	DWORD imageSize = 0;
	std::array<unsigned char, 16> pdbGuid{};
	DWORD pdbAge = 0;
	DWORD targetRva = 0;
	RenderAbi abi = RenderAbi::publicRender;
	const std::array<unsigned char, 32>* targetPrefix = nullptr;
	DWORD faceOpenRva = 0;
	const std::array<unsigned char, 32>* faceOpenPrefix = nullptr;
	FaceOpenAbi faceOpenAbi = FaceOpenAbi::unavailable;
	DWORD fontLoadRva = 0;
	const std::array<unsigned char, 32>* fontLoadPrefix = nullptr;
	FontLoadAbi fontLoadAbi = FontLoadAbi::unavailable;
	DWORD characterLookupRva = 0;
	const std::array<unsigned char, 32>* characterLookupPrefix = nullptr;
	CharacterLookupAbi characterLookupAbi = CharacterLookupAbi::unavailable;
	DWORD osFaceResolverRva = 0;
	const std::array<unsigned char, 32>* osFaceResolverPrefix = nullptr;
	CharacterLookupAbi osFaceResolverAbi = CharacterLookupAbi::unavailable;
	DWORD freeTypeCharIndexRva = 0;
	const std::array<unsigned char, 32>* freeTypeCharIndexPrefix = nullptr;
	FreeTypeCharIndexAbi freeTypeCharIndexAbi = FreeTypeCharIndexAbi::unavailable;
	DWORD fontCatalogLoadRva = 0;
	const std::array<unsigned char, 32>* fontCatalogLoadPrefix = nullptr;
	FontCatalogLoadAbi fontCatalogLoadAbi = FontCatalogLoadAbi::unavailable;
	DWORD faceDoneRva = 0;
	const std::array<unsigned char, 32>* faceDonePrefix = nullptr;
	FaceDoneAbi faceDoneAbi = FaceDoneAbi::unavailable;
};

struct ResolvedAdapter final
{
	const char* name = nullptr;
	DWORD targetRva = 0;
	DWORD faceOpenRva = 0;
	DWORD fontLoadRva = 0;
	DWORD characterLookupRva = 0;
	DWORD osFaceResolverRva = 0;
	DWORD freeTypeCharIndexRva = 0;
	DWORD fontCatalogLoadRva = 0;
	DWORD faceDoneRva = 0;
	RenderAbi abi = RenderAbi::publicRender;
	FaceOpenAbi faceOpenAbi = FaceOpenAbi::unavailable;
	FontLoadAbi fontLoadAbi = FontLoadAbi::unavailable;
	CharacterLookupAbi characterLookupAbi = CharacterLookupAbi::unavailable;
	CharacterLookupAbi osFaceResolverAbi = CharacterLookupAbi::unavailable;
	FreeTypeCharIndexAbi freeTypeCharIndexAbi = FreeTypeCharIndexAbi::unavailable;
	FontCatalogLoadAbi fontCatalogLoadAbi = FontCatalogLoadAbi::unavailable;
	FaceDoneAbi faceDoneAbi = FaceDoneAbi::unavailable;
};

struct AxisValue final
{
	std::uint32_t tag = 0;
	float value = 0.0F;
};

struct InstalledFontFace final
{
	std::wstring family;
	std::wstring filePath;
	long faceIndex = 0;
	std::vector<AxisValue> axisValues;
};

struct FaceOpenPathRedirect final
{
	std::wstring sourcePath;
	std::wstring replacementPath;
	std::string replacementUtf8;
	long replacementFaceIndex = 0;
};

class FaceReplacementRegistry final
{
public:
	enum class State : unsigned char
	{
		none,
		pending,
		published,
	};

	struct LookupResult final
	{
		State state = State::none;
		const void* replacement = nullptr;
	};

	struct ReleaseResult final
	{
		bool releasedNative = false;
		const void* pairedReplacement = nullptr;
	};

	[[nodiscard]] LookupResult Lookup(const void* native) const noexcept;
	[[nodiscard]] bool Reserve(const void* native) noexcept;
	[[nodiscard]] bool Publish(
		const void* native,
		const void* replacement) noexcept;
	void Abandon(const void* native) noexcept;
	[[nodiscard]] ReleaseResult Release(const void* face) noexcept;
	void Clear() noexcept;
	[[nodiscard]] std::size_t size() const noexcept;
	[[nodiscard]] bool IsReplacement(const void* face) const noexcept;

private:
	static constexpr std::size_t kMaximumEntries = 256;
	std::unordered_map<const void*, const void*> replacementsByNative_;
	std::unordered_set<const void*> replacementFaces_;
};

class FontFileRedirectTable final
{
public:
	static std::shared_ptr<const FontFileRedirectTable> Build(
		const std::vector<InstalledFontFace>& installedFonts,
		const font_substitution::Snapshot& substitutions) noexcept;

	[[nodiscard]] bool Resolve(
		const wchar_t* requestedPath,
		std::wstring& replacementPath) const noexcept;
	[[nodiscard]] bool ResolveFace(
		const wchar_t* requestedPath,
		long requestedFaceIndex,
		std::wstring& replacementPath,
		long& replacementFaceIndex) const noexcept;
	[[nodiscard]] bool ResolveFamilyFace(
		const wchar_t* requestedFamily,
		std::wstring& replacementPath,
		long& replacementFaceIndex) const noexcept;
	[[nodiscard]] bool empty() const noexcept { return redirects_.empty(); }

private:
	struct Redirect final
	{
		std::wstring sourcePath;
		long sourceFaceIndex = 0;
		std::vector<std::wstring> sourceFamilies;
		std::wstring replacementPath;
		long replacementFaceIndex = 0;
	};

	explicit FontFileRedirectTable(
		std::vector<Redirect> redirects)
		: redirects_(std::move(redirects))
	{
	}

	std::vector<Redirect> redirects_;
};

bool ResolveAdapter(
	const void* mappedImage,
	std::size_t mappedSize,
	const AdapterDescriptor* descriptors,
	std::size_t descriptorCount,
	ResolvedAdapter* resolved) noexcept;

bool ResolveFaceOpenPath(
	const FontFileRedirectTable& redirects,
	unsigned int openFlags,
	const char* requestedUtf8,
	long requestedFaceIndex,
	FaceOpenPathRedirect& redirect) noexcept;

bool ResolveTextCoreFontLoadPath(
	const FontFileRedirectTable& redirects,
	const char* requestedUtf8,
	long requestedFaceIndex,
	FaceOpenPathRedirect& redirect) noexcept;

bool ResolveFamilyFaceOpenPath(
	const FontFileRedirectTable& redirects,
	const wchar_t* requestedFamily,
	FaceOpenPathRedirect& redirect) noexcept;

bool ReadLegacyFontRefFamily(
	const void* fontRef,
	std::wstring& family) noexcept;

bool ReadReadableUtf8Path(
	const char* value,
	std::string& path) noexcept;

bool ParseSfntNamedInstanceIndex(
	const unsigned char* bytes,
	std::size_t size,
	long baseFaceIndex,
	const AxisValue* axisValues,
	std::size_t axisValueCount,
	long& instanceIndex) noexcept;

FontSubstitutionBoundary SelectFontSubstitutionBoundary(
	const ResolvedAdapter& adapter) noexcept;

const AdapterDescriptor* ProductionAdapterDescriptors(
	std::size_t* count) noexcept;

bool IsExecutableMemoryRange(const void* address, std::size_t length) noexcept;
bool IsReadableMemoryRange(const void* address, std::size_t length) noexcept;

bool ApplyCoverage(
	unsigned char* buffer,
	int pitch,
	unsigned int rows,
	unsigned int width,
	unsigned char pixelMode,
	const UnityCoverageLut& lut) noexcept;

}} // namespace renderer::unity
