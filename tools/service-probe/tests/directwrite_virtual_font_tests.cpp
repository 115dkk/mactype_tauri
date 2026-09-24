#include "../../../renderer/directwrite_virtual_font.h"
#include "../../../renderer/sfnt_names.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace sfnt = renderer::sfnt;

void Require(bool condition, char const* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

constexpr UINT32 kOs2 = 0x4F532F32;
constexpr UINT32 kHead = 0x68656164;
constexpr UINT32 kName = 0x6E616D65;

std::vector<BYTE> NameTable()
{
    std::wstring const family = L"Src";
    std::wstring const style = L"Medium";
    std::vector<BYTE> table(6 + 2 * 12, 0);
    sfnt::WriteU16(table, 2, 2);
    sfnt::WriteU16(table, 4, static_cast<UINT16>(table.size()));
    UINT16 offset = 0;
    UINT16 id = 1;
    for (std::wstring const* value : {&family, &style}) {
        std::size_t const record = 6 + static_cast<std::size_t>(id - 1) * 12;
        sfnt::WriteU16(table, record, 3);
        sfnt::WriteU16(table, record + 2, 1);
        sfnt::WriteU16(table, record + 4, 0x409);
        sfnt::WriteU16(table, record + 6, id++);
        sfnt::WriteU16(table, record + 8, static_cast<UINT16>(value->size() * 2));
        sfnt::WriteU16(table, record + 10, offset);
        for (wchar_t character : *value) {
            table.push_back(static_cast<BYTE>(character >> 8));
            table.push_back(static_cast<BYTE>(character));
        }
        offset = static_cast<UINT16>(offset + value->size() * 2);
    }
    return table;
}

std::vector<BYTE> Sfnt(bool withOs2)
{
    std::vector<std::pair<UINT32, std::vector<BYTE>>> tables;
    if (withOs2) {
        std::vector<BYTE> os2(96, 0);
        sfnt::WriteU16(os2, 4, 500);
        sfnt::WriteU16(os2, 62, 0x0040);
        tables.emplace_back(kOs2, os2);
    }
    tables.emplace_back(kHead, std::vector<BYTE>(54, 0));
    tables.emplace_back(kName, NameTable());
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

std::vector<BYTE> Table(std::vector<BYTE> const& font, UINT32 tag, bool verifyChecksum)
{
    UINT16 const count = sfnt::ReadU16(font, 4);
    for (UINT16 index = 0; index < count; ++index) {
        std::size_t const record = 12 + static_cast<std::size_t>(index) * 16;
        if (sfnt::ReadU32(font, record) != tag) {
            continue;
        }
        UINT32 const offset = sfnt::ReadU32(font, record + 8);
        UINT32 const length = sfnt::ReadU32(font, record + 12);
        Require(sfnt::CanRead(offset, length, font.size()), "table escapes the font");
        std::vector<BYTE> table(font.begin() + offset, font.begin() + offset + length);
        std::vector<BYTE> summed = table;
        if (tag == kHead) {
            sfnt::WriteU32(summed, 8, 0);
        }
        Require(!verifyChecksum ||
                    sfnt::ReadU32(font, record + 4) ==
                        sfnt::TableChecksum(summed.data(), summed.size()),
                "a rewritten table must carry its own checksum");
        return table;
    }
    Require(false, "an expected table is missing");
    return {};
}

} // namespace

int main()
{
    namespace font = directwrite_virtual_font;
    std::vector<BYTE> const source = Sfnt(true);
    std::vector<BYTE> plain;
    font::Identity identity;
    Require(SUCCEEDED(font::BuildAliasedFontFile(
                source, 0, L"Alias", font::AliasOptions(), plain, identity)) &&
                identity.family == L"Alias",
            "the plain alias must build");
    Require(Table(plain, kOs2, true) == Table(source, kOs2, false) &&
                sfnt::ReadU16(Table(plain, kHead, true), 44) == 0,
            "an alias without a weight override must keep OS/2 and head bytes");

    font::AliasOptions bold;
    bold.overrideWeight = true;
    bold.weight = 700;
    std::vector<BYTE> advertised;
    Require(SUCCEEDED(font::BuildAliasedFontFile(
                source, 0, L"Alias", bold, advertised, identity)),
            "the weight-advertising alias must build");
    std::vector<BYTE> const os2 = Table(advertised, kOs2, true);
    Require(sfnt::ReadU16(os2, 4) == 700 && sfnt::ReadU16(os2, 62) == 0x0020,
            "a bold advertisement must set usWeightClass and BOLD and clear REGULAR");
    Require((sfnt::ReadU16(Table(advertised, kHead, true), 44) & 1) != 0,
            "a bold advertisement must set head.macStyle bold");
    Require(sfnt::TableChecksum(advertised.data(), advertised.size()) == 0xB1B0AFBA,
            "the head checksum adjustment must cover the rewritten tables");
    Require(Table(advertised, kName, true) == Table(plain, kName, false),
            "a weight advertisement must not change the alias names");

    bold.weight = 400;
    Require(SUCCEEDED(font::BuildAliasedFontFile(
                advertised, 0, L"Alias", bold, plain, identity)) &&
                sfnt::ReadU16(Table(plain, kOs2, true), 62) == 0x0040 &&
                (sfnt::ReadU16(Table(plain, kHead, true), 44) & 1) == 0,
            "a regular advertisement must clear the bold flags again");

    bold.weight = 700;
    Require(font::BuildAliasedFontFile(
                Sfnt(false), 0, L"Alias", bold, advertised, identity) ==
                DWRITE_E_FILEFORMAT,
            "a weight advertisement without OS/2 must fail closed");

    std::cout << "DirectWrite virtual font tests passed.\n";
    return 0;
}
