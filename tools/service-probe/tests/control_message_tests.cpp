#include "../../../control_message.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void TestControlMessages()
{
    using renderer::ControlMessageKind;
    const auto parse = [](const std::string& text) {
        return renderer::ParseControlMessage(text.data(), text.size());
    };
    for (const char* invalid : {"", "{", "[]", "null", "{}", "{\"command\":1}",
        "{\"command\":\"unknown\"}", "{\"command\":\"loadprofile\"}",
        "{\"command\":\"loadprofile\",\"file\":false}",
        "{\"command\":\"loadprofile\",\"file\":\"\"}",
        "{\"command\":\"loadprofile\",\"file\":\"a\\u0000b\"}"}) {
        Require(parse(invalid).kind == ControlMessageKind::invalid,
            "malformed or wrongly typed control messages must cause no action");
    }
    const std::string ping = "{\"command\":\"ping\"}";
    Require(parse(ping).kind == ControlMessageKind::ping &&
        parse(ping + '\0').kind == ControlMessageKind::ping,
        "ping must accept counted and uncounted terminating NULs");
    Require(parse(ping + '\0' + "garbage").kind == ControlMessageKind::invalid &&
        parse(ping + '\0' + '\0').kind == ControlMessageKind::invalid,
        "embedded or repeated terminating NULs must not conceal trailing input");
    const auto load = parse("{\"command\":\"loadprofile\",\"file\":\"C:\\\\Temp\\\\preview.ini\"}");
    Require(load.kind == ControlMessageKind::loadProfile && load.path == "C:\\Temp\\preview.ini",
        "a valid temporary profile path must remain accepted");
    Require(renderer::ParseControlMessage(nullptr, 1).kind == ControlMessageKind::invalid &&
        renderer::ParseControlMessage(ping.data(), renderer::kMaxControlMessageBytes + 1).kind == ControlMessageKind::invalid,
        "null and oversized payloads must be rejected before any copy");
    std::string boundary = ping;
    boundary.resize(renderer::kMaxControlMessageBytes, ' ');
    Require(parse(boundary).kind == ControlMessageKind::ping,
        "the documented byte boundary must be accepted");
    boundary.push_back(' ');
    Require(parse(boundary).kind == ControlMessageKind::invalid,
        "the next byte must exceed the bound");
    Require(parse(std::string(32, '[') + "0" + std::string(32, ']')).kind == ControlMessageKind::invalid,
        "deeply nested input must be rejected without unbounded parser recursion");
}

} // namespace

int main()
{
    TestControlMessages();
    std::cout << "Control message tests passed\n";
    return 0;
}
