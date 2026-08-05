#include "render_probe.h"

#include "win32_support.h"

#include <bcrypt.h>
#include <dwrite.h>
#include <windows.h>

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
  IDWriteTextFormat* raw_format = nullptr;
  const HRESULT format_result = factory->CreateTextFormat(
      family_name.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24.0F, L"en-us",
      &raw_format);
  UniqueCom<IDWriteTextFormat> format(raw_format);
  if (FAILED(format_result) || format == nullptr) {
    error = L"IDWriteFactory::CreateTextFormat failed";
    return {};
  }
  const UINT32 length = format->GetFontFamilyNameLength();
  std::wstring resolved(static_cast<std::size_t>(length) + 1U, L'\0');
  if (FAILED(format->GetFontFamilyName(resolved.data(), length + 1U))) {
    error = L"IDWriteTextFormat::GetFontFamilyName failed";
    return {};
  }
  resolved.resize(length);
  return resolved;
}

}  // namespace

void* CreateDirectWriteFactory(std::wstring& error) {
  IDWriteFactory* factory = nullptr;
  const HRESULT result = DWriteCreateFactory(
      DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
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

  std::wstring repeated_disabled_source_family;
  std::wstring repeated_disabled_replacement_family;
  {
    FontSubstitutionSwitch disabled;
    static_cast<void>(ResolveDirectWriteFamily(source_family,
                                               directwrite_factory, error));
    result.direct_write.disabled_source_family =
        ResolveDirectWriteFamily(source_family, directwrite_factory, error);
    result.direct_write.disabled_replacement_family =
        ResolveDirectWriteFamily(replacement_family, directwrite_factory, error);
    repeated_disabled_source_family =
        ResolveDirectWriteFamily(source_family, directwrite_factory, error);
    repeated_disabled_replacement_family =
        ResolveDirectWriteFamily(replacement_family, directwrite_factory, error);
  }
  static_cast<void>(ResolveDirectWriteFamily(source_family,
                                             directwrite_factory, error));
  result.direct_write.active_source_family =
      ResolveDirectWriteFamily(source_family, directwrite_factory, error);
  const std::wstring repeated_active_source_family =
      ResolveDirectWriteFamily(source_family, directwrite_factory, error);
  if (result.direct_write.disabled_source_family.empty() ||
      result.direct_write.disabled_replacement_family.empty() ||
      result.direct_write.active_source_family.empty()) {
    result.active_source_fingerprint.clear();
    return result;
  }
  result.direct_write.controls_stable =
      _wcsicmp(result.direct_write.disabled_source_family.c_str(),
               repeated_disabled_source_family.c_str()) == 0 &&
      _wcsicmp(result.direct_write.disabled_replacement_family.c_str(),
               repeated_disabled_replacement_family.c_str()) == 0 &&
      _wcsicmp(result.direct_write.active_source_family.c_str(),
               repeated_active_source_family.c_str()) == 0;
  result.direct_write.replacement_observed =
      result.direct_write.controls_stable &&
      _wcsicmp(result.direct_write.disabled_source_family.c_str(),
               result.direct_write.disabled_replacement_family.c_str()) != 0 &&
      _wcsicmp(result.direct_write.active_source_family.c_str(),
               result.direct_write.disabled_replacement_family.c_str()) == 0;
  return result;
}

}  // namespace mactype::service_probe::internal
