#include "image_subsystem.h"

#include "renderer_raii.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace renderer {
namespace image_subsystem {
namespace {

constexpr std::size_t kMaximumReadBytes = 64U * 1024U;
constexpr std::size_t kDosMagicOffset = 0;
constexpr std::size_t kPeOffsetOffset = 0x3c;
constexpr std::size_t kCoffHeaderSize = 20;
constexpr std::size_t kOptionalMagicOffset = 0;
constexpr std::size_t kSubsystemOffset = 68;

bool Contains(
    std::size_t total,
    std::size_t offset,
    std::size_t length) noexcept
{
    return offset <= total && length <= total - offset;
}

template <typename Value>
bool ReadValue(
    const unsigned char* bytes,
    std::size_t size,
    std::size_t offset,
    Value& value) noexcept
{
    if (!Contains(size, offset, sizeof(Value)))
        return false;
    std::memcpy(&value, bytes + offset, sizeof(Value));
    return true;
}

} // namespace

ImageSubsystem Classify(const wchar_t* imagePath) noexcept
{
    if (imagePath == nullptr || *imagePath == L'\0')
        return ImageSubsystem::unavailable;

    try
    {
        renderer_raii::UniqueHandle input = renderer_raii::AdoptHandle(
            CreateFileW(
                imagePath, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!input)
            return ImageSubsystem::unavailable;

        std::vector<unsigned char> bytes(kMaximumReadBytes);
        DWORD bytesRead = 0;
        if (!ReadFile(
                input.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                &bytesRead, nullptr))
            return ImageSubsystem::unavailable;
        const std::size_t size = bytesRead;

        std::uint16_t dosMagic = 0;
        std::int32_t signedPeOffset = 0;
        if (!ReadValue(bytes.data(), size, kDosMagicOffset, dosMagic) ||
            dosMagic != 0x5a4d ||
            !ReadValue(bytes.data(), size, kPeOffsetOffset, signedPeOffset) ||
            signedPeOffset < 0x40)
            return ImageSubsystem::unavailable;
        const std::size_t peOffset = static_cast<std::size_t>(signedPeOffset);

        std::uint32_t peSignature = 0;
        if (!ReadValue(bytes.data(), size, peOffset, peSignature) ||
            peSignature != 0x00004550)
            return ImageSubsystem::unavailable;
        constexpr std::size_t peSignatureSize = sizeof(peSignature);
        if (!Contains(size, peOffset, peSignatureSize + kCoffHeaderSize))
            return ImageSubsystem::unavailable;
        const std::size_t coffOffset = peOffset + peSignatureSize;

        std::uint16_t optionalHeaderSize = 0;
        constexpr std::size_t sizeOfOptionalHeaderOffset = 16;
        if (!ReadValue(
                bytes.data(), size,
                coffOffset + sizeOfOptionalHeaderOffset,
                optionalHeaderSize))
            return ImageSubsystem::unavailable;
        constexpr std::size_t requiredOptionalBytes =
            kSubsystemOffset + sizeof(std::uint16_t);
        if (optionalHeaderSize < requiredOptionalBytes)
            return ImageSubsystem::unavailable;
        const std::size_t optionalOffset = coffOffset + kCoffHeaderSize;
        if (!Contains(size, optionalOffset, optionalHeaderSize))
            return ImageSubsystem::unavailable;

        std::uint16_t optionalMagic = 0;
        std::uint16_t subsystem = 0;
        if (!ReadValue(
                bytes.data(), size,
                optionalOffset + kOptionalMagicOffset, optionalMagic) ||
            (optionalMagic != 0x10b && optionalMagic != 0x20b) ||
            !ReadValue(
                bytes.data(), size,
                optionalOffset + kSubsystemOffset, subsystem))
            return ImageSubsystem::unavailable;

        if (subsystem == 3)
            return ImageSubsystem::console;
        if (subsystem == 2)
            return ImageSubsystem::gui;
        return ImageSubsystem::other;
    }
    catch (...)
    {
        return ImageSubsystem::unavailable;
    }
}

} // namespace image_subsystem
} // namespace renderer
