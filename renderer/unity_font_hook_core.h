#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "renderer_policy.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace renderer { namespace unity {

enum class RenderAbi : unsigned char
{
	publicRender,
	internalRender,
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
};

struct ResolvedAdapter final
{
	const char* name = nullptr;
	DWORD targetRva = 0;
	RenderAbi abi = RenderAbi::publicRender;
};

struct FileImportSlots final
{
	void** createFileA = nullptr;
	void** createFileW = nullptr;
};

struct InstalledFontFace final
{
	std::wstring family;
	std::wstring filePath;
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
	[[nodiscard]] bool empty() const noexcept { return redirects_.empty(); }

private:
	explicit FontFileRedirectTable(
		std::vector<std::pair<std::wstring, std::wstring>> redirects)
		: redirects_(std::move(redirects))
	{
	}

	std::vector<std::pair<std::wstring, std::wstring>> redirects_;
};

bool ResolveAdapter(
	const void* mappedImage,
	std::size_t mappedSize,
	const AdapterDescriptor* descriptors,
	std::size_t descriptorCount,
	ResolvedAdapter* resolved) noexcept;

bool ResolveFileImportSlots(
	void* mappedImage,
	std::size_t mappedSize,
	FileImportSlots* slots) noexcept;

bool ReadFileImportTarget(
	void** slot,
	void** target) noexcept;

const AdapterDescriptor* ProductionAdapterDescriptors(
	std::size_t* count) noexcept;

bool IsExecutableMemoryRange(const void* address, std::size_t length) noexcept;

bool ApplyCoverage(
	unsigned char* buffer,
	int pitch,
	unsigned int rows,
	unsigned int width,
	unsigned char pixelMode,
	const UnityCoverageLut& lut) noexcept;

}} // namespace renderer::unity
