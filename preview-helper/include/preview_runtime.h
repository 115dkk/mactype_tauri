#pragma once

#include "legacy_control_center.h"
#include "protocol.h"

#include <Windows.h>

#include <cstdint>
#include <string>

namespace mactype {

class PreviewRuntime {
 public:
  explicit PreviewRuntime(std::wstring install_root);
  ~PreviewRuntime();
  PreviewRuntime(const PreviewRuntime&) = delete;
  PreviewRuntime& operator=(const PreviewRuntime&) = delete;

  bool initialize(std::string& error);
  mtpc::Frame render(const mtpc::Frame& request);
  mtpc::Frame load_profile(const mtpc::Frame& request);
  mtpc::Frame show_native_preview(const mtpc::Frame& request, bool visible);
  std::string hello_json() const;
  void pump_messages();

 private:
  static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  bool create_windows(std::string& error);
  bool apply_request(const std::string& json, std::string& error);
  std::vector<std::uint8_t> render_png(const std::string& json, std::uint32_t& width,
                                      std::uint32_t& height, std::uint32_t& dpi, std::string& error);
  void paint_native(HWND window);

  std::wstring install_root_;
  std::wstring dll_path_;
  HMODULE module_{};
  IControlCenter* control_center_{};
  HWND hidden_window_{};
  HWND native_window_{};
  std::wstring sample_text_{L"MacType preview 123 ABC\n가나다라마바사 아자차카타파하"};
  std::wstring font_face_{L"Segoe UI"};
  float font_size_pt_{14.0F};
  COLORREF foreground_{RGB(24, 29, 35)};
  COLORREF background_{RGB(238, 241, 244)};
  std::uint32_t dpi_{96};
  std::uint32_t core_version_{};
  bool has_dll_get_version_{};
  bool com_initialized_{};
};

}  // namespace mactype
