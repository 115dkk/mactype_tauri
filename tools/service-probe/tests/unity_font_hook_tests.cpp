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
#include <memory>
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
