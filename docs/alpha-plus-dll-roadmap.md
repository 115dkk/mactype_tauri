# Alpha Plus DLL core modernization roadmap

## Status and scope

This roadmap applies only to the long-lived `codex/alpha-plus-dll` personal
distribution branch. The branch may carry invasive core changes that are not
appropriate for upstream review. It must never be used as the source of an
upstream pull request.

The regular fork `main` remains the source of Control Center, service,
installer, security, and generally useful product work. At the beginning of a
work session, this branch must inspect `origin/main` and cherry-pick useful
changes before starting overlapping work. The initial branch baseline is
`origin/main` at `14eba4f8636c70380d3dfb99c48fecdf6d0d5314` on 2026-08-05.

## Why this branch exists

The current open service can prove that a MacType DLL was loaded into a target
process, but module presence does not prove that the process used the selected
profile or substituted the requested font. A local Google Chrome experiment
showed this distinction: service telemetry recorded successful injection, but
the rendered source-font fingerprint remained identical to stock Windows.

The service may inject after a browser has already created its shared
DirectWrite factory. The former hook path only observed later calls to
`DWriteCreateFactory`, so an existing factory could remain unhooked. The first
branch patch schedules post-loader-lock discovery of the shared factory. The
service probe now reproduces the late-injection order by creating the factory
before loading MacType. This patch is a starting correction, not the final
DirectWrite architecture.

The branch exists to replace fragile accumulated hook behavior with a measured
and maintainable system while retaining MacType's profile semantics and the
Control Center's security boundaries.

## Non-negotiable invariants

Core work may be aggressive, but every change must preserve these rules.

- A process receives only an explicitly selected and validated profile. A
  missing or unreadable profile must not silently fall back to the DLL's
  historical defaults.
- Service, installer, elevation, installed-root, ownership, receipt, and
  payload-integrity checks remain enforced. Core work cannot add a developer
  backdoor or trust a DLL merely because it is adjacent to an arbitrary EXE.
- Protected processes, code-integrity restrictions, incompatible process
  architectures, and unsupported rendering paths must be reported honestly.
  Tests must not disable platform protections to manufacture a pass.
- x86 and x64 behavior must remain equivalent unless a difference is an
  explicit, documented Windows constraint.
- Hook installation must be idempotent and thread-safe. Loader-lock work stays
  minimal, and background initialization must retain the module until it has
  released every resource it owns.
- Changes to persisted profiles, manifests, caches, or runtime state require a
  versioned migration and rollback test.

## Target architecture

### 1. Ownership and RAII foundation

Replace implicit ownership and cleanup conventions before expanding the hook
surface. Introduce small, type-specific owners for Win32 handles, loaded module
references, mapped views, GDI objects, synchronization objects, and Detours
transactions. Borrowed `HMODULE` values must remain distinguishable from
references acquired with `LoadLibrary` or `GetModuleHandleEx`. COM objects use
`CComPtr` or an equivalent single convention, with explicit release before
`FreeLibraryAndExitThread` when stack unwinding cannot occur.

Move mutable global initialization into an explicit runtime state object with
well-defined `Uninitialized`, `Starting`, `Active`, `Failed`, and `Stopping`
states. Initialization and teardown must tolerate repeated entry, partial
failure, concurrent factory creation, and process shutdown. Every converted
resource needs a fault-injection test that proves cleanup after each failed
operation.

### 2. Hook lifecycle and capability registry

Build one hook coordinator instead of letting each rendering path infer global
state independently. The coordinator records which modules and interfaces are
present, which hooks were attempted, which succeeded, and why a capability is
unavailable. It owns Detours transactions and protects each target from double
patching.

Hook setup must cover modules loaded before MacType, modules loaded after
MacType, and interfaces created before service injection. Module notifications
or another loader-lock-safe scheduling mechanism may discover future modules;
heavy inspection and COM work must run after the loader lock is released.
Teardown must stop new work before releasing hook-owned state.

### 3. Unified font-substitution engine

Parse the active profile once into an immutable, reference-counted snapshot.
The substitution engine must normalize localized family names, typographic and
legacy family aliases, OpenType collection members, and variable-font family
names without discarding the user's exact rule. It must detect substitution
cycles, cap chains deterministically, and report the final family and the rule
that selected it.

GDI and DirectWrite adapters must call the same resolver. Per-process include
and exclude rules, font-specific overrides, weight, stretch, style, character
set, locale, and variable axes must be applied in a documented order. A
configuration reload publishes a new immutable snapshot; in-flight rendering
keeps the old snapshot until its references are gone.

No adapter may substitute a built-in DLL default when no profile is active.
The Control Center can publish the installed default profile before starting
the service, but the core itself must report an unconfigured state rather than
invent settings.

### 4. DirectWrite and DirectWriteCore support

