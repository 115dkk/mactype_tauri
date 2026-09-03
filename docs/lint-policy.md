# Lint policy

All new Control Center code is a merge-blocking lint target.

- `control-center/`: TypeScript strict mode, ESLint with zero warnings, production build.
- `control-center/src-tauri/`: `rustfmt`, Clippy with warnings denied, Rust tests.
- `service-runtime/`: `rustfmt`, Clippy with warnings denied, Rust tests; `host` and `setup` carry `#![forbid(unsafe_code)]`, and `platform` denies undocumented `unsafe` blocks and unsafe operations inside unsafe functions.
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
positive.

On ordinary branches, generated files, build output, required third-party
headers, `renderer/json.hpp`, and dependency trees remain outside first-party source
linting. On `codex/alpha-plus-dll`, the override above governs every tracked
rendering translation unit; dependency boundaries may be narrow and documented
but may not be used to hide first-party code.
