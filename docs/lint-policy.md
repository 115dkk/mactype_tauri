# Lint policy

All new Control Center code is a merge-blocking lint target.

- `control-center/`: TypeScript strict mode, ESLint with zero warnings, production build.
- `control-center/src-tauri/`: `rustfmt`, Clippy with warnings denied, Rust tests.
- `preview-helper/`: MSVC `/W4 /WX`, `/permissive-`, local whitespace gate, protocol tests, and an x86 DLL/WIC runtime integration test.
- `shared/settings-schema.json`: generation drift check for the committed Rust, TypeScript, and C++ views.

The existing MacType C and C++ core predates this policy and is temporarily outside automatic formatting and warnings-as-errors. This is an explicit compatibility boundary, not a blanket exemption. Files under `control-center/` and `preview-helper/` must never be added to the legacy exclusion. When a legacy core file is deliberately modernized, it should be opted into a narrow lint target in the same change.

Generated files, build output, third-party headers, `json.hpp`, and dependency trees are excluded from source linting.
