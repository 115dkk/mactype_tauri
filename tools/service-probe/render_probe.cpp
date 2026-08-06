#include "render_probe.h"

#include <sdkddkver.h>
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10_NI

#include "win32_support.h"

#include <bcrypt.h>
#include <dwrite_3.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace mactype::service_probe::internal {
namespace {

struct GdiObjectDeleter {
  void operator()(HGDIOBJ object) const noexcept {
    if (object != nullptr) {
      DeleteObject(object);
    }
  }
};

struct DcDeleter {
  void operator()(HDC dc) const noexcept {
    if (dc != nullptr) {
      DeleteDC(dc);
    }
  }
};

using UniqueGdiObject =
    std::unique_ptr<std::remove_pointer_t<HGDIOBJ>, GdiObjectDeleter>;
using UniqueDc = std::unique_ptr<std::remove_pointer_t<HDC>, DcDeleter>;

template <typename Interface>
struct ComReleaser {
  void operator()(Interface* value) const noexcept {
    if (value != nullptr) {
      value->Release();
    }
  }
};

template <typename Interface>
using UniqueCom = std::unique_ptr<Interface, ComReleaser<Interface>>;

class ScopedFontTable final {
 public:
  ScopedFontTable(IDWriteFontFace* face, void* context) noexcept
      : face_(face), context_(context) {}

  ~ScopedFontTable() noexcept {
    if (face_ != nullptr) {
      face_->ReleaseFontTable(context_);
    }
  }

  ScopedFontTable(const ScopedFontTable&) = delete;
  ScopedFontTable& operator=(const ScopedFontTable&) = delete;

 private:
  IDWriteFontFace* face_;
  void* context_;
};

bool FontNameTableContainsFamily(IDWriteFontFace* face,
                                 const std::wstring_view family) {
  if (face == nullptr || family.empty()) {
    return false;
  }
  const void* table_data = nullptr;
  UINT32 table_size = 0;
  void* table_context = nullptr;
  BOOL exists = FALSE;
  const HRESULT result = face->TryGetFontTable(
      DWRITE_MAKE_OPENTYPE_TAG('n', 'a', 'm', 'e'), &table_data, &table_size,
      &table_context, &exists);
  if (FAILED(result) || exists == FALSE || table_data == nullptr) {
    return false;
  }
  ScopedFontTable table(face, table_context);
  std::vector<std::byte> encoded_family;
  encoded_family.reserve(family.size() * 2U);
  for (const wchar_t character : family) {
    encoded_family.push_back(
        static_cast<std::byte>((static_cast<unsigned int>(character) >> 8U) &
                               0xffU));
    encoded_family.push_back(
        static_cast<std::byte>(static_cast<unsigned int>(character) & 0xffU));
  }
  const auto* first = static_cast<const std::byte*>(table_data);
  return std::search(first, first + table_size, encoded_family.begin(),
                     encoded_family.end()) != first + table_size;
}

class FontSubstitutionSwitch final {
 public:
  FontSubstitutionSwitch() {
    SetLastError(ERROR_SUCCESS);
    const DWORD required =
        GetEnvironmentVariableW(name_, nullptr, 0);
    existed_ = required != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    if (required != 0) {
      previous_.resize(required);
      GetEnvironmentVariableW(name_, previous_.data(), required);
    }
    SetEnvironmentVariableW(name_, L"1");
  }

  ~FontSubstitutionSwitch() {
    SetEnvironmentVariableW(name_,
                            existed_ ? previous_.c_str() : nullptr);
  }

  FontSubstitutionSwitch(const FontSubstitutionSwitch&) = delete;
  FontSubstitutionSwitch& operator=(const FontSubstitutionSwitch&) = delete;

 private:
  static constexpr wchar_t name_[] = L"MACTYPE_FONTSUBSTITUTES_ENV";
  bool existed_ = false;
  std::wstring previous_;
};

bool HashSha256(const std::byte* bytes, const std::size_t size,
                std::string& result) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                  0) < 0) {
    return false;
  }
  DWORD object_size = 0;
  DWORD returned = 0;
  if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                        reinterpret_cast<PUCHAR>(&object_size),
                        sizeof(object_size), &returned, 0) < 0) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return false;
  }
  std::vector<UCHAR> object(object_size);
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0,
                       0) < 0) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return false;
  }
  const NTSTATUS update = BCryptHashData(
      hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes)),
      static_cast<ULONG>(size), 0);
  std::array<UCHAR, 32> digest{};
  const NTSTATUS finish = update < 0
                              ? update
                              : BCryptFinishHash(
                                    hash, digest.data(),
                                    static_cast<ULONG>(digest.size()), 0);
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (finish < 0) {
    return false;
  }
  std::ostringstream output;
  output << "sha256:" << std::hex << std::setfill('0');
  for (const UCHAR value : digest) {
    output << std::setw(2) << static_cast<unsigned int>(value);
  }
  result = output.str();
  return true;
}

