#include "../../../renderer/profile_text.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::vector<unsigned char> Bytes(std::initializer_list<unsigned char> prefix, const std::string& body)
{
    std::vector<unsigned char> bytes(prefix);
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

std::vector<unsigned char> Utf16(std::initializer_list<unsigned char> bom, const std::wstring& text, bool bigEndian)
{
    std::vector<unsigned char> bytes(bom);
    for (wchar_t unit : text)
    {
        const unsigned char high = static_cast<unsigned char>((unit >> 8) & 0xFF);
        const unsigned char low = static_cast<unsigned char>(unit & 0xFF);
        bytes.push_back(bigEndian ? high : low);
        bytes.push_back(bigEndian ? low : high);
    }
    return bytes;
}

std::wstring Decoded(const std::vector<unsigned char>& bytes, unsigned int legacyCodePage)
{
    std::wstring text;
    Require(renderer::profile_text::Decode(bytes.data(), bytes.size(), legacyCodePage, text),
        "profile bytes must decode");
    return text;
}

unsigned int RegistryAnsiCodePage()
{
    wchar_t value[16] = {};
    DWORD size = sizeof(value);
    Require(RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Nls\\CodePage",
        L"ACP", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS,
        "the system ANSI code page must be readable from the registry");
    return static_cast<unsigned int>(std::wcstoul(value, nullptr, 10));
}

} // namespace

int main()
{
    // "[FontSubstitutes]\r\n굴림=Pretendard Variable Medium\r\n" as the
    // Control Center preserves a legacy Korean profile (CP949, no BOM).
    const std::string cp949Body =
        "[FontSubstitutes]\r\n\xB1\xBC\xB8\xB2=Pretendard Variable Medium\r\n";
    const std::wstring expected = L"[FontSubstitutes]\r\n굴림=Pretendard Variable Medium\r\n";
    const std::string utf8Body =
        "[FontSubstitutes]\r\n\xEA\xB5\xB4\xEB\xA6\xBC=Pretendard Variable Medium\r\n";

    Require(Decoded(Bytes({}, cp949Body), 949) == expected,
        "a legacy profile decodes with the supplied system code page");
    Require(Decoded(Bytes({}, utf8Body), 949) == expected,
        "UTF-8 without a BOM wins over the legacy code page, as in the Control Center");
    Require(Decoded(Bytes({0xEF, 0xBB, 0xBF}, utf8Body), 949) == expected,
        "a UTF-8 BOM is stripped");
    Require(Decoded(Utf16({0xFF, 0xFE}, expected, false), 949) == expected,
        "a UTF-16LE BOM is stripped");
    Require(Decoded(Utf16({0xFE, 0xFF}, expected, true), 949) == expected,
        "a UTF-16BE BOM is stripped");
    Require(Decoded(Bytes({}, "[General]\r\nHintingMode=1\r\n"), 1252) == L"[General]\r\nHintingMode=1\r\n",
        "ASCII profiles are unchanged");

    // A system-wide UTF-8 code page cannot represent CP949 family names. The
    // ASCII settings must still load, as they did before this decoder existed.
    const std::wstring lossy = Decoded(Bytes({}, cp949Body), CP_UTF8);
    Require(lossy.find(L"[FontSubstitutes]") == 0 && lossy.find(L"=Pretendard Variable Medium") != std::wstring::npos,
        "an undecodable legacy profile keeps its ASCII settings");

    std::wstring empty;
    Require(!renderer::profile_text::Decode(nullptr, 0, 949, empty), "an empty profile is rejected");

    // This executable declares activeCodePage UTF-8, as soffice.bin does:
    // CP_ACP is UTF-8 here, while the profile's legacy code page is not.
    Require(GetACP() == CP_UTF8, "the test manifest must select the UTF-8 active code page");
    Require(renderer::profile_text::SystemAnsiCodePage() == RegistryAnsiCodePage(),
        "the legacy code page comes from the system locale, not from the process manifest");
    return 0;
}
