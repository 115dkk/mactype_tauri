#include "preview_runtime.h"

#include "generated_settings.h"

#include <Shlwapi.h>
#include <Wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <vector>

namespace mactype {
namespace {

constexpr wchar_t kWindowClass[] = L"MacTypePreview32Window";

std::wstring full_path(const std::wstring& path) {
  const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (required == 0) return {};
  std::wstring result(required, L'\0');
  const DWORD written = GetFullPathNameW(path.c_str(), required, result.data(), nullptr);
  if (written == 0 || written >= required) return {};
  result.resize(written);
  return result;
}

bool regular_file(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool x86_image(const std::wstring& path) {
  std::ifstream input(path, std::ios::binary);
  IMAGE_DOS_HEADER dos{};
  input.read(reinterpret_cast<char*>(&dos), sizeof(dos));
  if (!input || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) return false;
  input.seekg(dos.e_lfanew, std::ios::beg);
  DWORD signature{};
  IMAGE_FILE_HEADER header{};
  input.read(reinterpret_cast<char*>(&signature), sizeof(signature));
  input.read(reinterpret_cast<char*>(&header), sizeof(header));
  return input && signature == IMAGE_NT_SIGNATURE && header.Machine == IMAGE_FILE_MACHINE_I386;
}

std::wstring utf8_to_wide(const std::string& value) {
  if (value.empty()) return {};
  const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
  if (required <= 0) return {};
  std::wstring result(required, L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                          result.data(), required) != required) {
    return {};
  }
  return result;
}

std::optional<std::size_t> json_value_start(const std::string& json, const std::string& key) {
  const std::string needle = '"' + key + '"';
  std::size_t position = json.find(needle);
  if (position == std::string::npos) return std::nullopt;
  position = json.find(':', position + needle.size());
  if (position == std::string::npos) return std::nullopt;
  do {
    ++position;
  } while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position])) != 0);
  return position < json.size() ? std::optional(position) : std::nullopt;
}

