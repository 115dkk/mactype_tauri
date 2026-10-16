#include "../../../renderer/unity_font_hook_core.h"
#include "../../../renderer/unity_font_catalog.h"
#include "../../../renderer/unity_font_evidence.h"
#include "../../../renderer/unity_font_selection_context.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

#ifdef _WIN64
using NativeNtHeaders = IMAGE_NT_HEADERS64;
constexpr WORD kNativeMachine = IMAGE_FILE_MACHINE_AMD64;
constexpr WORD kNativeOptionalMagic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
#else
using NativeNtHeaders = IMAGE_NT_HEADERS32;
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
    std::array<unsigned char, 32> faceOpenPrefix{};
    std::array<unsigned char, 32> charIndexPrefix{};
    std::array<unsigned char, 32> fontCatalogLoadPrefix{};
    std::array<unsigned char, 32> faceDonePrefix{};
    renderer::unity::AdapterDescriptor descriptor{};

    Fixture()
    {
        for (std::size_t index = 0; index < prefix.size(); ++index)
        {
            prefix[index] = static_cast<unsigned char>(0x40 + index);
            faceOpenPrefix[index] = static_cast<unsigned char>(0x80 + index);
            charIndexPrefix[index] = static_cast<unsigned char>(0xA0 + index);
            fontCatalogLoadPrefix[index] =
                static_cast<unsigned char>(0xC0 + index);
            faceDonePrefix[index] = static_cast<unsigned char>(0x20 + index);
        }

        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;
        auto* nt = reinterpret_cast<NativeNtHeaders*>(image.data() + dos->e_lfanew);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = kNativeMachine;
        nt->FileHeader.NumberOfSections = 2;
        nt->FileHeader.TimeDateStamp = 0x12345678;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(nt->OptionalHeader);
        nt->FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;
        nt->OptionalHeader.Magic = kNativeOptionalMagic;
        nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(image.size());
        nt->OptionalHeader.SizeOfHeaders = 0x400;
        nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG] = {
            0x1000, sizeof(IMAGE_DEBUG_DIRECTORY)};

        auto* sections = IMAGE_FIRST_SECTION(nt);
        std::memcpy(sections[0].Name, ".rdata", 6);
        sections[0].Misc.VirtualSize = 0x1000;
        sections[0].VirtualAddress = 0x1000;
        sections[0].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
        std::memcpy(sections[1].Name, ".text", 5);
        sections[1].Misc.VirtualSize = 0x1000;
        sections[1].VirtualAddress = 0x2000;
        sections[1].Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;

        auto* debug = reinterpret_cast<IMAGE_DEBUG_DIRECTORY*>(image.data() + 0x1000);
        debug->Type = IMAGE_DEBUG_TYPE_CODEVIEW;
        debug->SizeOfData = 32;
        debug->AddressOfRawData = 0x1100;
        std::memcpy(image.data() + 0x1100, "RSDS", 4);
        std::memcpy(image.data() + 0x1104, guid.data(), guid.size());
        DWORD const age = 7;
        std::memcpy(image.data() + 0x1114, &age, sizeof(age));
        std::memcpy(image.data() + 0x2000, prefix.data(), prefix.size());
        std::memcpy(image.data() + 0x2080,
            faceOpenPrefix.data(), faceOpenPrefix.size());
        std::memcpy(image.data() + 0x2100,
            charIndexPrefix.data(), charIndexPrefix.size());
        std::memcpy(image.data() + 0x2180,
            fontCatalogLoadPrefix.data(), fontCatalogLoadPrefix.size());
        std::memcpy(image.data() + 0x2200,
            faceDonePrefix.data(), faceDonePrefix.size());

        descriptor = {
            "fixture", kNativeMachine, 0x12345678,
            static_cast<DWORD>(image.size()), guid, age, 0x2000,
            renderer::unity::RenderAbi::publicRender, &prefix,
            0x2080, &faceOpenPrefix,
            renderer::unity::FaceOpenAbi::unityInternal};
        descriptor.freeTypeCharIndexRva = 0x2100;
        descriptor.freeTypeCharIndexPrefix = &charIndexPrefix;
        descriptor.freeTypeCharIndexAbi =
            renderer::unity::FreeTypeCharIndexAbi::standard;
        descriptor.fontCatalogLoadRva = 0x2180;
        descriptor.fontCatalogLoadPrefix = &fontCatalogLoadPrefix;
        descriptor.fontCatalogLoadAbi =
            renderer::unity::FontCatalogLoadAbi::systemCatalogEntry;
        descriptor.faceDoneRva = 0x2200;
        descriptor.faceDonePrefix = &faceDonePrefix;
        descriptor.faceDoneAbi = renderer::unity::FaceDoneAbi::standard;
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

bool WriteTemporaryFontFile(
    const std::vector<unsigned char>& bytes,
    std::wstring& path)
{
    path.clear();
    wchar_t directory[MAX_PATH]{};
    DWORD const directoryLength = GetTempPathW(_countof(directory), directory);
    if (directoryLength == 0 || directoryLength >= _countof(directory))
        return false;
    wchar_t temporary[MAX_PATH]{};
    if (GetTempFileNameW(directory, L"mtu", 0, temporary) == 0)
        return false;
    HANDLE const file = CreateFileW(
        temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        DeleteFileW(temporary);
        return false;
    }
    DWORD written = 0;
    bool const succeeded = bytes.size() <=
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()) &&
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
            &written, nullptr) != FALSE &&
        written == static_cast<DWORD>(bytes.size());
    CloseHandle(file);
    if (!succeeded)
    {
        DeleteFileW(temporary);
        return false;
    }
    path = temporary;
    return true;
}

} // namespace