std::wstring ResolveDirectWriteFamily(const std::wstring_view family,
                                      void* directwrite_factory,
                                      const bool use_custom_collection,
                                      std::wstring& error) {
  IDWriteFactory* factory = static_cast<IDWriteFactory*>(directwrite_factory);
  IDWriteFactory* raw_factory = nullptr;
  UniqueCom<IDWriteFactory> owned_factory;
  if (factory == nullptr) {
    const HRESULT factory_result = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&raw_factory));
    owned_factory.reset(raw_factory);
    if (FAILED(factory_result) || owned_factory == nullptr) {
      error = L"DWriteCreateFactory failed";
      return {};
    }
    factory = owned_factory.get();
  }
  const std::wstring family_name(family);
  IDWriteFontCollection* raw_collection = nullptr;
  const HRESULT collection_result =
      factory->GetSystemFontCollection(&raw_collection);
  UniqueCom<IDWriteFontCollection> collection(raw_collection);
  if (FAILED(collection_result) || collection == nullptr) {
    error = L"IDWriteFactory::GetSystemFontCollection failed";
    return {};
  }
  if (use_custom_collection) {
    IDWriteFactory3* raw_factory3 = nullptr;
    factory->QueryInterface(&raw_factory3);
    UniqueCom<IDWriteFactory3> factory3(raw_factory3);
    IDWriteFontCollection1* raw_system_collection1 = nullptr;
    collection->QueryInterface(&raw_system_collection1);
    UniqueCom<IDWriteFontCollection1> system_collection1(
        raw_system_collection1);
    IDWriteFontSet* raw_font_set = nullptr;
    if (system_collection1 != nullptr) {
      system_collection1->GetFontSet(&raw_font_set);
    }
    UniqueCom<IDWriteFontSet> font_set(raw_font_set);
    IDWriteFontCollection1* raw_custom_collection = nullptr;
    const HRESULT custom_result =
        factory3 == nullptr || font_set == nullptr
            ? E_NOINTERFACE
            : factory3->CreateFontCollectionFromFontSet(
                  font_set.get(), &raw_custom_collection);
    if (FAILED(custom_result) || raw_custom_collection == nullptr) {
      if (raw_custom_collection != nullptr) {
        raw_custom_collection->Release();
      }
      error = L"IDWriteFactory3::CreateFontCollectionFromFontSet failed";
      return {};
    }
    collection.reset(raw_custom_collection);
  }
  IDWriteFontCollection1* raw_collection1 = nullptr;
  collection->QueryInterface(&raw_collection1);
  UniqueCom<IDWriteFontCollection1> collection1(raw_collection1);
  if (!use_custom_collection && collection1 != nullptr) {
    IDWriteFontSet* raw_font_set = nullptr;
    collection1->GetFontSet(&raw_font_set);
    UniqueCom<IDWriteFontSet> font_set(raw_font_set);
    IDWriteFontSet4* raw_font_set4 = nullptr;
    if (font_set != nullptr) {
      font_set->QueryInterface(&raw_font_set4);
    }
    UniqueCom<IDWriteFontSet4> font_set4(raw_font_set4);
    if (font_set4 != nullptr) {
      std::array<DWRITE_FONT_AXIS_VALUE, DWRITE_STANDARD_FONT_AXIS_COUNT> axes{};
      const UINT32 axis_count =
          font_set4->ConvertWeightStretchStyleToFontAxisValues(
              nullptr, 0, DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, 24.0F,
              axes.data());
      IDWriteFontSet4* raw_matches = nullptr;
      const HRESULT matches_result = font_set4->GetMatchingFonts(
          family_name.c_str(), axes.data(), axis_count,
          static_cast<DWRITE_FONT_SIMULATIONS>(DWRITE_FONT_SIMULATIONS_BOLD |
                                               DWRITE_FONT_SIMULATIONS_OBLIQUE),
          &raw_matches);
      UniqueCom<IDWriteFontSet4> matches(raw_matches);
      if (SUCCEEDED(matches_result) && matches != nullptr &&
          matches->GetFontCount() != 0) {
        BOOL family_exists = FALSE;
        IDWriteLocalizedStrings* raw_family_names = nullptr;
        const HRESULT names_result = matches->GetPropertyValues(
            0, DWRITE_FONT_PROPERTY_ID_FAMILY_NAME, &family_exists,
            &raw_family_names);
        UniqueCom<IDWriteLocalizedStrings> family_names(raw_family_names);
        if (SUCCEEDED(names_result) && family_exists != FALSE &&
            family_names != nullptr && family_names->GetCount() != 0) {
          UINT32 length = 0;
          if (SUCCEEDED(family_names->GetStringLength(0, &length))) {
            std::wstring resolved(static_cast<std::size_t>(length) + 1U,
                                  L'\0');
            if (SUCCEEDED(
                    family_names->GetString(0, resolved.data(), length + 1U))) {
              resolved.resize(length);
              return resolved;
            }
          }
        }
      }
    }
  }
  UINT32 family_index = 0;
  BOOL family_exists = FALSE;
  if (FAILED(collection->FindFamilyName(family_name.c_str(), &family_index,
                                        &family_exists)) ||
      family_exists == FALSE) {
    error = L"IDWriteFontCollection::FindFamilyName failed";
    return {};
  }
  IDWriteFontFamily* raw_family = nullptr;
  const HRESULT family_result =
      collection->GetFontFamily(family_index, &raw_family);
  UniqueCom<IDWriteFontFamily> font_family(raw_family);
  if (FAILED(family_result) || font_family == nullptr) {
    error = L"IDWriteFontCollection::GetFontFamily failed";
    return {};
  }
  IDWriteFont* raw_font = nullptr;
  const HRESULT font_result = font_family->GetFirstMatchingFont(
          DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
          DWRITE_FONT_STYLE_NORMAL, &raw_font);
  UniqueCom<IDWriteFont> font(raw_font);
  if (FAILED(font_result) || font == nullptr) {
    error = L"IDWriteFontFamily::GetFirstMatchingFont failed";
    return {};
  }
  IDWriteFontFace* raw_face = nullptr;
  const HRESULT face_result = font->CreateFontFace(&raw_face);
  UniqueCom<IDWriteFontFace> face(raw_face);
  if (FAILED(face_result) || face == nullptr) {
    error = L"IDWriteFont::CreateFontFace failed";
    return {};
  }
  IDWriteGdiInterop* raw_interop = nullptr;
  const HRESULT interop_result = factory->GetGdiInterop(&raw_interop);
  UniqueCom<IDWriteGdiInterop> interop(raw_interop);
  if (FAILED(interop_result) || interop == nullptr) {
    error = L"IDWriteFactory::GetGdiInterop failed";
    return {};
  }
  LOGFONTW resolved{};
  if (FAILED(interop->ConvertFontFaceToLOGFONT(face.get(), &resolved))) {
    error = L"IDWriteGdiInterop::ConvertFontFaceToLOGFONT failed";
    return {};
  }
  return resolved.lfFaceName;
}

