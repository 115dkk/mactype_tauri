#include "protocol.h"

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <iostream>
#include <string>

int main() {
  if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) {
    std::cerr << "failed to set binary IPC mode\n";
    return 2;
  }

  for (;;) {
    mtpc::Frame request;
    std::string error;
    if (!mtpc::read_frame(std::cin, request, error)) {
      if (std::cin.eof()) return 0;
      std::cerr << "protocol error: " << error << '\n';
      return 3;
    }

    mtpc::Frame response;
    response.request_id = request.request_id;
    switch (request.kind) {
      case mtpc::MessageKind::hello:
        response.kind = mtpc::MessageKind::hello_ack;
        response.json = R"({"protocolVersion":1,"renderer":"placeholder","loadsMacType":false})";
        break;
      case mtpc::MessageKind::ping:
        response.kind = mtpc::MessageKind::pong;
        response.json = R"({"ok":true})";
        break;
      case mtpc::MessageKind::render_preview:
        response.kind = mtpc::MessageKind::preview_rendered;
        response.json = R"({"width":1,"height":1,"dpi":96,"placeholder":true})";
        response.binary = mtpc::placeholder_png();
        break;
      case mtpc::MessageKind::shutdown:
        response.kind = mtpc::MessageKind::ack;
        response.json = R"({"shutdown":true})";
        mtpc::write_frame(std::cout, response);
        return 0;
      default:
        response.kind = mtpc::MessageKind::error;
        response.json = R"({"code":"unsupported_message","recoverable":true})";
        break;
    }
    if (!mtpc::write_frame(std::cout, response)) {
      std::cerr << "failed to write protocol response\n";
      return 4;
    }
  }
}
