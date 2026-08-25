#include "../../../renderer/unity_font_catalog.h"
#include "../../../renderer/unity_font_evidence.h"

#include <windows.h>

#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

bool EqualOrdinalIgnoreCase(
    const std::wstring& left,
    const wchar_t* right) noexcept
{
    if (right == nullptr)
        return false;
    std::wstring const rightValue(right);
    if (left.size() != rightValue.size() ||
        left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        return false;
    return CompareStringOrdinal(
        left.data(), static_cast<int>(left.size()),
        rightValue.data(), static_cast<int>(rightValue.size()), TRUE) == CSTR_EQUAL;
}

std::size_t MappedImageSize(HMODULE module) noexcept
{
    if (module == nullptr)
        return 0;
    const auto* dos = static_cast<const IMAGE_DOS_HEADER*>(
        static_cast<const void*>(module));
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0)
        return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const unsigned char*>(module) + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE
        ? static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage)
        : 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        std::wcerr << L"usage: unity-font-catalog-probe <family> [...]\n";
        return 64;
    }
    if (argc == 3 && wcscmp(argv[1], L"--adapter") == 0)
    {
        HMODULE const module = LoadLibraryExW(
            argv[2], nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (module == nullptr)
            return 4;
        std::size_t const mappedSize = MappedImageSize(module);
        std::size_t descriptorCount = 0;
        auto const descriptors =
            renderer::unity::ProductionAdapterDescriptors(&descriptorCount);
        renderer::unity::ResolvedAdapter adapter{};
        bool const resolved = mappedSize != 0 &&
            renderer::unity::ResolveAdapter(
                module, mappedSize, descriptors, descriptorCount, &adapter);
        if (resolved)
            std::cout << adapter.name << "\t0x" << std::hex
                << adapter.targetRva << "\tface=0x"
                << adapter.faceOpenRva << '\n';
        FreeLibrary(module);
        return resolved ? 0 : 5;
    }
    if (argc == 3 && wcscmp(argv[1], L"--evidence") == 0)
    {
        wchar_t* end = nullptr;
        unsigned long const parsed = wcstoul(argv[2], &end, 10);
        if (parsed == 0 || end == nullptr || *end != L'\0' ||
            parsed > (std::numeric_limits<DWORD>::max)())
            return 6;
        wchar_t mappingName[96]{};
        if (!renderer::unity::FormatUnityFontEvidenceMappingName(
            static_cast<DWORD>(parsed), mappingName, _countof(mappingName)))
            return 6;
        HANDLE const mapping = OpenFileMappingW(
            FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mappingName);
        if (mapping == nullptr)
            return 7;
        void* const view = MapViewOfFile(
            mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
            sizeof(renderer::unity::UnityFontEvidenceV1));
        if (view == nullptr)
        {
            CloseHandle(mapping);
            return 7;
        }
        renderer::unity::UnityFontEvidenceSnapshot snapshot{};
        bool const read = renderer::unity::ReadUnityFontEvidence(
            *static_cast<const renderer::unity::UnityFontEvidenceV1*>(view),
            snapshot);
        if (read)
        {
            std::wcout << L"pid=" << snapshot.pid
                << L" observed=" << snapshot.observedFontOpens
                << L" attempts=" << snapshot.redirectAttempts
                << L" successes=" << snapshot.redirectSuccesses
                << L" fallbacks=" << snapshot.redirectFallbacks
                << L" renders=" << snapshot.renderCalls
                << L" render-successes=" << snapshot.renderSuccesses
                << L" bitmaps=" << snapshot.nonEmptyBitmaps
                << L" last-error=" << snapshot.lastRenderError
                << L" last-glyph=" << snapshot.lastGlyphIndex
                << L" last-width=" << snapshot.lastBitmapWidth
                << L" last-rows=" << snapshot.lastBitmapRows
                << L" charmap=" << snapshot.redirectedFaceHasCharmap
                << L" face-glyphs=" << snapshot.redirectedFaceGlyphs
                << L" sample-glyph=" << snapshot.sampleKoreanGlyph
                << L" lookups=" << snapshot.characterLookups
                << L" lookup-hits=" << snapshot.characterLookupHits
                << L" last-character=" << snapshot.lastCharacter
                << L" last-lookup-glyph=" << snapshot.lastLookupGlyph
                << L" face-resolutions=" << snapshot.faceResolutions
                << L" face-resolution-hits=" << snapshot.faceResolutionHits
                << L" face-glyph-hits=" << snapshot.faceResolutionGlyphHits
                << L" last-face-glyph=" << snapshot.lastFaceResolutionGlyph
                << L" last-face-sample="
                << snapshot.lastFaceResolutionSampleKoreanGlyph
                << L" family=" << snapshot.lastLookupFamily
                << L" mapped-lookups=" << snapshot.mappedCharacterLookups
                << L" mapped-hits=" << snapshot.mappedCharacterLookupHits
                << L" mapped-character=" << snapshot.lastMappedCharacter
                << L" mapped-glyph=" << snapshot.lastMappedGlyph
                << L" mapped-sample="
                << snapshot.lastMappedSampleKoreanGlyph
                << L" mapped-family=" << snapshot.lastMappedFamily
                << L" mapped-face-family="
                << snapshot.lastMappedResolvedFaceFamily
                << L" os-resolutions=" << snapshot.osFaceResolutions
                << L" os-hits=" << snapshot.osFaceResolutionHits
                << L" os-sample=" << snapshot.lastOsFaceSampleKoreanGlyph
                << L" os-family=" << snapshot.lastOsFaceFamily
                << L" os-face-family=" << snapshot.lastOsResolvedFaceFamily
                << L" mapped-os-resolutions="
                << snapshot.mappedOsFaceResolutions
                << L" mapped-os-hits="
                << snapshot.mappedOsFaceResolutionHits
                << L" mapped-os-sample="
                << snapshot.lastMappedOsFaceSampleKoreanGlyph
                << L" mapped-os-family=" << snapshot.lastMappedOsFaceFamily
                << L" mapped-os-face-family="
                << snapshot.lastMappedOsResolvedFaceFamily
                << L" observed-path=" << snapshot.observedPath
                << L" source=" << snapshot.sourcePath
                << L" replacement=" << snapshot.replacementPath << L'\n';
        }
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return read && snapshot.redirectSuccesses > 0 ? 0 : 8;
    }

    std::vector<renderer::unity::InstalledFontFace> fonts;
    if (!renderer::unity::EnumerateInstalledFontFaces(fonts))
        return 1;
    if (argc == 5 && wcscmp(argv[1], L"--redirect") == 0)
    {
        auto substitutions = renderer::font_substitution::Snapshot::Build(
            {{argv[2], argv[3]}}, 1);
        auto redirects = renderer::unity::FontFileRedirectTable::Build(
            fonts, *substitutions);
        std::wstring replacement;
        if (!redirects || !redirects->Resolve(argv[4], replacement))
            return 3;
        std::wcout << replacement << L'\n';
        return 0;
    }
    if (argc == 6 && wcscmp(argv[1], L"--redirect-face") == 0)
    {
        wchar_t* end = nullptr;
        long const sourceFaceIndex = wcstol(argv[5], &end, 10);
        if (end == nullptr || *end != L'\0')
            return 6;
        auto substitutions = renderer::font_substitution::Snapshot::Build(
            {{argv[2], argv[3]}}, 1);
        auto redirects = renderer::unity::FontFileRedirectTable::Build(
            fonts, *substitutions);
        std::wstring replacement;
        long replacementFaceIndex = 0;
        if (!redirects || !redirects->ResolveFace(
            argv[4], sourceFaceIndex,
            replacement, replacementFaceIndex))
            return 3;
        std::wcout << replacement << L'\t' << replacementFaceIndex << L'\n';
        return 0;
    }
    bool allFound = true;
    for (int argument = 1; argument < argc; ++argument)
    {
        bool found = false;
        for (const renderer::unity::InstalledFontFace& font : fonts)
        {
            if (!EqualOrdinalIgnoreCase(font.family, argv[argument]))
                continue;
            std::wcout << font.family << L'\t' << font.filePath
                << L'\t' << font.faceIndex << L'\n';
            found = true;
        }
        allFound = allFound && found;
    }
    return allFound ? 0 : 2;
}
