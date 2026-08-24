#include "unity_font_catalog.h"

#include "renderer_raii.h"

#include <dwrite.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <set>
#include <utility>

namespace renderer { namespace unity {
namespace {

template <typename T>
class ComOwner final
{
public:
	ComOwner() = default;
	~ComOwner() { reset(); }
	ComOwner(const ComOwner&) = delete;
	ComOwner& operator=(const ComOwner&) = delete;
	ComOwner(ComOwner&& other) noexcept : value_(other.release()) {}
	ComOwner& operator=(ComOwner&& other) noexcept
	{
		if (this != &other)
			reset(other.release());
		return *this;
	}

	T* get() const noexcept { return value_; }
	T* operator->() const noexcept { return value_; }
	explicit operator bool() const noexcept { return value_ != nullptr; }
	T** put() noexcept
	{
		reset();
		return &value_;
	}
	void reset(T* value = nullptr) noexcept
	{
		if (value_ != nullptr)
			value_->Release();
		value_ = value;
	}
	T* release() noexcept
	{
		T* const value = value_;
		value_ = nullptr;
		return value;
	}

private:
	T* value_ = nullptr;
};

using DWriteCreateFactoryFunction = HRESULT (WINAPI*)(
	DWRITE_FACTORY_TYPE, REFIID, IUnknown**);

bool EqualOrdinalIgnoreCase(
	const std::wstring& left,
	const std::wstring& right) noexcept
{
	if (left.size() != right.size() ||
		left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
		return false;
	return CompareStringOrdinal(
		left.data(), static_cast<int>(left.size()),
		right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

void Trim(std::wstring& value);

bool ReadBe16(
	const unsigned char* bytes,
	std::size_t size,
	std::size_t offset,
	std::uint16_t& value) noexcept
{
	if (bytes == nullptr || offset > size || 2 > size - offset)
		return false;
	value = static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(bytes[offset]) << 8) |
		bytes[offset + 1]);
	return true;
}

bool ReadBe32(
	const unsigned char* bytes,
	std::size_t size,
	std::size_t offset,
	std::uint32_t& value) noexcept
{
	if (bytes == nullptr || offset > size || 4 > size - offset)
		return false;
	value = (static_cast<std::uint32_t>(bytes[offset]) << 24) |
		(static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
		(static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
		bytes[offset + 3];
	return true;
}

bool AppendSfntFaceNames(
	const unsigned char* bytes,
	std::size_t size,
	std::size_t faceOffset,
	std::vector<std::wstring>& familyNames)
{
	std::uint32_t scaler = 0;
	std::uint16_t tableCount = 0;
	if (!ReadBe32(bytes, size, faceOffset, scaler) ||
		!ReadBe16(bytes, size, faceOffset + 4, tableCount) ||
		(scaler != 0x00010000 && scaler != 0x4f54544f &&
		 scaler != 0x74727565 && scaler != 0x74797031) ||
		tableCount == 0 || tableCount > 4096)
		return false;
	std::size_t const tableOffset = faceOffset + 12;
	std::size_t const tableBytes =
		static_cast<std::size_t>(tableCount) * 16;
	if (tableOffset > size || tableBytes > size - tableOffset)
		return false;
	std::uint32_t nameOffset = 0;
	std::uint32_t nameLength = 0;
	for (std::uint16_t index = 0; index < tableCount; ++index)
	{
		std::size_t const record = tableOffset +
			static_cast<std::size_t>(index) * 16;
		std::uint32_t tag = 0;
		if (!ReadBe32(bytes, size, record, tag))
			return false;
		if (tag == 0x6e616d65)
		{
			if (!ReadBe32(bytes, size, record + 8, nameOffset) ||
				!ReadBe32(bytes, size, record + 12, nameLength))
				return false;
			break;
		}
	}
	if (nameOffset == 0 || nameLength < 6 || nameOffset > size ||
		nameLength > size - nameOffset)
		return false;
	std::uint16_t format = 0;
	std::uint16_t count = 0;
	std::uint16_t stringsOffset = 0;
	if (!ReadBe16(bytes, size, nameOffset, format) || format > 1 ||
		!ReadBe16(bytes, size, nameOffset + 2, count) ||
		!ReadBe16(bytes, size, nameOffset + 4, stringsOffset) ||
		count == 0 || count > 4096)
		return false;
	std::size_t const recordsOffset = static_cast<std::size_t>(nameOffset) + 6;
	std::size_t const recordsBytes = static_cast<std::size_t>(count) * 12;
	std::size_t const tableEnd = static_cast<std::size_t>(nameOffset) + nameLength;
	if (recordsOffset > tableEnd || recordsBytes > tableEnd - recordsOffset ||
		stringsOffset > nameLength)
		return false;
	bool appended = false;
	for (std::uint16_t index = 0; index < count; ++index)
	{
		std::size_t const record = recordsOffset +
			static_cast<std::size_t>(index) * 12;
		std::uint16_t platform = 0;
		std::uint16_t nameId = 0;
		std::uint16_t length = 0;
		std::uint16_t offset = 0;
		if (!ReadBe16(bytes, size, record, platform) ||
			!ReadBe16(bytes, size, record + 6, nameId) ||
			!ReadBe16(bytes, size, record + 8, length) ||
			!ReadBe16(bytes, size, record + 10, offset))
			return false;
		if ((platform != 0 && platform != 3) ||
			(nameId != 1 && nameId != 16) || length == 0 ||
			(length & 1) != 0 || length > 4096)
			continue;
		std::size_t const stringStart = static_cast<std::size_t>(nameOffset) +
			stringsOffset + offset;
		if (stringStart > tableEnd || length > tableEnd - stringStart)
			continue;
		std::wstring family;
		family.reserve(length / 2);
		bool valid = true;
		for (std::size_t unit = 0; unit < length; unit += 2)
		{
			std::uint16_t character = 0;
			if (!ReadBe16(bytes, size, stringStart + unit, character) ||
				character == 0)
			{
				valid = false;
				break;
			}
			family.push_back(static_cast<wchar_t>(character));
		}
		Trim(family);
		if (!valid || family.empty())
			continue;
		if (std::none_of(
			familyNames.begin(), familyNames.end(),
			[&](const std::wstring& existing) {
				return EqualOrdinalIgnoreCase(existing, family);
			}))
		{
			familyNames.push_back(std::move(family));
			appended = true;
		}
	}
	return appended;
}

bool AppendUnique(
	std::vector<InstalledFontFace>& fonts,
	const std::wstring& family,
	const std::wstring& path)
{
	if (family.empty() || path.empty())
		return true;
	if (std::any_of(fonts.begin(), fonts.end(), [&](const InstalledFontFace& font) {
			return EqualOrdinalIgnoreCase(font.family, family) &&
				EqualOrdinalIgnoreCase(font.filePath, path);
		}))
		return true;
	fonts.push_back({family, path});
	return true;
}

bool AppendLocalizedStrings(
	IDWriteLocalizedStrings* localized,
	std::vector<std::wstring>& names)
{
	if (localized == nullptr)
		return false;
	UINT32 const count = localized->GetCount();
	if (count == 0 || count > 4096)
		return false;
	for (UINT32 index = 0; index < count; ++index)
	{
		UINT32 length = 0;
		if (FAILED(localized->GetStringLength(index, &length)) ||
			length == 0 || length > 4096)
			continue;
		std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 1);
		if (FAILED(localized->GetString(
			index, buffer.data(), static_cast<UINT32>(buffer.size()))))
			continue;
		std::wstring name(buffer.data(), length);
		if (std::none_of(names.begin(), names.end(), [&](const std::wstring& value) {
				return EqualOrdinalIgnoreCase(value, name);
			}))
			names.push_back(std::move(name));
	}
	return !names.empty();
}

bool ReadFamilyNames(
	IDWriteFontFamily* family,
	std::vector<std::wstring>& names)
{
	ComOwner<IDWriteLocalizedStrings> localized;
	return family != nullptr &&
		SUCCEEDED(family->GetFamilyNames(localized.put())) && localized &&
		AppendLocalizedStrings(localized.get(), names);
}

void AppendFontInformationalNames(
	IDWriteFont* font,
	std::vector<std::wstring>& names)
{
	constexpr DWRITE_INFORMATIONAL_STRING_ID nameKinds[] = {
		DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES,
		DWRITE_INFORMATIONAL_STRING_TYPOGRAPHIC_FAMILY_NAMES,
	};
	for (DWRITE_INFORMATIONAL_STRING_ID const kind : nameKinds)
	{
		ComOwner<IDWriteLocalizedStrings> localized;
		BOOL exists = FALSE;
		if (SUCCEEDED(font->GetInformationalStrings(
			kind, localized.put(), &exists)) && exists && localized)
			AppendLocalizedStrings(localized.get(), names);
	}
}

bool ReadLocalFilePath(
	IDWriteFontFile* file,
	std::wstring& path)
{
	path.clear();
	if (file == nullptr)
		return false;
	const void* referenceKey = nullptr;
	UINT32 referenceKeySize = 0;
	if (FAILED(file->GetReferenceKey(&referenceKey, &referenceKeySize)) ||
		referenceKey == nullptr || referenceKeySize == 0)
		return false;
	ComOwner<IDWriteFontFileLoader> loader;
	if (FAILED(file->GetLoader(loader.put())) || !loader)
		return false;
	ComOwner<IDWriteLocalFontFileLoader> localLoader;
	if (FAILED(loader->QueryInterface(
		__uuidof(IDWriteLocalFontFileLoader),
		reinterpret_cast<void**>(localLoader.put()))) || !localLoader)
		return false;
	UINT32 pathLength = 0;
	if (FAILED(localLoader->GetFilePathLengthFromKey(
		referenceKey, referenceKeySize, &pathLength)) ||
		pathLength == 0 || pathLength > 32767)
		return false;
	std::vector<wchar_t> buffer(static_cast<std::size_t>(pathLength) + 1);
	if (FAILED(localLoader->GetFilePathFromKey(
		referenceKey, referenceKeySize, buffer.data(),
		static_cast<UINT32>(buffer.size()))))
		return false;
	path.assign(buffer.data(), pathLength);
	return true;
}

bool AppendFamilyFiles(
	IDWriteFontFamily* family,
	const std::vector<std::wstring>& names,
	std::vector<InstalledFontFace>& fonts)
{
	UINT32 const fontCount = family->GetFontCount();
	if (fontCount == 0 || fontCount > 65536)
		return false;
	for (UINT32 fontIndex = 0; fontIndex < fontCount; ++fontIndex)
	{
		ComOwner<IDWriteFont> font;
		if (FAILED(family->GetFont(fontIndex, font.put())) || !font)
			continue;
		std::vector<std::wstring> fontNames = names;
		AppendFontInformationalNames(font.get(), fontNames);
		ComOwner<IDWriteFontFace> face;
		if (FAILED(font->CreateFontFace(face.put())) || !face)
			continue;
		UINT32 fileCount = 0;
		if (FAILED(face->GetFiles(&fileCount, nullptr)) ||
			fileCount == 0 || fileCount > 64)
			continue;
		std::vector<IDWriteFontFile*> rawFiles(fileCount, nullptr);
		if (FAILED(face->GetFiles(&fileCount, rawFiles.data())))
		{
			for (IDWriteFontFile* rawFile : rawFiles)
				if (rawFile != nullptr)
					rawFile->Release();
			continue;
		}
		for (IDWriteFontFile* rawFile : rawFiles)
		{
			ComOwner<IDWriteFontFile> file;
			file.reset(rawFile);
			std::wstring path;
			if (!ReadLocalFilePath(file.get(), path))
				continue;
			for (const std::wstring& name : fontNames)
				AppendUnique(fonts, name, path);
		}
	}
	return true;
}

void Trim(std::wstring& value)
{
	std::wstring::size_type const first = value.find_first_not_of(L" \t");
	if (first == std::wstring::npos)
	{
		value.clear();
		return;
	}
	std::wstring::size_type const last = value.find_last_not_of(L" \t");
	value = value.substr(first, last - first + 1);
}

void AppendRegisteredFamilyNames(
	const std::wstring& registeredName,
	const std::wstring& path,
	std::vector<InstalledFontFace>& fonts)
{
	std::wstring families = registeredName;
	std::wstring::size_type const suffix = families.rfind(L" (");
	if (suffix != std::wstring::npos && families.back() == L')')
		families.resize(suffix);
	std::wstring::size_type begin = 0;
	for (;;)
	{
		std::wstring::size_type const separator = families.find(L'&', begin);
		std::wstring family = families.substr(
			begin, separator == std::wstring::npos
				? std::wstring::npos
				: separator - begin);
		Trim(family);
		AppendUnique(fonts, family, path);
		if (separator == std::wstring::npos)
			break;
		begin = separator + 1;
	}
}

void AppendRegisteredFonts(
	HKEY root,
	std::vector<InstalledFontFace>& fonts)
{
	HKEY rawKey = nullptr;
	if (RegOpenKeyExW(
		root, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
		0, KEY_QUERY_VALUE, &rawKey) != ERROR_SUCCESS)
		return;
	renderer_raii::UniqueRegistryKey key(rawKey);
	std::vector<wchar_t> name(32768);
	std::vector<wchar_t> data(32768);
	for (DWORD index = 0; index < 65536; ++index)
	{
		DWORD nameLength = static_cast<DWORD>(name.size());
		DWORD dataBytes = static_cast<DWORD>(data.size() * sizeof(wchar_t));
		DWORD type = 0;
		LONG const status = RegEnumValueW(
			key.get(), index, name.data(), &nameLength, nullptr, &type,
			reinterpret_cast<BYTE*>(data.data()), &dataBytes);
		if (status == ERROR_NO_MORE_ITEMS)
			break;
		if (status != ERROR_SUCCESS ||
			(type != REG_SZ && type != REG_EXPAND_SZ) ||
			nameLength == 0 || dataBytes < sizeof(wchar_t) ||
			dataBytes > data.size() * sizeof(wchar_t) ||
			(dataBytes % sizeof(wchar_t)) != 0)
			continue;
		std::size_t const returnedCharacters =
			dataBytes / sizeof(wchar_t);
		data[(std::min)(returnedCharacters, data.size() - 1)] = L'\0';
		std::wstring path(data.data());
		if (type == REG_EXPAND_SZ)
		{
			DWORD const required = ExpandEnvironmentStringsW(
				path.c_str(), nullptr, 0);
			if (required == 0 || required > 32768)
				continue;
			std::vector<wchar_t> expanded(required);
			if (ExpandEnvironmentStringsW(
				path.c_str(), expanded.data(), required) != required)
				continue;
			path.assign(expanded.data());
		}
		bool const absolute = path.size() >= 3 && path[1] == L':' &&
			(path[2] == L'\\' || path[2] == L'/');
		if (!absolute)
		{
			std::wstring const relativePath = path;
			std::vector<wchar_t> windows(32768);
			UINT const length = GetWindowsDirectoryW(
				windows.data(), static_cast<UINT>(windows.size()));
			if (length == 0 || length >= windows.size())
				continue;
			path.assign(windows.data(), length);
			path.append(L"\\Fonts\\");
			path.append(relativePath);
		}
		if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
			continue;
		AppendRegisteredFamilyNames(
			std::wstring(name.data(), nameLength), path, fonts);
	}
}

bool AppendUniquePath(
	std::vector<std::wstring>& paths,
	const std::wstring& path)
{
	if (path.empty() || std::any_of(
		paths.begin(), paths.end(), [&](const std::wstring& existing) {
			return EqualOrdinalIgnoreCase(existing, path);
		}))
		return false;
	paths.push_back(path);
	return true;
}

void AppendSfntFile(
	const std::wstring& path,
	std::vector<InstalledFontFace>& fonts)
{
	DWORD const attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES ||
		(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
		return;
	renderer_raii::UniqueHandle file(CreateFileW(
		path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (!file)
		return;
	LARGE_INTEGER size{};
	constexpr LONGLONG kMaximumFontFileBytes = 256LL * 1024LL * 1024LL;
	if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 ||
		size.QuadPart > kMaximumFontFileBytes ||
		static_cast<ULONGLONG>(size.QuadPart) >
			(std::numeric_limits<std::size_t>::max)())
		return;
	renderer_raii::UniqueHandle mapping(CreateFileMappingW(
		file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
	if (!mapping)
		return;
	renderer_raii::UniqueMappedView view(MapViewOfFile(
		mapping.get(), FILE_MAP_READ, 0, 0, 0));
	if (!view)
		return;
	std::vector<std::wstring> names;
	if (!ParseSfntFamilyNames(
		static_cast<const unsigned char*>(view.get()),
		static_cast<std::size_t>(size.QuadPart), names))
		return;
	for (const std::wstring& name : names)
		AppendUnique(fonts, name, path);
}

void AppendSfntCatalog(std::vector<InstalledFontFace>& fonts)
{
	std::vector<std::wstring> paths;
	paths.reserve(fonts.size());
	for (const InstalledFontFace& font : fonts)
		AppendUniquePath(paths, font.filePath);

	std::vector<wchar_t> windows(32768);
	UINT const windowsLength = GetWindowsDirectoryW(
		windows.data(), static_cast<UINT>(windows.size()));
	if (windowsLength != 0 &&
		windowsLength < static_cast<UINT>(windows.size()))
	{
		std::wstring const fontDirectory =
			std::wstring(windows.data(), windowsLength) + L"\\Fonts\\";
		constexpr const wchar_t* patterns[] = {L"*.ttf", L"*.ttc", L"*.otf"};
		for (const wchar_t* const pattern : patterns)
		{
			WIN32_FIND_DATAW entry{};
			auto find = renderer_raii::AdoptFindHandle(
				FindFirstFileW((fontDirectory + pattern).c_str(), &entry));
			if (!find)
				continue;
			for (unsigned int count = 0; count < 4096; ++count)
			{
				if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
					AppendUniquePath(paths, fontDirectory + entry.cFileName);
				if (!FindNextFileW(find.get(), &entry))
					break;
			}
		}
	}
	if (paths.size() > 4096)
		paths.resize(4096);
	for (const std::wstring& path : paths)
		AppendSfntFile(path, fonts);
}

} // namespace

bool ParseSfntFamilyNames(
	const unsigned char* bytes,
	std::size_t size,
	std::vector<std::wstring>& familyNames) noexcept
{
	familyNames.clear();
	try
	{
		std::uint32_t signature = 0;
		if (!ReadBe32(bytes, size, 0, signature))
			return false;
		if (signature != 0x74746366)
			return AppendSfntFaceNames(bytes, size, 0, familyNames);
		std::uint32_t faceCount = 0;
		if (!ReadBe32(bytes, size, 8, faceCount) ||
			faceCount == 0 || faceCount > 256 ||
			12 > size || static_cast<std::size_t>(faceCount) * 4 > size - 12)
			return false;
		bool appended = false;
		for (std::uint32_t index = 0; index < faceCount; ++index)
		{
			std::uint32_t faceOffset = 0;
			if (!ReadBe32(
				bytes, size, 12 + static_cast<std::size_t>(index) * 4,
				faceOffset) || faceOffset >= size)
				return false;
			appended = AppendSfntFaceNames(
				bytes, size, faceOffset, familyNames) || appended;
		}
		return appended;
	}
	catch (...)
	{
		familyNames.clear();
		return false;
	}
}

bool EnumerateInstalledFontFaces(
	std::vector<InstalledFontFace>& fonts) noexcept
{
	fonts.clear();
	try
	{
		renderer_raii::UniqueModuleReference dwrite(LoadLibraryW(L"dwrite.dll"));
		if (!dwrite)
			return false;
		FARPROC const rawCreateFactory = GetProcAddress(
			dwrite.get(), "DWriteCreateFactory");
		if (rawCreateFactory == nullptr)
			return false;
		DWriteCreateFactoryFunction createFactory = nullptr;
		static_assert(sizeof(createFactory) == sizeof(rawCreateFactory));
		std::memcpy(&createFactory, &rawCreateFactory, sizeof(createFactory));

		ComOwner<IDWriteFactory> factory;
		if (FAILED(createFactory(
			DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
			reinterpret_cast<IUnknown**>(factory.put()))) || !factory)
			return false;
		ComOwner<IDWriteFontCollection> collection;
		if (FAILED(factory->GetSystemFontCollection(collection.put(), FALSE)) ||
			!collection)
			return false;
		UINT32 const familyCount = collection->GetFontFamilyCount();
		if (familyCount == 0 || familyCount > 65536)
			return false;
		for (UINT32 familyIndex = 0; familyIndex < familyCount; ++familyIndex)
		{
			ComOwner<IDWriteFontFamily> family;
			if (FAILED(collection->GetFontFamily(familyIndex, family.put())) ||
				!family)
				continue;
			std::vector<std::wstring> names;
			if (!ReadFamilyNames(family.get(), names))
				continue;
			AppendFamilyFiles(family.get(), names, fonts);
		}
		AppendRegisteredFonts(HKEY_LOCAL_MACHINE, fonts);
		AppendRegisteredFonts(HKEY_CURRENT_USER, fonts);
		AppendSfntCatalog(fonts);
		return !fonts.empty();
	}
	catch (...)
	{
		fonts.clear();
		return false;
	}
}

}} // namespace renderer::unity
