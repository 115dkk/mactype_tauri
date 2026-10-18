#include "../../../renderer/big_endian.h"
#include "../../../renderer/crc32.h"
#include "../../../renderer/directwrite_virtual_font.h"
#include "../../../renderer/sfnt_names.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifdef MACTYPE_PROBE_HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#endif

namespace {

namespace big_endian = renderer::big_endian;
namespace sfnt = renderer::sfnt;

void Require(bool condition, char const* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

UINT16 Read16(std::vector<BYTE> const& bytes, std::size_t offset)
{
    auto const value = big_endian::Read16(bytes, offset);
    Require(value.has_value(), "a test read escaped its buffer");
    return *value;
}

UINT32 Read32(std::vector<BYTE> const& bytes, std::size_t offset)
{
    auto const value = big_endian::Read32(bytes, offset);
    Require(value.has_value(), "a test read escaped its buffer");
    return *value;
}

void TestBigEndianReader()
{
    std::span<const unsigned char> const empty;
    Require(big_endian::CanRead(empty, 0, 0),
        "an empty span must allow an empty read");
    Require(!big_endian::Read16(empty, 0) && !big_endian::Read32(empty, 0),
        "an empty span must reject integer reads");

    constexpr std::array<unsigned char, 16> bytes = {
        0x00, 0x00, 0xff, 0xff,
        0x00, 0x00, 0x00, 0x00,
        0xff, 0xff, 0xff, 0xff,
        0x12, 0x34, 0x56, 0x78,
    };
    Require(big_endian::CanRead(bytes, bytes.size(), 0) &&
            !big_endian::CanRead(bytes, bytes.size(), 1),
        "offset equal to size must allow only an empty read");
    Require(!big_endian::Read16(bytes, bytes.size()) &&
            !big_endian::Read32(bytes, bytes.size()),
        "offset equal to size must reject integer reads");
    Require(!big_endian::Read16(bytes, bytes.size() - 1),
        "a 16-bit read must reject size minus one");
    Require(!big_endian::Read32(bytes, bytes.size() - 3),
        "a 32-bit read must reject size minus three");
    constexpr std::size_t maximum = (std::numeric_limits<std::size_t>::max)();
    Require(!big_endian::CanRead(bytes, maximum, 1) &&
            !big_endian::Read16(bytes, maximum) &&
            !big_endian::Read32(bytes, maximum),
        "SIZE_MAX must be rejected without overflow");

    auto const zero16 = big_endian::Read16(bytes, 0);
    auto const maximum16 = big_endian::Read16(bytes, 2);
    auto const zero32 = big_endian::Read32(bytes, 4);
    auto const maximum32 = big_endian::Read32(bytes, 8);
    auto const sequence16 = big_endian::Read16(bytes, 12);
    auto const sequence32 = big_endian::Read32(bytes, 12);
    Require(zero16 && *zero16 == 0 &&
            maximum16 && *maximum16 == 0xffffU &&
            zero32 && *zero32 == 0 &&
            maximum32 && *maximum32 == 0xffffffffU &&
            sequence16 && *sequence16 == 0x1234U &&
            sequence32 && *sequence32 == 0x12345678U,
        "the reader must preserve the full integer range and byte order");
}

void TestCrc32()
{
    constexpr char text[] = "123456789";
    Require(crc32::getCrc32(0, text, sizeof(text) - 1) == 0xcbf43926U,
        "CRC-32 changed for the standard check string");
}

constexpr UINT32 kOs2 = 0x4F532F32;
constexpr UINT32 kHead = 0x68656164;
constexpr UINT32 kName = 0x6E616D65;
constexpr UINT32 kFvar = 0x66766172;
constexpr UINT32 kGlyf = 0x676C7966;
constexpr UINT32 kGvar = 0x67766172;

std::vector<BYTE> NameTable(
    std::wstring const& style,
    std::vector<std::pair<int, std::wstring>> const& variableNames = {})
{
    std::vector<std::pair<int, std::wstring>> names = {
        {1, L"Src"},
        {2, style},
        {4, L"Src " + style},
        {6, L"Src-Medium"},
        {16, L"Src"},
        {17, style},
    };
    names.insert(names.end(), variableNames.begin(), variableNames.end());
    std::vector<BYTE> table(6 + names.size() * 12, 0);
    sfnt::WriteU16(table, 2, static_cast<UINT16>(names.size()));
    sfnt::WriteU16(table, 4, static_cast<UINT16>(table.size()));
    UINT16 offset = 0;
    for (std::size_t index = 0; index < names.size(); ++index) {
        std::wstring const& value = names[index].second;
        std::size_t const record = 6 + index * 12;
        sfnt::WriteU16(table, record, 3);
        sfnt::WriteU16(table, record + 2, 1);
        sfnt::WriteU16(table, record + 4, 0x409);
        sfnt::WriteU16(table, record + 6, static_cast<UINT16>(names[index].first));
        sfnt::WriteU16(table, record + 8, static_cast<UINT16>(value.size() * 2));
        sfnt::WriteU16(table, record + 10, offset);
        for (wchar_t character : value) {
            table.push_back(static_cast<BYTE>(character >> 8));
            table.push_back(static_cast<BYTE>(character));
        }
        offset = static_cast<UINT16>(offset + value.size() * 2);
    }
    return table;
}

std::vector<BYTE> Sfnt(bool withOs2, std::wstring const& style = L"Medium")
{
    std::vector<std::pair<UINT32, std::vector<BYTE>>> tables;
    if (withOs2) {
        std::vector<BYTE> os2(96, 0);
        sfnt::WriteU16(os2, 4, 500);
        sfnt::WriteU16(os2, 62, 0x0040);
        tables.emplace_back(kOs2, os2);
    }
    tables.emplace_back(kHead, std::vector<BYTE>(54, 0));
    tables.emplace_back(kName, NameTable(style));
    std::vector<BYTE> font(12 + tables.size() * 16, 0);
    sfnt::WriteU32(font, 0, 0x00010000);
    sfnt::WriteU16(font, 4, static_cast<UINT16>(tables.size()));
    for (std::size_t index = 0; index < tables.size(); ++index) {
        while ((font.size() & 3) != 0) {
            font.push_back(0);
        }
        std::size_t const record = 12 + index * 16;
        sfnt::WriteU32(font, record, tables[index].first);
        sfnt::WriteU32(font, record + 8, static_cast<UINT32>(font.size()));
        sfnt::WriteU32(font, record + 12, static_cast<UINT32>(tables[index].second.size()));
        font.insert(font.end(), tables[index].second.begin(), tables[index].second.end());
    }
    return font;
}

std::vector<BYTE> VariableSfnt()
{
    std::vector<BYTE> os2(96, 0);
    sfnt::WriteU16(os2, 4, 400);
    sfnt::WriteU16(os2, 6, 5);
    sfnt::WriteU16(os2, 62, 0x0040);

    std::vector<BYTE> fvar(84, 0);
    sfnt::WriteU32(fvar, 0, 0x00010000);
    sfnt::WriteU16(fvar, 4, 16);
    sfnt::WriteU16(fvar, 8, 2);
    sfnt::WriteU16(fvar, 10, 20);
    sfnt::WriteU16(fvar, 12, 2);
    sfnt::WriteU16(fvar, 14, 14);
    sfnt::WriteU32(fvar, 16, 0x77676874);
    sfnt::WriteU32(fvar, 20, 100U << 16);
    sfnt::WriteU32(fvar, 24, 400U << 16);
    sfnt::WriteU32(fvar, 28, 900U << 16);
    sfnt::WriteU16(fvar, 34, 2);
    sfnt::WriteU32(fvar, 36, 0x77647468);
    sfnt::WriteU32(fvar, 40, 50U << 16);
    sfnt::WriteU32(fvar, 44, 100U << 16);
    sfnt::WriteU32(fvar, 48, 200U << 16);
    sfnt::WriteU16(fvar, 54, 261);
    sfnt::WriteU16(fvar, 56, 2);
    sfnt::WriteU32(fvar, 60, 400U << 16);
    sfnt::WriteU32(fvar, 64, 100U << 16);
    sfnt::WriteU16(fvar, 68, 17);
    sfnt::WriteU16(fvar, 70, 258);
    sfnt::WriteU32(fvar, 74, 500U << 16);
    sfnt::WriteU32(fvar, 78, 100U << 16);
    sfnt::WriteU16(fvar, 82, 259);

    std::vector<std::pair<UINT32, std::vector<BYTE>>> tables = {
        {kOs2, os2},
        {kFvar, fvar},
        {kGlyf, {1, 2, 3, 4, 5}},
        {kGvar, {6, 7, 8, 9, 10, 11}},
        {kHead, std::vector<BYTE>(54, 0)},
        {kName, NameTable(L"Regular", {
            {256, L"Regular"},
            {257, L"Src-Regular"},
            {258, L"Medium"},
            {259, L"Src-Medium"},
            {260, L"Weight"},
            {261, L"Width"},
        })},
    };
    std::vector<BYTE> font(12 + tables.size() * 16, 0);
    sfnt::WriteU32(font, 0, 0x00010000);
    sfnt::WriteU16(font, 4, static_cast<UINT16>(tables.size()));
    for (std::size_t index = 0; index < tables.size(); ++index)
    {
        while ((font.size() & 3) != 0)
            font.push_back(0);
        std::size_t const record = 12 + index * 16;
        sfnt::WriteU32(font, record, tables[index].first);
        sfnt::WriteU32(font, record + 8, static_cast<UINT32>(font.size()));
        sfnt::WriteU32(
            font, record + 12, static_cast<UINT32>(tables[index].second.size()));
        font.insert(font.end(), tables[index].second.begin(), tables[index].second.end());
    }
    return font;
}

std::vector<BYTE> Table(std::vector<BYTE> const& font, UINT32 tag, bool verifyChecksum)
{
    UINT16 const count = Read16(font, 4);
    for (UINT16 index = 0; index < count; ++index) {
        std::size_t const record = 12 + static_cast<std::size_t>(index) * 16;
        if (Read32(font, record) != tag) {
            continue;
        }
        UINT32 const offset = Read32(font, record + 8);
        UINT32 const length = Read32(font, record + 12);
        Require(sfnt::CanRead(offset, length, font.size()), "table escapes the font");
        std::vector<BYTE> table(font.begin() + offset, font.begin() + offset + length);
        std::vector<BYTE> summed = table;
        if (tag == kHead) {
            sfnt::WriteU32(summed, 8, 0);
        }
        Require(!verifyChecksum ||
                    Read32(font, record + 4) ==
                        sfnt::TableChecksum(summed.data(), summed.size()),
                "a rewritten table must carry its own checksum");
        return table;
    }
    Require(false, "an expected table is missing");
    return {};
}

std::wstring Name(std::vector<BYTE> const& font, UINT16 id)
{
    UINT16 format = 0;
    std::vector<sfnt::NameRecord> records;
    std::vector<sfnt::LanguageTagRecord> tags;
    Require(sfnt::ReadNameTable(Table(font, kName, true), format, records, tags),
            "the rewritten name table must parse");
    for (sfnt::NameRecord const& record : records) {
        std::wstring value;
        if (record.id == id && sfnt::DecodeName(record, value)) {
            return value;
        }
    }
    Require(false, "an expected name record is missing");
    return {};
}

bool HasName(std::vector<BYTE> const& font, UINT16 id)
{
    UINT16 format = 0;
    std::vector<sfnt::NameRecord> records;
    std::vector<sfnt::LanguageTagRecord> tags;
    Require(sfnt::ReadNameTable(Table(font, kName, true), format, records, tags),
        "the variable name table must parse");
    return std::any_of(
        records.begin(), records.end(),
        [id](sfnt::NameRecord const& record) { return record.id == id; });
}

UINT32 SfntAxisTag(DWRITE_FONT_AXIS_TAG tag)
{
    UINT32 const value = static_cast<UINT32>(tag);
    return ((value & 0x000000ffU) << 24) |
        ((value & 0x0000ff00U) << 8) |
        ((value & 0x00ff0000U) >> 8) |
        ((value & 0xff000000U) >> 24);
}

std::wstring AxisTag(DWRITE_FONT_AXIS_TAG tag)
{
    UINT32 const value = static_cast<UINT32>(tag);
    std::wstring text;
    text.push_back(static_cast<WCHAR>(value & 0xff));
    text.push_back(static_cast<WCHAR>((value >> 8) & 0xff));
    text.push_back(static_cast<WCHAR>((value >> 16) & 0xff));
    text.push_back(static_cast<WCHAR>((value >> 24) & 0xff));
    return text;
}

struct ExpectedAxis
{
    UINT32 tag = 0;
    FLOAT value = 0;
};

#ifdef MACTYPE_PROBE_HAVE_FREETYPE
void ValidateFreeType(
    WCHAR const* path,
    std::vector<ExpectedAxis> const& expectedAxes)
{
    int const required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, nullptr, 0, nullptr, nullptr);
    Require(required > 1, "cannot encode the FreeType fixture path");
    std::vector<char> utf8(static_cast<size_t>(required));
    Require(WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
            utf8.data(), required, nullptr, nullptr) == required,
        "cannot encode the complete FreeType fixture path");

