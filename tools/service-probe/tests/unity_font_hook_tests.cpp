#include "../../../renderer/unity_font_hook_core.h"
#include "../../../renderer/unity_font_catalog.h"
#include "../../../renderer/unity_font_evidence.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace {

#ifdef _WIN64
using NativeNtHeaders = IMAGE_NT_HEADERS64;
using NativeThunkData = IMAGE_THUNK_DATA64;
constexpr WORD kNativeMachine = IMAGE_FILE_MACHINE_AMD64;
constexpr WORD kNativeOptionalMagic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
#else
using NativeNtHeaders = IMAGE_NT_HEADERS32;
using NativeThunkData = IMAGE_THUNK_DATA32;
constexpr WORD kNativeMachine = IMAGE_FILE_MACHINE_I386;
constexpr WORD kNativeOptionalMagic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
#endif

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct Fixture final
{
    std::vector<unsigned char> image = std::vector<unsigned char>(0x4000);
    std::array<unsigned char, 16> guid{{
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
        0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F,
    }};
    std::array<unsigned char, 32> prefix{};
    renderer::unity::AdapterDescriptor descriptor{};

    Fixture()
    {
        for (std::size_t index = 0; index < prefix.size(); ++index)
            prefix[index] = static_cast<unsigned char>(0x40 + index);

        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;
        auto* nt = reinterpret_cast<NativeNtHeaders*>(image.data() + dos->e_lfanew);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = kNativeMachine;
        nt->FileHeader.NumberOfSections = 3;
        nt->FileHeader.TimeDateStamp = 0x12345678;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(nt->OptionalHeader);
        nt->FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;
        nt->OptionalHeader.Magic = kNativeOptionalMagic;
        nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(image.size());
        nt->OptionalHeader.SizeOfHeaders = 0x400;
        nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG] = {
            0x1000, sizeof(IMAGE_DEBUG_DIRECTORY)};
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {
            0x3000, static_cast<DWORD>(sizeof(IMAGE_IMPORT_DESCRIPTOR) * 2)};

        auto* sections = IMAGE_FIRST_SECTION(nt);
        std::memcpy(sections[0].Name, ".rdata", 6);
        sections[0].Misc.VirtualSize = 0x1000;
        sections[0].VirtualAddress = 0x1000;
        sections[0].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
        std::memcpy(sections[1].Name, ".text", 5);
        sections[1].Misc.VirtualSize = 0x1000;
        sections[1].VirtualAddress = 0x2000;
        sections[1].Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
        std::memcpy(sections[2].Name, ".idata", 6);
        sections[2].Misc.VirtualSize = 0x1000;
        sections[2].VirtualAddress = 0x3000;
        sections[2].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA |
            IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

        auto* debug = reinterpret_cast<IMAGE_DEBUG_DIRECTORY*>(image.data() + 0x1000);
        debug->Type = IMAGE_DEBUG_TYPE_CODEVIEW;
        debug->SizeOfData = 32;
        debug->AddressOfRawData = 0x1100;
        std::memcpy(image.data() + 0x1100, "RSDS", 4);
        std::memcpy(image.data() + 0x1104, guid.data(), guid.size());
        DWORD const age = 7;
        std::memcpy(image.data() + 0x1114, &age, sizeof(age));
        std::memcpy(image.data() + 0x2000, prefix.data(), prefix.size());

        auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            image.data() + 0x3000);
        imports[0].OriginalFirstThunk = 0x3100;
        imports[0].FirstThunk = 0x3200;
        imports[0].Name = 0x3300;
        std::memcpy(image.data() + 0x3300, "KERNEL32.dll", 13);
        auto* names = reinterpret_cast<NativeThunkData*>(image.data() + 0x3100);
        names[0].u1.AddressOfData = 0x3400;
        names[1].u1.AddressOfData = 0x3440;
        auto* addresses = reinterpret_cast<NativeThunkData*>(image.data() + 0x3200);
        addresses[0].u1.Function = 0x11111111;
        addresses[1].u1.Function = 0x22222222;
        auto* createFileW = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(image.data() + 0x3400);
        std::memcpy(createFileW->Name, "CreateFileW", 12);
        auto* createFileA = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(image.data() + 0x3440);
        std::memcpy(createFileA->Name, "CreateFileA", 12);

        descriptor = {
            "fixture", kNativeMachine, 0x12345678,
            static_cast<DWORD>(image.size()), guid, age, 0x2000,
            renderer::unity::RenderAbi::publicRender, &prefix};
    }
};

renderer::UnityCoverageLut IdentityLut()
{
    renderer::UnityCoverageLut lut;
    for (unsigned int value = 0; value < 256; ++value)
    {
        lut.gray[value] = static_cast<unsigned char>(value);
        for (auto& channel : lut.rgb)
            channel[value] = static_cast<unsigned char>(value);
    }
    return lut;
}

} // namespace