std::optional<std::string> json_string(const std::string& json, const std::string& key) {
  const auto start = json_value_start(json, key);
  if (!start || json[*start] != '"') return std::nullopt;
  std::string result;
  for (std::size_t index = *start + 1; index < json.size(); ++index) {
    const char character = json[index];
    if (character == '"') return result;
    if (character != '\\') {
      result.push_back(character);
      continue;
    }
    if (++index >= json.size()) return std::nullopt;
    switch (json[index]) {
      case '"': result.push_back('"'); break;
      case '\\': result.push_back('\\'); break;
      case '/': result.push_back('/'); break;
      case 'b': result.push_back('\b'); break;
      case 'f': result.push_back('\f'); break;
      case 'n': result.push_back('\n'); break;
      case 'r': result.push_back('\r'); break;
      case 't': result.push_back('\t'); break;
      default: return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<double> json_number(const std::string& json, const std::string& key) {
  const auto start = json_value_start(json, key);
  if (!start) return std::nullopt;
  char* end{};
  const double value = std::strtod(json.c_str() + *start, &end);
  if (end == json.c_str() + *start || !std::isfinite(value)) return std::nullopt;
  return value;
}

std::optional<bool> json_bool(const std::string& json, const std::string& key) {
  const auto start = json_value_start(json, key);
  if (!start) return std::nullopt;
  if (json.compare(*start, 4, "true") == 0) return true;
  if (json.compare(*start, 5, "false") == 0) return false;
  return std::nullopt;
}

COLORREF parse_color(const std::string& value, COLORREF fallback) {
  if (value.size() != 7 || value[0] != '#') return fallback;
  unsigned int color{};
  std::istringstream input(value.substr(1));
  input >> std::hex >> color;
  if (!input || !input.eof()) return fallback;
  return RGB((color >> 16U) & 0xFFU, (color >> 8U) & 0xFFU, color & 0xFFU);
}

mtpc::Frame error_frame(std::uint64_t request_id, const char* code, const std::string& message) {
  mtpc::Frame response;
  response.kind = mtpc::MessageKind::error;
  response.request_id = request_id;
  std::string safe = message;
  std::replace(safe.begin(), safe.end(), '"', '\'');
  response.json = std::string{"{\"code\":\""} + code + "\",\"message\":\"" + safe +
                  "\",\"recoverable\":true}";
  return response;
}

void draw_sample(HDC dc, const RECT& area, const std::wstring& text, const std::wstring& face,
                 float point_size, std::uint32_t dpi, COLORREF foreground, COLORREF background,
                 bool bold, bool italic) {
  HBRUSH brush = CreateSolidBrush(background);
  FillRect(dc, &area, brush);
  DeleteObject(brush);
  const int height = -MulDiv(static_cast<int>(std::lround(point_size * 100.0F)),
                             static_cast<int>(dpi), 7200);
  HFONT font = CreateFontW(height, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, italic ? TRUE : FALSE,
                           FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face.c_str());
  HGDIOBJ previous_font = SelectObject(dc, font);
  SetTextColor(dc, foreground);
  SetBkMode(dc, TRANSPARENT);
  int y = area.top + std::max(8, height < 0 ? -height / 2 : 8);
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find(L'\n', start);
    const std::size_t length = (end == std::wstring::npos ? text.size() : end) - start;
    ExtTextOutW(dc, area.left + 18, y, ETO_CLIPPED, &area, text.data() + start,
                static_cast<UINT>(length), nullptr);
    y += std::max(22, std::abs(height) * 3 / 2);
    if (end == std::wstring::npos) break;
    start = end + 1;
  }
  SelectObject(dc, previous_font);
  DeleteObject(font);
}

std::vector<std::uint8_t> encode_png(const std::uint8_t* pixels, std::uint32_t width,
                                     std::uint32_t height, std::string& error) {
  IWICImagingFactory* factory{};
  IWICBitmapEncoder* encoder{};
  IWICBitmapFrameEncode* frame{};
  IPropertyBag2* properties{};
  IStream* stream{};
  std::vector<std::uint8_t> result;
  HRESULT status = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory));
  if (SUCCEEDED(status)) status = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
  if (SUCCEEDED(status)) status = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  if (SUCCEEDED(status)) status = encoder->Initialize(stream, WICBitmapEncoderNoCache);
  if (SUCCEEDED(status)) status = encoder->CreateNewFrame(&frame, &properties);
  if (SUCCEEDED(status)) status = frame->Initialize(properties);
  if (SUCCEEDED(status)) status = frame->SetSize(width, height);
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (SUCCEEDED(status)) status = frame->SetPixelFormat(&format);
  if (SUCCEEDED(status) && format != GUID_WICPixelFormat32bppBGRA) status = E_FAIL;
  const UINT stride = width * 4U;
  if (SUCCEEDED(status)) status = frame->WritePixels(height, stride, stride * height,
                                                     const_cast<BYTE*>(pixels));
  if (SUCCEEDED(status)) status = frame->Commit();
  if (SUCCEEDED(status)) status = encoder->Commit();
  if (SUCCEEDED(status)) {
    HGLOBAL memory{};
    status = GetHGlobalFromStream(stream, &memory);
    if (SUCCEEDED(status)) {
      const SIZE_T size = GlobalSize(memory);
      const void* data = GlobalLock(memory);
      if (data != nullptr) {
        const auto* begin = static_cast<const std::uint8_t*>(data);
        result.assign(begin, begin + size);
        GlobalUnlock(memory);
      } else {
        status = E_FAIL;
      }
    }
  }
  if (properties) properties->Release();
  if (frame) frame->Release();
  if (encoder) encoder->Release();
  if (stream) stream->Release();
  if (factory) factory->Release();
  if (FAILED(status)) {
    std::ostringstream message;
    message << "WIC PNG encoding failed: 0x" << std::hex << static_cast<unsigned long>(status);
    error = message.str();
    result.clear();
  }
  return result;
}

}  // namespace

PreviewRuntime::PreviewRuntime(std::wstring install_root)
    : install_root_(full_path(install_root)), dll_path_(install_root_ + L"\\MacType.dll") {}

PreviewRuntime::~PreviewRuntime() {
  if (control_center_) {
    control_center_->DestroyMessageWnd();
    control_center_->Release();
  }
  if (native_window_) DestroyWindow(native_window_);
  if (hidden_window_) DestroyWindow(hidden_window_);
  if (module_) FreeLibrary(module_);
  if (com_initialized_) CoUninitialize();
}