    FT_Library library = nullptr;
    FT_Face face = nullptr;
    FT_MM_Var* variation = nullptr;
    Require(FT_Init_FreeType(&library) == 0 && library != nullptr,
        "FT_Init_FreeType failed");
    Require(FT_New_Face(library, utf8.data(), 0, &face) == 0 && face != nullptr,
        "FT_New_Face rejected the alias");
    Require(FT_Get_MM_Var(face, &variation) == 0 && variation != nullptr,
        "FT_Get_MM_Var rejected the alias variation data");
    FT_UInt defaultInstance = 0;
    Require(FT_Get_Default_Named_Instance(face, &defaultInstance) == 0 &&
            defaultInstance != 0,
        "FreeType did not identify its default named instance");
    // FreeType synthesizes the missing default instance. The fvar table keeps
    // one file-backed instance, so the API reports that instance plus default.
    Require(variation->num_namedstyles == 2 && variation->namedstyle != nullptr,
        "FreeType did not report one file-backed pinned named instance");
    unsigned int pinnedInstances = 0;
    for (FT_UInt styleIndex = 0;
        styleIndex < variation->num_namedstyles; ++styleIndex)
    {
        bool pinned = true;
        for (ExpectedAxis const& expected : expectedAxes)
        {
            FT_UInt axisIndex = 0;
            while (axisIndex < variation->num_axis &&
                variation->axis[axisIndex].tag != expected.tag)
                ++axisIndex;
            Require(axisIndex < variation->num_axis,
                "FreeType did not report an expected variation axis");
            FT_Fixed const wanted = static_cast<FT_Fixed>(
                std::llround(static_cast<double>(expected.value) * 65536.0));
            if (variation->namedstyle[styleIndex].coords[axisIndex] != wanted)
                pinned = false;
        }
        if (pinned)
            ++pinnedInstances;
    }
    Require(pinnedInstances == 1,
        "FreeType did not report the pinned coordinates exactly once");
    std::wcout << L"freetype FT_New_Face=true FT_Get_MM_Var=true "
        L"named_instances=1 synthesized_default=1" << std::endl;
    Require(FT_Done_MM_Var(library, variation) == 0,
        "FT_Done_MM_Var failed");
    Require(FT_Done_Face(face) == 0, "FT_Done_Face failed");
    Require(FT_Done_FreeType(library) == 0, "FT_Done_FreeType failed");
}
#else
void ValidateFreeType(
    WCHAR const*,
    std::vector<ExpectedAxis> const&)
{
    std::cout <<
        "freetype validation: unavailable (configure MACTYPE_PROBE_FREETYPE_ROOT)\n";
}
#endif