std::wstring ResolveDirectWriteFontObject(
    IDWriteFont* font, void* directwrite_factory, std::wstring& error) {
  if (font == nullptr) {
    error = L"DirectWrite font object is unavailable";
    return {};
  }
  IDWriteFactory* factory = static_cast<IDWriteFactory*>(directwrite_factory);
  IDWriteFactory* raw_factory = nullptr;
  UniqueCom<IDWriteFactory> owned_factory;
  if (factory == nullptr) {
    const HRESULT factory_result = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&raw_factory));
    owned_factory.reset(raw_factory);
    if (FAILED(factory_result) || owned_factory == nullptr) {
      error = L"DWriteCreateFactory failed for indexed collection";
      return {};
    }
    factory = owned_factory.get();
  }

  IDWriteGdiInterop* raw_interop = nullptr;
  const HRESULT interop_result = factory->GetGdiInterop(&raw_interop);
  UniqueCom<IDWriteGdiInterop> interop(raw_interop);
  LOGFONTW resolved{};
  BOOL is_system_font = FALSE;
  if (FAILED(interop_result) || interop == nullptr ||
      FAILED(interop->ConvertFontToLOGFONT(font, &resolved,
                                          &is_system_font)) ||
      is_system_font == FALSE) {
    error = L"ConvertFontToLOGFONT failed for indexed collection";
    return {};
  }
  return resolved.lfFaceName;
}

