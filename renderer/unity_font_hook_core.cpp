#include "unity_font_hook_core.h"

#include "freetype_runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <set>

namespace renderer { namespace unity {
namespace {

constexpr unsigned char kPixelModeGray = 2;
constexpr unsigned char kPixelModeLcd = 5;
constexpr unsigned char kPixelModeLcdVertical = 6;
constexpr std::uint64_t kMaximumBitmapBytes = 64ULL * 1024ULL * 1024ULL;

bool EqualOrdinalIgnoreCase(
	const std::wstring& left,
	const std::wstring& right) noexcept
{
	if (left.size() != right.size() ||
		left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
		return false;
	return CompareStringOrdinal(
		left.data(), static_cast<int>(left.size()),
		right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool NormalizeAbsolutePath(
	const wchar_t* value,
	std::wstring& normalized)
{
	normalized.clear();
	if (value == nullptr || *value == L'\0')
		return false;
	normalized.assign(value);
	std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
	if (normalized.compare(0, 4, L"\\\\?\\") == 0)
	{
		if (normalized.compare(4, 4, L"UNC\\") == 0)
			normalized.replace(0, 8, L"\\\\");
		else
			normalized.erase(0, 4);
	}
	bool const driveAbsolute = normalized.size() >= 3 &&
		((normalized[0] >= L'A' && normalized[0] <= L'Z') ||
		 (normalized[0] >= L'a' && normalized[0] <= L'z')) &&
		normalized[1] == L':' && normalized[2] == L'\\';
	bool const uncAbsolute = normalized.size() >= 3 &&
		normalized[0] == L'\\' && normalized[1] == L'\\' &&
		normalized[2] != L'\\';
	return driveAbsolute || uncAbsolute;
}

std::vector<std::wstring> PathsForFamily(
	const std::vector<InstalledFontFace>& installedFonts,
	const std::wstring& family)
{
	std::vector<std::wstring> paths;
	for (const InstalledFontFace& font : installedFonts)
	{
		if (!EqualOrdinalIgnoreCase(font.family, family))
			continue;
		std::wstring normalized;
		if (!NormalizeAbsolutePath(font.filePath.c_str(), normalized))
			continue;
		if (std::none_of(paths.begin(), paths.end(), [&](const std::wstring& path) {
				return EqualOrdinalIgnoreCase(path, normalized);
			}))
			paths.push_back(std::move(normalized));
	}
	return paths;
}

constexpr std::array<unsigned char, 32> kPublicPrefixLegacy{{
	0x48,0x89,0x6C,0x24,0x18,0x56,0x48,0x83,0xEC,0x20,0x8B,0xEA,0x48,0x8B,0xF1,0x48,
	0x85,0xC9,0x0F,0x84,0x0F,0x01,0x00,0x00,0x48,0x8B,0x41,0x08,0x48,0x85,0xC0,0x0F,
}};
constexpr std::array<unsigned char, 32> kPublicPrefix2020{{
	0x48,0x89,0x6C,0x24,0x18,0x56,0x48,0x83,0xEC,0x20,0x8B,0xEA,0x48,0x8B,0xF1,0x48,
	0x85,0xC9,0x0F,0x84,0x22,0x01,0x00,0x00,0x48,0x8B,0x41,0x08,0x48,0x85,0xC0,0x0F,
}};
constexpr std::array<unsigned char, 32> kPublicPrefixWrapper{{
	0x48,0x85,0xC9,0x74,0x1F,0x48,0x8B,0x41,0x08,0x48,0x85,0xC0,0x74,0x16,0x48,0x8B,
	0x80,0x90,0x00,0x00,0x00,0x44,0x8B,0xC2,0x48,0x8B,0xD1,0x48,0x8B,0x48,0x08,0xE9,
}};
constexpr std::array<unsigned char, 32> kInternalPrefixX64{{
	0x4C,0x8B,0xDC,0x49,0x89,0x5B,0x08,0x49,0x89,0x6B,0x18,0x56,0x57,0x41,0x54,0x41,
	0x56,0x41,0x57,0x48,0x83,0xEC,0x40,0x48,0x8B,0x82,0xF0,0x00,0x00,0x00,0x45,0x33,
}};
constexpr std::array<unsigned char, 32> kInternalPrefixX86{{
	0x55,0x8B,0xEC,0x83,0xEC,0x1C,0x53,0x56,0x57,0x8B,0xFA,0x8B,0xD1,0x89,0x55,0xFC,
	0x8B,0x87,0x9C,0x00,0x00,0x00,0x8B,0x77,0x04,0xF7,0x40,0x28,0x00,0x00,0x10,0x00,
}};

constexpr AdapterDescriptor kProductionDescriptors[] = {
	{"unity-2018.4.32-x64",0x8664,0x60240965,0x01731000,
	 {{0xD5,0xC2,0x9A,0x59,0xCE,0x81,0x6E,0x4C,0xBA,0xA8,0x20,0x30,0xB6,0xAC,0xFB,0x9B}},1,0x00C633D0,RenderAbi::publicRender,&kPublicPrefixLegacy},
	{"unity-2019.4.9-x64",0x8664,0x5F3B44F7,0x01996000,
	 {{0x6F,0x2D,0xF0,0x51,0x37,0x70,0xC3,0x48,0x97,0x01,0xAF,0x4F,0xD6,0x8A,0xDC,0x70}},1,0x00E2AD80,RenderAbi::publicRender,&kPublicPrefixLegacy},
	{"unity-2019.4.41-x64",0x8664,0x68ED0247,0x019DB000,
	 {{0xF2,0x4E,0x36,0x83,0xDD,0x88,0x07,0x4D,0xA6,0x7A,0xDF,0xF6,0xDF,0x4B,0x04,0xBF}},1,0x00E49DE0,RenderAbi::publicRender,&kPublicPrefixLegacy},
	{"unity-2020.3.34-x64",0x8664,0x626E7CA6,0x01BE3000,
	 {{0x7E,0x65,0x5B,0x3F,0x92,0xED,0x2D,0x4B,0xBA,0x7D,0xCC,0xE5,0x01,0x35,0x19,0x82}},1,0x00F5C220,RenderAbi::publicRender,&kPublicPrefix2020},
	{"unity-2020.3.37-x64",0x8664,0x62C42C65,0x01BEE000,
	 {{0xEC,0xCD,0x3D,0x29,0xE0,0x5C,0x9E,0x45,0xA6,0x83,0x39,0x12,0x6C,0xCA,0x0D,0xCC}},1,0x00F62AB0,RenderAbi::publicRender,&kPublicPrefix2020},
	{"unity-2020.3.40-x64",0x8664,0x63219AD9,0x01C07000,
	 {{0x3E,0x66,0xD2,0xB0,0x5A,0xB7,0xB5,0x45,0x85,0x13,0x2E,0xE9,0x7A,0x7D,0x48,0x47}},1,0x00F6B430,RenderAbi::publicRender,&kPublicPrefixWrapper},
	{"unity-2021.1.29-x64",0x8664,0x68AC7825,0x01BB9000,
	 {{0xAC,0x3B,0x71,0x1B,0xAD,0x45,0x55,0x49,0x8C,0x03,0xC0,0x0A,0x27,0x34,0x9C,0x64}},1,0x00F33EF0,RenderAbi::publicRender,&kPublicPrefix2020},
	{"unity-2021.3.38-x64",0x8664,0x6630DCDF,0x01D04000,
	 {{0x1A,0xDF,0x64,0x56,0xAA,0xB6,0xC6,0x45,0xA4,0x0F,0xE4,0xD5,0xD6,0x3E,0x9B,0xEB}},1,0x0101F790,RenderAbi::publicRender,&kPublicPrefixWrapper},
	{"unity-2022.3.2-x64",0x8664,0x647FA560,0x01E24000,
	 {{0x40,0x65,0x3A,0x2B,0x29,0x82,0xEF,0x45,0xBF,0xB0,0x1C,0x66,0xA4,0xED,0x63,0x33}},1,0x012AD950,RenderAbi::internalRender,&kInternalPrefixX64},
	{"unity-2022.3.4-x64",0x8664,0x6493710F,0x01E25000,
	 {{0x06,0xC5,0x32,0x5F,0x14,0xEC,0xEC,0x48,0xAC,0xB8,0x00,0xE9,0x58,0xA4,0x3D,0x36}},1,0x012AE960,RenderAbi::internalRender,&kInternalPrefixX64},
	{"unity-2022.3.10-x64",0x8664,0x6501108B,0x01E2F000,
	 {{0x99,0x51,0x84,0x61,0x3D,0x0F,0x67,0x41,0x84,0x8C,0x2B,0x83,0x03,0x15,0x02,0x03}},1,0x012AA210,RenderAbi::internalRender,&kInternalPrefixX64},
	{"unity-2022.3.62f2-x64",0x8664,0x68D67E4D,0x01E7C000,
	 {{0xBE,0xBF,0x1E,0xA6,0xCD,0x39,0x79,0x4C,0xB4,0x64,0xF3,0x88,0x53,0xDB,0x30,0xB5}},1,0x012E2CE0,RenderAbi::internalRender,&kInternalPrefixX64},
	{"unity-2022.3.62f3-x86",0x014C,0x68FB5DE8,0x016A8000,
	 {{0x80,0x71,0x7F,0xE2,0x81,0x6B,0x30,0x40,0xB6,0x34,0x2E,0x0C,0xD5,0xE5,0x76,0x33}},1,0x00DFC1E0,RenderAbi::internalRender,&kInternalPrefixX86},
	{"unity-2022.3.62f3-x64",0x8664,0x68FB626F,0x01E65000,
	 {{0x85,0x43,0x52,0xC6,0x31,0x89,0x9F,0x4F,0x94,0x93,0xD4,0xDD,0xBC,0x52,0x6B,0xCB}},1,0x012CAAE0,RenderAbi::internalRender,&kInternalPrefixX64},
	{"unity-6000.0.50-x64",0x8664,0x68DE7994,0x0210F000,
	 {{0x50,0xCB,0xE4,0x59,0x8B,0x21,0xFA,0x41,0xBA,0xB5,0xFD,0x6B,0xE0,0x8A,0x52,0xB3}},1,0x014622C0,RenderAbi::internalRender,&kInternalPrefixX64},
	{"unity-6000.0.61-x64",0x8664,0x68F891E5,0x0211E000,
	 {{0xF3,0xE9,0xCA,0x21,0xF7,0x18,0x37,0x4D,0x8F,0x8A,0xF7,0x84,0x15,0x6A,0x20,0xF8}},1,0x0146D9D0,RenderAbi::internalRender,&kInternalPrefixX64},
	{"unity-6000.0.62-x64",0x8664,0x6902F1F8,0x0210D000,
	 {{0x01,0x5D,0x60,0x95,0x0D,0xCD,0xDF,0x4F,0x98,0x1E,0xC2,0x4B,0x13,0x06,0x48,0xE2}},1,0x01464760,RenderAbi::internalRender,&kInternalPrefixX64},
	{"unity-6000.3.21-x64",0x8664,0x6A607C81,0x02457000,
	 {{0x64,0xA7,0x50,0xE1,0xE7,0xC9,0xF0,0x44,0x89,0x07,0xDE,0x2D,0xB3,0x87,0x2D,0x5D}},1,0x0166D120,RenderAbi::internalRender,&kInternalPrefixX64},
};

bool CheckedAdd(std::size_t left, std::size_t right, std::size_t* result) noexcept
{
	if (result == nullptr || right > std::numeric_limits<std::size_t>::max() - left)
		return false;
	*result = left + right;
	return true;
}

bool IsReadableProtection(DWORD protection) noexcept
{
	switch (protection & 0xffu)
	{
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

bool IsWritableProtection(DWORD protection) noexcept
{
	switch (protection & 0xffu)
	{
	case PAGE_READWRITE:
	case PAGE_WRITECOPY:
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
		return true;
	default:
		return false;
	}
}

bool IsExecutableProtection(DWORD protection) noexcept
{
	switch (protection & 0xffu)
	{
	case PAGE_EXECUTE:
	case PAGE_EXECUTE_READ:
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
		return true;
	default:
		return false;
	}
}

template <typename Predicate>
bool IsMemoryRange(
	const void* address,
	std::size_t length,
	Predicate predicate) noexcept
{
	if (address == nullptr || length == 0)
		return false;
	std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(address);
	if (length > std::numeric_limits<std::uintptr_t>::max() - cursor)
		return false;
	std::uintptr_t const end = cursor + length;
	while (cursor < end)
	{
		MEMORY_BASIC_INFORMATION memory{};
		if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) == 0 ||
			memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) != 0 ||
			!predicate(memory.Protect))
			return false;
		std::uintptr_t const base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
		if (memory.RegionSize == 0 ||
			memory.RegionSize > std::numeric_limits<std::uintptr_t>::max() - base)
			return false;
		std::uintptr_t const regionEnd = base + memory.RegionSize;
		if (regionEnd <= cursor)
			return false;
		cursor = (std::min)(regionEnd, end);
	}
	return true;
}

class MappedImageView final
{
public:
	MappedImageView(const void* bytes, std::size_t size) noexcept
		: bytes_(static_cast<const unsigned char*>(bytes)), size_(size)
	{
		valid_ = Initialize();
	}

	bool valid() const noexcept { return valid_; }
	WORD machine() const noexcept { return machine_; }
	DWORD timestamp() const noexcept { return timestamp_; }
	DWORD image_size() const noexcept { return imageSize_; }

	bool CopyRva(DWORD rva, void* destination, std::size_t length) const noexcept
	{
		return Read(static_cast<std::size_t>(rva), destination, length);
	}

	bool IsExecutableRva(DWORD rva, std::size_t length) const noexcept
	{
		if (!valid_ || length == 0)
			return false;
		for (WORD index = 0; index < sectionCount_; ++index)
		{
			IMAGE_SECTION_HEADER section{};
			if (!Read(
				sectionTableOffset_ + static_cast<std::size_t>(index) * sizeof(section),
				&section, sizeof(section)))
				return false;
			std::uint64_t const start = section.VirtualAddress;
			std::uint64_t const span = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
			std::uint64_t const end = start + span;
			std::uint64_t const requestedEnd = static_cast<std::uint64_t>(rva) + length;
			if (rva >= start && requestedEnd <= end)
				return (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
		}
		return false;
	}

	bool FindFileImportSlots(FileImportSlots* slots) const noexcept
	{
		if (!valid_ || slots == nullptr || importDirectory_.VirtualAddress == 0 ||
			importDirectory_.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
			(is64Bit_ ? sizeof(void*) != 8 : sizeof(void*) != 4))
			return false;
		FileImportSlots found{};
		std::size_t const descriptorCount =
			importDirectory_.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
		bool terminated = false;
		for (std::size_t descriptorIndex = 0;
			descriptorIndex < descriptorCount; ++descriptorIndex)
		{
			IMAGE_IMPORT_DESCRIPTOR descriptor{};
			if (!CopyRva(
				importDirectory_.VirtualAddress + static_cast<DWORD>(
					descriptorIndex * sizeof(descriptor)),
				&descriptor, sizeof(descriptor)))
				return false;
			if (descriptor.Name == 0 && descriptor.FirstThunk == 0 &&
				descriptor.OriginalFirstThunk == 0)
			{
				terminated = true;
				break;
			}
			std::string moduleName;
			if (!ReadAnsiString(descriptor.Name, 128, moduleName))
				return false;
			if (_stricmp(moduleName.c_str(), "kernel32.dll") != 0 &&
				_stricmp(moduleName.c_str(), "kernelbase.dll") != 0)
				continue;
			DWORD const nameThunk = descriptor.OriginalFirstThunk != 0
				? descriptor.OriginalFirstThunk
				: descriptor.FirstThunk;
			if (!FindNamedFileImports(
				nameThunk, descriptor.FirstThunk, &found))
				return false;
		}
		if (!terminated || found.createFileA == nullptr ||
			found.createFileW == nullptr)
			return false;
		*slots = found;
		return true;
	}

	bool ReadCodeView(
		std::array<unsigned char, 16>* guid,
		DWORD* age) const noexcept
	{
		if (!valid_ || guid == nullptr || age == nullptr ||
			debugDirectory_.VirtualAddress == 0 ||
			debugDirectory_.Size < sizeof(IMAGE_DEBUG_DIRECTORY))
			return false;
		std::size_t const count =
			debugDirectory_.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
		for (std::size_t index = 0; index < count; ++index)
		{
			IMAGE_DEBUG_DIRECTORY entry{};
			if (!CopyRva(
				debugDirectory_.VirtualAddress +
					static_cast<DWORD>(index * sizeof(entry)),
				&entry, sizeof(entry)))
				return false;
			if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW ||
				entry.SizeOfData < 24 || entry.AddressOfRawData == 0)
				continue;
			std::array<unsigned char, 24> codeView{};
			if (!CopyRva(entry.AddressOfRawData, codeView.data(), codeView.size()) ||
				std::memcmp(codeView.data(), "RSDS", 4) != 0)
				continue;
			std::copy_n(codeView.data() + 4, guid->size(), guid->begin());
			std::memcpy(age, codeView.data() + 20, sizeof(*age));
			return true;
		}
		return false;
	}

private:
	bool ReadAnsiString(
		DWORD rva,
		std::size_t maximumLength,
		std::string& value) const noexcept
	{
		try
		{
			value.clear();
			for (std::size_t index = 0; index < maximumLength; ++index)
			{
				char character = 0;
				if (rva > (std::numeric_limits<DWORD>::max)() - index ||
					!CopyRva(rva + static_cast<DWORD>(index), &character, 1))
					return false;
				if (character == '\0')
					return !value.empty();
				value.push_back(character);
			}
		}
		catch (...)
		{
		}
		value.clear();
		return false;
	}

	bool FindNamedFileImports(
		DWORD nameThunkRva,
		DWORD addressThunkRva,
		FileImportSlots* slots) const noexcept
	{
		if (nameThunkRva == 0 || addressThunkRva == 0 || slots == nullptr)
			return false;
		std::size_t const thunkSize = is64Bit_
			? sizeof(IMAGE_THUNK_DATA64)
			: sizeof(IMAGE_THUNK_DATA32);
		constexpr std::size_t kMaximumImportsPerModule = 65536;
		for (std::size_t index = 0; index < kMaximumImportsPerModule; ++index)
		{
			std::uint64_t const byteOffset =
				static_cast<std::uint64_t>(index) * thunkSize;
			if (byteOffset > (std::numeric_limits<DWORD>::max)() ||
				nameThunkRva > (std::numeric_limits<DWORD>::max)() - byteOffset ||
				addressThunkRva > (std::numeric_limits<DWORD>::max)() - byteOffset)
				return false;
			DWORD const nameRva = nameThunkRva + static_cast<DWORD>(byteOffset);
			DWORD const slotRva = addressThunkRva + static_cast<DWORD>(byteOffset);
			std::uint64_t importValue = 0;
			if (is64Bit_)
			{
				IMAGE_THUNK_DATA64 thunk{};
				if (!CopyRva(nameRva, &thunk, sizeof(thunk)))
					return false;
				importValue = thunk.u1.AddressOfData;
			}
			else
			{
				IMAGE_THUNK_DATA32 thunk{};
				if (!CopyRva(nameRva, &thunk, sizeof(thunk)))
					return false;
				importValue = thunk.u1.AddressOfData;
			}
			if (importValue == 0)
				return true;
			std::uint64_t const ordinalFlag = is64Bit_
				? IMAGE_ORDINAL_FLAG64
				: IMAGE_ORDINAL_FLAG32;
			if ((importValue & ordinalFlag) != 0)
				continue;
			if (importValue > (std::numeric_limits<DWORD>::max)())
				return false;
			std::string importName;
			DWORD const importByName = static_cast<DWORD>(importValue);
			if (importByName > (std::numeric_limits<DWORD>::max)() -
				FIELD_OFFSET(IMAGE_IMPORT_BY_NAME, Name) ||
				!ReadAnsiString(
					importByName + FIELD_OFFSET(IMAGE_IMPORT_BY_NAME, Name),
					128, importName))
				return false;
			void** const slot = static_cast<void**>(MutableRva(slotRva, thunkSize));
			if (slot == nullptr)
				return false;
			if (_stricmp(importName.c_str(), "CreateFileA") == 0)
			{
				if (slots->createFileA != nullptr)
					return false;
				slots->createFileA = slot;
			}
			else if (_stricmp(importName.c_str(), "CreateFileW") == 0)
			{
				if (slots->createFileW != nullptr)
					return false;
				slots->createFileW = slot;
			}
		}
		return false;
	}

	void* MutableRva(DWORD rva, std::size_t length) const noexcept
	{
		if (bytes_ == nullptr || length == 0 || rva > size_ ||
			length > size_ - rva)
			return nullptr;
		std::uintptr_t const base = reinterpret_cast<std::uintptr_t>(bytes_);
		if (rva > (std::numeric_limits<std::uintptr_t>::max)() - base)
			return nullptr;
		void* const address = reinterpret_cast<void*>(base + rva);
		return IsMemoryRange(address, length, IsReadableProtection)
			? address
			: nullptr;
	}

	bool Read(std::size_t offset, void* destination, std::size_t length) const noexcept
	{
		if (bytes_ == nullptr || destination == nullptr || length == 0 ||
			offset > size_ || length > size_ - offset)
			return false;
		std::uintptr_t const base = reinterpret_cast<std::uintptr_t>(bytes_);
		if (offset > std::numeric_limits<std::uintptr_t>::max() - base)
			return false;
		const void* const source = reinterpret_cast<const void*>(base + offset);
		if (!IsMemoryRange(source, length, IsReadableProtection))
			return false;
		std::memcpy(destination, source, length);
		return true;
	}

	bool Initialize() noexcept
	{
		IMAGE_DOS_HEADER dos{};
		if (!Read(0, &dos, sizeof(dos)) ||
			dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
			return false;
		std::size_t const ntOffset = static_cast<std::size_t>(dos.e_lfanew);
		DWORD signature = 0;
		if (!Read(ntOffset, &signature, sizeof(signature)) ||
			signature != IMAGE_NT_SIGNATURE)
			return false;
		std::size_t fileHeaderOffset = 0;
		if (!CheckedAdd(ntOffset, sizeof(signature), &fileHeaderOffset))
			return false;
		IMAGE_FILE_HEADER file{};
		if (!Read(fileHeaderOffset, &file, sizeof(file)) ||
			file.NumberOfSections == 0 ||
			(file.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == 0)
			return false;
		machine_ = file.Machine;
		timestamp_ = file.TimeDateStamp;
		std::size_t optionalOffset = 0;
		if (!CheckedAdd(fileHeaderOffset, sizeof(file), &optionalOffset))
			return false;
		WORD magic = 0;
		if (!Read(optionalOffset, &magic, sizeof(magic)))
			return false;
		DWORD sizeOfHeaders = 0;
		if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
		{
			if (file.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32))
				return false;
			IMAGE_OPTIONAL_HEADER32 optional{};
			if (!Read(optionalOffset, &optional, sizeof(optional)) ||
				optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
				return false;
			imageSize_ = optional.SizeOfImage;
			is64Bit_ = false;
			sizeOfHeaders = optional.SizeOfHeaders;
			importDirectory_ = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			debugDirectory_ = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
		}
		else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		{
			if (file.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
				return false;
			IMAGE_OPTIONAL_HEADER64 optional{};
			if (!Read(optionalOffset, &optional, sizeof(optional)) ||
				optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
				return false;
			imageSize_ = optional.SizeOfImage;
			is64Bit_ = true;
			sizeOfHeaders = optional.SizeOfHeaders;
			importDirectory_ = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			debugDirectory_ = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
		}
		else
		{
			return false;
		}
		if (imageSize_ == 0 || imageSize_ > size_ || sizeOfHeaders == 0 ||
			sizeOfHeaders > imageSize_)
			return false;
		if (debugDirectory_.VirtualAddress == 0 || debugDirectory_.Size == 0 ||
			debugDirectory_.VirtualAddress >= imageSize_ ||
			debugDirectory_.Size > imageSize_ - debugDirectory_.VirtualAddress)
			return false;
		if (importDirectory_.VirtualAddress != 0 &&
			(importDirectory_.Size == 0 ||
			 importDirectory_.VirtualAddress >= imageSize_ ||
			 importDirectory_.Size > imageSize_ - importDirectory_.VirtualAddress))
			return false;
		sectionTableOffset_ = optionalOffset + file.SizeOfOptionalHeader;
		sectionCount_ = file.NumberOfSections;
		std::size_t const sectionBytes =
			static_cast<std::size_t>(sectionCount_) * sizeof(IMAGE_SECTION_HEADER);
		if (sectionTableOffset_ > sizeOfHeaders ||
			sectionBytes > sizeOfHeaders - sectionTableOffset_ ||
			sectionTableOffset_ > size_ || sectionBytes > size_ - sectionTableOffset_)
			return false;
		return true;
	}

	const unsigned char* bytes_ = nullptr;
	std::size_t size_ = 0;
	bool valid_ = false;
	WORD machine_ = 0;
	WORD sectionCount_ = 0;
	DWORD timestamp_ = 0;
	DWORD imageSize_ = 0;
	bool is64Bit_ = false;
	std::size_t sectionTableOffset_ = 0;
	IMAGE_DATA_DIRECTORY importDirectory_{};
	IMAGE_DATA_DIRECTORY debugDirectory_{};
};

} // namespace

std::shared_ptr<const FontFileRedirectTable> FontFileRedirectTable::Build(
	const std::vector<InstalledFontFace>& installedFonts,
	const font_substitution::Snapshot& substitutions) noexcept
{
	try
	{
		std::vector<std::pair<std::wstring, std::wstring>> redirects;
		std::set<std::wstring> ambiguousSources;
		for (const font_substitution::Rule& rule : substitutions.rules())
		{
			font_substitution::Resolution const resolution = substitutions.Resolve(
				font_substitution::Request(
					rule.sourceFamily,
					rule.charsetSpecific ? rule.charset : DEFAULT_CHARSET));
			if (!resolution.matched ||
				resolution.status != font_substitution::ResolutionStatus::applied)
				continue;
			std::vector<std::wstring> const sources = PathsForFamily(
				installedFonts, rule.sourceFamily);
			std::vector<std::wstring> const replacements = PathsForFamily(
				installedFonts, resolution.family);
			if (sources.empty() || replacements.size() != 1)
				continue;
			for (const std::wstring& source : sources)
			{
				if (EqualOrdinalIgnoreCase(source, replacements.front()))
					continue;
				auto existing = std::find_if(
					redirects.begin(), redirects.end(),
					[&](const auto& redirect) {
						return EqualOrdinalIgnoreCase(redirect.first, source);
					});
				if (existing == redirects.end())
				{
					redirects.emplace_back(source, replacements.front());
				}
				else if (!EqualOrdinalIgnoreCase(
					existing->second, replacements.front()))
				{
					ambiguousSources.insert(source);
				}
			}
		}
		redirects.erase(
			std::remove_if(
				redirects.begin(), redirects.end(),
				[&](const auto& redirect) {
					return std::any_of(
						ambiguousSources.begin(), ambiguousSources.end(),
						[&](const std::wstring& source) {
							return EqualOrdinalIgnoreCase(source, redirect.first);
						});
				}),
			redirects.end());
		return std::shared_ptr<const FontFileRedirectTable>(
			new FontFileRedirectTable(std::move(redirects)));
	}
	catch (...)
	{
		return {};
	}
}

bool FontFileRedirectTable::Resolve(
	const wchar_t* requestedPath,
	std::wstring& replacementPath) const noexcept
{
	try
	{
		replacementPath.clear();
		std::wstring normalized;
		if (!NormalizeAbsolutePath(requestedPath, normalized))
			return false;
		for (const auto& redirect : redirects_)
		{
			if (EqualOrdinalIgnoreCase(redirect.first, normalized))
			{
				replacementPath = redirect.second;
				return true;
			}
		}
	}
	catch (...)
	{
		replacementPath.clear();
	}
	return false;
}

bool ResolveAdapter(
	const void* mappedImage,
	std::size_t mappedSize,
	const AdapterDescriptor* descriptors,
	std::size_t descriptorCount,
	ResolvedAdapter* resolved) noexcept
{
	if (descriptors == nullptr || descriptorCount == 0 || resolved == nullptr)
		return false;
	MappedImageView const image(mappedImage, mappedSize);
	if (!image.valid())
		return false;
	std::array<unsigned char, 16> guid{};
	DWORD age = 0;
	if (!image.ReadCodeView(&guid, &age))
		return false;

	const AdapterDescriptor* match = nullptr;
	for (std::size_t index = 0; index < descriptorCount; ++index)
	{
		const AdapterDescriptor& candidate = descriptors[index];
		if (candidate.machine != image.machine() ||
			candidate.timestamp != image.timestamp() ||
			candidate.imageSize != image.image_size() ||
			candidate.pdbGuid != guid || candidate.pdbAge != age)
			continue;
		if (match != nullptr || candidate.targetPrefix == nullptr ||
			!image.IsExecutableRva(candidate.targetRva, candidate.targetPrefix->size()))
			return false;
		std::array<unsigned char, 32> prefix{};
		if (!image.CopyRva(candidate.targetRva, prefix.data(), prefix.size()) ||
			prefix != *candidate.targetPrefix)
			return false;
		match = &candidate;
	}
	if (match == nullptr)
		return false;
	resolved->name = match->name;
	resolved->targetRva = match->targetRva;
	resolved->abi = match->abi;
	return true;
}

bool ResolveFileImportSlots(
	void* mappedImage,
	std::size_t mappedSize,
	FileImportSlots* slots) noexcept
{
	if (slots == nullptr)
		return false;
	*slots = {};
	MappedImageView const image(mappedImage, mappedSize);
	return image.FindFileImportSlots(slots);
}

bool ReadFileImportTarget(
	void** slot,
	void** target) noexcept
{
	if (slot == nullptr || target == nullptr)
		return false;
	*target = nullptr;
	if (!IsMemoryRange(slot, sizeof(void*), IsReadableProtection))
		return false;
	std::memcpy(target, slot, sizeof(void*));
	return *target != nullptr;
}

const AdapterDescriptor* ProductionAdapterDescriptors(std::size_t* count) noexcept
{
	if (count != nullptr)
		*count = _countof(kProductionDescriptors);
	return kProductionDescriptors;
}

bool IsExecutableMemoryRange(const void* address, std::size_t length) noexcept
{
	return IsMemoryRange(address, length, IsExecutableProtection);
}

bool ApplyCoverage(
	unsigned char* buffer,
	int pitch,
	unsigned int rows,
	unsigned int width,
	unsigned char pixelMode,
	const UnityCoverageLut& lut) noexcept
{
	if (buffer == nullptr || rows == 0 || width == 0 ||
		(pixelMode != kPixelModeGray && pixelMode != kPixelModeLcd &&
		 pixelMode != kPixelModeLcdVertical))
		return false;
	std::int64_t const signedPitch = pitch;
	std::uint64_t const absolutePitch = signedPitch < 0
		? static_cast<std::uint64_t>(-signedPitch)
		: static_cast<std::uint64_t>(signedPitch);
	std::uint64_t const total = absolutePitch * rows;
	if (absolutePitch == 0 || width > absolutePitch ||
		total > kMaximumBitmapBytes ||
		total > std::numeric_limits<std::size_t>::max() ||
		!IsMemoryRange(buffer, static_cast<std::size_t>(total), IsWritableProtection))
		return false;

	for (unsigned int row = 0; row < rows; ++row)
	{
		freetype::BitmapRowView const rowView = freetype::CheckedBitmapRow(
			buffer, pitch, rows, row, width);
		if (!rowView)
			return false;
		unsigned char* const bytes = const_cast<unsigned char*>(rowView.data);
		for (unsigned int column = 0; column < width; ++column)
		{
			if (pixelMode == kPixelModeGray)
				bytes[column] = lut.gray[bytes[column]];
			else if (pixelMode == kPixelModeLcd)
				bytes[column] = lut.rgb[column % 3][bytes[column]];
			else
				bytes[column] = lut.rgb[row % 3][bytes[column]];
		}
	}
	return true;
}

}} // namespace renderer::unity
