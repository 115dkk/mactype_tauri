#include "../../../renderer/private_freetype_policy.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

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

} // namespace

int main()
{
    constexpr std::string_view marker = "windows:fontengine=freetype";
    Require(
        renderer::private_freetype::ContainsExplicitQtFreeTypeMarker(
            marker.data(), marker.size()),
        "an explicit Qt FreeType engine selection must be detected");
    constexpr wchar_t wideMarker[] = L"windows:fontengine=freetype";
    Require(
        renderer::private_freetype::ContainsExplicitQtFreeTypeMarker(
            wideMarker, sizeof(wideMarker)),
        "an explicit UTF-16 Qt FreeType engine selection must be detected");
    Require(
        renderer::private_freetype::ShouldSkipHooks(true, true, false),
        "the opt-in policy must skip a detected non-Unity private FreeType process");
    Require(
        !renderer::private_freetype::ShouldSkipHooks(true, true, true),
        "private FreeType avoidance must not disable an enabled Unity hook target");

    const std::filesystem::path fixtureRoot =
        std::filesystem::temp_directory_path() /
        ("mactype-private-freetype-" +
         std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64()));
    std::filesystem::create_directories(fixtureRoot);
    const std::filesystem::path image = fixtureRoot / "probe.exe";
    {
        std::ofstream output(image, std::ios::binary | std::ios::trunc);
        const std::string filler(65'580, 'x');
        output.write(filler.data(), static_cast<std::streamsize>(filler.size()));
        output.write(marker.data(), static_cast<std::streamsize>(marker.size()));
    }
    Require(
        renderer::private_freetype::ClassifyImage(image.c_str()) ==
            renderer::private_freetype::ImageClassification::detected,
        "the pathname classifier must detect an explicit UTF-16 Qt FreeType marker");
    const std::filesystem::path unityPlayer = fixtureRoot / "UnityPlayer.dll";
    {
        std::ofstream output(unityPlayer, std::ios::binary | std::ios::trunc);
        output << "fixture";
    }
    Require(
        renderer::private_freetype::HasUnityPlayerSibling(image.c_str()),
        "a regular UnityPlayer sibling must identify a Unity installation");
    std::error_code ignored;
    std::filesystem::remove_all(fixtureRoot, ignored);
    return 0;
}
