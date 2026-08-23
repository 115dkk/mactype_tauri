#include "../../../renderer/dll.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kNtOffset = 0x80;
constexpr std::size_t kRawSectionOffset = 0x200;
constexpr DWORD kSectionRva = 0x1000;

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

template <typename T>
void Write(std::vector<unsigned char>& image, std::size_t offset, const T& value)
{
    Require(offset <= image.size() && sizeof(value) <= image.size() - offset,
            "the synthetic PE fixture write must stay in range");
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

std::size_t RawOffset(DWORD rva)
{
    return kRawSectionOffset + static_cast<std::size_t>(rva - kSectionRva);
}

std::size_t SectionHeaderOffset()
{
    return kNtOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
           sizeof(IMAGE_OPTIONAL_HEADER32);
}

std::vector<unsigned char> MakeRawImage()
{
    std::vector<unsigned char> image(0x600, 0);

    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = static_cast<LONG>(kNtOffset);
    Write(image, 0, dos);

    DWORD signature = IMAGE_NT_SIGNATURE;
    Write(image, kNtOffset, signature);

    IMAGE_FILE_HEADER file{};
    file.Machine = IMAGE_FILE_MACHINE_I386;
    file.NumberOfSections = 1;
    file.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
    file.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_DLL;
    Write(image, kNtOffset + sizeof(signature), file);

    IMAGE_OPTIONAL_HEADER32 optional{};
    optional.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    optional.SectionAlignment = 0x1000;
    optional.FileAlignment = 0x200;
    optional.SizeOfImage = 0x2000;
    optional.SizeOfHeaders = 0x200;
    optional.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress = 0x1000;
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size = 0x100;
    Write(image, kNtOffset + sizeof(signature) + sizeof(file), optional);

    IMAGE_SECTION_HEADER section{};
    std::memcpy(section.Name, ".edata", 6);
    section.Misc.VirtualSize = 0x400;
    section.VirtualAddress = kSectionRva;
    section.SizeOfRawData = 0x400;
    section.PointerToRawData = static_cast<DWORD>(kRawSectionOffset);
    Write(image, SectionHeaderOffset(), section);

    IMAGE_EXPORT_DIRECTORY exports{};
    exports.Base = 1;
    exports.NumberOfFunctions = 1;
    exports.NumberOfNames = 1;
    exports.AddressOfFunctions = 0x1040;
    exports.AddressOfNames = 0x1044;
    exports.AddressOfNameOrdinals = 0x1048;
    Write(image, RawOffset(0x1000), exports);

    DWORD functionRva = 0x1200;
    DWORD nameRva = 0x1060;
    WORD ordinal = 0;
    Write(image, RawOffset(0x1040), functionRva);
    Write(image, RawOffset(0x1044), nameRva);
    Write(image, RawOffset(0x1048), ordinal);
    char const name[] = "LoadLibraryW";
    Require(RawOffset(nameRva) + sizeof(name) <= image.size(),
            "the synthetic export name must fit");
    std::memcpy(image.data() + RawOffset(nameRva), name, sizeof(name));
    image[RawOffset(functionRva)] = 0xC3;
    return image;
}

std::vector<unsigned char> MakeMappedImage(const std::vector<unsigned char>& raw)
{
    std::vector<unsigned char> mapped(0x2000, 0);
    std::memcpy(mapped.data(), raw.data(), 0x200);
    std::memcpy(mapped.data() + kSectionRva,
                raw.data() + kRawSectionOffset,
                0x400);
    return mapped;
}

} // namespace