bool ReadAxes(
    IDWriteFont* font,
    std::vector<DWRITE_FONT_AXIS_VALUE>& values)
{
    values.clear();
    CComPtr<IDWriteFont3> font3;
    CComPtr<IDWriteFontFaceReference> reference;
    CComPtr<IDWriteFontFaceReference1> reference1;
    if (font == nullptr || FAILED(font->QueryInterface(&font3)) || font3 == nullptr ||
        FAILED(font3->GetFontFaceReference(&reference)) || reference == nullptr ||
        FAILED(reference->QueryInterface(&reference1)) || reference1 == nullptr)
        return false;
    UINT32 const count = reference1->GetFontAxisValueCount();
    values.resize(count);
    return count == 0 || SUCCEEDED(reference1->GetFontAxisValues(values.data(), count));
}

bool AxisValue(
    IDWriteFont* font,
    DWRITE_FONT_AXIS_TAG wanted,
    FLOAT& value)
{
    std::vector<DWRITE_FONT_AXIS_VALUE> values;
    if (!ReadAxes(font, values))
        return false;
    for (DWRITE_FONT_AXIS_VALUE const& axis : values)
    {
        if (axis.axisTag == wanted)
        {
            value = axis.value;
            return true;
        }
    }
    return false;
}

void PrintAxes(IDWriteFont* font)
{
    std::vector<DWRITE_FONT_AXIS_VALUE> values;
    if (!ReadAxes(font, values))
    {
        std::wcout << L" axes=<error>";
        return;
    }
    std::wcout << L" axes=[";
    for (size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0)
            std::wcout << L", ";
        std::wcout << AxisTag(values[index].axisTag) << L'=' << std::fixed <<
            std::setprecision(4) << values[index].value;
    }
    std::wcout << L']';
}

