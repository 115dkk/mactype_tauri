#include "runtime_pointer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string_view>
#include <utility>
#include <vector>

namespace renderer::runtime_pointer {
namespace {

constexpr LONGLONG kMaximumPointerBytes = 4096;
constexpr size_t kMaximumVersionCharacters = 128;

class FileHandle
{
public:
    explicit FileHandle(HANDLE value) noexcept : value_(value) {}
    ~FileHandle()
    {
        if (value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }

    FileHandle(FileHandle const&) = delete;
    FileHandle& operator=(FileHandle const&) = delete;

    HANDLE get() const noexcept { return value_; }

private:
    HANDLE value_;
};

class PointerParser
{
public:
    explicit PointerParser(std::string_view text) noexcept : text_(text) {}

    bool Parse(std::string& version)
    {
        SkipWhitespace();
        if (!Consume('{'))
            return false;

        bool schemaSeen = false;
        bool versionSeen = false;
        for (int fieldIndex = 0; fieldIndex != 2; ++fieldIndex)
        {
            SkipWhitespace();
            Field field{};
            if (!ParseField(field) || !ConsumeAfterWhitespace(':'))
                return false;
            SkipWhitespace();
            if (field == Field::schema)
            {
                if (schemaSeen || !Consume('1'))
                    return false;
                schemaSeen = true;
            }
            else
            {
                if (versionSeen || !ParseVersion(version))
                    return false;
                versionSeen = true;
            }
            SkipWhitespace();
            if (fieldIndex == 0)
            {
                if (!Consume(','))
                    return false;
            }
            else if (!Consume('}'))
            {
                return false;
            }
        }
        SkipWhitespace();
        return schemaSeen && versionSeen && position_ == text_.size();
    }

private:
    enum class Field
    {
        schema,
        version,
    };

    bool ParseField(Field& field) noexcept
    {
        if (ConsumeLiteral("\"schema\""))
        {
            field = Field::schema;
            return true;
        }
        if (ConsumeLiteral("\"version\""))
        {
            field = Field::version;
            return true;
        }
        return false;
    }

    bool ParseVersion(std::string& version)
    {
        if (!Consume('"'))
            return false;
        size_t const begin = position_;
        while (position_ < text_.size() && text_[position_] != '"')
        {
            unsigned char const character =
                static_cast<unsigned char>(text_[position_]);
            if (!IsVersionCharacter(character) ||
                position_ - begin == kMaximumVersionCharacters)
                return false;
            ++position_;
        }
        size_t const length = position_ - begin;
        if (length == 0 || !Consume('"'))
            return false;
        version.assign(text_.data() + begin, length);
        return version != "." && version != "..";
    }

    static bool IsVersionCharacter(unsigned char character) noexcept
    {
        return (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') ||
            character == '.' || character == '+' || character == '_' ||
            character == '-';
    }

    bool Consume(char expected) noexcept
    {
        if (position_ == text_.size() || text_[position_] != expected)
            return false;
        ++position_;
        return true;
    }

    bool ConsumeAfterWhitespace(char expected) noexcept
    {
        SkipWhitespace();
        return Consume(expected);
    }

    bool ConsumeLiteral(std::string_view expected) noexcept
    {
        if (text_.substr(position_, expected.size()) != expected)
            return false;
        position_ += expected.size();
        return true;
    }

    void SkipWhitespace() noexcept
    {
        while (position_ < text_.size())
        {
            char const character = text_[position_];
            if (character != ' ' && character != '\t' &&
                character != '\r' && character != '\n')
                break;
            ++position_;
        }
    }

    std::string_view text_;
    size_t position_ = 0;
};

bool NormalizeDirectory(
    std::wstring const& directory, std::wstring& normalized) noexcept
{
    try
    {
        if (directory.empty())
            return false;
        DWORD const required = GetFullPathNameW(
            directory.c_str(), 0, nullptr, nullptr);
        if (required == 0)
            return false;
        std::vector<WCHAR> buffer(static_cast<size_t>(required) + 1, L'\0');
        DWORD const length = GetFullPathNameW(
            directory.c_str(), static_cast<DWORD>(buffer.size()),
            buffer.data(), nullptr);
        if (length == 0 || length >= static_cast<DWORD>(buffer.size()))
            return false;
        normalized.assign(buffer.data(), length);
        while (normalized.size() > 3 &&
               (normalized.back() == L'\\' || normalized.back() == L'/'))
            normalized.pop_back();
        if (normalized.back() == L'/')
            normalized.back() = L'\\';
        else if (normalized.back() != L'\\')
            normalized.push_back(L'\\');
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool ReadPointer(std::wstring const& path, std::string& contents) noexcept
{
    try
    {
        FileHandle file(CreateFileW(
            path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE)
            return false;

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 ||
            size.QuadPart > kMaximumPointerBytes)
            return false;

        std::vector<char> bytes(static_cast<size_t>(size.QuadPart) + 1);
        DWORD bytesRead = 0;
        if (!ReadFile(
                file.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                &bytesRead, nullptr) ||
            bytesRead != static_cast<DWORD>(size.QuadPart))
            return false;
        LARGE_INTEGER finalSize{};
        if (!GetFileSizeEx(file.get(), &finalSize) ||
            finalSize.QuadPart != size.QuadPart)
            return false;
        contents.assign(bytes.data(), bytesRead);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool HasProfile(std::wstring const& directory) noexcept
{
    try
    {
        std::wstring const profile = directory + L"MacType.ini";
        DWORD const attributes = GetFileAttributesW(profile.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace

bool CurrentRuntimeDirectory(
    std::wstring const& moduleDirectory,
    std::wstring& runtimeDirectory) noexcept
{
    try
    {
        std::wstring normalizedModule;
        if (!NormalizeDirectory(moduleDirectory, normalizedModule))
            return false;

        std::string pointerContents;
        if (!ReadPointer(
                normalizedModule + L"..\\..\\current.json",
                pointerContents))
            return false;
        std::string version;
        PointerParser parser(pointerContents);
        if (!parser.Parse(version))
            return false;

        std::wstring wideVersion(version.begin(), version.end());
        std::wstring candidate;
        if (!NormalizeDirectory(
                normalizedModule + L"..\\" + wideVersion,
                candidate) ||
            _wcsicmp(candidate.c_str(), normalizedModule.c_str()) == 0 ||
            !HasProfile(candidate))
            return false;
        runtimeDirectory = std::move(candidate);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace renderer::runtime_pointer