Replace guessed version-dependent vtable behavior with a registry of supported
interfaces and verified method layouts. Cover shared and isolated factories,
factories that existed before injection, and factories created afterward.
Investigate and test `dwrite.dll` and Windows 11's `DWriteCore.dll` separately.

The implementation must trace substitution through font collections, font
sets, font fallback, text formats, text layouts, font faces, and glyph-run
analysis. It must not claim success merely because `CreateTextFormat` returned
the replacement family; rendered pixels or resolved font-face identity must
confirm the result. Factory/interface hooks need concurrency, reentrancy,
multiple-factory, and process-shutdown tests.

### 5. FreeType and rendering modernization

Inventory the current pinned FreeType fork, local patches, compile flags, and
ABI assumptions before updating it. Separate MacType-specific rendering policy
from FreeType object lifetime and cache management. Then modernize library,
face, size, glyph, and cache ownership using the RAII foundation.

Add measured support for variable fonts, TrueType collections, CJK fallback,
color-font interactions, vertical metrics, high-DPI scaling, and current
Windows font files. Rendering changes require golden-image evidence at several
DPI values plus performance and memory comparisons against the branch
baseline. A visually different result is not automatically an improvement;
the evidence must record the intended metric or rasterization change.

### 6. Windows 11 and application compatibility

Replace deprecated manifest-sensitive OS-version branching with feature
detection and a trustworthy version query where a version is genuinely
required. Test current Windows 11 builds with GDI, classic DirectWrite,
DirectWriteCore, WPF, WinUI 3, WebView2, Chromium, Edge, Firefox, Electron, and
representative native applications.

Record platform restrictions instead of hiding them. Protected Process Light,
Arbitrary Code Guard, Code Integrity Guard, sandbox policies, anti-cheat, and
application-owned bitmap or glyph-atlas text may prevent injection or make
font substitution irrelevant. The diagnostic output must distinguish these
states from a successful hook that rendered the wrong font.

Rebel Inc remains a local compatibility target because its licensed binaries
cannot be redistributed in CI. Its test records process/module evidence and a
repeatable visual capture, while noting that Unity assets may contain
pre-rendered glyphs that no system-font hook can replace.

### 7. Diagnostics and field evidence

Every renderer probe should emit the active profile digest, source and resolved
families, rendering API, factory timing, hook capabilities, loaded MacType DLL
path, process architecture, OS build, and whether machine state changed.
Failures must retain the first meaningful Win32 or HRESULT value before a later
cleanup call can overwrite it.

The Control Center diagnostic log and CI JSON artifacts must use the same state
names. Module-loaded, hook-installed, rule-resolved, and pixel-substituted are
separate claims and must never be collapsed into one success flag.

### 8. Sanitizers, verifier, and leak evidence

Build dedicated x86 and x64 diagnostic cores with MSVC AddressSanitizer and
debug information. These DLLs, matching sanitizer runtimes, symbols, and dump
settings are CI-only artifacts and must never enter an installer, release, or
integration/developer bundle. Run them through the native late-injection
probe, the open-service contract, and the pinned Chromium and Firefox pixel
proofs so that the exercised code is the same injected rendering code used by
the product rather than an isolated unit-test substitute.

An ASan lane is RED when the sanitizer reports an error, writes an
`ASAN_SAVE_DUMPS` dump, terminates a target, causes a browser renderer or
service host to restart, or prevents required hook/profile/pixel telemetry
from completing. Retain stdout, stderr, symbols, module hashes, dumps, service
events, browser JSON, and screenshots on every failure. Keep the ordinary
non-sanitized lane because sanitizer instrumentation changes layout and timing
and cannot prove production artifacts by itself.

MSVC AddressSanitizer does not implement LeakSanitizer. Leak checking is a
separate job using Application Verifier for heap, handle, lock, and exception
checks plus UMDH snapshots and bounded process counters. Run verifier against
purpose-built native and browser-host probes, not indiscriminately against the
entire hosted runner. Include an explicit DLL unload cycle: process-exit-only
cleanup cannot prove that an injected MacType module releases its own state.
Repeated samples fail on a confirmed leaked allocation stack, leaked handle,
verifier stop, or statistically meaningful private-bytes/handle/GDI/USER
object growth after warm-up. Threshold changes require retained before/after
evidence, never a silent increase.

Partial instrumentation is recorded honestly. ASan can diagnose accesses made
by the instrumented core and helper modules even when the stock browser is not
instrumented, but it cannot certify uninstrumented browser or Windows code.
The diagnostic manifest therefore lists every instrumented module and the
exact sanitizer runtime loaded in each target process.

## Required evidence matrix

Every substantial core tranche runs the applicable rows below on both x86 and
x64. A missing result is `UNKNOWN`, not `PASS`.