UINT32 FaceIndex(IDWriteFont* font)
{
    CComPtr<IDWriteFontFace> face;
    Require(font != nullptr && SUCCEEDED(font->CreateFontFace(&face)) && face != nullptr,
        "cannot create a face while reporting the pinned family");
    return face->GetIndex();
}

int ProbeFontFile(
    WCHAR const* path,
    UINT16 expectedWeight = 0,
    std::vector<ExpectedAxis> expectedAxes = {})
{
    Require(GetModuleHandleW(L"MacType64.dll") == nullptr &&
        GetModuleHandleW(L"MacType.dll") == nullptr,
        "the file measurement must run in an unhooked process");

    CComPtr<IDWriteFactory3> factory;
    HRESULT result = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_ISOLATED,
        __uuidof(IDWriteFactory3),
        reinterpret_cast<IUnknown**>(&factory));
    Require(SUCCEEDED(result) && factory != nullptr,
        "cannot create the DirectWrite factory");

    CComPtr<IDWriteFontFile> file;
    result = factory->CreateFontFileReference(path, nullptr, &file);
    Require(SUCCEEDED(result) && file != nullptr,
        "cannot create the DirectWrite font file reference");
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE fileType = DWRITE_FONT_FILE_TYPE_UNKNOWN;
    DWRITE_FONT_FACE_TYPE faceType = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    UINT32 faceCount = 0;
    result = file->Analyze(&supported, &fileType, &faceType, &faceCount);
    Require(SUCCEEDED(result) && supported && faceCount != 0,
        "IDWriteFontFile::Analyze rejected the file");
    std::wcout << L"analyze supported=true face_count=" << faceCount << L'\n';
    if (expectedWeight != 0)
    {
        Require(!expectedAxes.empty(),
            "the strict variable probe has no expected axes");
        ValidateFreeType(path, expectedAxes);
    }

    CComPtr<IDWriteFontSetBuilder> builder;
    CComPtr<IDWriteFontSetBuilder1> builder1;
    Require(SUCCEEDED(factory->CreateFontSetBuilder(&builder)) && builder != nullptr &&
        SUCCEEDED(builder->QueryInterface(&builder1)) && builder1 != nullptr &&
        SUCCEEDED(builder1->AddFontFile(file)),
        "cannot add the font file to a DirectWrite font set builder");
    CComPtr<IDWriteFontSet> fontSet;
    Require(SUCCEEDED(builder->CreateFontSet(&fontSet)) && fontSet != nullptr,
        "cannot create the DirectWrite font set");
    CComPtr<IDWriteFontCollection1> collection;
    Require(SUCCEEDED(factory->CreateFontCollectionFromFontSet(fontSet, &collection)) &&
        collection != nullptr, "cannot create the DirectWrite font collection");

    UINT32 const familyCount = collection->GetFontFamilyCount();
    std::wcout << L"family_count=" << familyCount << L'\n';
    Require(familyCount != 0, "the DirectWrite collection has no families");
    for (UINT32 familyIndex = 0; familyIndex < familyCount; ++familyIndex)
    {
        CComPtr<IDWriteFontFamily> family;
        Require(SUCCEEDED(collection->GetFontFamily(familyIndex, &family)) &&
            family != nullptr, "cannot read a DirectWrite font family");
        UINT32 const fontCount = family->GetFontCount();
        std::wcout << L"family[" << familyIndex << L"].font_count=" <<
            fontCount << L'\n';
        for (UINT32 fontIndex = 0; fontIndex < fontCount; ++fontIndex)
        {
            CComPtr<IDWriteFont> font;
            Require(SUCCEEDED(family->GetFont(fontIndex, &font)) && font != nullptr,
                "cannot read a DirectWrite font");
            UINT32 const weight = static_cast<UINT32>(font->GetWeight());
            FLOAT axisWeight = 0;
            if (expectedWeight != 0)
            {
                Require(weight != 400,
                    "a pinned family still reports a weight-400 font");
                Require(AxisValue(font, DWRITE_FONT_AXIS_TAG_WEIGHT, axisWeight) &&
                    std::fabs(axisWeight - 400.0f) > 0.01f,
                    "a pinned family still reports a wght-400 font");
            }
            std::wcout << L"font[" << fontIndex << L"] weight=" << weight <<
                L" simulations=0x" << std::hex <<
                static_cast<UINT32>(font->GetSimulations()) << std::dec <<
                L" face_index=" << FaceIndex(font);
            PrintAxes(font);
            std::wcout << L'\n';
        }

        CComPtr<IDWriteFont> matching;
        Require(SUCCEEDED(family->GetFirstMatchingFont(
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                &matching)) && matching != nullptr,
            "GetFirstMatchingFont(400) failed");
        std::wcout << L"first_match_400 weight=" <<
            static_cast<unsigned int>(matching->GetWeight());
        PrintAxes(matching);
        std::wcout << L'\n';
        if (expectedWeight != 0)
        {
            FLOAT matchingWeight = 0;
            Require(AxisValue(
                    matching, DWRITE_FONT_AXIS_TAG_WEIGHT, matchingWeight) &&
                    std::fabs(matchingWeight - expectedWeight) < 0.01f,
                "GetFirstMatchingFont(400) did not retain the pinned coordinate");
        }
    }
    return 0;
}