std::wstring ResolveDirectWriteFontDescriptor(
    IDWriteFontFace* face, void* directwrite_factory, std::wstring& error) {
  if (face == nullptr) {
    error = L"DirectWrite font face is unavailable";
    return {};
  }
  IDWriteFactory* factory = static_cast<IDWriteFactory*>(directwrite_factory);
  IDWriteFactory* raw_factory = nullptr;
  UniqueCom<IDWriteFactory> owned_factory;
  if (factory == nullptr) {
    const HRESULT factory_result = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&raw_factory));
    owned_factory.reset(raw_factory);
    if (FAILED(factory_result) || owned_factory == nullptr) {
      error = L"DWriteCreateFactory failed for retained face descriptor";
      return {};
    }
    factory = owned_factory.get();
  }

  UINT32 file_count = 0;
  if (FAILED(face->GetFiles(&file_count, nullptr)) || file_count == 0) {
    error = L"GetFiles count failed for retained face descriptor";
    return {};
  }
  std::vector<IDWriteFontFile*> files(file_count, nullptr);
  if (FAILED(face->GetFiles(&file_count, files.data()))) {
    for (IDWriteFontFile* file : files) {
      if (file != nullptr) {
        file->Release();
      }
    }
    error = L"GetFiles failed for retained face descriptor";
    return {};
  }
  std::vector<UniqueCom<IDWriteFontFile>> owned_files;
  owned_files.reserve(files.size());
  for (IDWriteFontFile* file : files) {
    owned_files.emplace_back(file);
  }

  if (file_count != 1) {
    error = L"Retained face descriptor must contain exactly one font file";
    return {};
  }
  BOOL is_supported = FALSE;
  DWRITE_FONT_FILE_TYPE file_type = DWRITE_FONT_FILE_TYPE_UNKNOWN;
  DWRITE_FONT_FACE_TYPE face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
  UINT32 face_count = 0;
  if (FAILED(files.front()->Analyze(&is_supported, &file_type, &face_type,
                                    &face_count)) ||
      is_supported == FALSE || file_type == DWRITE_FONT_FILE_TYPE_UNKNOWN ||
      face_count == 0) {
    error = L"Analyze failed for retained face descriptor";
    return {};
  }
  const UINT32 face_index = face->GetIndex();
  const DWRITE_FONT_SIMULATIONS simulations = face->GetSimulations();
  IDWriteFontFace* raw_descriptor_face = nullptr;
  const HRESULT face_result = factory->CreateFontFace(
      face_type, file_count, files.data(), face_index, simulations,
      &raw_descriptor_face);
  UniqueCom<IDWriteFontFace> descriptor_face(raw_descriptor_face);
  if (FAILED(face_result) || descriptor_face == nullptr) {
    std::wostringstream message;
    message << L"CreateFontFace failed for retained face descriptor: HRESULT 0x"
            << std::hex << static_cast<unsigned long>(face_result)
            << L", type " << static_cast<unsigned int>(face_type)
            << L", files " << std::dec << file_count << L", index "
            << face_index << L", simulations "
            << static_cast<unsigned int>(simulations);
    error = message.str();
    return {};
  }

  IDWriteGdiInterop* raw_interop = nullptr;
  const HRESULT interop_result = factory->GetGdiInterop(&raw_interop);
  UniqueCom<IDWriteGdiInterop> interop(raw_interop);
  LOGFONTW resolved{};
  if (FAILED(interop_result) || interop == nullptr ||
      FAILED(interop->ConvertFontFaceToLOGFONT(descriptor_face.get(),
                                              &resolved))) {
    error = L"ConvertFontFaceToLOGFONT failed for retained face descriptor";
    return {};
  }
  return resolved.lfFaceName;
}

