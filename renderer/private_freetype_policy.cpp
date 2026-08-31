#include "private_freetype_policy.h"
#include "renderer_raii.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace renderer {
namespace private_freetype {
namespace {

constexpr std::array<unsigned char, 27> kQtFreeTypeMarker = {
    'w', 'i', 'n', 'd', 'o', 'w', 's', ':', 'f', 'o', 'n', 't', 'e', 'n', 'g',
    'i', 'n', 'e', '=', 'f', 'r', 'e', 'e', 't', 'y', 'p', 'e'};
constexpr DWORD kMaximumImageBytes = 64U * 1024U * 1024U;
constexpr DWORD kScanBytes = 64U * 1024U;

unsigned char AsciiLower(unsigned char value) noexcept
{
    return value >= 'A' && value <= 'Z'
        ? static_cast<unsigned char>(value + ('a' - 'A'))
        : value;
}

bool MatchesAt(
    const unsigned char* bytes,
    std::size_t size,
    std::size_t offset,
    bool utf16) noexcept
{
    const std::size_t stride = utf16 ? 2 : 1;
    if (offset > size || kQtFreeTypeMarker.size() > (size - offset) / stride)
        return false;
    for (std::size_t index = 0; index < kQtFreeTypeMarker.size(); ++index)
    {
        if (AsciiLower(bytes[offset + index * stride]) != kQtFreeTypeMarker[index] ||
            (utf16 && bytes[offset + index * stride + 1] != 0))
            return false;
    }
    return true;
}

} // namespace

bool ContainsExplicitQtFreeTypeMarker(
    const void* bytes,
    std::size_t size) noexcept
{
    if (bytes == nullptr || size < kQtFreeTypeMarker.size())
        return false;
    const auto* data = static_cast<const unsigned char*>(bytes);
    for (std::size_t offset = 0; offset < size; ++offset)
    {
        if (MatchesAt(data, size, offset, false) ||
            MatchesAt(data, size, offset, true))
            return true;
    }
    return false;
}

bool ShouldSkipHooks(
    bool optionEnabled,
    bool privateFreeTypeDetected,
    bool unityHookTarget) noexcept
{
    return optionEnabled && privateFreeTypeDetected && !unityHookTarget;
}

ImageClassification ClassifyImage(const wchar_t* imagePath) noexcept
{
    if (imagePath == nullptr || *imagePath == L'\0')
        return ImageClassification::unavailable;
    try
    {
        renderer_raii::UniqueHandle input = renderer_raii::AdoptHandle(
            CreateFileW(
                imagePath, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!input)
            return ImageClassification::unavailable;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(input.get(), &size) || size.QuadPart <= 0 ||
            size.QuadPart > kMaximumImageBytes)
            return ImageClassification::unavailable;
        constexpr std::size_t overlap = kQtFreeTypeMarker.size() * 2 - 1;
        std::vector<unsigned char> bytes(kScanBytes + overlap);
        std::size_t retained = 0;
        unsigned long long totalRead = 0;
        for (;;)
        {
            DWORD bytesRead = 0;
            if (!ReadFile(
                    input.get(), bytes.data() + retained,
                    static_cast<DWORD>(bytes.size() - retained),
                    &bytesRead, nullptr))
                return ImageClassification::unavailable;
            if (bytesRead == 0)
                return totalRead == static_cast<unsigned long long>(size.QuadPart)
                    ? ImageClassification::notDetected
                    : ImageClassification::unavailable;
            totalRead += bytesRead;
            const std::size_t used = retained + bytesRead;
            if (ContainsExplicitQtFreeTypeMarker(bytes.data(), used))
                return ImageClassification::detected;
            retained = (std::min)(overlap, used);
            std::memmove(
                bytes.data(), bytes.data() + used - retained, retained);
        }
    }
    catch (...)
    {
        return ImageClassification::unavailable;
    }
}

bool HasUnityPlayerSibling(const wchar_t* imagePath) noexcept
{
    if (imagePath == nullptr || *imagePath == L'\0')
        return false;
    try
    {
        std::wstring unityPlayer(imagePath);
        const std::wstring::size_type slash = unityPlayer.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return false;
        unityPlayer.resize(slash + 1);
        unityPlayer.append(L"UnityPlayer.dll");
        const DWORD attributes = GetFileAttributesW(unityPlayer.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace private_freetype
} // namespace renderer
