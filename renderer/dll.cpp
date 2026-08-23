#include "dll.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

namespace renderer {

namespace {

bool CheckedAdd(std::size_t left, std::size_t right, std::size_t* result) noexcept
{
	if (result == nullptr || right > std::numeric_limits<std::size_t>::max() - left)
		return false;
	*result = left + right;
	return true;
}

bool CheckedMultiply(std::size_t left, std::size_t right, std::size_t* result) noexcept
{
	if (result == nullptr ||
		(left != 0 && right > std::numeric_limits<std::size_t>::max() / left))
		return false;
	*result = left * right;
	return true;
}

bool FitsDwordRange(DWORD start, DWORD length) noexcept
{
	return static_cast<std::uint64_t>(start) + length <=
		static_cast<std::uint64_t>(std::numeric_limits<DWORD>::max()) + 1u;
}

bool IsReadableProtection(DWORD protection) noexcept
{
	switch (protection & 0xffu) {
	case PAGE_READONLY:
	case PAGE_READWRITE:
	case PAGE_WRITECOPY:
	case PAGE_EXECUTE_READ:
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
		return true;
	default:
		return false;
	}
}

bool IsReadableMemoryRange(const void* address, std::size_t length) noexcept
{
	if (address == nullptr || length == 0)
		return false;
	std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(address);
	if (length > std::numeric_limits<std::uintptr_t>::max() - cursor)
		return false;
	std::uintptr_t const end = cursor + length;
	while (cursor < end) {
		MEMORY_BASIC_INFORMATION memory{};
		if (::VirtualQuery(
				reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) == 0 ||
			memory.State != MEM_COMMIT ||
			(memory.Protect & PAGE_GUARD) != 0 ||
			!IsReadableProtection(memory.Protect))
			return false;
		std::uintptr_t const regionBase =
			reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
		if (memory.RegionSize == 0 ||
			memory.RegionSize > std::numeric_limits<std::uintptr_t>::max() - regionBase)
			return false;
		std::uintptr_t const regionEnd = regionBase + memory.RegionSize;
		if (regionEnd <= cursor)
			return false;
		cursor = (std::min)(regionEnd, end);
	}
	return true;
}

} // namespace

PeExportView::PeExportView(
	const void* bytes,
	std::size_t size,
	PeImageLayout layout) noexcept
	: bytes_(static_cast<const unsigned char*>(bytes))
	, size_(size)
	, layout_(layout)
	, valid_(false)
	, sectionCount_(0)
	, sectionTableOffset_(0)
	, sizeOfImage_(0)
	, sizeOfHeaders_(0)
	, exportDirectory_{}
{
	valid_ = Initialize();
}

bool PeExportView::ReadBytes(
	std::size_t offset,
	void* destination,
	std::size_t length) const noexcept
{
	if (bytes_ == nullptr || destination == nullptr || offset > size_ ||
		length > size_ - offset)
		return false;
	std::uintptr_t const base = reinterpret_cast<std::uintptr_t>(bytes_);
	if (offset > std::numeric_limits<std::uintptr_t>::max() - base)
		return false;
	const unsigned char* const source =
		reinterpret_cast<const unsigned char*>(base + offset);
	if (!IsReadableMemoryRange(source, length))
		return false;
	std::memcpy(destination, source, length);
	return true;
}

bool PeExportView::Initialize() noexcept
{
	IMAGE_DOS_HEADER dosHeader{};
	if (!ReadBytes(0, &dosHeader, sizeof(dosHeader)) ||
		dosHeader.e_magic != IMAGE_DOS_SIGNATURE || dosHeader.e_lfanew < 0)
		return false;

	std::size_t const ntOffset = static_cast<std::size_t>(dosHeader.e_lfanew);
	DWORD signature = 0;
	if (!ReadBytes(ntOffset, &signature, sizeof(signature)) ||
		signature != IMAGE_NT_SIGNATURE)
		return false;

	std::size_t fileHeaderOffset = 0;
	if (!CheckedAdd(ntOffset, sizeof(signature), &fileHeaderOffset))
		return false;
	IMAGE_FILE_HEADER fileHeader{};
	if (!ReadBytes(fileHeaderOffset, &fileHeader, sizeof(fileHeader)) ||
		(fileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == 0 ||
		fileHeader.NumberOfSections == 0)
		return false;

	std::size_t optionalHeaderOffset = 0;
	if (!CheckedAdd(fileHeaderOffset, sizeof(fileHeader), &optionalHeaderOffset))
		return false;
	WORD optionalMagic = 0;
	if (!ReadBytes(optionalHeaderOffset, &optionalMagic, sizeof(optionalMagic)))
		return false;

	if (optionalMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
		if (fileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32))
			return false;
		IMAGE_OPTIONAL_HEADER32 optionalHeader{};
		if (!ReadBytes(optionalHeaderOffset, &optionalHeader, sizeof(optionalHeader)) ||
			optionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
			return false;
		sizeOfImage_ = optionalHeader.SizeOfImage;
		sizeOfHeaders_ = optionalHeader.SizeOfHeaders;
		exportDirectory_ = optionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
	}
	else if (optionalMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		if (fileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
			return false;
		IMAGE_OPTIONAL_HEADER64 optionalHeader{};
		if (!ReadBytes(optionalHeaderOffset, &optionalHeader, sizeof(optionalHeader)) ||
			optionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
			return false;
		sizeOfImage_ = optionalHeader.SizeOfImage;
		sizeOfHeaders_ = optionalHeader.SizeOfHeaders;
		exportDirectory_ = optionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
	}
	else {
		return false;
	}

	if (sizeOfImage_ == 0 || sizeOfHeaders_ == 0 ||
		sizeOfHeaders_ > sizeOfImage_ ||
		exportDirectory_.VirtualAddress == 0 || exportDirectory_.Size == 0 ||
		!FitsDwordRange(exportDirectory_.VirtualAddress, exportDirectory_.Size))
		return false;

	std::size_t optionalHeaderEnd = 0;
	if (!CheckedAdd(
			optionalHeaderOffset,
			fileHeader.SizeOfOptionalHeader,
			&optionalHeaderEnd))
		return false;
	sectionTableOffset_ = optionalHeaderEnd;
	sectionCount_ = fileHeader.NumberOfSections;

	std::size_t sectionTableSize = 0;
	std::size_t sectionTableEnd = 0;
	if (!CheckedMultiply(sectionCount_, sizeof(IMAGE_SECTION_HEADER), &sectionTableSize) ||
		!CheckedAdd(sectionTableOffset_, sectionTableSize, &sectionTableEnd) ||
		sectionTableEnd > sizeOfHeaders_ || sectionTableEnd > size_)
		return false;

	if (layout_ == PeImageLayout::mappedImage) {
		if (sizeOfImage_ > size_)
			return false;
	}
	else if (sizeOfHeaders_ > size_) {
		return false;
	}

	for (WORD index = 0; index < sectionCount_; ++index) {
		IMAGE_SECTION_HEADER section{};
		std::size_t sectionOffset = 0;
		if (!CheckedAdd(
				sectionTableOffset_,
				static_cast<std::size_t>(index) * sizeof(section),
				&sectionOffset) ||
			!ReadBytes(sectionOffset, &section, sizeof(section)))
			return false;

		DWORD const virtualSpan = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
		if (virtualSpan != 0 &&
			(!FitsDwordRange(section.VirtualAddress, virtualSpan) ||
			 static_cast<std::uint64_t>(section.VirtualAddress) + virtualSpan > sizeOfImage_))
			return false;
		if (layout_ == PeImageLayout::rawFile && section.SizeOfRawData != 0) {
			std::uint64_t const rawEnd =
				static_cast<std::uint64_t>(section.PointerToRawData) + section.SizeOfRawData;
			if (rawEnd > size_)
				return false;
		}
	}

	std::size_t exportOffset = 0;
	return RvaToOffset(
		exportDirectory_.VirtualAddress,
		exportDirectory_.Size,
		&exportOffset);
}

bool PeExportView::RvaToOffset(
	DWORD rva,
	std::size_t length,
	std::size_t* offset) const noexcept
{
	if (offset == nullptr || length == 0)
		return false;

	if (layout_ == PeImageLayout::mappedImage) {
		std::size_t const candidate = rva;
		if (candidate >= sizeOfImage_ || length > sizeOfImage_ - candidate ||
			candidate > size_ || length > size_ - candidate)
			return false;
		*offset = candidate;
		return true;
	}

	if (rva < sizeOfHeaders_) {
		std::size_t const candidate = rva;
		if (length > sizeOfHeaders_ - candidate || candidate > size_ ||
			length > size_ - candidate)
			return false;
		*offset = candidate;
		return true;
	}

	for (WORD index = 0; index < sectionCount_; ++index) {
		IMAGE_SECTION_HEADER section{};
		std::size_t const sectionOffset = sectionTableOffset_ +
			static_cast<std::size_t>(index) * sizeof(section);
		if (!ReadBytes(sectionOffset, &section, sizeof(section)))
			return false;

		DWORD const virtualSpan = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
		if (rva < section.VirtualAddress ||
			static_cast<std::uint64_t>(rva) >=
				static_cast<std::uint64_t>(section.VirtualAddress) + virtualSpan)
			continue;

		std::size_t const delta =
			static_cast<std::size_t>(rva - section.VirtualAddress);
		if (delta > section.SizeOfRawData || length > section.SizeOfRawData - delta)
			return false;
		std::size_t candidate = 0;
		if (!CheckedAdd(section.PointerToRawData, delta, &candidate) ||
			candidate > size_ || length > size_ - candidate)
			return false;
		*offset = candidate;
		return true;
	}
	return false;
}

bool PeExportView::RvaStringEquals(DWORD rva, const char* expected) const noexcept
{
	if (expected == nullptr)
		return false;
	for (std::size_t index = 0;; ++index) {
		if (index > std::numeric_limits<DWORD>::max() - rva)
			return false;
		std::size_t offset = 0;
		if (!RvaToOffset(rva + static_cast<DWORD>(index), 1, &offset))
			return false;
		char actual = '\0';
		if (!ReadBytes(offset, &actual, sizeof(actual)))
			return false;
		if (actual != expected[index])
			return false;
		if (actual == '\0')
			return true;
	}
}

bool PeExportView::FindNamedExport(
	const char* name,
	DWORD* functionRva,
	DWORD* addressTableEntryRva) const noexcept
{
	if (!valid_ || name == nullptr ||
		(functionRva == nullptr && addressTableEntryRva == nullptr))
		return false;

	std::size_t exportOffset = 0;
	if (!RvaToOffset(exportDirectory_.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY),
			&exportOffset))
		return false;
	IMAGE_EXPORT_DIRECTORY directory{};
	if (!ReadBytes(exportOffset, &directory, sizeof(directory)) ||
		directory.NumberOfFunctions == 0 || directory.NumberOfNames == 0)
		return false;

	std::size_t functionBytes = 0;
	std::size_t nameBytes = 0;
	std::size_t ordinalBytes = 0;
	if (!CheckedMultiply(directory.NumberOfFunctions, sizeof(DWORD), &functionBytes) ||
		!CheckedMultiply(directory.NumberOfNames, sizeof(DWORD), &nameBytes) ||
		!CheckedMultiply(directory.NumberOfNames, sizeof(WORD), &ordinalBytes))
		return false;

	std::size_t functionsOffset = 0;
	std::size_t namesOffset = 0;
	std::size_t ordinalsOffset = 0;
	if (!RvaToOffset(directory.AddressOfFunctions, functionBytes, &functionsOffset) ||
		!RvaToOffset(directory.AddressOfNames, nameBytes, &namesOffset) ||
		!RvaToOffset(directory.AddressOfNameOrdinals, ordinalBytes, &ordinalsOffset))
		return false;

	for (DWORD index = 0; index < directory.NumberOfNames; ++index) {
		DWORD nameRva = 0;
		if (!ReadBytes(
				namesOffset + static_cast<std::size_t>(index) * sizeof(nameRva),
				&nameRva,
				sizeof(nameRva)))
			return false;
		if (!RvaStringEquals(nameRva, name))
			continue;

		WORD ordinal = 0;
		if (!ReadBytes(
				ordinalsOffset + static_cast<std::size_t>(index) * sizeof(ordinal),
				&ordinal,
				sizeof(ordinal)) ||
			ordinal >= directory.NumberOfFunctions)
			return false;

		DWORD resolvedRva = 0;
		std::size_t const functionOffset =
			functionsOffset + static_cast<std::size_t>(ordinal) * sizeof(resolvedRva);
		if (!ReadBytes(functionOffset, &resolvedRva, sizeof(resolvedRva)) ||
			resolvedRva == 0)
			return false;

		std::uint64_t const exportEnd =
			static_cast<std::uint64_t>(exportDirectory_.VirtualAddress) +
			exportDirectory_.Size;
		if (resolvedRva >= exportDirectory_.VirtualAddress && resolvedRva < exportEnd)
			return false;
		std::size_t ignored = 0;
		if (!RvaToOffset(resolvedRva, 1, &ignored))
			return false;

		if (functionRva != nullptr)
			*functionRva = resolvedRva;
		if (addressTableEntryRva != nullptr) {
			std::uint64_t const slotRva =
				static_cast<std::uint64_t>(directory.AddressOfFunctions) +
				static_cast<std::uint64_t>(ordinal) * sizeof(DWORD);
			if (slotRva > std::numeric_limits<DWORD>::max())
				return false;
			*addressTableEntryRva = static_cast<DWORD>(slotRva);
		}
		return true;
	}
	return false;
}

bool PeExportView::FindFunctionRva(
	const char* name,
	DWORD* functionRva) const noexcept
{
	return FindNamedExport(name, functionRva, nullptr);
}

bool PeExportView::FindAddressTableEntryRva(
	const char* name,
	DWORD* addressTableEntryRva) const noexcept
{
	return FindNamedExport(name, nullptr, addressTableEntryRva);
}

bool QueryMappedModuleSize(HMODULE module, std::size_t* size) noexcept
{
	if (module == nullptr || size == nullptr)
		return false;

	std::uintptr_t const base = reinterpret_cast<std::uintptr_t>(module);
	std::uintptr_t cursor = base;
	std::uintptr_t end = base;
	for (;;) {
		MEMORY_BASIC_INFORMATION memory{};
		if (::VirtualQuery(
				reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) == 0)
			return false;
		if (memory.AllocationBase != module)
			break;
		std::uintptr_t const regionBase =
			reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
		if (memory.RegionSize == 0 ||
			memory.RegionSize > std::numeric_limits<std::uintptr_t>::max() - regionBase)
			return false;
		std::uintptr_t const regionEnd = regionBase + memory.RegionSize;
		if (regionEnd <= cursor)
			return false;
		end = (std::max)(end, regionEnd);
		cursor = regionEnd;
	}
	if (end <= base || end - base > std::numeric_limits<std::size_t>::max())
		return false;
	*size = static_cast<std::size_t>(end - base);
	return true;
}

} // namespace renderer
