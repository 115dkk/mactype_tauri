#pragma once

namespace renderer {
namespace image_subsystem {

enum class ImageSubsystem : unsigned char { console, gui, other, unavailable };

[[nodiscard]] ImageSubsystem Classify(const wchar_t* imagePath) noexcept;

} // namespace image_subsystem
} // namespace renderer
