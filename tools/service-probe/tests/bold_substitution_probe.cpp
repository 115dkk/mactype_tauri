#include <sdkddkver.h>
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10_NI

#include "../probe_common.h"
#include "../process_observation.h"
#include "../win32_support.h"

#include <bcrypt.h>
#include <dwrite_3.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using mactype::service_probe::EscapeJson;

constexpr int kRenderWidth = 480;
constexpr int kRenderHeight = 64;
constexpr DWORD kOs2Tag = 0x322F534FUL;
constexpr DWORD kTtcfTag = 0x66637474UL;

struct Arguments {
  std::wstring out;
  std::wstring core;
  std::wstring source = L"맑은 고딕";
  std::wstring replacement = L"Pretendard Medium";
  std::wstring pair = L"Pretendard ExtraBold";
  std::wstring text =
      L"맑은 고딕 Bold 굵게 ABC xyz";
  DWORD wait_ms = 5000;
};

void PrintUsage() {
  std::wcerr << L"Usage: bold-substitution-probe{32|64}.exe --out <json> "
                L"[--core <MacType64.dll|MacType.dll>] [--source <family>] "
                L"[--replacement <family>] [--pair <family>] "
                L"[--text <sample>] [--wait-ms <milliseconds>]\n";
}

bool ParseUnsigned(const wchar_t* text, DWORD& value) {
  if (text == nullptr || *text == L'\0') {
    return false;
  }
  wchar_t* end = nullptr;
  const unsigned long parsed = wcstoul(text, &end, 10);
  if (end == text || *end != L'\0' || parsed > MAXDWORD) {
    return false;
  }
  value = static_cast<DWORD>(parsed);
  return true;
}

bool ParseArguments(const int argc, wchar_t** argv, Arguments& result,
                    std::wstring& error) {
  for (int index = 1; index < argc; ++index) {
    const std::wstring_view argument(argv[index]);
    const bool has_value = index + 1 < argc;
    if (argument == L"--out" && has_value) {
      result.out = argv[++index];
    } else if (argument == L"--core" && has_value) {
      result.core = argv[++index];
    } else if (argument == L"--source" && has_value) {
      result.source = argv[++index];
    } else if (argument == L"--replacement" && has_value) {
      result.replacement = argv[++index];
    } else if (argument == L"--pair" && has_value) {
      result.pair = argv[++index];
    } else if (argument == L"--text" && has_value) {
      result.text = argv[++index];
    } else if (argument == L"--wait-ms" && has_value) {
      if (!ParseUnsigned(argv[++index], result.wait_ms)) {
        error = L"--wait-ms must be an unsigned integer";
        return false;
      }
    } else {
      error = L"Unknown or incomplete argument: " + std::wstring(argument);
      return false;
    }
  }
  if (result.out.empty()) {
    error = L"--out <json> is required";
    return false;
  }
  if (result.source.empty() || result.text.empty()) {
    error = L"--source and --text must not be empty";
    return false;
  }
  return true;
}

std::wstring HresultText(const wchar_t* operation, const HRESULT result) {
  std::wostringstream text;
  text << operation << L" failed: HRESULT 0x" << std::hex << std::setw(8)
       << std::setfill(L'0') << static_cast<unsigned long>(result);
  return text.str();
}

std::wstring LastErrorText(const wchar_t* operation) {
  const DWORD code = GetLastError();
  std::wostringstream text;
  text << operation << L" failed: error " << code;
  return text.str();
}

class Sha256 final {
 public:
  Sha256() {
    if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) < 0) {
      algorithm_ = nullptr;
      return;
    }
    DWORD object_size = 0;
    DWORD returned = 0;
    if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_size),
                          sizeof(object_size), &returned, 0) < 0) {
      return;
    }
    object_.resize(object_size);
    if (BCryptCreateHash(algorithm_, &hash_, object_.data(), object_size,
                         nullptr, 0, 0) < 0) {
      hash_ = nullptr;
      return;
    }
    ok_ = true;
  }

  ~Sha256() {
    if (hash_ != nullptr) {
      BCryptDestroyHash(hash_);
    }
    if (algorithm_ != nullptr) {
      BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
  }

  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  void Update(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const UCHAR*>(data);
    while (ok_ && size != 0) {
      const ULONG chunk = static_cast<ULONG>(
          size > 0x40000000U ? 0x40000000U : size);
      if (BCryptHashData(hash_, const_cast<PUCHAR>(bytes), chunk, 0) < 0) {
        ok_ = false;
        return;
      }
      bytes += chunk;
      size -= chunk;
    }
  }

  std::string Finish() {
    if (!ok_) {
      return {};
    }
    std::array<UCHAR, 32> digest{};
    if (BCryptFinishHash(hash_, digest.data(),
                         static_cast<ULONG>(digest.size()), 0) < 0) {
      ok_ = false;
      return {};
    }
    ok_ = false;
    std::ostringstream output;
    output << "sha256:" << std::hex << std::setfill('0');
    for (const UCHAR value : digest) {
      output << std::setw(2) << static_cast<unsigned int>(value);
    }
    return output.str();
  }

 private:
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
  std::vector<UCHAR> object_;
  bool ok_ = false;
};

std::string HashBytes(const void* data, const std::size_t size) {
  Sha256 hash;
  hash.Update(data, size);
  return hash.Finish();
}

std::string HashFile(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return {};
  }
  Sha256 hash;
  std::array<BYTE, 64 * 1024> buffer{};
  for (;;) {
    DWORD read = 0;
    if (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                 &read, nullptr) == FALSE) {
      CloseHandle(file);
      return {};
    }
    if (read == 0) {
      CloseHandle(file);
      return hash.Finish();
    }
    hash.Update(buffer.data(), read);
  }
}

class JsonWriter final {
 public:
  void BeginObject() {
    BeforeValue();
    out_ += '{';
    first_.push_back(true);
  }
  void EndObject() { Close('}'); }
  void BeginArray() {
    BeforeValue();
    out_ += '[';
    first_.push_back(true);
  }
  void EndArray() { Close(']'); }
  void Key(const std::string_view key) {
    Separate();
    out_ += '"';
    out_ += key;
    out_ += "\": ";
    after_key_ = true;
  }
  void String(const std::wstring_view value) {
    BeforeValue();
    out_ += '"';
    out_ += EscapeJson(value);
    out_ += '"';
  }
  void Ascii(const std::string_view value) {
    BeforeValue();
    out_ += '"';
    out_ += value;
    out_ += '"';
  }
  void AsciiOrNull(const std::string_view value) {
    if (value.empty()) {
      Null();
    } else {
      Ascii(value);
    }
  }
  void StringOrNull(const std::wstring_view value) {
    if (value.empty()) {
      Null();
    } else {
      String(value);
    }
  }
  void Bool(const bool value) {
    BeforeValue();
    out_ += value ? "true" : "false";
  }
  void Number(const long long value) {
    BeforeValue();
    out_ += std::to_string(value);
  }
  void Real(const double value) {
    BeforeValue();
    std::ostringstream text;
    text << std::setprecision(6) << value;
    out_ += text.str();
  }
  void Null() {
    BeforeValue();
    out_ += "null";
  }
  std::string Text() const { return out_ + "\n"; }

 private:
  void BeforeValue() {
    if (after_key_) {
      after_key_ = false;
      return;
    }
    Separate();
  }
  void Separate() {
    if (first_.empty()) {
      return;
    }
    if (!first_.back()) {
      out_ += ',';
    }
    first_.back() = false;
    NewLine();
  }
  void NewLine() {
    out_ += '\n';
    out_.append(first_.size() * 2U, ' ');
  }
  void Close(const char token) {
    const bool empty = first_.back();
    first_.pop_back();
    if (!empty) {
      NewLine();
    }
    out_ += token;
  }

  std::string out_;
  std::vector<bool> first_;
  bool after_key_ = false;
};

struct DcDeleter {
  void operator()(HDC dc) const noexcept {
    if (dc != nullptr) {
      DeleteDC(dc);
    }
  }
};
using UniqueDc = std::unique_ptr<std::remove_pointer_t<HDC>, DcDeleter>;

struct GdiObjectDeleter {
  void operator()(HGDIOBJ object) const noexcept {
    if (object != nullptr) {
      DeleteObject(object);
    }
  }
};
using UniqueGdiObject =
    std::unique_ptr<std::remove_pointer_t<HGDIOBJ>, GdiObjectDeleter>;

class WindowDcLease final {
 public:
  WindowDcLease(HWND window, HDC dc) noexcept : window_(window), dc_(dc) {}
  ~WindowDcLease() {
    if (dc_ != nullptr) {
      ReleaseDC(window_, dc_);
    }
  }
  WindowDcLease(const WindowDcLease&) = delete;
  WindowDcLease& operator=(const WindowDcLease&) = delete;

