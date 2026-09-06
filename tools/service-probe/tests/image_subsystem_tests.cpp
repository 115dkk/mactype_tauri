#include "../../../renderer/image_subsystem.h"

#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <windows.h>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path Expand(const wchar_t* value)
{
    const DWORD required = ExpandEnvironmentStringsW(value, nullptr, 0);
    Require(required > 1, "environment expansion failed");
    std::wstring expanded(required, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(
        value, expanded.data(), static_cast<DWORD>(expanded.size()));
    Require(written == required, "environment expansion was truncated");
    expanded.resize(required - 1);
    return expanded;
}

template <typename Value>
void Store(std::vector<unsigned char>& bytes, std::size_t offset, Value value)
{
    Require(offset <= bytes.size() && sizeof(Value) <= bytes.size() - offset,
            "test fixture write exceeded its buffer");
    std::memcpy(bytes.data() + offset, &value, sizeof(Value));
}

} // namespace

int main()
{
    using renderer::image_subsystem::ImageSubsystem;

    Require(
        renderer::image_subsystem::Classify(
            Expand(L"%SystemRoot%\\System32\\cmd.exe").c_str()) ==
            ImageSubsystem::console,
        "cmd.exe must be classified as a console image");
    Require(
        renderer::image_subsystem::Classify(
            Expand(L"%SystemRoot%\\explorer.exe").c_str()) ==
            ImageSubsystem::gui,
        "explorer.exe must be classified as a GUI image");

    const std::filesystem::path fixtureRoot =
        std::filesystem::temp_directory_path() /
        ("mactype-image-subsystem-" +
         std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64()));
    std::filesystem::create_directories(fixtureRoot);

    const std::filesystem::path junk = fixtureRoot / "junk.exe";
    {
        std::ofstream output(junk, std::ios::binary | std::ios::trunc);
        output << "not a portable executable";
    }
    Require(
        renderer::image_subsystem::Classify(junk.c_str()) ==
            ImageSubsystem::unavailable,
        "junk input must be unavailable");
    Require(
        renderer::image_subsystem::Classify(
            (fixtureRoot / "missing.exe").c_str()) ==
            ImageSubsystem::unavailable,
        "a missing image must be unavailable");

    const std::filesystem::path truncated = fixtureRoot / "truncated.exe";
    {
        std::vector<unsigned char> bytes(0x78, 0);
        Store<std::uint16_t>(bytes, 0, 0x5a4d);
        Store<std::int32_t>(bytes, 0x3c, 0x60);
        Store<std::uint32_t>(bytes, 0x60, 0x00004550);
        Store<std::uint16_t>(bytes, 0x60 + 4 + 16, 0x00f0);
        std::ofstream output(truncated, std::ios::binary | std::ios::trunc);
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    Require(
        renderer::image_subsystem::Classify(truncated.c_str()) ==
            ImageSubsystem::unavailable,
        "an optional header truncated from the bytes read must be unavailable");

    std::error_code ignored;
    std::filesystem::remove_all(fixtureRoot, ignored);
    return 0;
}
