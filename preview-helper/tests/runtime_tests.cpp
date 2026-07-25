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

  mtpc::Frame listing;
  listing.kind = mtpc::MessageKind::show_native_preview;
  listing.request_id = 44;
  listing.json = R"({"displayMode":"listing","listingText":"The quick brown fox jumps over the lazy dog."})";
  const mtpc::Frame listingResponse = runtime.show_native_preview(listing, true);
  if (listingResponse.kind != mtpc::MessageKind::native_preview_state) return 10;
  if (listingResponse.json.find("\"visible\":true") == std::string::npos) return 11;
  if (listingResponse.json.find("\"displayMode\":\"listing\"") == std::string::npos) return 12;
  runtime.pump_messages();

  mtpc::Frame plain;
  plain.kind = mtpc::MessageKind::show_native_preview;
  plain.request_id = 45;
  plain.json = R"({"displayMode":"default"})";
  if (runtime.show_native_preview(plain, true).json.find("\"displayMode\":\"default\"") ==
      std::string::npos) {
    return 13;
  }
  runtime.pump_messages();
  if (runtime.show_native_preview(plain, false).kind != mtpc::MessageKind::native_preview_state) return 14;

  /* The native window keeps its own colours: the show request sets them, and
     an omitted field leaves the previous choice in place. */
  mtpc::Frame dark;
  dark.kind = mtpc::MessageKind::show_native_preview;
  dark.request_id = 46;
  dark.json = R"({"displayMode":"default","foreground":"#F1F3F5","background":"#171A1F"})";
  if (runtime.show_native_preview(dark, true).json.find("\"background\":\"#171A1F\"") ==
      std::string::npos) {
    return 15;
  }
  runtime.pump_messages();
  if (runtime.show_native_preview(plain, true).json.find("\"background\":\"#171A1F\"") ==
      std::string::npos) {
    return 16;
  }
  runtime.pump_messages();
  mtpc::Frame light;
  light.kind = mtpc::MessageKind::show_native_preview;
  light.request_id = 47;
  light.json = R"({"displayMode":"listing","background":"#EEF1F4"})";
  if (runtime.show_native_preview(light, true).json.find("\"background\":\"#EEF1F4\"") ==
      std::string::npos) {
    return 17;
  }
  runtime.pump_messages();
  if (runtime.show_native_preview(light, false).kind != mtpc::MessageKind::native_preview_state) return 18;
  return 0;
}