 private:
  HWND window_;
  HDC dc_;
};

class SelectionGuard final {
 public:
  SelectionGuard(HDC dc, HGDIOBJ previous) noexcept
      : dc_(dc), previous_(previous) {}
  ~SelectionGuard() {
    if (dc_ != nullptr && previous_ != nullptr && previous_ != HGDI_ERROR) {
      SelectObject(dc_, previous_);
    }
  }
  SelectionGuard(const SelectionGuard&) = delete;
  SelectionGuard& operator=(const SelectionGuard&) = delete;

 private:
  HDC dc_;
  HGDIOBJ previous_;
};

class HiddenWindow final {
 public:
  HiddenWindow() {
    instance_ = GetModuleHandleW(nullptr);
    WNDCLASSW registration{};
    registration.hInstance = instance_;
    registration.lpfnWndProc = DefWindowProcW;
    registration.lpszClassName = kClassName;
    if (RegisterClassW(&registration) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      error_ = LastErrorText(L"RegisterClassW");
      return;
    }
    registered_ = true;
    window_ = CreateWindowExW(0, kClassName, L"MacType bold substitution probe",
                              WS_OVERLAPPEDWINDOW, 0, 0, 640, 160, nullptr,
                              nullptr, instance_, nullptr);
    if (window_ == nullptr) {
      error_ = LastErrorText(L"CreateWindowExW");
    }
  }
  ~HiddenWindow() {
    if (window_ != nullptr) {
      DestroyWindow(window_);
    }
    if (registered_) {
      UnregisterClassW(kClassName, instance_);
    }
  }
  HiddenWindow(const HiddenWindow&) = delete;
  HiddenWindow& operator=(const HiddenWindow&) = delete;

  HWND Handle() const noexcept { return window_; }
  const std::wstring& Error() const noexcept { return error_; }

 private:
  static constexpr wchar_t kClassName[] = L"MacTypeBoldSubstitutionProbe";
  HINSTANCE instance_ = nullptr;
  HWND window_ = nullptr;
  bool registered_ = false;
  std::wstring error_;
};

struct GdiRequest {
  const char* key;
  std::wstring family;
  LONG weight;
};

struct GdiMeasurement {
  std::string key;
  std::wstring family;
  LONG weight = 0;
  const char* dc_kind = "memory";
  std::vector<std::wstring> errors;
  bool otm_ok = false;
  std::wstring otm_family;
  std::wstring otm_face;
  std::wstring otm_style;
  LONG tm_weight = 0;
  bool os2_ok = false;
  unsigned int us_weight_class = 0;
  unsigned int fs_selection = 0;
  std::string file_hash;
  DWORD file_size = 0;
  bool collection = false;
  std::wstring text_face;
  bool log_ok = false;
  std::wstring log_face;
  LONG log_weight = 0;
  std::string pixel_hash;
};

std::wstring OtmString(const std::vector<BYTE>& buffer, const PSTR offset) {
  const auto position = reinterpret_cast<std::uintptr_t>(offset);
  if (position == 0 || position >= buffer.size()) {
    return {};
  }
  const auto* begin = reinterpret_cast<const wchar_t*>(buffer.data() + position);
  const std::size_t limit = (buffer.size() - position) / sizeof(wchar_t);
  std::size_t length = 0;
  while (length < limit && begin[length] != L'\0') {
    ++length;
  }
  return std::wstring(begin, length);
}

std::string RenderPixels(HDC compatible_with, HFONT font,
                         const std::wstring& text,
                         std::vector<std::wstring>& errors) {
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = kRenderWidth;
  info.bmiHeader.biHeight = -kRenderHeight;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* pixels = nullptr;
  UniqueGdiObject bitmap(
      CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0));
  if (bitmap == nullptr || pixels == nullptr) {
    errors.push_back(LastErrorText(L"CreateDIBSection"));
    return {};
  }
  UniqueDc dc(CreateCompatibleDC(compatible_with));
  if (dc == nullptr) {
    errors.push_back(LastErrorText(L"CreateCompatibleDC(render)"));
    return {};
  }
  const HGDIOBJ old_bitmap = SelectObject(dc.get(), bitmap.get());
  if (old_bitmap == nullptr || old_bitmap == HGDI_ERROR) {
    errors.push_back(LastErrorText(L"SelectObject(bitmap)"));
    return {};
  }
  SelectionGuard bitmap_guard(dc.get(), old_bitmap);
  const HGDIOBJ old_font = SelectObject(dc.get(), font);
  if (old_font == nullptr || old_font == HGDI_ERROR) {
    errors.push_back(LastErrorText(L"SelectObject(render font)"));
    return {};
  }
  SelectionGuard font_guard(dc.get(), old_font);
  RECT area{0, 0, kRenderWidth, kRenderHeight};
  FillRect(dc.get(), &area, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
  SetTextColor(dc.get(), RGB(0, 0, 0));
  SetBkMode(dc.get(), TRANSPARENT);
  if (ExtTextOutW(dc.get(), 8, 16, 0, nullptr, text.c_str(),
                  static_cast<UINT>(text.size()), nullptr) == FALSE) {
    errors.push_back(LastErrorText(L"ExtTextOutW"));
    return {};
  }
  GdiFlush();
  const std::size_t size = static_cast<std::size_t>(kRenderWidth) *
                           static_cast<std::size_t>(kRenderHeight) * 4U;
  std::string digest = HashBytes(pixels, size);
  if (digest.empty()) {
    errors.push_back(L"SHA-256 of rendered pixels failed");
  }
  return digest;
}

GdiMeasurement MeasureGdi(const GdiRequest& request, const bool window_dc,
                          HWND window, const std::wstring& text) {
  GdiMeasurement measurement;
  measurement.key = request.key;
  measurement.family = request.family;
  measurement.weight = request.weight;
  measurement.dc_kind = window_dc ? "window" : "memory";

  UniqueDc memory_dc;
  HDC dc = nullptr;
  if (window_dc) {
    if (window == nullptr) {
      measurement.errors.push_back(L"hidden window is unavailable");
      return measurement;
    }
    dc = GetDC(window);
    if (dc == nullptr) {
      measurement.errors.push_back(LastErrorText(L"GetDC(window)"));
      return measurement;
    }
  } else {
    memory_dc.reset(CreateCompatibleDC(nullptr));
    dc = memory_dc.get();
    if (dc == nullptr) {
      measurement.errors.push_back(LastErrorText(L"CreateCompatibleDC"));
      return measurement;
    }
  }
  WindowDcLease lease(window_dc ? window : nullptr, window_dc ? dc : nullptr);

  UniqueGdiObject font(CreateFontW(
      -24, 0, 0, 0, request.weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH, request.family.c_str()));
  if (font == nullptr) {
    measurement.errors.push_back(LastErrorText(L"CreateFontW"));
    return measurement;
  }
  const HGDIOBJ previous = SelectObject(dc, font.get());
  if (previous == nullptr || previous == HGDI_ERROR) {
    measurement.errors.push_back(LastErrorText(L"SelectObject(font)"));
    return measurement;
  }
  SelectionGuard font_guard(dc, previous);

  const UINT otm_size = GetOutlineTextMetricsW(dc, 0, nullptr);
  if (otm_size < sizeof(OUTLINETEXTMETRICW)) {
    measurement.errors.push_back(LastErrorText(L"GetOutlineTextMetricsW(size)"));
  } else {
    std::vector<BYTE> buffer(otm_size);
    auto* metrics = reinterpret_cast<OUTLINETEXTMETRICW*>(buffer.data());
    if (GetOutlineTextMetricsW(dc, otm_size, metrics) == 0) {
      measurement.errors.push_back(LastErrorText(L"GetOutlineTextMetricsW"));
    } else {
      measurement.otm_ok = true;
      measurement.otm_family = OtmString(buffer, metrics->otmpFamilyName);
      measurement.otm_face = OtmString(buffer, metrics->otmpFaceName);
      measurement.otm_style = OtmString(buffer, metrics->otmpStyleName);
      measurement.tm_weight = metrics->otmTextMetrics.tmWeight;
    }
  }

  const DWORD os2_size = GetFontData(dc, kOs2Tag, 0, nullptr, 0);
  if (os2_size == GDI_ERROR || os2_size < 64) {
    measurement.errors.push_back(L"GetFontData(OS/2) unavailable");
  } else {
    std::vector<BYTE> os2(os2_size);
    if (GetFontData(dc, kOs2Tag, 0, os2.data(), os2_size) != os2_size) {
      measurement.errors.push_back(L"GetFontData(OS/2) read failed");
    } else {
      measurement.os2_ok = true;
      measurement.us_weight_class =
          (static_cast<unsigned int>(os2[4]) << 8U) | os2[5];
      measurement.fs_selection =
          (static_cast<unsigned int>(os2[62]) << 8U) | os2[63];
    }
  }

  measurement.collection =
      GetFontData(dc, kTtcfTag, 0, nullptr, 0) != GDI_ERROR;
  const DWORD file_size = GetFontData(dc, 0, 0, nullptr, 0);
  if (file_size == GDI_ERROR || file_size == 0) {
    measurement.errors.push_back(L"GetFontData(whole file) unavailable");
  } else {
    std::vector<BYTE> file(file_size);
    if (GetFontData(dc, 0, 0, file.data(), file_size) != file_size) {
      measurement.errors.push_back(L"GetFontData(whole file) read failed");
    } else {
      measurement.file_size = file_size;
      measurement.file_hash = HashBytes(file.data(), file.size());
    }
  }

  const int face_length = GetTextFaceW(dc, 0, nullptr);
  if (face_length > 0) {
    std::vector<wchar_t> face(static_cast<std::size_t>(face_length) + 1U);
    const int copied =
        GetTextFaceW(dc, face_length + 1, face.data());
    if (copied > 0) {
      measurement.text_face.assign(face.data());
    }
  }
  if (measurement.text_face.empty()) {
    measurement.errors.push_back(LastErrorText(L"GetTextFaceW"));
  }

  LOGFONTW logfont{};
  if (GetObjectW(font.get(), sizeof(logfont), &logfont) == sizeof(logfont)) {
    measurement.log_ok = true;
    logfont.lfFaceName[LF_FACESIZE - 1] = L'\0';
    measurement.log_face = logfont.lfFaceName;
    measurement.log_weight = logfont.lfWeight;
  } else {
    measurement.errors.push_back(LastErrorText(L"GetObjectW(font)"));
  }

  measurement.pixel_hash = RenderPixels(
      dc, static_cast<HFONT>(font.get()), text, measurement.errors);
  return measurement;
}

