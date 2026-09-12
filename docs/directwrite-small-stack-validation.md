# DirectWrite small-stack validation

## Reported binary

The SokudaKun startup report names the x86 `MacType.dll` from runtime
`0.2.0+ci.33859821530.c42c42e0ba91.current`, with exception
`0xc00000fd` at RVA `0x0009fff7`.

On 2026-09-13, the `mactype-open-core` artifact from fork Actions run
`33859821530` was downloaded without executing its binaries. The run identifies
source commit `c42c42e0ba91228eeb129b451a98c476dd967d65`.
The reported x86 DLL's SHA-256 is
`be79ce9c8c2864aa943700d9a073a75fcb72b676278ebb16cd2c42b12158db30`.

MSVC disassembly places RVA `0x9fff7` at `test dword ptr [eax],eax` in
`__chkstk`'s page-probing loop, starting at RVA `0x9ffd0`. This identifies a
stack probe, not the caller that exhausted the stack. An Event Viewer offset
alone cannot distinguish a large frame from accumulated stack use or recursion.

## Existing fix and regression coverage

Commit `7aceccc703300dbdac80a8ae212c2b7b7ec8fe2d`, already on the alpha branch,
removes two 64 KB buffers from application-thread paths:

- `IsDWriteCoreModule` now uses `module_name::BaseNameEquals`, with a small
  initial path buffer and heap growth for long paths.
- `directwrite_virtual_font::FileMatches` uses a heap-backed comparison chunk
  while checking a persisted alias font.

`module-name-tests` already covers the first path. The new
`directwrite-font-cache-tests` compiles the actual private virtual-font
implementation, without installing hooks or creating alias-cache files. Its
header includes only the Windows/DirectWrite/ATL declarations it uses, allowing
the test to compile without the legacy renderer settings and array headers.

The cache test reads the architecture's mapped `kernel32.dll` as an immutable
multi-chunk fixture. Six separate threads, each with an explicitly reserved
64 KB stack, check an exact match, mismatches in the first/second/final chunks,
a size mismatch, and a missing file. It also verifies that both output flags
are reset. Both small-stack tests run in the regular probe contract; the cache
test also joins the x86/x64 MSVC ASan gate.

## Local results, 2026-09-13

- Negative control: temporarily restoring `FileMatches`' old stack-backed
  `std::array<BYTE, 64 * 1024>` caused the x86 Release test to exit with
  `-1073741571` (`0xc00000fd`). The heap implementation was restored before
  building the delivered binaries.
- x86 and x64 Release: all six cache comparisons and `module-name-tests` pass.
- `Test-RendererAsan.ps1`: all 13 focused test executables pass on both x86
  and x64. This instruments focused modules, not an injected full core.
- Both `Rel+Detours` core/loader builds pass with MSVC v143. Existing warnings
  remain in other renderer translation units.
- `Test-RendererStackFrames.ps1` passes against the rebuilt DLLs and matching
  PDBs. There are no first-party probed frames on x86; the largest x64
  first-party probed frame is 4,312 bytes, under the 16,384-byte budget.
  Largest dependency frames are 16,532 and 25,616 bytes respectively.
- New-C++ style, source-comment policy, alpha branch policy, and
  `git diff --check` pass.
- Pinned Cppcheck 2.20.0 reports no diagnostics for either Release core
  configuration.

The rebuilt x86 DLL is `Rel+Detours/MacType.Core.dll`, SHA-256
`d89398f0bd1b4dc1b548a45ff42b0392fc3a87fbe6cc8bc0f6c848194f1d8101`.
The rebuilt x64 DLL is `x64/Rel+Detours/MacType64.Core.dll`, SHA-256
`b14ef290cffa17113cdde04d6e630ed06af897ca834b3b16ab3bc9e4a615bf1b`.
Stack inventories are retained under `build/issue55-stack-frames/`.

Reproduce the focused Release tests without elevation:

```powershell
cmake -S tools/service-probe -B build/directwrite-stack/Win32 -A Win32
cmake --build build/directwrite-stack/Win32 --config Release --target directwrite-font-cache-tests module-name-tests
& build/directwrite-stack/Win32/Release/directwrite-font-cache-tests.exe
& build/directwrite-stack/Win32/Release/module-name-tests.exe
```

Use `x64` in place of `Win32` for the other architecture. The instrumented gate
is `scripts/ci/Test-RendererAsan.ps1 -Architecture Win32` (or `x64`).

## Remaining field check

The running workstation service and its installed payload were left unchanged.
UAC approval was unavailable, so the required service stop and isolated
SokudaKun DLL-injection test were deferred. No application crash dump was
available. The existing fix removes a demonstrated failure mechanism consistent
with the report, but SokudaKun-specific resolution remains unverified.

When elevation is available, use the supported service stop, retain the exact
renderer/profile hashes, and compare stock, DirectWrite-disabled, and
DirectWrite-enabled SokudaKun startup with one intended renderer module. If
the corrected build still fails, collect the crashing thread's stack and
reservation size before choosing another fix. Follow the injection and
lifetime boundaries in `docs/hooking-compatibility.md`.