int RewriteVariableFile(WCHAR const* input, WCHAR const* output, UINT16 weight)
{
    std::ifstream stream(std::filesystem::path(input), std::ios::binary);
    Require(static_cast<bool>(stream), "cannot open the variable alias input");
    std::istreambuf_iterator<char> first(stream);
    std::istreambuf_iterator<char> last;
    std::vector<BYTE> source(first, last);
    Require(!source.empty(), "the variable alias input is empty");

    directwrite_virtual_font::AliasOptions options;
    options.axisValues = {
        {DWRITE_FONT_AXIS_TAG_WEIGHT, static_cast<FLOAT>(weight)},
        {DWRITE_FONT_AXIS_TAG_ITALIC, 0.0f},
        {DWRITE_FONT_AXIS_TAG_WIDTH, 100.0f},
        {DWRITE_FONT_AXIS_TAG_SLANT, 0.0f},
    };
    std::vector<BYTE> rewritten;
    directwrite_virtual_font::Identity identity;
    Require(SUCCEEDED(directwrite_virtual_font::BuildAliasedSfnt(
            source, 0, L"Malgun Gothic", options, rewritten, identity)),
        "cannot rewrite the pinned variable alias");

    std::vector<BYTE> const sourceFvar = Table(source, kFvar, false);
    std::vector<BYTE> const rewrittenFvar = Table(rewritten, kFvar, true);
    UINT16 const sourceAxesOffset = Read16(sourceFvar, 4);
    UINT16 const rewrittenAxesOffset = Read16(rewrittenFvar, 4);
    UINT16 const axisCount = Read16(sourceFvar, 8);
    UINT16 const axisSize = Read16(sourceFvar, 10);
    size_t const axesBytes = static_cast<size_t>(axisCount) * axisSize;
    Require(sourceAxesOffset == rewrittenAxesOffset &&
            Read16(rewrittenFvar, 8) == axisCount &&
            Read16(rewrittenFvar, 10) == axisSize &&
            sfnt::CanRead(sourceAxesOffset, axesBytes, sourceFvar.size()) &&
            sfnt::CanRead(rewrittenAxesOffset, axesBytes, rewrittenFvar.size()) &&
            std::equal(
                sourceFvar.begin() + sourceAxesOffset,
                sourceFvar.begin() + sourceAxesOffset + axesBytes,
                rewrittenFvar.begin() + rewrittenAxesOffset),
        "fvar axis records or defaults changed while pinning");
    Require(Read16(rewrittenFvar, 12) == 1,
        "the rewritten fvar table does not contain one named instance");
    std::vector<ExpectedAxis> expectedAxes;
    for (UINT16 axisIndex = 0; axisIndex < axisCount; ++axisIndex)
    {
        UINT32 const tag = Read32(
            rewrittenFvar, rewrittenAxesOffset +
                static_cast<size_t>(axisIndex) * axisSize);
        auto const option = std::find_if(
            options.axisValues.begin(), options.axisValues.end(),
            [tag](DWRITE_FONT_AXIS_VALUE const& axis) {
                return SfntAxisTag(axis.axisTag) == tag;
            });
        Require(option != options.axisValues.end(),
            "the rewritten fvar table retained an unpinned axis");
        expectedAxes.push_back({tag, option->value});
    }
    Require(Table(rewritten, kGlyf, true) == Table(source, kGlyf, false) &&
            Table(rewritten, kGvar, true) == Table(source, kGvar, false),
        "glyf or gvar bytes changed while pinning");

    std::ofstream destination(
        std::filesystem::path(output), std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(destination), "cannot create the rewritten alias");
    destination.write(
        reinterpret_cast<char const*>(rewritten.data()),
        static_cast<std::streamsize>(rewritten.size()));
    Require(static_cast<bool>(destination), "cannot write the rewritten alias");
    destination.close();
    Require(static_cast<bool>(destination), "cannot close the rewritten alias");
    std::wcout << L"rewritten_bytes=" << rewritten.size() << L" weight=" <<
        weight << L'\n';
    return ProbeFontFile(output, weight, std::move(expectedAxes));
}

} // namespace