struct FontFileRecord {
  std::wstring path;
  bool local = false;
  std::string hash;
  std::wstring error;
};

struct FaceRecord {
  bool ok = false;
  std::vector<std::wstring> errors;
  UINT32 index = 0;
  DWRITE_FONT_SIMULATIONS simulations = DWRITE_FONT_SIMULATIONS_NONE;
  std::vector<FontFileRecord> files;
  std::string geometry_hash;
  std::vector<std::string> geometry_tables;
  bool has_weight = false;
  UINT32 weight = 0;
  std::wstring family_name;
  std::wstring face_name;
  bool has_axes = false;
  std::vector<DWRITE_FONT_AXIS_VALUE> axes;
};

struct DWriteFontRecord {
  std::string label;
  std::wstring family;
  UINT32 requested_weight = 0;
  bool found = false;
  std::vector<std::wstring> errors;
  UINT32 weight = 0;
  DWRITE_FONT_SIMULATIONS simulations = DWRITE_FONT_SIMULATIONS_NONE;
  std::wstring face_name;
  std::vector<std::pair<std::wstring, std::wstring>> family_names;
  std::wstring win32_family;
  std::wstring full_name;
  std::wstring postscript_name;
  FaceRecord face;
};

struct GlyphRunRecord {
  UINT32 text_position = 0;
  UINT32 text_length = 0;
  UINT32 glyph_count = 0;
  float em_size = 0.0F;
  FaceRecord face;
};

struct TextFormatRecord {
  UINT32 requested_weight = 0;
  bool ok = false;
  std::vector<std::wstring> errors;
  std::vector<GlyphRunRecord> runs;
};

struct FallbackRecord {
  std::wstring primary_family;
  std::wstring sample_text;
  bool ok = false;
  std::vector<std::wstring> errors;
  UINT32 text_position = 0;
  UINT32 text_length = 0;
  UINT32 mapped_length = 0;
  float scale = 0.0F;
  bool has_font = false;
  UINT32 weight = 0;
  DWRITE_FONT_SIMULATIONS simulations = DWRITE_FONT_SIMULATIONS_NONE;
  std::vector<std::pair<std::wstring, std::wstring>> family_names;
  std::wstring win32_family;
  FaceRecord face;
  TextFormatRecord layout;
};

std::wstring LocalizedAt(IDWriteLocalizedStrings* strings, const UINT32 index) {
  UINT32 length = 0;
  if (strings == nullptr || FAILED(strings->GetStringLength(index, &length))) {
    return {};
  }
  std::vector<wchar_t> value(static_cast<std::size_t>(length) + 1U, L'\0');
  if (FAILED(strings->GetString(index, value.data(), length + 1U))) {
    return {};
  }
  return std::wstring(value.data(), length);
}

std::wstring LocaleAt(IDWriteLocalizedStrings* strings, const UINT32 index) {
  UINT32 length = 0;
  if (strings == nullptr ||
      FAILED(strings->GetLocaleNameLength(index, &length))) {
    return {};
  }
  std::vector<wchar_t> value(static_cast<std::size_t>(length) + 1U, L'\0');
  if (FAILED(strings->GetLocaleName(index, value.data(), length + 1U))) {
    return {};
  }
  return std::wstring(value.data(), length);
}

std::wstring InformationalString(IDWriteFont* font,
                                 const DWRITE_INFORMATIONAL_STRING_ID id) {
  ComPtr<IDWriteLocalizedStrings> strings;
  BOOL exists = FALSE;
  if (FAILED(font->GetInformationalStrings(id, &strings, &exists)) ||
      exists == FALSE || strings == nullptr || strings->GetCount() == 0) {
    return {};
  }
  return LocalizedAt(strings.Get(), 0);
}

std::string TagText(const UINT32 tag) {
  std::string text(4, ' ');
  text[0] = static_cast<char>(tag & 0xFFU);
  text[1] = static_cast<char>((tag >> 8U) & 0xFFU);
  text[2] = static_cast<char>((tag >> 16U) & 0xFFU);
  text[3] = static_cast<char>((tag >> 24U) & 0xFFU);
  return text;
}

class ScopedFontTable final {
 public:
  ScopedFontTable(IDWriteFontFace* face, void* context,
                  const bool acquired) noexcept
      : face_(face), context_(context), acquired_(acquired) {}
  ~ScopedFontTable() noexcept {
    if (face_ != nullptr && acquired_) {
      face_->ReleaseFontTable(context_);
    }
  }
  ScopedFontTable(const ScopedFontTable&) = delete;
  ScopedFontTable& operator=(const ScopedFontTable&) = delete;

 private:
  IDWriteFontFace* face_;
  void* context_;
  bool acquired_;
};

