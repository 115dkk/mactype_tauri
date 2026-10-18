#pragma once

// nlohmann/json 3.10.5 declares its literal operators as `operator "" _json`.
// C++23 deprecates that spelling (CWG 2521), and MSVC 19.51 reports C5311 for
// each declaration, which /WX targets turn into errors. The vendored header
// stays unchanged.
#pragma warning(push)
#pragma warning(disable : 5311)
#include "json.hpp"
#pragma warning(pop)