int wmain(int argc, WCHAR** argv)
{
    if (argc == 2)
        return ProbeFontFile(argv[1]);
    if (argc == 5 && wcscmp(argv[1], L"--rewrite-variable") == 0)
        return RewriteVariableFile(
            argv[2], argv[3], static_cast<UINT16>(_wtoi(argv[4])));
    Require(argc == 1,
        "usage: directwrite-virtual-font-tests.exe [font-file] | "
        "--rewrite-variable input output weight");
    TestBigEndianReader();
    TestCrc32();
#ifndef MACTYPE_PROBE_HAVE_FREETYPE
    ValidateFreeType(nullptr, {});
#endif
    namespace font = directwrite_virtual_font;
    std::vector<BYTE> const source = Sfnt(true);
    std::vector<BYTE> plain;
    font::Identity identity;
    Require(SUCCEEDED(font::BuildAliasedSfnt(
                source, 0, L"Alias", font::AliasOptions(), plain, identity)) &&
                identity.family == L"Alias",
            "the plain alias must build");
    Require(Table(plain, kOs2, true) == Table(source, kOs2, false) &&
                Read16(Table(plain, kHead, true), 44) == 0,
            "an alias without a weight override must keep OS/2 and head bytes");

    font::AliasOptions bold;
    bold.overrideWeight = true;
    bold.weight = 700;
    std::vector<BYTE> advertised;
    Require(SUCCEEDED(font::BuildAliasedSfnt(
                source, 0, L"Alias", bold, advertised, identity)),
            "the weight-advertising alias must build");
    std::vector<BYTE> const os2 = Table(advertised, kOs2, true);
    Require(Read16(os2, 4) == 700 && Read16(os2, 62) == 0x0020,
            "a bold advertisement must set usWeightClass and BOLD and clear REGULAR");
    Require((Read16(Table(advertised, kHead, true), 44) & 1) != 0,
            "a bold advertisement must set head.macStyle bold");
    Require(sfnt::TableChecksum(advertised.data(), advertised.size()) == 0xB1B0AFBA,
            "the head checksum adjustment must cover the rewritten tables");
    Require(Name(advertised, 1) == L"Alias" && Name(advertised, 16) == L"Alias" &&
                Name(advertised, 2) == L"Bold" && Name(advertised, 17) == L"Bold" &&
                Name(advertised, 4) == L"Alias Bold" &&
                Name(advertised, 6) == L"Alias-Bold" &&
                identity.fullName == L"Alias Bold" &&
                identity.postScriptName == L"Alias-Bold",
            "a bold advertisement must name its own Bold subfamily, full and PostScript names");
    Require(Name(plain, 2) == L"Medium" && Name(plain, 17) == L"Medium" &&
                Name(plain, 4) == L"Alias Medium" && Name(plain, 6) == L"Alias-Medium",
            "the plain alias must keep the backing subfamily");

    std::vector<BYTE> italic;
    Require(SUCCEEDED(font::BuildAliasedSfnt(
                Sfnt(true, L"Medium Italic"), 0, L"Alias", bold, italic, identity)) &&
                Name(italic, 2) == L"Bold Italic" && Name(italic, 17) == L"Bold Italic" &&
                Name(italic, 4) == L"Alias Bold Italic" &&
                Name(italic, 6) == L"Alias-BoldItalic",
            "a bold advertisement of an italic backing must stay italic");

    bold.weight = 400;
    Require(SUCCEEDED(font::BuildAliasedSfnt(
                advertised, 0, L"Alias", bold, plain, identity)) &&
                Read16(Table(plain, kOs2, true), 62) == 0x0040 &&
                (Read16(Table(plain, kHead, true), 44) & 1) == 0,
            "a regular advertisement must clear the bold flags again");
    Require(Name(plain, 2) == L"Bold",
            "a regular advertisement must leave the subfamily names alone");

    bold.weight = 700;
    Require(font::BuildAliasedSfnt(
                Sfnt(false), 0, L"Alias", bold, advertised, identity) ==
                DWRITE_E_FILEFORMAT,
            "a weight advertisement without OS/2 must fail closed");

    std::vector<BYTE> const variableSource = VariableSfnt();
    font::AliasOptions pinned;
    pinned.axisValues = {
        {DWRITE_FONT_AXIS_TAG_WEIGHT, 500.0f},
        {DWRITE_FONT_AXIS_TAG_WIDTH, 100.0f},
    };
    std::vector<BYTE> pinnedAlias;
    Require(SUCCEEDED(font::BuildAliasedSfnt(
                variableSource, 0, L"Alias", pinned, pinnedAlias, identity)),
            "the pinned variable alias must build");
    std::vector<BYTE> const pinnedFvar = Table(pinnedAlias, kFvar, true);
    Require(Read16(pinnedFvar, 12) == 1 && pinnedFvar.size() == 70 &&
                Read16(pinnedFvar, 56) == 258 &&
                Read32(pinnedFvar, 60) == (500U << 16) &&
                Read32(pinnedFvar, 64) == (100U << 16),
            "fvar must retain only the pinned named instance without moving its axes");
    Require(HasName(pinnedAlias, 2) && HasName(pinnedAlias, 17) &&
                HasName(pinnedAlias, 256) && HasName(pinnedAlias, 257) &&
                HasName(pinnedAlias, 258) && HasName(pinnedAlias, 259) &&
                HasName(pinnedAlias, 260) && HasName(pinnedAlias, 261) &&
                Read16(pinnedFvar, 34) == 2,
            "pinning removed shared or unused name records");
    Require(Table(pinnedAlias, kGlyf, true) == Table(variableSource, kGlyf, false) &&
                Table(pinnedAlias, kGvar, true) == Table(variableSource, kGvar, false),
            "pinning a variable alias must leave glyf and gvar bytes unchanged");
    std::vector<BYTE> const pinnedOs2 = Table(pinnedAlias, kOs2, true);
    Require(Read16(pinnedOs2, 4) == 500 &&
                Read16(pinnedOs2, 6) == 5 &&
                (Read16(pinnedOs2, 62) & 0x0020) == 0 &&
                (Read16(Table(pinnedAlias, kHead, true), 44) & 1) == 0,
            "pinned axes must set OS/2 weight and width and head bold state");

    font::AliasOptions pinnedBold = pinned;
    pinnedBold.overrideWeight = true;
    pinnedBold.weight = 700;
    std::vector<BYTE> pinnedBoldAlias;
    Require(SUCCEEDED(font::BuildAliasedSfnt(
                variableSource, 0, L"Alias", pinnedBold, pinnedBoldAlias, identity)),
            "the pinned bold-advertised alias must build");
    std::vector<BYTE> const pinnedBoldOs2 = Table(pinnedBoldAlias, kOs2, true);
    Require(Read16(pinnedBoldOs2, 4) == 700 &&
                (Read16(pinnedBoldOs2, 62) & 0x0020) != 0 &&
                (Read16(Table(pinnedBoldAlias, kHead, true), 44) & 1) != 0 &&
                pinnedBoldAlias != pinnedAlias,
            "an explicit weight override was replaced by the pinned coordinate");

    pinned.axisValues[0].value = 550.0f;
    Require(font::BuildAliasedSfnt(
                variableSource, 0, L"Alias", pinned, pinnedAlias, identity) ==
                DWRITE_E_FILEFORMAT,
            "an unnamed variable coordinate must fail instead of moving fvar defaults");

    std::cout << "DirectWrite virtual font tests passed.\n";
    return 0;
}