bool PreviewRuntime::initialize(std::string& error) {
  if (install_root_.empty() || !regular_file(dll_path_)) {
    error = "MacType.dll was not found in the selected installation root";
    return false;
  }
  if (!regular_file(install_root_ + L"\\MacType.ini")) {
    error = "MacType.ini is missing from the selected installation root";
    return false;
  }
  if (!x86_image(dll_path_)) {
    error = "MacType.dll is not an x86 PE image";
    return false;
  }
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    error = "COM initialization failed";
    return false;
  }
  com_initialized_ = true;
  SetEnvironmentVariableW(L"MACTYPE_FORCE_LOAD", L"1");
  module_ = LoadLibraryExW(dll_path_.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!module_) {
    error = "LoadLibraryW failed for MacType.dll";
    return false;
  }
  const auto create = reinterpret_cast<CreateControlCenter>(GetProcAddress(module_, "CreateControlCenter"));
  const auto version = reinterpret_cast<DllGetVersion>(GetProcAddress(module_, "DllGetVersion"));
  if (!create) {
    error = "MacType.dll does not export CreateControlCenter";
    return false;
  }
  has_dll_get_version_ = version != nullptr;
  if (version) {
    DLLVERSIONINFO version_info{sizeof(DLLVERSIONINFO)};
    if (FAILED(version(&version_info))) {
      error = "DllGetVersion export returned an error";
      return false;
    }
  }
  create(&control_center_);
  if (!control_center_) {
    error = "CreateControlCenter returned no interface";
    return false;
  }
  core_version_ = control_center_->GetVersion();
  control_center_->EnableCache(FALSE);
  control_center_->EnableRender(TRUE);
  control_center_->CreateMessageWnd();
  return create_windows(error);
}

bool PreviewRuntime::create_windows(std::string& error) {
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kWindowClass;
  RegisterClassW(&window_class);
  hidden_window_ = CreateWindowExW(0, kWindowClass, L"", WS_OVERLAPPED, 0, 0, 1, 1, nullptr,
                                   nullptr, window_class.hInstance, this);
  native_window_ = CreateWindowExW(0, kWindowClass, L"MacType Preview", WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 900, 360, nullptr, nullptr,
                                   window_class.hInstance, this);
  if (!hidden_window_ || !native_window_) {
    error = "failed to create preview windows";
    return false;
  }
  return true;
}

std::string PreviewRuntime::hello_json() const {
  return std::string{"{\"protocolVersion\":1,\"renderer\":\"mactype-gdi\",\"loadsMacType\":true,\"coreVersion\":"} +
         std::to_string(core_version_) + ",\"dllGetVersion\":" +
         (has_dll_get_version_ ? "true" : "false") + "}";
}

bool PreviewRuntime::apply_request(const std::string& json, std::string& error) {
  if (const auto profile = json_string(json, "profilePath"); profile && !profile->empty()) {
    const std::wstring profile_path = full_path(utf8_to_wide(*profile));
    if (profile_path.empty() || !regular_file(profile_path) ||
        _wcsicmp(PathFindExtensionW(profile_path.c_str()), L".ini") != 0) {
      error = "profilePath is not an existing INI file";
      return false;
    }
    control_center_->LoadSetting(profile_path.c_str());
  }
  for (const auto& setting : kSettings) {
    const auto value = json_number(json, setting.id);
    if (!value) continue;
    const BOOL applied = setting.is_float
                             ? control_center_->SetFloatAttribute(setting.ordinal, static_cast<float>(*value))
                             : control_center_->SetIntAttribute(setting.ordinal, static_cast<int>(*value));
    if (!applied) {
      error = std::string{"MacType rejected setting "} + setting.id;
      return false;
    }
  }
  control_center_->RefreshSetting();
  if (const auto value = json_string(json, "text")) sample_text_ = utf8_to_wide(*value);
  if (const auto value = json_string(json, "fontFace")) font_face_ = utf8_to_wide(*value);
  if (const auto value = json_number(json, "fontSizePt")) font_size_pt_ = static_cast<float>(*value);
  if (const auto value = json_number(json, "dpi")) dpi_ = static_cast<std::uint32_t>(*value);
  if (const auto value = json_string(json, "foreground")) foreground_ = parse_color(*value, foreground_);
  if (const auto value = json_string(json, "background")) background_ = parse_color(*value, background_);
  if (const auto value = json_bool(json, "bold")) sample_bold_ = *value;
  if (const auto value = json_bool(json, "italic")) sample_italic_ = *value;
  return true;
}