| Mode | Required proof |
| --- | --- |
| Stock Windows | Source and replacement controls render differently; no MacType module or machine-state change is present. |
| Legacy MacTray | The same profile, process, API, and pixel fingerprint test runs with interactive tray injection. |
| Legacy MacType service | The same test runs with the legacy machine service and records its injection state. |
| Alpha open service | The service records the exact profile/runtime generation, target DLL, resolved family, and changed pixel fingerprint. |

The browser gate downloads pinned Playwright Chromium and Firefox builds. It
renders identical source and replacement samples, stores screenshots and
pixel hashes, and passes substitution only when the active source result
matches the independently rendered replacement result. Current installed
Chrome and Edge may be added as non-pinned compatibility lanes, but they cannot
replace the pinned reproducible browsers.

Chromium 149 enables `FontDataServiceAllWebContents` by default. That isolated
browser-side path validates the embedded family name after MacType resolves a
replacement and therefore rejects a replacement file whose family differs
from the requested family. The pinned Chromium substitution proof explicitly
uses Chromium's renderer-side DirectWrite compatibility path by disabling that
feature; the selected mode is recorded in the retained JSON. Stock Chromium
controls continue to run with the default feature set, and this compatibility
mode does not relax the substitution criterion: the active source pixels must
still exactly match the independently rendered replacement pixels.

Firefox normally initializes its parent font cache before the open service can
load the renderer DLL. The pinned Firefox substitution proof therefore starts
the parent under `DEBUG_ONLY_THIS_PROCESS` through the tested browser launch
gate. The gate restores a temporary PE image-entry breakpoint after Windows
has completed DLL initialization, detaches the debugger with the main thread
still suspended, and resumes that thread only after the exact Firefox PID
publishes a DirectWrite `hook-ready` event after all synchronous demand hooks
are installed. No Firefox user entry-point code runs before injection is ready.
The gate also forwards Playwright's inherited CRT
descriptors 3 and 4 for `-juggler-pipe`, restoring their inheritance flags with
RAII after the real Firefox process is created. Mozilla's
`MOZ_DEBUG_CHILD_PAUSE=10` diagnostic also holds each content process before
its normal sandbox starts so the service can load the renderer DLL; the stock
control uses neither gate nor pause. The selected gate and pause are recorded
in the retained JSON, renderer hook diagnostics are still mandatory before the
first font lookup, and the active source pixels must still exactly match the
independently rendered replacement pixels.

The native probe covers GDI and DirectWrite with injection before factory
creation, injection after factory creation, multiple factories, worker-thread
creation, child renderer processes, profile reload, missing profile, damaged
profile, and shutdown during initialization. CI retains the JSON, screenshots,
module inventory, profile, and exact binary hashes needed to reproduce a
failure.

## Delivery sequence

Work proceeds in bounded tranches. A later tranche may begin only when the
previous tranche's required evidence is retained.

1. **Evidence baseline and alpha release:** Finish the real browser gate,
   capture stock/legacy/open-service comparisons, enable all `main` and core
   CI on this branch, establish the Cppcheck safety ratchet and sanitizer/leak
   job contracts, and publish only `alpha-` prereleases.
2. **RAII and runtime state:** Introduce ownership primitives and the explicit
   runtime state machine without changing rendering. Prove behavioral parity,
   cleanup under injected failures, and clean process shutdown.
3. **Substitution resolver:** Centralize parsing and family resolution, add
   alias/cycle/variable-font tests, then switch GDI and DirectWrite adapters to
   the shared immutable snapshot.
4. **DirectWrite lifecycle:** Replace the provisional shared-factory fix with
   the capability registry and cover pre-existing/future factories,
   collections, font sets, fallbacks, and DirectWriteCore.
5. **FreeType modernization:** Update ownership and caches first, then evaluate
   a pinned FreeType update and rendering changes with golden images and
   performance evidence.
6. **Windows 11 application work:** Expand the application matrix, fix only
   reproduced incompatibilities, and document platform-enforced exclusions.
7. **Stabilization:** Run long browser/native soak tests, concurrent profile
   reload, repeated service restart and upgrade, memory/leak checks, crash dump
   collection, and rollback tests before widening personal use.

## Per-change completion gate

A core change is complete only when its focused regression test first fails on
the prior branch state and passes with the change, both core architectures
build, relevant native and browser proofs pass, security/static policy tests
remain green, and diagnostics can distinguish a real substitution from mere
DLL presence. Performance-sensitive changes also record before/after CPU,
allocation, working-set, and render-time measurements.

All branch releases remain GitHub prereleases. Tags, titles, installers,
integration/developer bundles, and checksum downloads use the `alpha-` prefix
so users cannot mistake this personal experimental core for the regular fork
or upstream MacType.