std::wstring ResolveIndexedDirectWriteFamily(
    const std::wstring_view family, void* directwrite_factory,
    UINT32& pinned_index, const bool discover_index,
    std::wstring& postscript_name, UniqueCom<IDWriteFont>* retained_font,
    UniqueCom<IDWriteFontFace>* retained_face, std::wstring& error) {
  IDWriteFactory* factory = static_cast<IDWriteFactory*>(directwrite_factory);
  IDWriteFactory* raw_factory = nullptr;
  UniqueCom<IDWriteFactory> owned_factory;
  if (factory == nullptr) {
    const HRESULT factory_result = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&raw_factory));
    owned_factory.reset(raw_factory);
    if (FAILED(factory_result) || owned_factory == nullptr) {
      error = L"DWriteCreateFactory failed for indexed collection";
      return {};
    }
    factory = owned_factory.get();
  }

  IDWriteFontCollection* raw_collection = nullptr;
  const HRESULT collection_result =
      factory->GetSystemFontCollection(&raw_collection);
  UniqueCom<IDWriteFontCollection> collection(raw_collection);
  if (FAILED(collection_result) || collection == nullptr) {
    error = L"GetSystemFontCollection failed for indexed collection";
    return {};
  }
  if (discover_index) {
    BOOL exists = FALSE;
    const std::wstring family_name(family);
    if (FAILED(collection->FindFamilyName(family_name.c_str(), &pinned_index,
                                          &exists)) ||
        exists == FALSE) {
      error = L"FindFamilyName failed for indexed collection";
      return {};
    }
  }

  IDWriteFontFamily* raw_family = nullptr;
  const HRESULT family_result =
      collection->GetFontFamily(pinned_index, &raw_family);
  UniqueCom<IDWriteFontFamily> font_family(raw_family);
  if (FAILED(family_result) || font_family == nullptr) {
    error = L"GetFontFamily failed for indexed collection";
    return {};
  }
  IDWriteFont* raw_font = nullptr;
  const HRESULT font_result = font_family->GetFont(0, &raw_font);
  UniqueCom<IDWriteFont> font(raw_font);
  if (FAILED(font_result) || font == nullptr) {
    error = L"GetFont failed for indexed collection";
    return {};
  }

  auto read_postscript_name = [&](IDWriteFont* target,
                                  std::wstring& value) -> bool {
    BOOL exists = FALSE;
    IDWriteLocalizedStrings* raw_names = nullptr;
    const HRESULT names_result = target->GetInformationalStrings(
        DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME, &raw_names, &exists);
    UniqueCom<IDWriteLocalizedStrings> names(raw_names);
    if (FAILED(names_result) || exists == FALSE || names == nullptr ||
        names->GetCount() == 0) {
      error = L"GetInformationalStrings failed for indexed collection";
      return false;
    }
    UINT32 length = 0;
    if (FAILED(names->GetStringLength(0, &length))) {
      error = L"GetStringLength failed for indexed collection";
      return false;
    }
    value.assign(static_cast<std::size_t>(length) + 1U, L'\0');
    if (FAILED(names->GetString(0, value.data(), length + 1U))) {
      error = L"GetString failed for indexed collection";
      return false;
    }
    value.resize(length);
    return true;
  };
  if (!read_postscript_name(font.get(), postscript_name)) {
    return {};
  }

  if (retained_face != nullptr) {
    IDWriteFontFace* raw_face = nullptr;
    const HRESULT face_result = font->CreateFontFace(&raw_face);
    retained_face->reset(raw_face);
    if (FAILED(face_result) || *retained_face == nullptr) {
      error = L"CreateFontFace failed for retained indexed font";
      return {};
    }
  }

  std::wstring resolved = ResolveDirectWriteFontObject(
      font.get(), factory, error);
  if (!resolved.empty() && retained_font != nullptr) {
    retained_font->reset(font.release());
  }
  return resolved;
}

}  // namespace

void* CreateDirectWriteFactory(const bool isolated, std::wstring& error) {
  IDWriteFactory* factory = nullptr;
  const HRESULT result = DWriteCreateFactory(
      isolated ? DWRITE_FACTORY_TYPE_ISOLATED : DWRITE_FACTORY_TYPE_SHARED,
      __uuidof(IDWriteFactory),
      reinterpret_cast<IUnknown**>(&factory));
  if (FAILED(result) || factory == nullptr) {
    error = L"DWriteCreateFactory failed before MacType preload";
    return nullptr;
  }
  return factory;
}

void ReleaseDirectWriteFactory(void* factory) noexcept {
  if (factory != nullptr) {
    static_cast<IDWriteFactory*>(factory)->Release();
  }
}