int main()
{
    std::vector<unsigned char> raw = MakeRawImage();
    renderer::PeExportView rawView(
        raw.data(), raw.size(), renderer::PeImageLayout::rawFile);
    Require(rawView.valid(), "a bounded raw PE fixture must be accepted");
    DWORD rva = 0;
    Require(rawView.FindFunctionRva("LoadLibraryW", &rva) && rva == 0x1200,
            "the raw view must resolve the named function RVA");
    Require(rawView.FindAddressTableEntryRva("LoadLibraryW", &rva) && rva == 0x1040,
            "the raw view must expose the checked export address-table slot RVA");
    Require(!rawView.FindFunctionRva("Missing", &rva),
            "a missing export must fail without reading beyond the name table");

    std::vector<unsigned char> mapped = MakeMappedImage(raw);
    renderer::PeExportView mappedView(
        mapped.data(), mapped.size(), renderer::PeImageLayout::mappedImage);
    Require(mappedView.valid(), "a bounded loader-layout fixture must be accepted");
    Require(mappedView.FindFunctionRva("LoadLibraryW", &rva) && rva == 0x1200,
            "the mapped view must resolve the same function RVA");

    std::vector<unsigned char> truncated(sizeof(IMAGE_DOS_HEADER) - 1, 0);
    Require(!renderer::PeExportView(
                 truncated.data(), truncated.size(), renderer::PeImageLayout::rawFile)
                 .valid(),
            "a truncated DOS header must be rejected");

    std::vector<unsigned char> badNtOffset = raw;
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, badNtOffset.data(), sizeof(dos));
    dos.e_lfanew = static_cast<LONG>(badNtOffset.size());
    Write(badNtOffset, 0, dos);
    Require(!renderer::PeExportView(
                 badNtOffset.data(), badNtOffset.size(), renderer::PeImageLayout::rawFile)
                 .valid(),
            "an NT header outside the byte view must be rejected");

    std::vector<unsigned char> badSection = raw;
    IMAGE_SECTION_HEADER section{};
    std::memcpy(&section, badSection.data() + SectionHeaderOffset(), sizeof(section));
    section.PointerToRawData = 0x500;
    Write(badSection, SectionHeaderOffset(), section);
    Require(!renderer::PeExportView(
                 badSection.data(), badSection.size(), renderer::PeImageLayout::rawFile)
                 .valid(),
            "a section raw range outside the file must be rejected");

    std::vector<unsigned char> headerOverlap = raw;
    IMAGE_OPTIONAL_HEADER32 optional{};
    const std::size_t optionalOffset =
        kNtOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    std::memcpy(&optional, headerOverlap.data() + optionalOffset, sizeof(optional));
    optional.SizeOfHeaders = static_cast<DWORD>(SectionHeaderOffset() + 1);
    Write(headerOverlap, optionalOffset, optional);
    Require(!renderer::PeExportView(
                 headerOverlap.data(), headerOverlap.size(),
                 renderer::PeImageLayout::rawFile)
                 .valid(),
            "a section table extending beyond SizeOfHeaders must be rejected");

    std::vector<unsigned char> badArray = raw;
    IMAGE_EXPORT_DIRECTORY exports{};
    std::memcpy(&exports, badArray.data() + RawOffset(0x1000), sizeof(exports));
    exports.AddressOfNames = 0x13FF;
    Write(badArray, RawOffset(0x1000), exports);
    renderer::PeExportView badArrayView(
        badArray.data(), badArray.size(), renderer::PeImageLayout::rawFile);
    Require(badArrayView.valid() &&
                !badArrayView.FindFunctionRva("LoadLibraryW", &rva),
            "an export name array crossing its section boundary must be rejected");

    std::vector<unsigned char> forwarded = raw;
    DWORD forwarderRva = 0x1080;
    Write(forwarded, RawOffset(0x1040), forwarderRva);
    renderer::PeExportView forwarderView(
        forwarded.data(), forwarded.size(), renderer::PeImageLayout::rawFile);
    Require(forwarderView.valid() &&
                !forwarderView.FindFunctionRva("LoadLibraryW", &rva),
            "a forwarded export must not be mistaken for executable code");

    std::vector<unsigned char> unterminated = raw;
    std::fill(unterminated.begin() + RawOffset(0x1060), unterminated.end(), 'A');
    std::string expected(unterminated.size() - RawOffset(0x1060), 'A');
    renderer::PeExportView unterminatedView(
        unterminated.data(), unterminated.size(), renderer::PeImageLayout::rawFile);
    Require(unterminatedView.valid() &&
                !unterminatedView.FindFunctionRva(expected.c_str(), &rva),
            "an unterminated export name must stop at the mapped section boundary");

    std::size_t kernel32Size = 0;
    Require(renderer::QueryMappedModuleSize(
                GetModuleHandleW(L"kernel32.dll"), &kernel32Size) &&
                kernel32Size != 0,
            "a loader-mapped module must expose a bounded allocation span");

    std::cout << "Checked PE export view tests passed.\n";
    return 0;
}