std::vector<std::uint8_t> PreviewRuntime::render_png(const std::string& json, std::uint32_t& width,
                                                     std::uint32_t& height, std::uint32_t& dpi,
                                                     std::string& error) {
  width = static_cast<std::uint32_t>(json_number(json, "widthPx").value_or(1000));
  height = static_cast<std::uint32_t>(json_number(json, "heightPx").value_or(280));
  dpi = static_cast<std::uint32_t>(json_number(json, "dpi").value_or(dpi_));
  if (width < 64 || width > 4096 || height < 64 || height > 2048 || dpi < 72 || dpi > 768) {
    error = "preview dimensions or DPI are outside the supported range";
    return {};
  }
  HDC screen = GetDC(hidden_window_);
  HDC memory = CreateCompatibleDC(screen);
  BITMAPINFO bitmap{};
  bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bitmap.bmiHeader.biWidth = static_cast<LONG>(width);
  bitmap.bmiHeader.biHeight = -static_cast<LONG>(height);
  bitmap.bmiHeader.biPlanes = 1;
  bitmap.bmiHeader.biBitCount = 32;
  bitmap.bmiHeader.biCompression = BI_RGB;
  void* bits{};
  HBITMAP dib = CreateDIBSection(screen, &bitmap, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(hidden_window_, screen);
  if (!memory || !dib || !bits) {
    if (dib) DeleteObject(dib);
    if (memory) DeleteDC(memory);
    error = "CreateDIBSection failed";
    return {};
  }
  HGDIOBJ previous = SelectObject(memory, dib);
  const RECT area{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
  draw_sample(memory, area, sample_text_, font_face_, font_size_pt_, dpi, foreground_, background_,
              sample_bold_, sample_italic_);
  auto* pixels = static_cast<std::uint8_t*>(bits);
  for (std::size_t index = 3; index < static_cast<std::size_t>(width) * height * 4U; index += 4) {
    pixels[index] = 0xFF;
  }
  auto png = encode_png(pixels, width, height, error);
  SelectObject(memory, previous);
  DeleteObject(dib);
  DeleteDC(memory);
  return png;
}

mtpc::Frame PreviewRuntime::render(const mtpc::Frame& request) {
  const auto started = std::chrono::steady_clock::now();
  std::string error;
  if (!apply_request(request.json, error)) return error_frame(request.request_id, "invalid_request", error);
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t dpi{};
  auto png = render_png(request.json, width, height, dpi, error);
  if (png.empty()) return error_frame(request.request_id, "render_failed", error);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  mtpc::Frame response;
  response.kind = mtpc::MessageKind::preview_rendered;
  response.request_id = request.request_id;
  response.binary = std::move(png);
  response.json = std::string{"{\"width\":"} + std::to_string(width) + ",\"height\":" +
                  std::to_string(height) + ",\"dpi\":" + std::to_string(dpi) +
                  ",\"elapsedMs\":" + std::to_string(elapsed.count()) +
                  ",\"coreVersion\":" + std::to_string(core_version_) + "}";
  InvalidateRect(native_window_, nullptr, FALSE);
  return response;
}

mtpc::Frame PreviewRuntime::load_profile(const mtpc::Frame& request) {
  std::string error;
  if (!apply_request(request.json, error)) return error_frame(request.request_id, "load_failed", error);
  mtpc::Frame response;
  response.kind = mtpc::MessageKind::ack;
  response.request_id = request.request_id;
  response.json = R"({"loaded":true})";
  return response;
}

mtpc::Frame PreviewRuntime::show_native_preview(const mtpc::Frame& request, bool visible) {
  if (visible) {
    ShowWindow(native_window_, SW_SHOWNORMAL);
    UpdateWindow(native_window_);
  } else {
    ShowWindow(native_window_, SW_HIDE);
  }
  mtpc::Frame response;
  response.kind = mtpc::MessageKind::native_preview_state;
  response.request_id = request.request_id;
  response.json = visible ? R"({"visible":true})" : R"({"visible":false})";
  return response;
}

void PreviewRuntime::paint_native(HWND window) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(window, &paint);
  RECT area{};
  GetClientRect(window, &area);
  draw_sample(dc, area, sample_text_, font_face_, font_size_pt_, GetDpiForWindow(window), foreground_,
              background_, sample_bold_, sample_italic_);
  EndPaint(window, &paint);
}

LRESULT CALLBACK PreviewRuntime::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* runtime = reinterpret_cast<PreviewRuntime*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (runtime && message == WM_PAINT) {
    runtime->paint_native(window);
    return 0;
  }
  if (message == WM_CLOSE) {
    ShowWindow(window, SW_HIDE);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void PreviewRuntime::pump_messages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

}  // namespace mactype
