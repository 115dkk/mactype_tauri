# Lint policy

All new Control Center code is a merge-blocking lint target.

- `control-center/`: TypeScript strict mode, ESLint with zero warnings, production build.
- `control-center/src-tauri/`: `rustfmt`, Clippy with warnings denied, Rust tests; the crate carries `#![forbid(unsafe_code)]` and reaches Win32 only through `mactype-service-platform`.
- `service-runtime/`: `rustfmt`, Clippy with warnings denied, Rust tests; `host` and `setup` carry `#![forbid(unsafe_code)]`, and `platform`, the only crate in either Rust tree that may contain `unsafe`, denies undocumented `unsafe` blocks and unsafe operations inside unsafe functions.
- `preview-helper/`: MSVC `/W4 /WX`, `/permissive-`, local whitespace gate, protocol tests, and an x86 DLL/WIC runtime integration test.
- `shared/settings-schema.json`: generation drift check for the committed Rust, TypeScript, and C++ views.

The existing MacType C and C++ core predates this policy and is temporarily outside automatic formatting and compiler warnings-as-errors on ordinary branches. This is an explicit compatibility boundary, not a blanket exemption. Files under `control-center/` and `preview-helper/` must never be added to the legacy exclusion. When a legacy core file is deliberately modernized, it should be opted into a narrow lint target in the same change.

`codex/alpha-plus-dll` deliberately overrides that compatibility boundary. Its
`Alpha rendering core / Cppcheck x86+x64` job builds Cppcheck 2.20.0 from the
official repository at pinned commit
`502c802a69c78f3d8cfd9973aa2108ae169c73b5`, parses both `Release|Win32` and
`Release|x64`, and rejects new memory, resource, leak, lifetime,
initialization, and output-parameter diagnostics. The first ratchet keeps a
short, named list of lower-priority legacy categories as debt; it does not
suppress allocation failure, null dereference, resource leak, or uninitialized
state checks. Each later modernization tranche must shrink that list.

Every first-party translation unit in `gdipp.vcxproj` stays in the project
analysis. A first-party file that has no build reference, include reference,
or runtime purpose is deleted rather than placed on an exclusion list.
`renderer/json.hpp` is required third-party source and retains a narrow documented
suppression for Cppcheck 2.20's `missingReturn` template-analysis false
positive. First-party code includes it through `renderer/json_include.h`, which
disables only MSVC warning C5311 around it: version 3.10.5 declares
`operator "" _json`, a spelling C++23 deprecates, and MSVC 19.51 reports it.

On `codex/alpha-plus-dll`, both renderer projects (`gdipp.vcxproj` and
`macloader.vcxproj`) compile as C++23 (`/std:c++23preview`, MSBuild
`LanguageStandard` `stdcpp23`, and `/std:c17` for the C sources) with
`/permissive-` and `/Zc:__cplusplus` in every configuration, so two-phase
lookup and the other conformance rules apply to the rendering core as they do
to the preview helper. Thread-safe static initialization stays on in every
configuration: the Win32 configurations used to disable it with
`/Zc:threadSafeInit-` (an old Windows XP accommodation; x64 never did), and a
second thread that reached a function-local static while the first was still
initializing it read a null pointer, which crashed x86 processes whose existing
DirectWrite factory was hooked while the application was using it. Static
initializers that run under the loader lock must not call loader functions, so
a thread waiting on such an initializer never waits for the lock its
initializer needs. MSVC 17.14 and 18 offer C++23 only as this preview
mode; the renderer exports no C++ ABI and links the CRT statically, so the
mode's missing ABI guarantee reaches no other module. MSVC ignores an unknown
`/std` value with warning D9002 and compiles as C++14, so
`scripts/ci/Test-AlphaBranchPolicy.ps1` pins `stdcpp23` in every configuration
of both projects. The renderer tests in `tools/service-probe` compile renderer
sources and headers in the same mode through `MACTYPE_RENDERER_CXX_STANDARD`;
`cxx_std_23` is not used there because CMake maps it to `/std:c++latest`.
The Cppcheck job reads the language level from the project. Cppcheck 2.21,
newer than the pinned 2.20, still reports `if consteval` as a syntax error and
stops analyzing that translation unit, so first-party code does not use it;
C++23 adoption here means library features such as `std::expected` and
`std::to_underlying`. The preview helper, the service injector and the other
CMake trees stay on C++20. On other branches the renderer keeps the MSVC
default language mode.

`gdipp.vcxproj` carries two configurations per platform on this branch:
`Rel+Detours`, which every package and gate uses, and `Debug`, an unoptimized
build with full symbols on the same release CRT (`/MT`) and dependency libraries,
without `_DEBUG` and with `assert` enabled. The dependencies are built only as
release libraries (IniParser with `/MT` and `/GL`), so a debug-CRT renderer
would not link against them. `Build-OpenCore.ps1` builds `Debug|x64` after the
release cores so the configuration keeps compiling; its output is never
packaged. The EasyHook, Infinality, EXE and trace configurations were removed
because nothing built them and they no longer compiled.

On ordinary branches, generated files, build output, required third-party
headers, `renderer/json.hpp`, and dependency trees remain outside first-party source
linting. On `codex/alpha-plus-dll`, the override above governs every tracked
rendering translation unit; dependency boundaries may be narrow and documented
but may not be used to hide first-party code.