int main()
{
    std::vector<unsigned char> sfnt(256, 0);
    auto writeBe16 = [&](std::size_t offset, unsigned int value) {
        sfnt[offset] = static_cast<unsigned char>(value >> 8);
        sfnt[offset + 1] = static_cast<unsigned char>(value);
    };
    auto writeBe32 = [&](std::size_t offset, std::uint32_t value) {
        sfnt[offset] = static_cast<unsigned char>(value >> 24);
        sfnt[offset + 1] = static_cast<unsigned char>(value >> 16);
        sfnt[offset + 2] = static_cast<unsigned char>(value >> 8);
        sfnt[offset + 3] = static_cast<unsigned char>(value);
    };
    writeBe32(0, 0x00010000);
    writeBe16(4, 1);
    std::memcpy(sfnt.data() + 12, "name", 4);
    writeBe32(20, 64);
    writeBe32(24, 64);
    writeBe16(64, 0);
    writeBe16(66, 2);
    writeBe16(68, 30);
    writeBe16(70, 3);
    writeBe16(72, 1);
    writeBe16(74, 0x0409);
    writeBe16(76, 1);
    writeBe16(78, 10);
    writeBe16(80, 0);
    writeBe16(82, 3);
    writeBe16(84, 1);
    writeBe16(86, 0x0412);
    writeBe16(88, 1);
    writeBe16(90, 4);
    writeBe16(92, 10);
    for (std::size_t index = 0; index < 5; ++index)
        writeBe16(94 + index * 2, static_cast<unsigned int>(L"Gulim"[index]));
    writeBe16(104, 0xAD74);
    writeBe16(106, 0xB9BC);
    std::vector<std::wstring> sfntFamilies;
    Require(renderer::unity::ParseSfntFamilyNames(
        sfnt.data(), sfnt.size(), sfntFamilies),
        "a bounded SFNT family-name table did not parse");
    Require(std::any_of(sfntFamilies.begin(), sfntFamilies.end(),
        [](const std::wstring& family) { return family == L"Gulim"; }) &&
        std::any_of(sfntFamilies.begin(), sfntFamilies.end(),
        [](const std::wstring& family) { return family == L"굴림"; }),
        "SFNT parsing did not preserve English and Korean family names");

    renderer::unity::UnityFontEvidenceV1 evidence{};
    renderer::unity::InitializeUnityFontEvidence(evidence, 1234);
    renderer::unity::RecordUnityFontFileOpen(
        evidence, L"C:\\Windows\\Fonts\\gulim.ttc");
    renderer::unity::RecordUnityFontRedirect(
        evidence, L"C:\\Windows\\Fonts\\gulim.ttc",
        L"C:\\Fonts\\PretendardVariable.ttf", true);
    renderer::unity::RecordUnityFontRedirect(
        evidence, L"C:\\Windows\\Fonts\\gulim.ttc",
        L"C:\\Fonts\\PretendardVariable.ttf", false);
    renderer::unity::UnityFontEvidenceSnapshot evidenceSnapshot{};
    Require(renderer::unity::ReadUnityFontEvidence(evidence, evidenceSnapshot),
        "a valid Unity font redirect evidence record was unreadable");
    Require(evidenceSnapshot.pid == 1234 &&
        evidenceSnapshot.redirectAttempts == 2 &&
        evidenceSnapshot.redirectSuccesses == 1 &&
        evidenceSnapshot.redirectFallbacks == 1 &&
        evidenceSnapshot.observedFontOpens == 1 &&
        wcscmp(evidenceSnapshot.observedPath,
            L"C:\\Windows\\Fonts\\gulim.ttc") == 0 &&
        wcscmp(evidenceSnapshot.sourcePath,
            L"C:\\Windows\\Fonts\\gulim.ttc") == 0 &&
        wcscmp(evidenceSnapshot.replacementPath,
            L"C:\\Fonts\\PretendardVariable.ttf") == 0,
        "Unity font redirect evidence lost its observable contract");

    std::vector<renderer::unity::InstalledFontFace> systemFonts;
    Require(renderer::unity::EnumerateInstalledFontFaces(systemFonts),
        "the installed DirectWrite font catalog was unavailable");
    Require(std::any_of(
        systemFonts.begin(), systemFonts.end(), [](const auto& font) {
            return _wcsicmp(font.family.c_str(), L"Segoe UI") == 0 &&
                font.filePath.size() >= 3 && font.filePath[1] == L':' &&
                (font.filePath[2] == L'\\' || font.filePath[2] == L'/');
        }),
        "the installed catalog did not expose Segoe UI's absolute font file");

    auto substitutions = renderer::font_substitution::Snapshot::Build(
        {{L"굴림", L"Pretendard Variable"}}, 1);
    std::vector<renderer::unity::InstalledFontFace> installedFonts{
        {L"굴림", L"C:\\Windows\\Fonts\\gulim.ttc"},
        {L"Pretendard Variable", L"C:\\Users\\tester\\Fonts\\PretendardVariable.ttf"},
    };
    auto redirects = renderer::unity::FontFileRedirectTable::Build(
        installedFonts, *substitutions);
    Require(redirects && !redirects->empty(),
        "a configured Unity font substitution did not produce a redirect");
    std::wstring redirectedPath;
    Require(redirects->Resolve(
        L"c:/windows/fonts/GULIM.TTC", redirectedPath),
        "the exact configured system font path did not redirect");
    Require(redirectedPath ==
        L"C:\\Users\\tester\\Fonts\\PretendardVariable.ttf",
        "the configured system font redirected to the wrong file");
    Require(!redirects->Resolve(L"gulim.ttc", redirectedPath),
        "a relative game asset path was redirected");
    Require(!redirects->Resolve(
        L"C:\\Games\\PlagueInc\\gulim.ttc", redirectedPath),
        "an unrelated game asset with the same filename was redirected");

    Fixture fixture;
    renderer::unity::FileImportSlots fileImports{};
    Require(renderer::unity::ResolveFileImportSlots(
        fixture.image.data(), fixture.image.size(), &fileImports),
        "UnityPlayer's exact CreateFile import slots did not resolve");
    Require(fileImports.createFileW == reinterpret_cast<void**>(
        fixture.image.data() + 0x3200) &&
        fileImports.createFileA == reinterpret_cast<void**>(
            fixture.image.data() + 0x3200 + sizeof(NativeThunkData)),
        "the wrong UnityPlayer import slots resolved");
    void* readOnlyImage = VirtualAlloc(
        nullptr, fixture.image.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    Require(readOnlyImage != nullptr, "could not allocate the read-only IAT fixture");
    std::memcpy(readOnlyImage, fixture.image.data(), fixture.image.size());
    DWORD oldProtection = 0;
    Require(VirtualProtect(
        readOnlyImage, fixture.image.size(), PAGE_READONLY, &oldProtection) != FALSE,
        "could not protect the read-only IAT fixture");
    renderer::unity::FileImportSlots readOnlyImports{};
    Require(renderer::unity::ResolveFileImportSlots(
        readOnlyImage, fixture.image.size(), &readOnlyImports),
        "CreateFile slots did not resolve from a read-only loader image");
    void* importedTarget = nullptr;
    Require(renderer::unity::ReadFileImportTarget(
        readOnlyImports.createFileA, &importedTarget) &&
        reinterpret_cast<std::uintptr_t>(importedTarget) == 0x22222222,
        "reading a target from a read-only IAT failed");
    VirtualFree(readOnlyImage, 0, MEM_RELEASE);
    renderer::unity::ResolvedAdapter resolved{};
    Require(renderer::unity::ResolveAdapter(
                fixture.image.data(), fixture.image.size(), &fixture.descriptor, 1, &resolved),
            "an exact PE/PDB identity did not resolve");
    Require(resolved.targetRva == 0x2000 &&
                resolved.abi == renderer::unity::RenderAbi::publicRender,
            "the exact descriptor resolved the wrong target");

    auto wrongGuid = fixture.descriptor;
    wrongGuid.pdbGuid[0] ^= 0xff;
    Require(!renderer::unity::ResolveAdapter(
                fixture.image.data(), fixture.image.size(), &wrongGuid, 1, &resolved),
            "a mismatched PDB identity resolved");
    fixture.image[0x2000] ^= 0xff;
    Require(!renderer::unity::ResolveAdapter(
                fixture.image.data(), fixture.image.size(), &fixture.descriptor, 1, &resolved),
            "a mismatched function prologue resolved");
    fixture.image[0x2000] ^= 0xff;
    Require(!renderer::unity::ResolveAdapter(
                fixture.image.data(), 0x1800, &fixture.descriptor, 1, &resolved),
            "a truncated mapped image resolved");

    renderer::UnityCoverageLut lut = IdentityLut();
    lut.gray[128] = 200;
    std::array<unsigned char, 8> gray{{128, 64, 7, 7, 128, 32, 7, 7}};
    Require(renderer::unity::ApplyCoverage(gray.data(), 4, 2, 2, 2, lut),
            "valid grayscale coverage was rejected");
    Require(gray == std::array<unsigned char, 8>{{200, 64, 7, 7, 200, 32, 7, 7}},
            "grayscale coverage touched padding or used the wrong LUT");

    lut.rgb[0][1] = 11;
    lut.rgb[1][1] = 22;
    lut.rgb[2][1] = 33;
    std::array<unsigned char, 6> lcd{{1, 1, 1, 1, 1, 1}};
    Require(renderer::unity::ApplyCoverage(lcd.data(), -3, 2, 3, 5, lut),
            "valid negative-pitch LCD coverage was rejected");
    Require(lcd == std::array<unsigned char, 6>{{11, 22, 33, 11, 22, 33}},
            "LCD channel LUT selection is incorrect");

    auto unchanged = lcd;
    Require(!renderer::unity::ApplyCoverage(lcd.data(), 3, 2, 3, 7, lut),
            "BGRA coverage was accepted");
    Require(lcd == unchanged, "unsupported pixel data was modified");
    Require(!renderer::unity::ApplyCoverage(lcd.data(), 2, 2, 3, 5, lut),
            "a row wider than its pitch was accepted");
    return 0;
}