FaceRecord DescribeFace(IDWriteFontFace* face) {
  FaceRecord record;
  if (face == nullptr) {
    record.errors.push_back(L"font face is null");
    return record;
  }
  record.ok = true;
  record.index = face->GetIndex();
  record.simulations = face->GetSimulations();

  UINT32 file_count = 0;
  HRESULT result = face->GetFiles(&file_count, nullptr);
  if (FAILED(result)) {
    record.errors.push_back(HresultText(L"IDWriteFontFace::GetFiles(count)", result));
  } else if (file_count != 0) {
    std::vector<IDWriteFontFile*> raw_files(file_count, nullptr);
    result = face->GetFiles(&file_count, raw_files.data());
    std::vector<ComPtr<IDWriteFontFile>> files;
    files.reserve(raw_files.size());
    for (IDWriteFontFile* file : raw_files) {
      ComPtr<IDWriteFontFile> owned;
      owned.Attach(file);
      files.push_back(std::move(owned));
    }
    if (FAILED(result)) {
      record.errors.push_back(HresultText(L"IDWriteFontFace::GetFiles", result));
    } else {
      for (const ComPtr<IDWriteFontFile>& file : files) {
        FontFileRecord file_record;
        const void* key = nullptr;
        UINT32 key_size = 0;
        ComPtr<IDWriteFontFileLoader> loader;
        ComPtr<IDWriteLocalFontFileLoader> local;
        if (file == nullptr) {
          file_record.error = L"font file is null";
        } else if (FAILED(file->GetReferenceKey(&key, &key_size))) {
          file_record.error = L"GetReferenceKey failed";
        } else if (FAILED(file->GetLoader(&loader)) || loader == nullptr) {
          file_record.error = L"GetLoader failed";
        } else if (FAILED(loader.As(&local)) || local == nullptr) {
          file_record.error = L"loader is not local";
        } else {
          file_record.local = true;
          UINT32 length = 0;
          if (FAILED(local->GetFilePathLengthFromKey(key, key_size, &length))) {
            file_record.error = L"GetFilePathLengthFromKey failed";
          } else {
            std::vector<wchar_t> path(static_cast<std::size_t>(length) + 1U,
                                      L'\0');
            if (FAILED(local->GetFilePathFromKey(key, key_size, path.data(),
                                                 length + 1U))) {
              file_record.error = L"GetFilePathFromKey failed";
            } else {
              file_record.path.assign(path.data(), length);
              file_record.hash = HashFile(file_record.path);
            }
          }
        }
        record.files.push_back(std::move(file_record));
      }
    }
  }

  constexpr std::array<UINT32, 5> tables = {
      DWRITE_MAKE_OPENTYPE_TAG('c', 'm', 'a', 'p'),
      DWRITE_MAKE_OPENTYPE_TAG('g', 'l', 'y', 'f'),
      DWRITE_MAKE_OPENTYPE_TAG('C', 'F', 'F', ' '),
      DWRITE_MAKE_OPENTYPE_TAG('h', 'm', 't', 'x'),
      DWRITE_MAKE_OPENTYPE_TAG('h', 'h', 'e', 'a')};
  Sha256 geometry;
  for (const UINT32 tag : tables) {
    const void* data = nullptr;
    UINT32 size = 0;
    void* context = nullptr;
    BOOL exists = FALSE;
    result = face->TryGetFontTable(tag, &data, &size, &context, &exists);
    if (FAILED(result)) {
      record.errors.push_back(HresultText(L"TryGetFontTable", result));
      continue;
    }
    ScopedFontTable table(face, context, exists != FALSE);
    if (exists == FALSE || data == nullptr) {
      continue;
    }
    record.geometry_tables.push_back(TagText(tag));
    geometry.Update(&tag, sizeof(tag));
    geometry.Update(&size, sizeof(size));
    geometry.Update(data, size);
  }
  if (!record.geometry_tables.empty()) {
    record.geometry_hash = geometry.Finish();
  }

  ComPtr<IDWriteFontFace3> face3;
  if (SUCCEEDED(face->QueryInterface(IID_PPV_ARGS(&face3))) &&
      face3 != nullptr) {
    record.has_weight = true;
    record.weight = static_cast<UINT32>(face3->GetWeight());
    ComPtr<IDWriteLocalizedStrings> names;
    if (SUCCEEDED(face3->GetFamilyNames(&names)) && names != nullptr &&
        names->GetCount() != 0) {
      record.family_name = LocalizedAt(names.Get(), 0);
    }
    names.Reset();
    if (SUCCEEDED(face3->GetFaceNames(&names)) && names != nullptr &&
        names->GetCount() != 0) {
      record.face_name = LocalizedAt(names.Get(), 0);
    }
  }

  ComPtr<IDWriteFontFace5> face5;
  if (SUCCEEDED(face->QueryInterface(IID_PPV_ARGS(&face5))) &&
      face5 != nullptr) {
    const UINT32 count = face5->GetFontAxisValueCount();
    record.axes.resize(count);
    if (count == 0 ||
        SUCCEEDED(face5->GetFontAxisValues(record.axes.data(), count))) {
      record.has_axes = true;
    } else {
      record.axes.clear();
      record.errors.push_back(L"IDWriteFontFace5::GetFontAxisValues failed");
    }
  }
  return record;
}

DWriteFontRecord DescribeCollectionFont(IDWriteFontCollection* collection,
                                        const char* label,
                                        const std::wstring& family,
                                        const UINT32 weight) {
  DWriteFontRecord record;
  record.label = label;
  record.family = family;
  record.requested_weight = weight;
  UINT32 index = 0;
  BOOL exists = FALSE;
  HRESULT result = collection->FindFamilyName(family.c_str(), &index, &exists);
  if (FAILED(result)) {
    record.errors.push_back(HresultText(L"FindFamilyName", result));
    return record;
  }
  if (exists == FALSE) {
    record.errors.push_back(L"family not found in the system collection");
    return record;
  }
  ComPtr<IDWriteFontFamily> font_family;
  result = collection->GetFontFamily(index, &font_family);
  if (FAILED(result) || font_family == nullptr) {
    record.errors.push_back(HresultText(L"GetFontFamily", result));
    return record;
  }
  ComPtr<IDWriteFont> font;
  result = font_family->GetFirstMatchingFont(
      static_cast<DWRITE_FONT_WEIGHT>(weight), DWRITE_FONT_STRETCH_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, &font);
  if (FAILED(result) || font == nullptr) {
    record.errors.push_back(HresultText(L"GetFirstMatchingFont", result));
    return record;
  }
  record.found = true;
  record.weight = static_cast<UINT32>(font->GetWeight());
  record.simulations = font->GetSimulations();
  ComPtr<IDWriteLocalizedStrings> face_names;
  if (SUCCEEDED(font->GetFaceNames(&face_names)) && face_names != nullptr &&
      face_names->GetCount() != 0) {
    record.face_name = LocalizedAt(face_names.Get(), 0);
  }
  ComPtr<IDWriteFontFamily> owning_family;
  ComPtr<IDWriteLocalizedStrings> family_names;
  if (SUCCEEDED(font->GetFontFamily(&owning_family)) &&
      owning_family != nullptr &&
      SUCCEEDED(owning_family->GetFamilyNames(&family_names)) &&
      family_names != nullptr) {
    const UINT32 count = family_names->GetCount();
    for (UINT32 name = 0; name < count; ++name) {
      record.family_names.emplace_back(LocaleAt(family_names.Get(), name),
                                       LocalizedAt(family_names.Get(), name));
    }
  }
  record.win32_family =
      InformationalString(font.Get(), DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES);
  record.full_name =
      InformationalString(font.Get(), DWRITE_INFORMATIONAL_STRING_FULL_NAME);
  record.postscript_name = InformationalString(
      font.Get(), DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME);
  ComPtr<IDWriteFontFace> face;
  result = font->CreateFontFace(&face);
  if (FAILED(result) || face == nullptr) {
    record.errors.push_back(HresultText(L"IDWriteFont::CreateFontFace", result));
    return record;
  }
  record.face = DescribeFace(face.Get());
  return record;
}

class CaptureRenderer final : public IDWriteTextRenderer {
 public:
  IFACEMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) ||
        riid == __uuidof(IDWriteTextRenderer)) {
      *object = static_cast<IDWriteTextRenderer*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&references_));
  }
  IFACEMETHODIMP_(ULONG) Release() override {
    const LONG remaining = InterlockedDecrement(&references_);
    if (remaining == 0) {
      delete this;
    }
    return static_cast<ULONG>(remaining);
  }
  IFACEMETHODIMP IsPixelSnappingDisabled(void*, BOOL* disabled) override {
    if (disabled == nullptr) {
      return E_POINTER;
    }
    *disabled = FALSE;
    return S_OK;
  }
  IFACEMETHODIMP GetCurrentTransform(void*, DWRITE_MATRIX* transform) override {
    if (transform == nullptr) {
      return E_POINTER;
    }
    *transform = DWRITE_MATRIX{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
    return S_OK;
  }
  IFACEMETHODIMP GetPixelsPerDip(void*, FLOAT* pixels) override {
    if (pixels == nullptr) {
      return E_POINTER;
    }
    *pixels = 1.0F;
    return S_OK;
  }
  IFACEMETHODIMP DrawGlyphRun(void*, FLOAT, FLOAT, DWRITE_MEASURING_MODE,
                              const DWRITE_GLYPH_RUN* run,
                              const DWRITE_GLYPH_RUN_DESCRIPTION* description,
                              IUnknown*) override {
    if (run == nullptr) {
      return S_OK;
    }
    GlyphRunRecord record;
    if (description != nullptr) {
      record.text_position = description->textPosition;
      record.text_length = description->stringLength;
    }
    record.glyph_count = run->glyphCount;
    record.em_size = run->fontEmSize;
    record.face = DescribeFace(run->fontFace);
    runs_.push_back(std::move(record));
    return S_OK;
  }
  IFACEMETHODIMP DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*,
                               IUnknown*) override {
    return S_OK;
  }
  IFACEMETHODIMP DrawStrikethrough(void*, FLOAT, FLOAT,
                                   const DWRITE_STRIKETHROUGH*,
                                   IUnknown*) override {
    return S_OK;
  }
  IFACEMETHODIMP DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*,
                                  BOOL, BOOL, IUnknown*) override {
    return S_OK;
  }

  std::vector<GlyphRunRecord> TakeRuns() { return std::move(runs_); }

 private:
  ~CaptureRenderer() = default;

  LONG references_ = 1;
  std::vector<GlyphRunRecord> runs_;
};

class AnalysisSource final : public IDWriteTextAnalysisSource {
 public:
  explicit AnalysisSource(const std::wstring& text) : text_(text) {}

  IFACEMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IDWriteTextAnalysisSource)) {
      *object = static_cast<IDWriteTextAnalysisSource*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&references_));
  }
  IFACEMETHODIMP_(ULONG) Release() override {
    const LONG remaining = InterlockedDecrement(&references_);
    if (remaining == 0) {
      delete this;
    }
    return static_cast<ULONG>(remaining);
  }
  IFACEMETHODIMP GetTextAtPosition(UINT32 position, const wchar_t** text,
                                   UINT32* length) override {
    if (text == nullptr || length == nullptr) {
      return E_POINTER;
    }
    if (position >= text_.size()) {
      *text = nullptr;
      *length = 0;
      return S_OK;
    }
    *text = text_.c_str() + position;
    *length = static_cast<UINT32>(text_.size() - position);
    return S_OK;
  }
  IFACEMETHODIMP GetTextBeforePosition(UINT32 position, const wchar_t** text,
                                       UINT32* length) override {
    if (text == nullptr || length == nullptr) {
      return E_POINTER;
    }
    const UINT32 available = static_cast<UINT32>(
        (std::min)(static_cast<std::size_t>(position), text_.size()));
    *text = available == 0 ? nullptr : text_.c_str();
    *length = available;
    return S_OK;
  }
  IFACEMETHODIMP_(DWRITE_READING_DIRECTION)
  GetParagraphReadingDirection() override {
    return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
  }
  IFACEMETHODIMP GetLocaleName(UINT32 position, UINT32* length,
                               const wchar_t** locale) override {
    if (length == nullptr || locale == nullptr) {
      return E_POINTER;
    }
    *length = position >= text_.size()
                  ? 0
                  : static_cast<UINT32>(text_.size() - position);
    *locale = L"ko-kr";
    return S_OK;
  }
  IFACEMETHODIMP GetNumberSubstitution(
      UINT32 position, UINT32* length,
      IDWriteNumberSubstitution** substitution) override {
    if (length == nullptr || substitution == nullptr) {
      return E_POINTER;
    }
    *length = position >= text_.size()
                  ? 0
                  : static_cast<UINT32>(text_.size() - position);
    *substitution = nullptr;
    return S_OK;
  }

 private:
  ~AnalysisSource() = default;

  LONG references_ = 1;
  std::wstring text_;
};

TextFormatRecord MeasureTextFormat(IDWriteFactory* factory,
                                   const std::wstring& family,
                                   DWRITE_FONT_WEIGHT weight,
                                   const std::wstring& text);

FallbackRecord MeasureFallback(IDWriteFactory* factory,
                               const std::wstring& primary_family,
                               const std::wstring& text,
                               const UINT32 text_position,
                               const UINT32 text_length) {
  FallbackRecord record;
  record.primary_family = primary_family;
  record.sample_text = text;
  record.text_position = text_position;
  record.text_length = text_length;
  record.layout = MeasureTextFormat(factory, primary_family,
                                    DWRITE_FONT_WEIGHT_NORMAL, text);
  ComPtr<IDWriteFactory2> factory2;
  ComPtr<IDWriteFontFallback> fallback;
  if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory2))) ||
      factory2 == nullptr ||
      FAILED(factory2->GetSystemFontFallback(&fallback)) ||
      fallback == nullptr) {
    record.errors.push_back(L"GetSystemFontFallback failed");
    return record;
  }
  ComPtr<AnalysisSource> source;
  source.Attach(new AnalysisSource(text));
  ComPtr<IDWriteFont> font;
  HRESULT result = fallback->MapCharacters(
      source.Get(), text_position, text_length, nullptr,
      primary_family.c_str(), DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
      &record.mapped_length, &font, &record.scale);
  if (FAILED(result)) {
    record.errors.push_back(HresultText(L"IDWriteFontFallback::MapCharacters", result));
    return record;
  }
  record.ok = true;
  if (font == nullptr) {
    return record;
  }
  record.has_font = true;
  record.weight = static_cast<UINT32>(font->GetWeight());
  record.simulations = font->GetSimulations();
  ComPtr<IDWriteFontFamily> family;
  ComPtr<IDWriteLocalizedStrings> names;
  if (SUCCEEDED(font->GetFontFamily(&family)) && family != nullptr &&
      SUCCEEDED(family->GetFamilyNames(&names)) && names != nullptr) {
    for (UINT32 index = 0; index < names->GetCount(); ++index) {
      record.family_names.emplace_back(LocaleAt(names.Get(), index),
                                       LocalizedAt(names.Get(), index));
    }
  }
  record.win32_family = InformationalString(
      font.Get(), DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES);
  ComPtr<IDWriteFontFace> face;
  result = font->CreateFontFace(&face);
  if (FAILED(result) || face == nullptr) {
    record.errors.push_back(HresultText(L"fallback IDWriteFont::CreateFontFace", result));
    return record;
  }
  record.face = DescribeFace(face.Get());
  return record;
}

TextFormatRecord MeasureTextFormat(IDWriteFactory* factory,
                                   const std::wstring& family,
                                   const DWRITE_FONT_WEIGHT weight,
                                   const std::wstring& text) {
  TextFormatRecord record;
  record.requested_weight = static_cast<UINT32>(weight);
  ComPtr<IDWriteTextFormat> format;
  HRESULT result = factory->CreateTextFormat(
      family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, 24.0F, L"ko-kr", &format);
  if (FAILED(result) || format == nullptr) {
    record.errors.push_back(HresultText(L"CreateTextFormat", result));
    return record;
  }
  ComPtr<IDWriteTextLayout> layout;
  result = factory->CreateTextLayout(text.c_str(),
                                     static_cast<UINT32>(text.size()),
                                     format.Get(), 2000.0F, 200.0F, &layout);
  if (FAILED(result) || layout == nullptr) {
    record.errors.push_back(HresultText(L"CreateTextLayout", result));
    return record;
  }
  ComPtr<CaptureRenderer> renderer;
  renderer.Attach(new CaptureRenderer());
  result = layout->Draw(nullptr, renderer.Get(), 0.0F, 0.0F);
  record.runs = renderer->TakeRuns();
  if (FAILED(result)) {
    record.errors.push_back(HresultText(L"IDWriteTextLayout::Draw", result));
    return record;
  }
  record.ok = true;
  return record;
}

struct StateMeasurement {
  std::string name;
  std::vector<GdiMeasurement> gdi;
  bool dwrite_available = false;
  std::vector<std::wstring> dwrite_errors;
  std::vector<DWriteFontRecord> dwrite_fonts;
  std::vector<TextFormatRecord> text_formats;
  std::vector<FallbackRecord> fallbacks;
};

