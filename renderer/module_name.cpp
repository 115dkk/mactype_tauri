#include "module_name.h"

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <vector>

namespace renderer {
namespace module_name {
namespace {

constexpr std::size_t kMaximumPathCharacters = 32'768;

const wchar_t* BaseName(const wchar_t* path) noexcept
{
    const wchar_t* name = path;
    for (const wchar_t* cursor = path; *cursor != L'\0'; ++cursor)
    {
        if (*cursor == L'\\' || *cursor == L'/')
            name = cursor + 1;
    }
    return name;
}

enum class PathQuery : unsigned char { complete, truncated, failed };

// GetModuleFileNameW reports truncation by returning the capacity, and an
// older loader may leave that truncated copy unterminated.
PathQuery QueryModulePath(
    HMODULE module,
    wchar_t* buffer,
    std::size_t capacity) noexcept
{
    const DWORD length =
        GetModuleFileNameW(module, buffer, static_cast<DWORD>(capacity));
    if (length == 0)
        return PathQuery::failed;
    if (static_cast<std::size_t>(length) >= capacity)
        return PathQuery::truncated;
    buffer[length] = L'\0';
    return PathQuery::complete;
}

} // namespace

bool BaseNameEquals(HMODULE module, const wchar_t* expectedBaseName) noexcept
{
    if (module == nullptr || expectedBaseName == nullptr ||
        *expectedBaseName == L'\0')
        return false;

    wchar_t shortPath[MAX_PATH] = {};
    switch (QueryModulePath(module, shortPath, MAX_PATH))
    {
    case PathQuery::complete:
        return _wcsicmp(BaseName(shortPath), expectedBaseName) == 0;
    case PathQuery::failed:
        return false;
    case PathQuery::truncated:
        break;
    }

    try
    {
        std::vector<wchar_t> longPath(static_cast<std::size_t>(MAX_PATH) * 2);
        for (;;)
        {
            switch (QueryModulePath(module, longPath.data(), longPath.size()))
            {
            case PathQuery::complete:
                return _wcsicmp(BaseName(longPath.data()), expectedBaseName) == 0;
            case PathQuery::failed:
                return false;
            case PathQuery::truncated:
                break;
            }
            if (longPath.size() >= kMaximumPathCharacters)
                return false;
            longPath.resize((std::min)(longPath.size() * 2, kMaximumPathCharacters));
        }
    }
    catch (...)
    {
        return false;
    }
}

} // namespace module_name
} // namespace renderer
