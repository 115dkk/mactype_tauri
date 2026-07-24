#include "preview_runtime.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
  if (argc != 2) return 1;
  mactype::PreviewRuntime runtime(std::filesystem::path(argv[1]).wstring());
  std::string error;
  if (!runtime.initialize(error)) {
    std::cerr << error << '\n';
    return 2;
  }
  if (runtime.hello_json().find("\"loadsMacType\":true") == std::string::npos) return 3;

  mtpc::Frame request;
  request.kind = mtpc::MessageKind::render_preview;
  request.request_id = 42;
  request.json = R"({"overrides":{"normal_weight":4,"gamma_value":1.2},"sample":{"text":"MacType preview 123 ABC","fontFace":"Segoe UI","fontSizePt":14,"widthPx":640,"heightPx":180,"dpi":96,"foreground":"#181D23","background":"#EEF1F4"}})";
  const mtpc::Frame response = runtime.render(request);
  if (response.kind != mtpc::MessageKind::preview_rendered || response.request_id != 42) return 4;
  constexpr std::array<std::uint8_t, 8> signature{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (response.binary.size() <= signature.size() ||
      !std::equal(signature.begin(), signature.end(), response.binary.begin())) {
    return 5;
  }

  mtpc::Frame styled;
  styled.kind = mtpc::MessageKind::render_preview;
  styled.request_id = 43;
  styled.json = R"({"overrides":{},"sample":{"text":"MacType preview 123 ABC","fontFace":"Segoe UI","fontSizePt":14,"widthPx":640,"heightPx":96,"dpi":96,"foreground":"#181D23","background":"#EEF1F4","bold":true,"italic":true}})";
  const mtpc::Frame styledResponse = runtime.render(styled);
  if (styledResponse.kind != mtpc::MessageKind::preview_rendered || styledResponse.request_id != 43) return 8;
  if (styledResponse.binary.size() <= signature.size() ||
      !std::equal(signature.begin(), signature.end(), styledResponse.binary.begin())) {
    return 9;
  }

  if (runtime.show_native_preview(request, true).kind != mtpc::MessageKind::native_preview_state) return 6;
  runtime.pump_messages();
  if (runtime.show_native_preview(request, false).kind != mtpc::MessageKind::native_preview_state) return 7;
  return 0;
}