StateMeasurement MeasureState(const char* name, const Arguments& arguments,
                              HWND window) {
  StateMeasurement state;
  state.name = name;
  const std::vector<GdiRequest> requests = {
      {"source400", arguments.source, 400},
      {"source600", arguments.source, 600},
      {"source700", arguments.source, 700},
      {"pretendard700", L"Pretendard", 700},
      {"pretendard400", L"Pretendard", 400},
      {"pretendardMedium400", L"Pretendard Medium", 400},
      {"pretendardMedium700", L"Pretendard Medium", 700},
      {"pretendardExtraBold700", L"Pretendard ExtraBold", 700},
      {"pretendardExtraBold800", L"Pretendard ExtraBold", 800},
  };
  for (const bool window_dc : {false, true}) {
    for (const GdiRequest& request : requests) {
      state.gdi.push_back(MeasureGdi(request, window_dc, window, arguments.text));
    }
  }

  ComPtr<IDWriteFactory> factory;
  HRESULT result = DWriteCreateFactory(
      DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
      reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
  if (FAILED(result) || factory == nullptr) {
    state.dwrite_errors.push_back(HresultText(L"DWriteCreateFactory", result));
    return state;
  }
  ComPtr<IDWriteFontCollection> collection;
  result = factory->GetSystemFontCollection(&collection, TRUE);
  if (FAILED(result) || collection == nullptr) {
    state.dwrite_errors.push_back(
        HresultText(L"GetSystemFontCollection", result));
    return state;
  }
  state.dwrite_available = true;
  state.dwrite_fonts.push_back(
      DescribeCollectionFont(collection.Get(), "source400", arguments.source, 400));
  state.dwrite_fonts.push_back(
      DescribeCollectionFont(collection.Get(), "source700", arguments.source, 700));
  state.dwrite_fonts.push_back(
      DescribeCollectionFont(collection.Get(), "pretendard400", L"Pretendard", 400));
  state.dwrite_fonts.push_back(
      DescribeCollectionFont(collection.Get(), "pretendard500", L"Pretendard", 500));
  state.dwrite_fonts.push_back(
      DescribeCollectionFont(collection.Get(), "pretendard700", L"Pretendard", 700));
  state.dwrite_fonts.push_back(
      DescribeCollectionFont(collection.Get(), "pretendard800", L"Pretendard", 800));
  state.text_formats.push_back(MeasureTextFormat(
      factory.Get(), arguments.source, DWRITE_FONT_WEIGHT_BOLD, arguments.text));
  state.text_formats.push_back(MeasureTextFormat(
      factory.Get(), arguments.source, DWRITE_FONT_WEIGHT_NORMAL, arguments.text));
  constexpr wchar_t fallback_text[] = L"Abc 검색 대체로 흐림";
  constexpr UINT32 hangul_position = 4;
  constexpr UINT32 hangul_length =
      static_cast<UINT32>(_countof(fallback_text) - 1) - hangul_position;
  for (const wchar_t* primary : {L"Segoe UI", L"Segoe UI Variable"}) {
    FallbackRecord fallback = MeasureFallback(
        factory.Get(), primary, fallback_text, hangul_position, hangul_length);
    if (fallback.mapped_length != 0 &&
        fallback.mapped_length < fallback.text_length) {
      fallback = MeasureFallback(
          factory.Get(), primary, fallback_text, hangul_position,
          fallback.mapped_length);
    }
    state.fallbacks.push_back(std::move(fallback));
  }
  return state;
}

std::string SimulationsText(const DWRITE_FONT_SIMULATIONS simulations) {
  const bool bold = (simulations & DWRITE_FONT_SIMULATIONS_BOLD) != 0;
  const bool oblique = (simulations & DWRITE_FONT_SIMULATIONS_OBLIQUE) != 0;
  if (bold && oblique) {
    return "bold+oblique";
  }
  if (bold) {
    return "bold";
  }
  if (oblique) {
    return "oblique";
  }
  return "none";
}

void WriteErrors(JsonWriter& json, const std::vector<std::wstring>& errors) {
  json.Key("errors");
  json.BeginArray();
  for (const std::wstring& error : errors) {
    json.String(error);
  }
  json.EndArray();
}

void WriteFace(JsonWriter& json, const FaceRecord& face) {
  json.BeginObject();
  json.Key("ok");
  json.Bool(face.ok);
  json.Key("index");
  json.Number(face.index);
  json.Key("simulations");
  json.Ascii(SimulationsText(face.simulations));
  json.Key("files");
  json.BeginArray();
  for (const FontFileRecord& file : face.files) {
    json.BeginObject();
    json.Key("path");
    json.StringOrNull(file.path);
    json.Key("local");
    json.Bool(file.local);
    json.Key("sha256");
    json.AsciiOrNull(file.hash);
    json.Key("error");
    json.StringOrNull(file.error);
    json.EndObject();
  }
  json.EndArray();
  json.Key("weight");
  if (face.has_weight) {
    json.Number(face.weight);
  } else {
    json.Null();
  }
  json.Key("familyName");
  json.StringOrNull(face.family_name);
  json.Key("faceName");
  json.StringOrNull(face.face_name);
  json.Key("axes");
  if (face.has_axes) {
    json.BeginArray();
    for (const DWRITE_FONT_AXIS_VALUE& axis : face.axes) {
      json.BeginObject();
      json.Key("tag");
      json.Ascii(TagText(static_cast<UINT32>(axis.axisTag)));
      json.Key("value");
      json.Real(axis.value);
      json.EndObject();
    }
    json.EndArray();
  } else {
    json.Null();
  }
  json.Key("geometrySha256");
  json.AsciiOrNull(face.geometry_hash);
  json.Key("geometryTables");
  json.BeginArray();
  for (const std::string& table : face.geometry_tables) {
    json.Ascii(table);
  }
  json.EndArray();
  WriteErrors(json, face.errors);
  json.EndObject();
}

void WriteGdiMeasurement(JsonWriter& json, const GdiMeasurement& measurement) {
  json.BeginObject();
  json.Key("key");
  json.Ascii(measurement.key);
  json.Key("dc");
  json.Ascii(measurement.dc_kind);
  json.Key("family");
  json.String(measurement.family);
  json.Key("requestedWeight");
  json.Number(measurement.weight);
  json.Key("selected");
  if (measurement.otm_ok) {
    json.BeginObject();
    json.Key("otmpFamilyName");
    json.String(measurement.otm_family);
    json.Key("otmpFaceName");
    json.String(measurement.otm_face);
    json.Key("otmpStyleName");
    json.String(measurement.otm_style);
    json.Key("tmWeight");
    json.Number(measurement.tm_weight);
    json.EndObject();
  } else {
    json.Null();
  }
  json.Key("os2");
  if (measurement.os2_ok) {
    json.BeginObject();
    json.Key("usWeightClass");
    json.Number(measurement.us_weight_class);
    json.Key("fsSelection");
    json.Number(measurement.fs_selection);
    json.EndObject();
  } else {
    json.Null();
  }
  json.Key("file");
  if (!measurement.file_hash.empty()) {
    json.BeginObject();
    json.Key("sha256");
    json.Ascii(measurement.file_hash);
    json.Key("size");
    json.Number(measurement.file_size);
    json.Key("collection");
    json.Bool(measurement.collection);
    json.EndObject();
  } else {
    json.Null();
  }
  json.Key("hooked");
  json.BeginObject();
  json.Key("textFace");
  json.StringOrNull(measurement.text_face);
  json.Key("logFontFace");
  if (measurement.log_ok) {
    json.String(measurement.log_face);
  } else {
    json.Null();
  }
  json.Key("logFontWeight");
  if (measurement.log_ok) {
    json.Number(measurement.log_weight);
  } else {
    json.Null();
  }
  json.EndObject();
  json.Key("render");
  json.BeginObject();
  json.Key("width");
  json.Number(kRenderWidth);
  json.Key("height");
  json.Number(kRenderHeight);
  json.Key("sha256");
  json.AsciiOrNull(measurement.pixel_hash);
  json.EndObject();
  WriteErrors(json, measurement.errors);
  json.EndObject();
}

const GdiMeasurement* FindGdi(const StateMeasurement& state,
                              const std::string_view key,
                              const std::string_view dc_kind) {
  for (const GdiMeasurement& measurement : state.gdi) {
    if (measurement.key == key && dc_kind == measurement.dc_kind) {
      return &measurement;
    }
  }
  return nullptr;
}

const DWriteFontRecord* FindDWrite(const StateMeasurement& state,
                                   const std::string_view label) {
  for (const DWriteFontRecord& record : state.dwrite_fonts) {
    if (record.label == label) {
      return &record;
    }
  }
  return nullptr;
}

struct FaceClass {
  const char* name;
  std::string hash;
};

std::vector<FaceClass> GdiFileClasses(const StateMeasurement& state,
                                      const std::string_view dc_kind) {
  const std::array<std::pair<const char*, const char*>, 4> map = {{
      {"bold", "pretendard700"},
      {"medium", "pretendardMedium400"},
      {"extrabold", "pretendardExtraBold800"},
      {"regular", "pretendard400"},
  }};
  std::vector<FaceClass> classes;
  for (const auto& [name, key] : map) {
    const GdiMeasurement* reference = FindGdi(state, key, dc_kind);
    classes.push_back(
        {name, reference == nullptr ? std::string{} : reference->file_hash});
  }
  return classes;
}

std::vector<FaceClass> DWriteGeometryClasses(const StateMeasurement& state) {
  const std::array<std::pair<const char*, const char*>, 4> map = {{
      {"bold", "pretendard700"},
      {"medium", "pretendard500"},
      {"extrabold", "pretendard800"},
      {"regular", "pretendard400"},
  }};
  std::vector<FaceClass> classes;
  for (const auto& [name, label] : map) {
    const DWriteFontRecord* reference = FindDWrite(state, label);
    classes.push_back({name, reference == nullptr || !reference->found
                                 ? std::string{}
                                 : reference->face.geometry_hash});
  }
  return classes;
}

std::string Classify(const std::string& hash,
                     const std::vector<FaceClass>& classes) {
  if (hash.empty()) {
    return "other";
  }
  for (const FaceClass& face_class : classes) {
    if (!face_class.hash.empty() && face_class.hash == hash) {
      return face_class.name;
    }
  }
  return "other";
}

void WriteClassHashes(JsonWriter& json, const std::vector<FaceClass>& classes) {
  json.BeginObject();
  for (const FaceClass& face_class : classes) {
    json.Key(face_class.name);
    json.AsciiOrNull(face_class.hash);
  }
  json.EndObject();
}

void WriteGdiVerdict(JsonWriter& json, const StateMeasurement& state,
                     const char* key) {
  const std::array<const char*, 6> pixel_references = {
      "pretendard700",          "pretendard400",
      "pretendardMedium400",    "pretendardMedium700",
      "pretendardExtraBold700", "pretendardExtraBold800"};
  json.Key(key);
  json.BeginObject();
  for (const char* dc_kind : {"memory", "window"}) {
    const GdiMeasurement* source = FindGdi(state, key, dc_kind);
    const std::vector<FaceClass> classes = GdiFileClasses(state, dc_kind);
    const bool memory = std::string_view(dc_kind) == "memory";
    if (!memory) {
      json.Key("windowDc");
      json.BeginObject();
    }
    json.Key("face");
    json.Ascii(Classify(source == nullptr ? std::string{} : source->file_hash,
                        classes));
    json.Key("synthetic");
    if (source != nullptr && source->otm_ok && source->os2_ok) {
      json.Bool(source->tm_weight >= 700 && source->us_weight_class < 700);
    } else {
      json.Null();
    }
    json.Key("tmWeight");
    if (source != nullptr && source->otm_ok) {
      json.Number(source->tm_weight);
    } else {
      json.Null();
    }
    json.Key("usWeightClass");
    if (source != nullptr && source->os2_ok) {
      json.Number(source->us_weight_class);
    } else {
      json.Null();
    }
    json.Key("selectedFace");
    if (source != nullptr && source->otm_ok) {
      json.String(source->otm_face);
    } else {
      json.Null();
    }
    std::string match;
    json.Key("pixelsEqual");
    json.BeginObject();
    for (const char* reference_key : pixel_references) {
      const GdiMeasurement* reference = FindGdi(state, reference_key, dc_kind);
      const bool equal = source != nullptr && reference != nullptr &&
                         !source->pixel_hash.empty() &&
                         source->pixel_hash == reference->pixel_hash;
      if (equal && match.empty()) {
        match = reference_key;
      }
      json.Key(reference_key);
      json.Bool(equal);
    }
    json.EndObject();
    json.Key("pixelsMatch");
    json.AsciiOrNull(match);
    if (!memory) {
      json.EndObject();
    }
  }
  json.EndObject();
}

void WriteDWriteCollectionVerdict(JsonWriter& json, const StateMeasurement& state,
                                  const char* verdict_key, const char* label) {
  const DWriteFontRecord* record = FindDWrite(state, label);
  json.Key(verdict_key);
  json.BeginObject();
  const bool found = record != nullptr && record->found;
  json.Key("backing");
  json.Ascii(Classify(found ? record->face.geometry_hash : std::string{},
                      DWriteGeometryClasses(state)));
  json.Key("simulations");
  if (found) {
    json.Ascii(SimulationsText(record->simulations));
  } else {
    json.Null();
  }
  json.Key("faceSimulations");
  if (found) {
    json.Ascii(SimulationsText(record->face.simulations));
  } else {
    json.Null();
  }
  json.Key("weight");
  if (found) {
    json.Number(record->weight);
  } else {
    json.Null();
  }
  json.EndObject();
}

// Every candidate family covers Latin, so the run holding the first Latin
// letter shows the requested font; earlier runs may be script fallback.
std::size_t VerdictRunIndex(const TextFormatRecord& record,
                            const std::wstring& text) {
  std::size_t letter = text.size();
  for (std::size_t index = 0; index < text.size(); ++index) {
    const wchar_t value = text[index];
    if ((value >= L'A' && value <= L'Z') || (value >= L'a' && value <= L'z')) {
      letter = index;
      break;
    }
  }
  for (std::size_t index = 0; index < record.runs.size(); ++index) {
    const GlyphRunRecord& run = record.runs[index];
    if (letter >= run.text_position &&
        letter < static_cast<std::size_t>(run.text_position) + run.text_length) {
      return index;
    }
  }
  return 0;
}

void WriteTextFormatVerdict(JsonWriter& json, const StateMeasurement& state,
                            const char* verdict_key, const UINT32 weight,
                            const std::wstring& text) {
  const TextFormatRecord* record = nullptr;
  for (const TextFormatRecord& candidate : state.text_formats) {
    if (candidate.requested_weight == weight) {
      record = &candidate;
    }
  }
  const std::vector<FaceClass> classes = DWriteGeometryClasses(state);
  json.Key(verdict_key);
  json.BeginObject();
  const bool has_run = record != nullptr && !record->runs.empty();
  const std::size_t verdict_run =
      has_run ? VerdictRunIndex(*record, text) : 0;
  json.Key("verdictRun");
  if (has_run) {
    json.Number(static_cast<long long>(verdict_run));
  } else {
    json.Null();
  }
  json.Key("backing");
  json.Ascii(Classify(has_run ? record->runs[verdict_run].face.geometry_hash
                              : std::string{},
                      classes));
  json.Key("simulations");
  if (has_run) {
    json.Ascii(SimulationsText(record->runs[verdict_run].face.simulations));
  } else {
    json.Null();
  }
  json.Key("runCount");
  json.Number(record == nullptr ? 0 : static_cast<long long>(record->runs.size()));
  json.Key("runBackings");
  json.BeginArray();
  if (record != nullptr) {
    for (const GlyphRunRecord& run : record->runs) {
      json.Ascii(Classify(run.face.geometry_hash, classes) + "/" +
                 SimulationsText(run.face.simulations));
    }
  }
  json.EndArray();
  json.EndObject();
}

void WriteState(JsonWriter& json, const StateMeasurement& state,
                const std::wstring& text) {
  json.Key(state.name);
  json.BeginObject();
  json.Key("gdi");
  json.BeginArray();
  for (const GdiMeasurement& measurement : state.gdi) {
    WriteGdiMeasurement(json, measurement);
  }
  json.EndArray();

  json.Key("directWrite");
  json.BeginObject();
  json.Key("available");
  json.Bool(state.dwrite_available);
  WriteErrors(json, state.dwrite_errors);
  json.Key("collection");
  json.BeginArray();
  for (const DWriteFontRecord& record : state.dwrite_fonts) {
    json.BeginObject();
    json.Key("label");
    json.Ascii(record.label);
    json.Key("family");
    json.String(record.family);
    json.Key("requestedWeight");
    json.Number(record.requested_weight);
    json.Key("found");
    json.Bool(record.found);
    json.Key("weight");
    if (record.found) {
      json.Number(record.weight);
    } else {
      json.Null();
    }
    json.Key("simulations");
    json.Ascii(SimulationsText(record.simulations));
    json.Key("faceName");
    json.StringOrNull(record.face_name);
    json.Key("familyNames");
    json.BeginArray();
    for (const auto& [locale, name] : record.family_names) {
      json.BeginObject();
      json.Key("locale");
      json.String(locale);
      json.Key("name");
      json.String(name);
      json.EndObject();
    }
    json.EndArray();
    json.Key("win32FamilyName");
    json.StringOrNull(record.win32_family);
    json.Key("fullName");
    json.StringOrNull(record.full_name);
    json.Key("postScriptName");
    json.StringOrNull(record.postscript_name);
    json.Key("face");
    if (record.found) {
      WriteFace(json, record.face);
    } else {
      json.Null();
    }
    WriteErrors(json, record.errors);
    json.EndObject();
  }
  json.EndArray();
  json.Key("textFormat");
  json.BeginArray();
  for (const TextFormatRecord& record : state.text_formats) {
    json.BeginObject();
    json.Key("requestedWeight");
    json.Number(record.requested_weight);
    json.Key("ok");
    json.Bool(record.ok);
    json.Key("runs");
    json.BeginArray();
    for (const GlyphRunRecord& run : record.runs) {
      json.BeginObject();
      json.Key("textPosition");
      json.Number(run.text_position);
      json.Key("textLength");
      json.Number(run.text_length);
      json.Key("glyphCount");
      json.Number(run.glyph_count);
      json.Key("emSize");
      json.Real(run.em_size);
      json.Key("face");
      WriteFace(json, run.face);
      json.EndObject();
    }
    json.EndArray();
    WriteErrors(json, record.errors);
    json.EndObject();
  }
  json.EndArray();
  json.Key("fallback");
  json.BeginArray();
  for (const FallbackRecord& fallback : state.fallbacks) {
    json.BeginObject();
    json.Key("primaryFamily");
    json.String(fallback.primary_family);
    json.Key("text");
    json.String(fallback.sample_text);
    json.Key("ok");
    json.Bool(fallback.ok);
    json.Key("textPosition");
    json.Number(fallback.text_position);
    json.Key("textLength");
    json.Number(fallback.text_length);
    json.Key("mappedLength");
    json.Number(fallback.mapped_length);
    json.Key("scale");
    json.Real(fallback.scale);
    json.Key("hasFont");
    json.Bool(fallback.has_font);
    json.Key("weight");
    if (fallback.has_font) {
      json.Number(fallback.weight);
    } else {
      json.Null();
    }
    json.Key("simulations");
    json.Ascii(SimulationsText(fallback.simulations));
    json.Key("familyNames");
    json.BeginArray();
    for (const auto& [locale, name] : fallback.family_names) {
      json.BeginObject();
      json.Key("locale");
      json.String(locale);
      json.Key("name");
      json.String(name);
      json.EndObject();
    }
    json.EndArray();
    json.Key("win32FamilyName");
    json.StringOrNull(fallback.win32_family);
    json.Key("face");
    if (fallback.has_font) {
      WriteFace(json, fallback.face);
    } else {
      json.Null();
    }
    json.Key("layoutRuns");
    json.BeginArray();
    for (const GlyphRunRecord& run : fallback.layout.runs) {
      json.BeginObject();
      json.Key("textPosition");
      json.Number(run.text_position);
      json.Key("textLength");
      json.Number(run.text_length);
      json.Key("glyphCount");
      json.Number(run.glyph_count);
      json.Key("face");
      WriteFace(json, run.face);
      json.EndObject();
    }
    json.EndArray();
    WriteErrors(json, fallback.errors);
    json.EndObject();
  }
  json.EndArray();
  json.EndObject();

  json.Key("verdicts");
  json.BeginObject();
  json.Key("referenceFiles");
  json.BeginObject();
  json.Key("gdi");
  WriteClassHashes(json, GdiFileClasses(state, "memory"));
  json.Key("gdiWindowDc");
  WriteClassHashes(json, GdiFileClasses(state, "window"));
  json.Key("dwriteGeometry");
  WriteClassHashes(json, DWriteGeometryClasses(state));
  json.EndObject();
  json.Key("gdi");
  json.BeginObject();
  WriteGdiVerdict(json, state, "source400");
  WriteGdiVerdict(json, state, "source600");
  WriteGdiVerdict(json, state, "source700");
  json.EndObject();
  json.Key("dwrite");
  json.BeginObject();
  json.Key("available");
  json.Bool(state.dwrite_available);
  WriteDWriteCollectionVerdict(json, state, "collection400", "source400");
  WriteDWriteCollectionVerdict(json, state, "collection700", "source700");
  WriteTextFormatVerdict(json, state, "textFormat400",
                         DWRITE_FONT_WEIGHT_NORMAL, text);
  WriteTextFormatVerdict(json, state, "textFormat700", DWRITE_FONT_WEIGHT_BOLD,
                         text);
  json.EndObject();
  json.EndObject();
  json.EndObject();
}

std::wstring FullPath(const std::wstring& path) {
  const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (required == 0) {
    return path;
  }
  std::vector<wchar_t> buffer(required, L'\0');
  const DWORD written =
      GetFullPathNameW(path.c_str(), required, buffer.data(), nullptr);
  if (written == 0 || written >= required) {
    return path;
  }
  return std::wstring(buffer.data(), written);
}

void MergeModules(std::vector<mactype::service_probe::internal::ModuleObservation>&
                      modules) {
  for (auto& module : mactype::service_probe::internal::FindMacTypeModules()) {
    bool known = false;
    for (const auto& existing : modules) {
      if (_wcsicmp(existing.path.c_str(), module.path.c_str()) == 0) {
        known = true;
        break;
      }
    }
    if (!known) {
      module.first_observed_at = mactype::service_probe::UtcNow();
      modules.push_back(std::move(module));
    }
  }
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
  Arguments arguments;
  std::wstring error;
  if (!ParseArguments(argc, argv, arguments, error)) {
    std::wcerr << error << L'\n';
    PrintUsage();
    return 2;
  }

  const std::string started_at = mactype::service_probe::UtcNow();
  const DWORD pid = GetCurrentProcessId();
  std::wstring core_path;
  std::wstring diagnostics_namespace;
  bool core_loaded = false;
  std::wstring load_error;
  std::string readiness;
  unsigned long long readiness_ms = 0;
  if (!arguments.core.empty()) {
    core_path = FullPath(arguments.core);
    diagnostics_namespace =
        L"boldfield-" + std::to_wstring(pid) + L"-" +
        std::to_wstring(GetTickCount64());
    SetEnvironmentVariableW(L"MACTYPE_DIRECTWRITE_DIAGNOSTICS",
                            diagnostics_namespace.c_str());
    if (LoadLibraryW(core_path.c_str()) == nullptr) {
      load_error = LastErrorText(L"LoadLibraryW(core)");
    } else {
      core_loaded = true;
    }
    const std::wstring event_name = L"Local\\MacType." + diagnostics_namespace +
                                    L".pid-" + std::to_wstring(pid) +
                                    L".hook-ready";
    const ULONGLONG start = GetTickCount64();
    readiness = "timeout-slept-wait-ms";
    for (;;) {
      const HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, event_name.c_str());
      if (event != nullptr) {
        const DWORD remaining = static_cast<DWORD>(
            arguments.wait_ms > GetTickCount64() - start
                ? arguments.wait_ms - (GetTickCount64() - start)
                : 0);
        const DWORD wait = WaitForSingleObject(event, remaining);
        CloseHandle(event);
        if (wait == WAIT_OBJECT_0) {
          readiness = "hook-ready-event";
        }
        break;
      }
      if (GetTickCount64() - start >= arguments.wait_ms) {
        break;
      }
      Sleep(25);
    }
    readiness_ms = GetTickCount64() - start;
  }

  std::vector<mactype::service_probe::internal::ModuleObservation> modules;
  MergeModules(modules);

  HiddenWindow window;
  std::vector<StateMeasurement> states;
  if (arguments.core.empty()) {
    states.push_back(MeasureState("stock", arguments, window.Handle()));
  } else {
    states.push_back(MeasureState("active", arguments, window.Handle()));
    SetEnvironmentVariableW(L"MACTYPE_FONTSUBSTITUTES_ENV", L"1");
    states.push_back(MeasureState("disabled", arguments, window.Handle()));
  }
  MergeModules(modules);

  JsonWriter json;
  json.BeginObject();
  json.Key("schemaVersion");
  json.Number(1);
  json.Key("kind");
  json.Ascii("mactype-bold-substitution-probe");
  json.Key("architecture");
  json.Ascii(mactype::service_probe::CurrentArchitecture());
  json.Key("pid");
  json.Number(pid);
  json.Key("sessionId");
  DWORD session_id = 0;
  if (ProcessIdToSessionId(pid, &session_id) != FALSE) {
    json.Number(session_id);
  } else {
    json.Null();
  }
  json.Key("startedAt");
  json.Ascii(started_at);
  json.Key("observedAt");
  json.Ascii(mactype::service_probe::UtcNow());
  json.Key("arguments");
  json.BeginObject();
  json.Key("source");
  json.String(arguments.source);
  json.Key("replacement");
  json.String(arguments.replacement);
  json.Key("pair");
  json.String(arguments.pair);
  json.Key("text");
  json.String(arguments.text);
  json.Key("core");
  json.StringOrNull(core_path);
  json.Key("waitMs");
  json.Number(arguments.wait_ms);
  json.EndObject();
  json.Key("core");
  if (core_path.empty()) {
    json.Null();
  } else {
    json.BeginObject();
    json.Key("path");
    json.String(core_path);
    json.Key("loaded");
    json.Bool(core_loaded);
    json.Key("loadError");
    json.StringOrNull(load_error);
    json.Key("diagnosticsNamespace");
    json.String(diagnostics_namespace);
    json.Key("readiness");
    json.Ascii(readiness);
    json.Key("readinessMs");
    json.Number(static_cast<long long>(readiness_ms));
    json.EndObject();
  }
  json.Key("hiddenWindowError");
  json.StringOrNull(window.Error());
  json.Key("modules");
  json.BeginArray();
  for (const auto& module : modules) {
    json.BeginObject();
    json.Key("name");
    json.String(module.name);
    json.Key("path");
    json.String(module.path);
    json.Key("version");
    json.StringOrNull(module.version);
    json.Key("firstObservedAt");
    json.Ascii(module.first_observed_at);
    json.EndObject();
  }
  json.EndArray();
  json.Key("isolated");
  if (core_path.empty()) {
    json.Null();
  } else {
    json.Bool(modules.size() == 1 &&
              _wcsicmp(FullPath(modules.front().path).c_str(),
                       core_path.c_str()) == 0);
  }
  json.Key("stateOrder");
  json.BeginArray();
  for (const StateMeasurement& state : states) {
    json.Ascii(state.name);
  }
  json.EndArray();
  json.Key("states");
  json.BeginObject();
  for (const StateMeasurement& state : states) {
    WriteState(json, state, arguments.text);
  }
  json.EndObject();
  json.EndObject();

  if (!mactype::service_probe::WriteUtf8Json(arguments.out, json.Text(),
                                             error)) {
    std::wcerr << error << L'\n';
    return 1;
  }
  return 0;
}