std::string RenderFingerprint(const std::wstring_view family,
                              std::wstring& error) {
  constexpr int width = 320;
  constexpr int height = 96;
  constexpr std::wstring_view text = L"MacType probe 0123456789 Aa 中 あ";

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  void* pixels = nullptr;
  UniqueGdiObject bitmap(
      CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0));
  UniqueDc dc(CreateCompatibleDC(nullptr));
  if (bitmap == nullptr || dc == nullptr || pixels == nullptr) {
    error = L"CreateDIBSection/CreateCompatibleDC failed: " +
            Win32ErrorMessage(GetLastError());
    return {};
  }
  const HGDIOBJ old_bitmap = SelectObject(dc.get(), bitmap.get());
  if (old_bitmap == nullptr || old_bitmap == HGDI_ERROR) {
    error =
        L"SelectObject(bitmap) failed: " + Win32ErrorMessage(GetLastError());
    return {};
  }
  RECT rectangle{0, 0, width, height};
  FillRect(dc.get(), &rectangle,
           static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
  SetTextColor(dc.get(), RGB(16, 24, 32));
  SetBkColor(dc.get(), RGB(255, 255, 255));
  SetBkMode(dc.get(), OPAQUE);
  const std::wstring family_name(family);
  UniqueGdiObject font(CreateFontW(
      -24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, family_name.c_str()));
  if (font == nullptr) {
    error = L"CreateFontW failed: " + Win32ErrorMessage(GetLastError());
    SelectObject(dc.get(), old_bitmap);
    return {};
  }
  const HGDIOBJ old_font =
      font == nullptr ? nullptr : SelectObject(dc.get(), font.get());
  RECT text_rectangle{12, 12, width - 12, height - 12};
  DrawTextW(dc.get(), text.data(), static_cast<int>(text.size()),
            &text_rectangle, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
  GdiFlush();

  std::string digest;
  const auto pixel_size = static_cast<std::size_t>(width) *
                          static_cast<std::size_t>(height) * 4U;
  if (!HashSha256(static_cast<const std::byte*>(pixels), pixel_size, digest)) {
    error = L"BCrypt SHA-256 calculation failed";
  }
  if (old_font != nullptr && old_font != HGDI_ERROR) {
    SelectObject(dc.get(), old_font);
  }
  SelectObject(dc.get(), old_bitmap);
  return digest;
}

FontSubstitutionObservation ObserveFontSubstitution(
    const std::wstring_view source_family,
    const std::wstring_view replacement_family, void* directwrite_factory,
    std::wstring& error) {
  FontSubstitutionObservation result;
  std::string repeated_disabled_source;
  std::string repeated_disabled_replacement;
  {
    FontSubstitutionSwitch disabled;
    if (RenderFingerprint(source_family, error).empty()) {
      return result;
    }
    result.disabled_source_fingerprint =
        RenderFingerprint(source_family, error);
    if (result.disabled_source_fingerprint.empty()) {
      return result;
    }
    result.disabled_replacement_fingerprint =
        RenderFingerprint(replacement_family, error);
    if (result.disabled_replacement_fingerprint.empty()) {
      return result;
    }
    repeated_disabled_source = RenderFingerprint(source_family, error);
    repeated_disabled_replacement =
        RenderFingerprint(replacement_family, error);
  }
  if (RenderFingerprint(source_family, error).empty()) {
    return result;
  }
  result.active_source_fingerprint = RenderFingerprint(source_family, error);
  const std::string repeated_active_source =
      RenderFingerprint(source_family, error);
  result.controls_stable =
      result.disabled_source_fingerprint == repeated_disabled_source &&
      result.disabled_replacement_fingerprint ==
          repeated_disabled_replacement &&
      result.active_source_fingerprint == repeated_active_source;
  result.replacement_observed =
      result.controls_stable && result.disabled_source_fingerprint !=
          result.disabled_replacement_fingerprint &&
      result.active_source_fingerprint ==
          result.disabled_replacement_fingerprint;

  const auto observe_direct_write = [&](const bool use_custom_collection) {
    DirectWriteSubstitutionObservation observation;
    std::wstring repeated_disabled_source_family;
    std::wstring repeated_disabled_replacement_family;
    {
      FontSubstitutionSwitch disabled;
      static_cast<void>(ResolveDirectWriteFamily(
          source_family, directwrite_factory, use_custom_collection, error));
      observation.disabled_source_family = ResolveDirectWriteFamily(
          source_family, directwrite_factory, use_custom_collection, error);
      observation.disabled_replacement_family = ResolveDirectWriteFamily(
          replacement_family, directwrite_factory, use_custom_collection,
          error);
      repeated_disabled_source_family = ResolveDirectWriteFamily(
          source_family, directwrite_factory, use_custom_collection, error);
      repeated_disabled_replacement_family = ResolveDirectWriteFamily(
          replacement_family, directwrite_factory, use_custom_collection,
          error);
    }
    static_cast<void>(ResolveDirectWriteFamily(
        source_family, directwrite_factory, use_custom_collection, error));
    observation.active_source_family = ResolveDirectWriteFamily(
        source_family, directwrite_factory, use_custom_collection, error);
    const std::wstring repeated_active_source_family = ResolveDirectWriteFamily(
        source_family, directwrite_factory, use_custom_collection, error);
    if (observation.disabled_source_family.empty() ||
        observation.disabled_replacement_family.empty() ||
        observation.active_source_family.empty()) {
      return observation;
    }
    observation.controls_stable =
        _wcsicmp(observation.disabled_source_family.c_str(),
                 repeated_disabled_source_family.c_str()) == 0 &&
        _wcsicmp(observation.disabled_replacement_family.c_str(),
                 repeated_disabled_replacement_family.c_str()) == 0 &&
        _wcsicmp(observation.active_source_family.c_str(),
                 repeated_active_source_family.c_str()) == 0;
    observation.replacement_observed =
        observation.controls_stable &&
        _wcsicmp(observation.disabled_source_family.c_str(),
                 observation.disabled_replacement_family.c_str()) != 0 &&
        _wcsicmp(observation.active_source_family.c_str(),
                 observation.disabled_replacement_family.c_str()) == 0;
    return observation;
  };
  result.direct_write = observe_direct_write(false);
  DirectWriteSubstitutionObservation& indexed =
      result.direct_write_indexed_collection;
  UINT32 indexed_source = 0;
  UINT32 indexed_replacement = 0;
  std::wstring repeated_disabled_indexed_source;
  std::wstring repeated_disabled_indexed_replacement;
  std::wstring repeated_disabled_source_postscript_name;
  std::wstring repeated_disabled_replacement_postscript_name;
  UniqueCom<IDWriteFont> pinned_disabled_source;
  UniqueCom<IDWriteFontFace> pinned_disabled_source_face;
  {
    FontSubstitutionSwitch disabled;
    indexed.disabled_source_family =
        ResolveIndexedDirectWriteFamily(source_family, directwrite_factory,
            indexed_source, true, indexed.disabled_source_postscript_name,
            &pinned_disabled_source, &pinned_disabled_source_face, error);
    indexed.disabled_replacement_family =
        ResolveIndexedDirectWriteFamily(replacement_family, directwrite_factory,
            indexed_replacement, true,
            indexed.disabled_replacement_postscript_name, nullptr, nullptr,
            error);
    repeated_disabled_indexed_source = ResolveIndexedDirectWriteFamily(
        source_family, directwrite_factory, indexed_source, false,
        repeated_disabled_source_postscript_name, nullptr, nullptr, error);
    repeated_disabled_indexed_replacement = ResolveIndexedDirectWriteFamily(
        replacement_family, directwrite_factory, indexed_replacement, false,
        repeated_disabled_replacement_postscript_name, nullptr, nullptr, error);
  }
  UniqueCom<IDWriteFont> retained_active_source;
  UniqueCom<IDWriteFontFace> retained_active_source_face;
  indexed.active_source_family =
      ResolveIndexedDirectWriteFamily(source_family, directwrite_factory,
          indexed_source, false, indexed.active_source_postscript_name,
          &retained_active_source,
          &retained_active_source_face, error);
  std::wstring repeated_active_source_postscript_name;
  const std::wstring repeated_active_indexed_source =
      ResolveIndexedDirectWriteFamily(source_family, directwrite_factory,
          indexed_source, false, repeated_active_source_postscript_name,
          nullptr, nullptr, error);
  UINT32 competing_index = 0;
  std::wstring competing_postscript_name;
  std::wstring const competing_family =
      _wcsicmp(std::wstring(source_family).c_str(), L"Impact") == 0
          ? L"Cambria"
          : L"Impact";
  ResolveIndexedDirectWriteFamily(
      competing_family, directwrite_factory, competing_index, true,
      competing_postscript_name, nullptr, nullptr, error);
  if (retained_active_source != nullptr) {
    BOOL exists = FALSE;
    IDWriteLocalizedStrings* raw_names = nullptr;
    const HRESULT names_result = retained_active_source->GetInformationalStrings(
        DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME, &raw_names, &exists);
    UniqueCom<IDWriteLocalizedStrings> names(raw_names);
    if (SUCCEEDED(names_result) && exists != FALSE && names != nullptr &&
        names->GetCount() != 0) {
      UINT32 length = 0;
      if (SUCCEEDED(names->GetStringLength(0, &length))) {
        indexed.active_retained_source_postscript_name.assign(
            static_cast<std::size_t>(length) + 1U, L'\0');
        if (SUCCEEDED(names->GetString(
                0, indexed.active_retained_source_postscript_name.data(),
                length + 1U))) {
          indexed.active_retained_source_postscript_name.resize(length);
        } else {
          indexed.active_retained_source_postscript_name.clear();
        }
      }
    }
  }
  indexed.active_pinned_source_family = ResolveDirectWriteFontObject(
      pinned_disabled_source.get(), directwrite_factory, error);
  indexed.active_pinned_source_descriptor_family =
      ResolveDirectWriteFontDescriptor(
          pinned_disabled_source_face.get(), directwrite_factory, error);
  indexed.controls_stable =
      !indexed.disabled_source_family.empty() &&
      !indexed.disabled_replacement_family.empty() &&
      _wcsicmp(indexed.disabled_source_family.c_str(),
          repeated_disabled_indexed_source.c_str()) == 0 &&
      _wcsicmp(indexed.disabled_replacement_family.c_str(),
          repeated_disabled_indexed_replacement.c_str()) == 0 &&
      _wcsicmp(indexed.active_source_family.c_str(),
          repeated_active_indexed_source.c_str()) == 0 &&
      _wcsicmp(indexed.disabled_source_postscript_name.c_str(),
          repeated_disabled_source_postscript_name.c_str()) == 0 &&
      _wcsicmp(indexed.disabled_replacement_postscript_name.c_str(),
          repeated_disabled_replacement_postscript_name.c_str()) == 0 &&
      _wcsicmp(indexed.active_source_postscript_name.c_str(),
          repeated_active_source_postscript_name.c_str()) == 0 &&
      _wcsicmp(indexed.disabled_source_postscript_name.c_str(),
          indexed.active_source_postscript_name.c_str()) == 0 &&
      _wcsicmp(indexed.disabled_source_postscript_name.c_str(),
          indexed.disabled_replacement_postscript_name.c_str()) != 0 &&
      _wcsicmp(indexed.disabled_source_family.c_str(),
               indexed.disabled_replacement_family.c_str()) != 0;
  indexed.retained_object_replacement_observed =
      _wcsicmp(indexed.active_pinned_source_family.c_str(),
                indexed.disabled_replacement_family.c_str()) == 0;
  indexed.retained_descriptor_replacement_observed =
      _wcsicmp(indexed.active_pinned_source_descriptor_family.c_str(),
               indexed.disabled_replacement_family.c_str()) == 0;
  indexed.retained_metadata_stable =
      !indexed.active_retained_source_postscript_name.empty() &&
      _wcsicmp(indexed.active_retained_source_postscript_name.c_str(),
               indexed.active_source_postscript_name.c_str()) == 0;
  indexed.retained_name_table_stable = FontNameTableContainsFamily(
      retained_active_source_face.get(), source_family);
  indexed.replacement_observed =
      indexed.controls_stable &&
      _wcsicmp(indexed.active_source_family.c_str(),
               indexed.disabled_replacement_family.c_str()) == 0 &&
      indexed.retained_object_replacement_observed &&
      indexed.retained_descriptor_replacement_observed &&
      indexed.retained_metadata_stable && indexed.retained_name_table_stable;
  result.direct_write_custom_collection = observe_direct_write(true);
  if (result.direct_write.active_source_family.empty() ||
      indexed.active_source_family.empty() ||
      indexed.active_pinned_source_descriptor_family.empty() ||
      result.direct_write_custom_collection.active_source_family.empty()) {
    result.active_source_fingerprint.clear();
  }
  return result;
}

}  // namespace mactype::service_probe::internal