int main()
{
    renderer::unity::FaceReplacementRegistry faceRegistry;
    int nativeFace = 0;
    int replacementFace = 0;
    auto faceLookup = faceRegistry.Lookup(&nativeFace);
    Require(faceLookup.state ==
            renderer::unity::FaceReplacementRegistry::State::none &&
        faceLookup.replacement == nullptr,
        "a missing face replacement did not report the none state");
    Require(faceRegistry.Reserve(&nativeFace),
        "a native face could not reserve a replacement slot");
    faceLookup = faceRegistry.Lookup(&nativeFace);
    Require(faceLookup.state ==
            renderer::unity::FaceReplacementRegistry::State::pending &&
        faceLookup.replacement == nullptr,
        "a reserved face replacement did not report the pending state");
    Require(faceRegistry.Publish(&nativeFace, &replacementFace),
        "a pending face replacement could not be published");
    faceLookup = faceRegistry.Lookup(&nativeFace);
    Require(faceLookup.state ==
            renderer::unity::FaceReplacementRegistry::State::published &&
        faceLookup.replacement == &replacementFace &&
        faceRegistry.IsReplacement(&replacementFace),
        "a published face replacement lost either registry direction");
    auto faceRelease = faceRegistry.Release(&nativeFace);
    Require(faceRelease.releasedNative &&
        faceRelease.pairedReplacement == &replacementFace &&
        faceRegistry.Lookup(&nativeFace).state ==
            renderer::unity::FaceReplacementRegistry::State::none &&
        !faceRegistry.IsReplacement(&replacementFace),
        "releasing a native face did not return and remove its replacement");

    int releasedReservation = 0;
    int lateReplacement = 0;
    Require(faceRegistry.Reserve(&releasedReservation),
        "a release-race fixture could not reserve a slot");
    faceRelease = faceRegistry.Release(&releasedReservation);
    Require(faceRelease.releasedNative &&
        faceRelease.pairedReplacement == nullptr &&
        !faceRegistry.Publish(&releasedReservation, &lateReplacement),
        "a replacement published after its native reservation was released");

    int pendingNative = 0;
    int unpublishedReplacement = 0;
    Require(faceRegistry.Reserve(&pendingNative),
        "an open-failure fixture could not reserve a slot");
    faceRelease = faceRegistry.Release(&unpublishedReplacement);
    Require(!faceRelease.releasedNative &&
        faceRelease.pairedReplacement == nullptr &&
        faceRegistry.Lookup(&pendingNative).state ==
            renderer::unity::FaceReplacementRegistry::State::pending,
        "Done_Face for an unpublished replacement erased the native reservation");
    faceRegistry.Abandon(&pendingNative);
    Require(faceRegistry.Reserve(&pendingNative),
        "an abandoned face replacement reservation still consumed its slot");
    faceRelease = faceRegistry.Release(&pendingNative);
    Require(faceRelease.releasedNative &&
        faceRelease.pairedReplacement == nullptr,
        "releasing a pending native face reported a replacement");

    int reverseNative = 0;
    int reverseReplacement = 0;
    Require(faceRegistry.Reserve(&reverseNative) &&
        faceRegistry.Publish(&reverseNative, &reverseReplacement),
        "a reverse-release fixture could not publish its pair");
    faceRelease = faceRegistry.Release(&reverseReplacement);
    Require(!faceRelease.releasedNative &&
        faceRelease.pairedReplacement == nullptr &&
        faceRegistry.Lookup(&reverseNative).state ==
            renderer::unity::FaceReplacementRegistry::State::none &&
        !faceRegistry.IsReplacement(&reverseReplacement),
        "releasing by replacement key did not remove the pair");

    std::array<int, 257> capacityFaces{};
    for (std::size_t index = 0; index < 256; ++index)
    {
        Require(faceRegistry.Reserve(&capacityFaces[index]),
            "the face replacement registry reached capacity too early");
    }
    Require(faceRegistry.size() == 256 &&
        !faceRegistry.Reserve(&capacityFaces.back()),
        "the face replacement registry exceeded its 256-entry cap");
    faceRegistry.Clear();
    Require(faceRegistry.size() == 0,
        "clearing the face replacement registry retained entries");

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

    std::vector<unsigned char> fvar(160, 0);
    auto writeFvarBe16 = [&](std::size_t offset, unsigned int value) {
        fvar[offset] = static_cast<unsigned char>(value >> 8);
        fvar[offset + 1] = static_cast<unsigned char>(value);
    };
    auto writeFvarBe32 = [&](std::size_t offset, std::uint32_t value) {
        fvar[offset] = static_cast<unsigned char>(value >> 24);
        fvar[offset + 1] = static_cast<unsigned char>(value >> 16);
        fvar[offset + 2] = static_cast<unsigned char>(value >> 8);
        fvar[offset + 3] = static_cast<unsigned char>(value);
    };
    writeFvarBe32(0, 0x00010000);
    writeFvarBe16(4, 1);
    std::memcpy(fvar.data() + 12, "fvar", 4);
    writeFvarBe32(20, 64);
    writeFvarBe32(24, 52);
    writeFvarBe16(64, 1);
    writeFvarBe16(66, 0);
    writeFvarBe16(68, 16);
    writeFvarBe16(70, 2);
    writeFvarBe16(72, 1);
    writeFvarBe16(74, 20);
    writeFvarBe16(76, 2);
    writeFvarBe16(78, 8);
    std::memcpy(fvar.data() + 80, "wght", 4);
    writeFvarBe32(84, 100U << 16);
    writeFvarBe32(88, 400U << 16);
    writeFvarBe32(92, 900U << 16);
    writeFvarBe16(98, 256);
    writeFvarBe16(100, 257);
    writeFvarBe32(104, 400U << 16);
    writeFvarBe16(108, 258);
    writeFvarBe32(112, 500U << 16);
    constexpr std::uint32_t weightTag =
        static_cast<std::uint32_t>('w') |
        (static_cast<std::uint32_t>('g') << 8) |
        (static_cast<std::uint32_t>('h') << 16) |
        (static_cast<std::uint32_t>('t') << 24);
    renderer::unity::AxisValue weightAxis{weightTag, 500.0F};
    long namedInstance = -1;
    Require(renderer::unity::ParseSfntNamedInstanceIndex(
        fvar.data(), fvar.size(), 0, &weightAxis, 1, namedInstance) &&
        namedInstance == 2,
        "the 500-weight fvar instance did not receive its one-based index");
    weightAxis.value = 400.0F;
    Require(renderer::unity::ParseSfntNamedInstanceIndex(
        fvar.data(), fvar.size(), 0, &weightAxis, 1, namedInstance) &&
        namedInstance == 0,
        "the default fvar instance did not keep face index zero");
    weightAxis.value = 450.0F;
    Require(!renderer::unity::ParseSfntNamedInstanceIndex(
        fvar.data(), fvar.size(), 0, &weightAxis, 1, namedInstance) &&
        namedInstance == 0,
        "an unmatched fvar coordinate selected a named instance");
    // The rejections below must come from the mutated condition alone, so the
    // coordinate goes back to one the unchanged fixture resolves.
    weightAxis.value = 500.0F;
    Require(renderer::unity::ParseSfntNamedInstanceIndex(
        fvar.data(), fvar.size(), 0, &weightAxis, 1, namedInstance) &&
        namedInstance == 2,
        "the unchanged fixture stopped resolving the 500-weight instance");
    Require(!renderer::unity::ParseSfntNamedInstanceIndex(
        fvar.data(), fvar.size(), 1, &weightAxis, 1, namedInstance),
        "a standalone SFNT accepted a nonzero base face index");
    auto truncatedFvarInstances = fvar;
    truncatedFvarInstances[24] = 0;
    truncatedFvarInstances[25] = 0;
    truncatedFvarInstances[26] = 0;
    truncatedFvarInstances[27] = 51;
    Require(!renderer::unity::ParseSfntNamedInstanceIndex(
        truncatedFvarInstances.data(), truncatedFvarInstances.size(), 0,
        &weightAxis, 1, namedInstance),
        "an fvar instance array extending past its table was accepted");
    auto zeroAxisFvar = fvar;
    zeroAxisFvar[72] = 0;
    zeroAxisFvar[73] = 0;
    Require(!renderer::unity::ParseSfntNamedInstanceIndex(
        zeroAxisFvar.data(), zeroAxisFvar.size(), 0,
        &weightAxis, 1, namedInstance),
        "an fvar table with no axes was accepted");

    std::vector<unsigned char> ttc(512, 0);
    auto writeTtcBe16 = [&](std::size_t offset, unsigned int value) {
        ttc[offset] = static_cast<unsigned char>(value >> 8);
        ttc[offset + 1] = static_cast<unsigned char>(value);
    };
    auto writeTtcBe32 = [&](std::size_t offset, std::uint32_t value) {
        ttc[offset] = static_cast<unsigned char>(value >> 24);
        ttc[offset + 1] = static_cast<unsigned char>(value >> 16);
        ttc[offset + 2] = static_cast<unsigned char>(value >> 8);
        ttc[offset + 3] = static_cast<unsigned char>(value);
    };
    std::memcpy(ttc.data(), "ttcf", 4);
    writeTtcBe32(4, 0x00010000);
    writeTtcBe32(8, 2);
    writeTtcBe32(12, 32);
    writeTtcBe32(16, 128);
    auto writeTtcFace = [&](std::size_t faceOffset, std::size_t nameOffset,
                            const wchar_t* family) {
        writeTtcBe32(faceOffset, 0x00010000);
        writeTtcBe16(faceOffset + 4, 1);
        std::memcpy(ttc.data() + faceOffset + 12, "name", 4);
        writeTtcBe32(faceOffset + 20, static_cast<std::uint32_t>(nameOffset));
        std::size_t const familyLength = std::wcslen(family);
        std::uint32_t const tableLength =
            static_cast<std::uint32_t>(18 + familyLength * 2);
        writeTtcBe32(faceOffset + 24, tableLength);
        writeTtcBe16(nameOffset, 0);
        writeTtcBe16(nameOffset + 2, 1);
        writeTtcBe16(nameOffset + 4, 18);
        writeTtcBe16(nameOffset + 6, 3);
        writeTtcBe16(nameOffset + 8, 1);
        writeTtcBe16(nameOffset + 10, 0x0409);
        writeTtcBe16(nameOffset + 12, 1);
        writeTtcBe16(nameOffset + 14,
            static_cast<unsigned int>(familyLength * 2));
        writeTtcBe16(nameOffset + 16, 0);
        for (std::size_t index = 0; index < familyLength; ++index)
            writeTtcBe16(nameOffset + 18 + index * 2, family[index]);
    };
    writeTtcFace(32, 256, L"First Face");
    writeTtcFace(128, 320, L"Second Face");
    std::vector<renderer::unity::SfntFamilyFace> ttcFaces;
    Require(renderer::unity::ParseSfntFamilyFaces(
        ttc.data(), ttc.size(), ttcFaces) && ttcFaces.size() == 2 &&
        ttcFaces[0].family == L"First Face" && ttcFaces[0].faceIndex == 0 &&
        ttcFaces[1].family == L"Second Face" && ttcFaces[1].faceIndex == 1,
        "TTC family parsing assigned names to the wrong face index");

    std::vector<unsigned char> variableTtc(640, 0);
    auto writeVariableTtcBe16 = [&](std::size_t offset, unsigned int value) {
        variableTtc[offset] = static_cast<unsigned char>(value >> 8);
        variableTtc[offset + 1] = static_cast<unsigned char>(value);
    };
    auto writeVariableTtcBe32 = [&](std::size_t offset, std::uint32_t value) {
        variableTtc[offset] = static_cast<unsigned char>(value >> 24);
        variableTtc[offset + 1] = static_cast<unsigned char>(value >> 16);
        variableTtc[offset + 2] = static_cast<unsigned char>(value >> 8);
        variableTtc[offset + 3] = static_cast<unsigned char>(value);
    };
    std::memcpy(variableTtc.data(), "ttcf", 4);
    writeVariableTtcBe32(4, 0x00010000);
    writeVariableTtcBe32(8, 2);
    writeVariableTtcBe32(12, 32);
    writeVariableTtcBe32(16, 128);
    auto writeVariableTtcFace = [&](std::size_t faceOffset,
                                    std::size_t tableOffset,
                                    const std::vector<unsigned int>& weights) {
        writeVariableTtcBe32(faceOffset, 0x00010000);
        writeVariableTtcBe16(faceOffset + 4, 1);
        std::memcpy(variableTtc.data() + faceOffset + 12, "fvar", 4);
        writeVariableTtcBe32(faceOffset + 20,
            static_cast<std::uint32_t>(tableOffset));
        writeVariableTtcBe32(faceOffset + 24,
            static_cast<std::uint32_t>(36 + weights.size() * 8));
        writeVariableTtcBe16(tableOffset, 1);
        writeVariableTtcBe16(tableOffset + 2, 0);
        writeVariableTtcBe16(tableOffset + 4, 16);
        writeVariableTtcBe16(tableOffset + 6, 2);
        writeVariableTtcBe16(tableOffset + 8, 1);
        writeVariableTtcBe16(tableOffset + 10, 20);
        writeVariableTtcBe16(tableOffset + 12,
            static_cast<unsigned int>(weights.size()));
        writeVariableTtcBe16(tableOffset + 14, 8);
        std::memcpy(variableTtc.data() + tableOffset + 16, "wght", 4);
        writeVariableTtcBe32(tableOffset + 20, 100U << 16);
        writeVariableTtcBe32(tableOffset + 24, 400U << 16);
        writeVariableTtcBe32(tableOffset + 28, 900U << 16);
        for (std::size_t index = 0; index < weights.size(); ++index)
        {
            std::size_t const instance = tableOffset + 36 + index * 8;
            writeVariableTtcBe16(instance,
                static_cast<unsigned int>(300 + index));
            writeVariableTtcBe32(instance + 4, weights[index] << 16);
        }
    };
    writeVariableTtcFace(32, 256, {500});
    writeVariableTtcFace(128, 384, {300, 500});
    weightAxis.value = 500.0F;
    Require(renderer::unity::ParseSfntNamedInstanceIndex(
        variableTtc.data(), variableTtc.size(), 0,
        &weightAxis, 1, namedInstance) && namedInstance == 1,
        "the first TTC face did not resolve its fvar instance");
    Require(renderer::unity::ParseSfntNamedInstanceIndex(
        variableTtc.data(), variableTtc.size(), 1,
        &weightAxis, 1, namedInstance) && namedInstance == 2,
        "the second TTC face used the first face's fvar instance position");
    std::vector<unsigned char> truncatedTtcHeader(
        variableTtc.begin(), variableTtc.begin() + 16);
    Require(!renderer::unity::ParseSfntNamedInstanceIndex(
        truncatedTtcHeader.data(), truncatedTtcHeader.size(), 0,
        &weightAxis, 1, namedInstance),
        "a TTC header cut before its offset array was accepted");
    auto invalidTtcOffset = variableTtc;
    invalidTtcOffset[16] = 0;
    invalidTtcOffset[17] = 0;
    invalidTtcOffset[18] = 3;
    invalidTtcOffset[19] = 0;
    Require(!renderer::unity::ParseSfntNamedInstanceIndex(
        invalidTtcOffset.data(), invalidTtcOffset.size(), 1,
        &weightAxis, 1, namedInstance),
        "a TTC face offset beyond the file was accepted");

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
    renderer::unity::RecordUnityFontRender(
        evidence, 0, 123, 17, 19);
    renderer::unity::RecordUnityFontRender(
        evidence, 6, 0, 0, 0);
    renderer::unity::RecordUnityFontFaceDetails(
        evidence, true, 14747, 6946);
    renderer::unity::RecordUnityFontCharacterLookup(
        evidence, L"Gulim", true, 0xC124, true, 6946, 6946,
        L"Pretendard Variable");
    renderer::unity::RecordUnityFontCharacterLookup(
        evidence, L"Malgun Gothic", false, 0xC815, false, 0, 0, nullptr);
    renderer::unity::RecordUnityFontFaceResolution(
        evidence, true, 8723, 6946);
    renderer::unity::RecordUnityFontOsFaceResolution(
        evidence, L"Gulim", true, true, 6946, L"Pretendard Variable");
    renderer::unity::RecordUnityFontFaceReplacementCandidate(
        evidence, L"Malgun Gothic");
    renderer::unity::RecordUnityFontFaceReplacementHit(evidence);
    renderer::unity::RecordUnityFontFaceReplacement(
        evidence, true, L"C:\\Fonts\\PretendardVariable.ttf");
    renderer::unity::RecordUnityFontFaceReplacement(
        evidence, false, L"C:\\Fonts\\PretendardVariable.ttf");
    renderer::unity::UnityFontEvidenceSnapshot evidenceSnapshot{};
    Require(renderer::unity::ReadUnityFontEvidence(evidence, evidenceSnapshot),
        "a valid Unity font redirect evidence record was unreadable");
    Require(evidenceSnapshot.pid == 1234 &&
        evidenceSnapshot.redirectAttempts == 2 &&
        evidenceSnapshot.redirectSuccesses == 1 &&
        evidenceSnapshot.redirectFallbacks == 1 &&
        evidenceSnapshot.observedFontOpens == 1 &&
        evidenceSnapshot.renderCalls == 2 &&
        evidenceSnapshot.renderSuccesses == 1 &&
        evidenceSnapshot.nonEmptyBitmaps == 1 &&
        evidenceSnapshot.lastRenderError == 6 &&
        evidenceSnapshot.lastGlyphIndex == 0 &&
        evidenceSnapshot.lastBitmapWidth == 0 &&
        evidenceSnapshot.lastBitmapRows == 0 &&
        evidenceSnapshot.redirectedFaceHasCharmap == 1 &&
        evidenceSnapshot.redirectedFaceGlyphs == 14747 &&
        evidenceSnapshot.sampleKoreanGlyph == 6946 &&
        evidenceSnapshot.characterLookups == 2 &&
        evidenceSnapshot.characterLookupHits == 1 &&
        evidenceSnapshot.lastCharacter == 0xC815 &&
        evidenceSnapshot.lastLookupGlyph == 0 &&
        wcscmp(evidenceSnapshot.lastLookupFamily, L"Malgun Gothic") == 0 &&
        evidenceSnapshot.mappedCharacterLookups == 1 &&
        evidenceSnapshot.mappedCharacterLookupHits == 1 &&
        evidenceSnapshot.lastMappedCharacter == 0xC124 &&
        evidenceSnapshot.lastMappedGlyph == 6946 &&
        evidenceSnapshot.lastMappedSampleKoreanGlyph == 6946 &&
        wcscmp(evidenceSnapshot.lastMappedFamily, L"Gulim") == 0 &&
        wcscmp(evidenceSnapshot.lastMappedResolvedFaceFamily,
            L"Pretendard Variable") == 0 &&
        evidenceSnapshot.faceResolutions == 1 &&
        evidenceSnapshot.faceResolutionHits == 1 &&
        evidenceSnapshot.faceResolutionGlyphHits == 1 &&
        evidenceSnapshot.lastFaceResolutionGlyph == 8723 &&
        evidenceSnapshot.lastFaceResolutionSampleKoreanGlyph == 6946 &&
        evidenceSnapshot.osFaceResolutions == 1 &&
        evidenceSnapshot.osFaceResolutionHits == 1 &&
        evidenceSnapshot.lastOsFaceSampleKoreanGlyph == 6946 &&
        wcscmp(evidenceSnapshot.lastOsFaceFamily, L"Gulim") == 0 &&
        wcscmp(evidenceSnapshot.lastOsResolvedFaceFamily,
            L"Pretendard Variable") == 0 &&
        evidenceSnapshot.mappedOsFaceResolutions == 1 &&
        evidenceSnapshot.mappedOsFaceResolutionHits == 1 &&
        evidenceSnapshot.lastMappedOsFaceSampleKoreanGlyph == 6946 &&
        evidenceSnapshot.faceReplacements == 1 &&
        evidenceSnapshot.faceReplacementFallbacks == 1 &&
        evidenceSnapshot.faceReplacementCandidates == 1 &&
        evidenceSnapshot.faceReplacementHits == 1 &&
        wcscmp(evidenceSnapshot.lastFaceReplacementFamily,
            L"Malgun Gothic") == 0 &&
        wcscmp(evidenceSnapshot.lastFaceReplacementPath,
            L"C:\\Fonts\\PretendardVariable.ttf") == 0 &&
        wcscmp(evidenceSnapshot.lastMappedOsFaceFamily, L"Gulim") == 0 &&
        wcscmp(evidenceSnapshot.lastMappedOsResolvedFaceFamily,
            L"Pretendard Variable") == 0 &&
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
        {L"굴림", L"C:\\Windows\\Fonts\\gulim.ttc", 2},
        {L"Gulim", L"C:\\Windows\\Fonts\\gulim.ttc", 2},
        {L"Pretendard Variable", L"C:\\Users\\tester\\Fonts\\PretendardVariable.ttf", 3},
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
    long familyReplacementFaceIndex = 0;
    Require(redirects->ResolveFamilyFace(
        L"Gulim", redirectedPath, familyReplacementFaceIndex) &&
        redirectedPath ==
            L"C:\\Users\\tester\\Fonts\\PretendardVariable.ttf" &&
        familyReplacementFaceIndex == 3,
        "an alternate family alias for the same source face did not resolve");

    std::array<unsigned char, 40> inlineFontRef{};
    std::memcpy(inlineFontRef.data(), "Gulim", 5);
    inlineFontRef[24] = 19;
    inlineFontRef[32] = 1;
    std::wstring legacyFamily;
    Require(renderer::unity::ReadLegacyFontRefFamily(
        inlineFontRef.data(), legacyFamily) && legacyFamily == L"Gulim",
        "the exact inline Unity core string ABI did not decode");
    std::array<unsigned char, 40> heapFontRef{};
    const char heapFamily[] = "Malgun Gothic";
    auto const heapFamilyPointer = heapFamily;
    std::size_t const heapFamilyLength = std::strlen(heapFamily);
    std::memcpy(heapFontRef.data(), &heapFamilyPointer,
        sizeof(heapFamilyPointer));
    std::memcpy(heapFontRef.data() + 16, &heapFamilyLength,
        sizeof(heapFamilyLength));
    Require(renderer::unity::ReadLegacyFontRefFamily(
        heapFontRef.data(), legacyFamily) &&
        legacyFamily == L"Malgun Gothic",
        "the exact heap Unity core string ABI did not decode");
    inlineFontRef[24] = 0xff;
    Require(!renderer::unity::ReadLegacyFontRefFamily(
        inlineFontRef.data(), legacyFamily),
        "an invalid Unity inline-string length was accepted");
    auto familySubstitutions = renderer::font_substitution::Snapshot::Build(
        {{L"Malgun Gothic", L"Pretendard Variable Medium"}}, 2);
    std::vector<renderer::unity::InstalledFontFace> familyFonts{
        {L"Malgun Gothic", L"C:\\Windows\\Fonts\\malgun.ttf", 0},
        {L"맑은 고딕", L"C:\\Windows\\Fonts\\malgun.ttf", 0},
        {L"Pretendard Variable Medium",
            L"C:\\Fonts\\PretendardVariable.ttf", 5L << 16},
    };
    auto familyRedirects = renderer::unity::FontFileRedirectTable::Build(
        familyFonts, *familySubstitutions);
    renderer::unity::FaceOpenPathRedirect familyRedirect;
    Require(familyRedirects && renderer::unity::ResolveFamilyFaceOpenPath(
        *familyRedirects, L"Malgun Gothic", familyRedirect) &&
        familyRedirect.replacementPath ==
            L"C:\\Fonts\\PretendardVariable.ttf" &&
        familyRedirect.replacementFaceIndex == (5L << 16),
        "a face identified only by Malgun Gothic did not resolve its encoded replacement");

    std::vector<unsigned char> namedInstanceSfnt(192, 0);
    auto writeNamedInstanceBe16 = [&](std::size_t offset, unsigned int value) {
        namedInstanceSfnt[offset] = static_cast<unsigned char>(value >> 8);
        namedInstanceSfnt[offset + 1] = static_cast<unsigned char>(value);
    };
    auto writeNamedInstanceBe32 = [&](std::size_t offset,
                                      std::uint32_t value) {
        namedInstanceSfnt[offset] = static_cast<unsigned char>(value >> 24);
        namedInstanceSfnt[offset + 1] = static_cast<unsigned char>(value >> 16);
        namedInstanceSfnt[offset + 2] = static_cast<unsigned char>(value >> 8);
        namedInstanceSfnt[offset + 3] = static_cast<unsigned char>(value);
    };
    writeNamedInstanceBe32(0, 0x00010000);
    writeNamedInstanceBe16(4, 1);
    std::memcpy(namedInstanceSfnt.data() + 12, "fvar", 4);
    writeNamedInstanceBe32(20, 64);
    writeNamedInstanceBe32(24, 84);
    writeNamedInstanceBe16(64, 1);
    writeNamedInstanceBe16(66, 0);
    writeNamedInstanceBe16(68, 16);
    writeNamedInstanceBe16(70, 2);
    writeNamedInstanceBe16(72, 1);
    writeNamedInstanceBe16(74, 20);
    writeNamedInstanceBe16(76, 6);
    writeNamedInstanceBe16(78, 8);
    std::memcpy(namedInstanceSfnt.data() + 80, "wght", 4);
    writeNamedInstanceBe32(84, 100U << 16);
    writeNamedInstanceBe32(88, 400U << 16);
    writeNamedInstanceBe32(92, 900U << 16);
    constexpr std::array<unsigned int, 6> instanceWeights{
        {100, 200, 300, 400, 500, 600}};
    for (std::size_t index = 0; index < instanceWeights.size(); ++index)
    {
        std::size_t const instance = 100 + index * 8;
        writeNamedInstanceBe16(instance,
            static_cast<unsigned int>(400 + index));
        writeNamedInstanceBe32(instance + 4, instanceWeights[index] << 16);
    }
    std::wstring namedInstancePath;
    Require(WriteTemporaryFontFile(namedInstanceSfnt, namedInstancePath),
        "the named-instance redirect fixture could not create its font file");
    auto namedInstanceSubstitutions =
        renderer::font_substitution::Snapshot::Build({
            {L"Source Medium", L"Variable Medium"},
            {L"Source SemiBold", L"Variable SemiBold"},
            {L"Source Default", L"Variable Default"}}, 3);
    std::vector<renderer::unity::InstalledFontFace> namedInstanceFonts{
        {L"Source Medium", L"C:\\Fonts\\source-medium.ttf", 0},
        {L"Source SemiBold", L"C:\\Fonts\\source-semibold.ttf", 0},
        {L"Source Default", L"C:\\Fonts\\source-default.ttf", 0},
        {L"Variable Medium", namedInstancePath, 0, {{weightTag, 500.0F}}},
        {L"Variable SemiBold", namedInstancePath, 0, {{weightTag, 600.0F}}},
        {L"Variable Default", namedInstancePath, 0},
    };
    auto namedInstanceRedirects = renderer::unity::FontFileRedirectTable::Build(
        namedInstanceFonts, *namedInstanceSubstitutions);
    long mediumFaceIndex = 0;
    long semiBoldFaceIndex = 0;
    long defaultFaceIndex = -1;
    std::wstring namedInstanceReplacement;
    Require(namedInstanceRedirects &&
        namedInstanceRedirects->ResolveFamilyFace(
            L"Source Medium", namedInstanceReplacement, mediumFaceIndex) &&
        namedInstanceRedirects->ResolveFamilyFace(
            L"Source SemiBold", namedInstanceReplacement, semiBoldFaceIndex) &&
        namedInstanceRedirects->ResolveFamilyFace(
            L"Source Default", namedInstanceReplacement, defaultFaceIndex) &&
        mediumFaceIndex == (5L << 16) &&
        semiBoldFaceIndex == (6L << 16) &&
        mediumFaceIndex != semiBoldFaceIndex && defaultFaceIndex == 0,
        "named instances from one file shared a path-only cached face index");
    DeleteFileW(namedInstancePath.c_str());

    renderer::unity::FaceOpenPathRedirect faceRedirect;
    Require(renderer::unity::ResolveFaceOpenPath(
        *redirects, 0x04, "c:/windows/fonts/GULIM.TTC", 2, faceRedirect),
        "Unity's pathname face-open request did not resolve");
    Require(faceRedirect.sourcePath == L"c:/windows/fonts/GULIM.TTC" &&
        faceRedirect.replacementPath ==
            L"C:\\Users\\tester\\Fonts\\PretendardVariable.ttf" &&
        faceRedirect.replacementUtf8 ==
            "C:\\Users\\tester\\Fonts\\PretendardVariable.ttf" &&
        faceRedirect.replacementFaceIndex == 3,
        "the face-open redirect lost its source or UTF-8 replacement path");
    Require(!renderer::unity::ResolveFaceOpenPath(
        *redirects, 0x04, "c:/windows/fonts/GULIM.TTC", 1, faceRedirect),
        "a different face in the same TTC was redirected");
    Require(!renderer::unity::ResolveFaceOpenPath(
        *redirects, 0x02, "c:/windows/fonts/GULIM.TTC", 2, faceRedirect),
        "a stream-backed face-open request was redirected");
    Require(!renderer::unity::ResolveFaceOpenPath(
        *redirects, 0x05, "c:/windows/fonts/GULIM.TTC", 2, faceRedirect),
        "a memory-backed face-open request was redirected");
    const char invalidUtf8[] = {'C', ':', '\\', static_cast<char>(0xff), 0};
    Require(!renderer::unity::ResolveFaceOpenPath(
        *redirects, 0x04, invalidUtf8, 2, faceRedirect),
        "an invalid UTF-8 face path was redirected");
    Require(renderer::unity::CurrentFontRefAllowsFaceRedirect(),
        "a Unity build without a FontRef resolver lost its compatible fallback");
    {
        renderer::unity::ScopedFontRefSelectionContext nativeFamily(
            renderer::unity::FontRefSelection::nativeFamily);
        Require(!renderer::unity::CurrentFontRefAllowsFaceRedirect(),
            "an unmapped FontRef allowed a path-only redirect");
        {
            renderer::unity::ScopedFontRefSelectionContext mappedFamily(
                renderer::unity::FontRefSelection::mappedFamily);
            Require(renderer::unity::CurrentFontRefAllowsFaceRedirect(),
                "a mapped FontRef rejected its replacement face");
        }
        {
            renderer::unity::ScopedFontRefSelectionContext undecoded(
                renderer::unity::FontRefSelection::undecoded);
            Require(renderer::unity::CurrentFontRefAllowsFaceRedirect(),
                "an undecodable FontRef rejected path-based fallback");
        }
        Require(!renderer::unity::CurrentFontRefAllowsFaceRedirect(),
            "a nested mapped FontRef did not restore its native parent");
    }
    Require(renderer::unity::CurrentFontRefAllowsFaceRedirect(),
        "leaving the FontRef resolver did not restore the fallback state");
    renderer::unity::FaceOpenPathRedirect fontLoadRedirect;
    Require(renderer::unity::ResolveTextCoreFontLoadPath(
        *redirects, "c:/windows/fonts/GULIM.TTC", 2,
        fontLoadRedirect),
        "Unity TextCore's path-based font-load request did not resolve");
    Require(fontLoadRedirect.sourcePath == L"c:/windows/fonts/GULIM.TTC" &&
        fontLoadRedirect.replacementPath ==
            L"C:\\Users\\tester\\Fonts\\PretendardVariable.ttf" &&
        fontLoadRedirect.replacementUtf8 ==
            "C:\\Users\\tester\\Fonts\\PretendardVariable.ttf" &&
        fontLoadRedirect.replacementFaceIndex == 3,
        "the TextCore font-load redirect lost its path or face index");
    Require(!renderer::unity::ResolveTextCoreFontLoadPath(
        *redirects, invalidUtf8, 2, fontLoadRedirect),
        "an invalid UTF-8 TextCore font path was redirected");

    Fixture fixture;
    renderer::unity::ResolvedAdapter resolved{};
    Require(renderer::unity::ResolveAdapter(
                fixture.image.data(), fixture.image.size(), &fixture.descriptor, 1, &resolved),
            "an exact PE/PDB identity did not resolve");
    Require(resolved.targetRva == 0x2000 &&
                resolved.faceOpenRva == 0x2080 &&
                resolved.freeTypeCharIndexRva == 0x2100 &&
                resolved.fontCatalogLoadRva == 0x2180 &&
                resolved.faceDoneRva == 0x2200 &&
                resolved.faceDoneAbi == renderer::unity::FaceDoneAbi::standard &&
                resolved.abi == renderer::unity::RenderAbi::publicRender &&
                resolved.faceOpenAbi ==
                    renderer::unity::FaceOpenAbi::unityInternal,
            "the exact descriptor resolved the wrong target");
    Require(renderer::unity::SelectFontSubstitutionBoundary(resolved) ==
        renderer::unity::FontSubstitutionBoundary::freeTypeFaceOpen,
        "a legacy adapter without TextCore did not select face-open fallback");
    resolved.fontLoadRva = 0x2100;
    resolved.fontLoadAbi =
        renderer::unity::FontLoadAbi::textCorePathSizeFace;
    Require(renderer::unity::SelectFontSubstitutionBoundary(resolved) ==
        renderer::unity::FontSubstitutionBoundary::textCoreFontLoad,
        "an adapter with TextCore did not prefer the higher font-load boundary");

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
    fixture.image[0x2080] ^= 0xff;
    Require(!renderer::unity::ResolveAdapter(
                fixture.image.data(), fixture.image.size(), &fixture.descriptor, 1, &resolved),
            "a mismatched face-open prologue resolved");
    fixture.image[0x2080] ^= 0xff;
    fixture.image[0x2100] ^= 0xff;
    Require(!renderer::unity::ResolveAdapter(
                fixture.image.data(), fixture.image.size(), &fixture.descriptor, 1, &resolved),
            "a mismatched FreeType character-index prologue resolved");
    fixture.image[0x2100] ^= 0xff;
    fixture.image[0x2180] ^= 0xff;
    Require(!renderer::unity::ResolveAdapter(
                fixture.image.data(), fixture.image.size(), &fixture.descriptor, 1, &resolved),
            "a mismatched system-font catalog prologue resolved");
    fixture.image[0x2180] ^= 0xff;
    fixture.image[0x2200] ^= 0xff;
    Require(!renderer::unity::ResolveAdapter(
                fixture.image.data(), fixture.image.size(), &fixture.descriptor, 1, &resolved),
            "a mismatched face-done prologue resolved");
    fixture.image[0x2200] ^= 0xff;
    Require(!renderer::unity::ResolveAdapter(
                fixture.image.data(), 0x1800, &fixture.descriptor, 1, &resolved),
            "a truncated mapped image resolved");

    std::size_t productionCount = 0;
    auto const production =
        renderer::unity::ProductionAdapterDescriptors(&productionCount);
    Require(production != nullptr && productionCount != 0,
        "the production Unity adapter catalog is empty");
    for (std::size_t index = 0; index < productionCount; ++index)
    {
        Require(production[index].faceOpenRva != 0 &&
            production[index].faceOpenPrefix != nullptr &&
            production[index].faceOpenAbi ==
                renderer::unity::FaceOpenAbi::unityInternal,
            "a production Unity adapter retained render-only substitution");
    }
    auto const rebelAdapter = std::find_if(
        production, production + productionCount, [](const auto& adapter) {
            return std::strcmp(adapter.name, "unity-2022.3.62f3-x64") == 0;
        });
    Require(rebelAdapter != production + productionCount &&
        rebelAdapter->fontLoadRva != 0 &&
        rebelAdapter->fontLoadPrefix != nullptr &&
        rebelAdapter->fontLoadAbi ==
            renderer::unity::FontLoadAbi::textCorePathSizeFace &&
        rebelAdapter->characterLookupRva != 0 &&
        rebelAdapter->characterLookupPrefix != nullptr &&
        rebelAdapter->characterLookupAbi ==
            renderer::unity::CharacterLookupAbi::legacyDynamicFont &&
        rebelAdapter->osFaceResolverRva != 0 &&
        rebelAdapter->osFaceResolverPrefix != nullptr &&
        rebelAdapter->osFaceResolverAbi ==
            renderer::unity::CharacterLookupAbi::legacyDynamicFont,
        "Rebel's exact Unity adapter did not select the TextCore font-load boundary");

    auto const plagueAdapter = std::find_if(
        production, production + productionCount, [](const auto& adapter) {
            return std::strcmp(adapter.name, "unity-2019.4.41-x64") == 0;
        });
    Require(plagueAdapter != production + productionCount &&
        plagueAdapter->freeTypeCharIndexRva == 0x00E48FF0 &&
        plagueAdapter->freeTypeCharIndexPrefix != nullptr &&
        plagueAdapter->freeTypeCharIndexAbi ==
            renderer::unity::FreeTypeCharIndexAbi::standard &&
        plagueAdapter->fontCatalogLoadRva == 0x00C28960 &&
        plagueAdapter->fontCatalogLoadPrefix != nullptr &&
        plagueAdapter->fontCatalogLoadAbi ==
            renderer::unity::FontCatalogLoadAbi::systemCatalogEntry,
        "Plague's exact Unity adapter cannot preserve or verify its font catalog");

    // Plague Inc moved to Unity 6000.3.21 in 2026-09; that build keeps the
    // pathname-based legacy catalog, so its adapter must carry the same
    // discovery boundary or Korean fallback text vanishes.
    auto const plague6000Adapter = std::find_if(
        production, production + productionCount, [](const auto& adapter) {
            return std::strcmp(adapter.name, "unity-6000.3.21-x64") == 0;
        });
    Require(plague6000Adapter != production + productionCount &&
        plague6000Adapter->fontCatalogLoadRva == 0x00D9E250 &&
        plague6000Adapter->fontCatalogLoadPrefix != nullptr &&
        plague6000Adapter->fontCatalogLoadAbi ==
            renderer::unity::FontCatalogLoadAbi::systemCatalogEntry &&
        plague6000Adapter->fontLoadRva == 0 &&
        plague6000Adapter->fontLoadAbi ==
            renderer::unity::FontLoadAbi::unavailable &&
        plague6000Adapter->osFaceResolverRva == 0x00D9EC70 &&
        plague6000Adapter->osFaceResolverPrefix != nullptr &&
        plague6000Adapter->osFaceResolverAbi ==
            renderer::unity::CharacterLookupAbi::legacyDynamicFont &&
        plague6000Adapter->faceDoneRva == 0x0166C2A0 &&
        plague6000Adapter->faceDonePrefix != nullptr &&
        plague6000Adapter->faceDoneAbi ==
            renderer::unity::FaceDoneAbi::standard,
        "Plague's Unity 6000.3.21 adapter lost a required font hook boundary");

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
