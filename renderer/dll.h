#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>

namespace renderer {

enum class PeImageLayout : unsigned char
{
	rawFile,
	mappedImage,
};

// A read-only PE export view. It never maps executable memory, applies
// relocations, or calls a foreign entry point. Every header, section, array,
// string, and RVA conversion is bounded by the supplied byte range.
class PeExportView
{
public:
	PeExportView(
		const void* bytes,
		std::size_t size,
		PeImageLayout layout) noexcept;

	bool valid() const noexcept { return valid_; }
	bool FindFunctionRva(const char* name, DWORD* functionRva) const noexcept;
	bool FindAddressTableEntryRva(
		const char* name,
		DWORD* addressTableEntryRva) const noexcept;

private:
	bool Initialize() noexcept;
	bool ReadBytes(
		std::size_t offset,
		void* destination,
		std::size_t length) const noexcept;
	bool RvaToOffset(
		DWORD rva,
		std::size_t length,
		std::size_t* offset) const noexcept;
	bool RvaStringEquals(DWORD rva, const char* expected) const noexcept;
	bool FindNamedExport(
		const char* name,
		DWORD* functionRva,
		DWORD* addressTableEntryRva) const noexcept;

	const unsigned char* bytes_;
	std::size_t size_;
	PeImageLayout layout_;
	bool valid_;
	WORD sectionCount_;
	std::size_t sectionTableOffset_;
	DWORD sizeOfImage_;
	DWORD sizeOfHeaders_;
	IMAGE_DATA_DIRECTORY exportDirectory_;
};

// Returns the complete allocation span for a loader-mapped module without
// trusting the module's own SizeOfImage field first.
bool QueryMappedModuleSize(HMODULE module, std::size_t* size) noexcept;

} // namespace renderer
