#pragma once

#include <cstddef>

namespace renderer {
namespace private_freetype {

enum class ImageClassification : unsigned char
{
    notDetected,
    detected,
    unavailable,
};

[[nodiscard]] bool ContainsExplicitQtFreeTypeMarker(
    const void* bytes,
    std::size_t size) noexcept;
[[nodiscard]] bool ShouldSkipHooks(
    bool optionEnabled,
    bool privateFreeTypeDetected,
    bool unityHookTarget) noexcept;
[[nodiscard]] ImageClassification ClassifyImage(
    const wchar_t* imagePath) noexcept;
[[nodiscard]] bool HasUnityPlayerSibling(const wchar_t* imagePath) noexcept;

} // namespace private_freetype
} // namespace renderer
