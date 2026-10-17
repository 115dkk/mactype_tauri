# Alpha Plus DLL core modernization record

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

## Completion record (2026-08-22)

The large renderer code tranche is complete on `codex/alpha-plus-dll`. This
means the planned in-repository ownership, lifecycle, substitution, profile,
service-policy, and CI seams have executable contracts. It does not turn an
unrun machine or application experiment into a pass. The terms below are
deliberately narrower than “works everywhere”:

- `IMPLEMENTED`: the production path and an executable regression contract
  exist in this repository.
- `PROVED LOCAL`: the contract also passed on the development Windows host for
  both applicable architectures unless a narrower result is stated.
- `CI REQUIRED`: GitHub Actions must pass for the exact delivered commit.
- `LAB ONLY`: the claim needs external software, licensed content, a disposable
  machine, or machine-global verifier configuration and is not a code blocker.

| Area | Final code status | Evidence or deliberate boundary |
| --- | --- | --- |
| Renderer module boundary | `IMPLEMENTED` | Renderer-owned sources and resources live under `renderer/`; workspace manifests remain at the root. |
| Hook lifecycle | `IMPLEMENTED`, `PROVED LOCAL`, `CI REQUIRED` | `HookCoordinator` owns five runtime phases, concrete-target attempt records, first-failure evidence, duplicate suppression, the process-global Detours transaction lock, and stop admission. DirectWrite performs heavy startup on a self-pinned worker. Existing shared and isolated factories are exercised on x86/x64, while `LdrLoadDll`, `LoadLibraryExW`, and `GetProcAddress` cover future app-local DWriteCore loads. The direct native-loader path passed locally on x64; endpoint protection removed the purpose-built x86 loader probe, so the isolated exact-commit CI result is authoritative for that cell. |
| FreeType runtime | `IMPLEMENTED`, `PROVED LOCAL` | Manager-before-library ownership, transactional/idempotent initialization, bounded streams, signed-pitch accounting, typed LRU keys, immutable per-render policy, and cache teardown have focused x86/x64 tests. The pinned fork is intentionally retained; no unmeasured raster-output change is claimed. See [`freetype-runtime.md`](freetype-runtime.md). |
| Font substitution | `IMPLEMENTED`, `PROVED LOCAL` | GDI and DirectWrite consume one immutable generation snapshot. Chaining, cycles, depth, charset precedence, concurrent reload, stable rule identity, coherent virtual SFNT identity, collection generations, and variable axes are covered. An object returned before injection remains its honest older generation. |
| Profile selection | `IMPLEMENTED`, `PROVED LOCAL` | The adjacent `MacType.ini` and its exact `AlternativeFile` must be bounded, regular, readable INI files with `[General]`; missing, empty, malformed, selected-missing, reparse, and oversized inputs fail closed instead of loading historical defaults. CI exercises a valid profile and the portable negative cases; reparse rejection is enforced by the file-attribute check. |
| Explicit process refusal | `IMPLEMENTED`, `PROVED LOCAL` | The Rust validator retains the exact `(pid, creation time)` and reason in the bounded process result. Re-observation is a duplicate, service health stays `Ready`, no `lastError` or performance warning is manufactured, and another PID with the same image name remains eligible. The health-v1 wire schema is unchanged for rollback compatibility. |
| Windows feature detection | `IMPLEMENTED` | Manifest-sensitive OS-version guesses were removed from renderer decisions. Interface queries, module/export presence, and concrete mitigation facts now select capabilities. |
| Sanitizer gate | `IMPLEMENTED`, `PROVED LOCAL`, `CI REQUIRED` | MSVC ASan instruments the new ownership, lifecycle, FreeType-policy, and substitution modules on x86/x64. A fully instrumented injected DLL remains unavailable because stock IniParser and other linked dependencies do not share ASan/STL annotations; CI must not mislabel module coverage as whole-process certification. |
| Application Verifier/UMDH | `LAB ONLY`, not run | The current host lacks the complete `gflags`/UMDH toolchain and these tools mutate machine-global verifier state. The lane was reevaluated and intentionally not automated after the surrounding modules changed. Run it only in a disposable Windows lab with explicit cleanup evidence. |
| Browser, WinUI, WPF, Qt, licensed games, soak and golden images | `LAB ONLY` | Native contracts establish the API seams. Retained local Unity runs bind hashed Rebel Inc and Plague Inc binaries to exact injected module identity, responsiveness, WER, private-FreeType markers, nonzero Korean glyph lookup, and full-window captures. Portability to other game revisions and machines remains `UNKNOWN`; this is compatibility evidence, not unfinished renderer ownership code. |

The classic-C++ upstream renderer branch is a separate contribution experiment.
None of its no-RAII assumptions or commits are inputs to this record.

The requested post-work architecture pass was reevaluated after the lifecycle,
FreeType, substitution, and Rust compatibility tranches. Its high-leverage
changes were already made in place: the coordinator, immutable snapshot,
ordered FreeType runtime, process validator, and orchestrator result registry
are the deep modules and seams. A second sweeping refactor would now cut these
new modules into pass-through layers and churn freshly verified adapters, so
that separate step is intentionally skipped.

## Why this branch exists

The current open service can prove that a MacType DLL was loaded into a target
process, but module presence does not prove that the process used the selected
profile or substituted the requested font. A local Google Chrome experiment
showed this distinction: service telemetry recorded successful injection, but
the rendered source-font fingerprint remained identical to stock Windows.

The service may inject after a browser has already created a DirectWrite
factory. The former hook path only observed later calls to
`DWriteCreateFactory`, so an existing factory could remain unhooked. The final
branch path now coordinates existing shared and isolated factories after the
loader lock, tracks their concrete hook targets, and also observes future
classic and app-local DWriteCore factory entry points. An immutable font
collection already returned to an application is still not mutated
retroactively; diagnostics preserve that generation boundary.

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

The current upstream failure inventory, quiet per-process exclusion rule, and
implemented compatibility paths are tracked in
[`hooking-compatibility.md`](hooking-compatibility.md).

Rebel Inc and Plague Inc remain local compatibility targets because their
licensed binaries cannot be redistributed in CI. The repeatable lab script
records exact process/module and WER evidence plus Unity renderer markers. The
allowlisted Unity adapter now covers private FreeType render output and the
exact internal face-open boundary reached after Unity selects an OS `FontRef`.
It copies pathname-backed arguments, substitutes the replacement file and TTC
face index together, and retries the original face only when replacement
creation fails. Test-only shared-memory evidence keeps observed face opens,
successful redirects, fallbacks, and exact paths separate from visual identity.
Visual capture remains a separate manual artifact when Windows denies capture,
and pre-rendered assets remain outside the font-hook seam.

Adapters for the legacy `DynamicFontData` text path must also distinguish font
discovery from font use. Its private system catalog opens every installed face
to learn family metadata; a pathname substitution at that point erases the
source family from fallback selection even though `FT_Open_Face` succeeds. The
exact 2019.4.41 adapter therefore suppresses redirects only inside the
catalog-entry loader and resumes them for the selected rendering face. Unity
6000.3.21 still builds that catalog through
`TextRenderingPrivate::ReadFontFileAndAddToCache`, so its adapter carries the
same boundary since 2026-10-16: Plague Inc moved to that engine in 2026-09 and
drew no Hangul whenever the renderer attached before the catalog scan (five
redirected discovery opens ending in `malgunsl.ttf`, about 2,560 glyph renders);
with the boundary every isolated run shows only the use-opens (four, ending in
`malgunbd.ttf`) and about 7,600 renders. A field run is not successful when it
observes character lookups but every lookup resolves to glyph zero.

The discovery boundary alone still leaves one order unsubstituted: when the
renderer attaches after `DynamicFontData::GetFaceForFontRefFromOSFonts` has
already opened the mapped file natively, `GetFaceForFontRef` caches that face
per `FontRef` and the resolver is never consulted for it again (measured: 10,149
resolver calls, 15 hits, in a run that attached at 1.6 s; 0 hits when attaching
at 4 s). The replacement therefore happens in
`DynamicFontData::GetFaceForCharacterIfAvailableInFont`, which Unity calls per
character and fallback `FontRef` and which returns the face it is about to use.
The adapter identifies a native face only by its `family_name` (the stream
pathname is a borrowed caller pointer in FreeType and dangles for these faces,
so it is never read), replaces only faces whose stream carries FreeType's own
file read callback (learned from the replacement the renderer opens by pathname
itself, which excludes memory and custom-stream faces), opens the replacement
once through Unity's FreeType library via the original face-open trampoline
while no registry lock is held (FreeType's open failure path calls
`FT_Done_Face`, which is the hooked function; a pending reservation keeps
concurrent lookups on the native face until the open settles), returns the
replacement only when it has a glyph for the character, counts those hits
separately from constructed replacements, and releases both faces through the
hooked `FT_Done_Face` when Unity frees the native one. That release is final by
construction: the player exports no `FT_Reference_Face`. Detach leaves published
replacement faces alive on purpose, because Unity may still hold a returned
pointer and its library outlives the renderer; each renderer generation can
therefore leave at most 256 faces behind until the library is destroyed, an
accepted cost of the rare unload/reload cycle. The hook-level sequences (open
failure re-entering Done_Face, callback mismatch before and after the canonical
callback is learned) are covered by field runs and by the registry tests, not by
a hook orchestration seam; that seam is a follow-up. The 6000.3.21 `FontRef` layout is
not decodable, so that build selects `undecoded` instead of failing closed. The
same table now carries the named instance of a variable replacement
(`Pretendard Variable Medium` is `PretendardVariable.ttf` with instance bits in
the FreeType face index), keyed by file, base face index and axis values so two
rules on one variable file (`맑은 고딕` to Medium, `맑은 고딕 Semilight` to
SemiBold) resolve to their own instances, and the fvar parser follows `ttcf`
collections to the selected face. Bold source faces still land on the mapped
instance (the Unity table is weight-blind for bold).

For builds with an exact OS `FontRef` resolver, a scoped selection context
distinguishes an explicitly mapped family from an unrelated family that merely
shares the same system font file. The context restores correctly under nesting;
diagnostics no longer substitute a resolver result or retain raw FreeType face
pointers. Exact older adapters without that resolver keep the checked
path-and-face-index fallback.

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

The repeatable CI seam is narrower than the original full-DLL proposal. The
x86 and x64 MSVC AddressSanitizer jobs instrument the new renderer ownership,
hook-lifecycle, FreeType runtime-policy, and substitution modules, run them
with the matching Visual C++ ASan runtime, and fail on any non-zero result.
Ordinary non-sanitized core builds remain mandatory because instrumentation
changes layout and timing.

A full injected `MacType.Core.dll` cannot yet be honestly instrumented in this
tree. The stock static IniParser build carries incompatible STL annotation
settings, producing link-time annotation mismatches when only the core uses
ASan. Full-DLL ASan becomes valid only when every linked C++ dependency is
built with matching compiler, runtime, iterator-debug, and sanitizer settings.
Until then, neither native nor browser injection may be described as
whole-core ASan coverage.

MSVC AddressSanitizer does not implement LeakSanitizer. Application Verifier
plus UMDH therefore remains a disposable-lab procedure for heap, handle, lock,
exception, and explicit-unload evidence. It is intentionally absent from the
hosted workflow: the current environment lacks the complete tools, and
verifier configuration is machine-global. Any future lane must record its
exact target image, before/after configuration, symbols, dumps, bounded
process counters, and cleanup rather than silently changing runner state.

## Required evidence matrix

Every substantial core tranche runs the applicable rows below on both x86 and
x64. A missing result is `UNKNOWN`, not `PASS`.

| Mode | Required proof |
| --- | --- |
| Stock Windows | Source and replacement controls render differently; no MacType module or machine-state change is present. |
| Legacy MacTray | The same profile, process, API, and pixel fingerprint test runs with interactive tray injection. |
| Legacy MacType service | The same test runs with the legacy machine service and records its injection state. |
| Alpha open service | The service records the exact profile/runtime generation and target DLL; an already-retained immutable browser collection is reported explicitly as unsupported-late. |
| Alpha product loader | The shipped MacLoader injects before process entry and records the resolved family and changed pixel fingerprint. |

The browser gate downloads pinned Playwright Chromium and Firefox builds. It
renders identical source and replacement samples, stores screenshots and
pixel hashes, and passes substitution only when the active source result
matches the independently rendered replacement result. Current installed
Chrome and Edge may be added as non-pinned compatibility lanes, but they cannot
replace the pinned reproducible browsers.

DirectWrite substitution now uses an immutable custom font set backed by
coherent virtual SFNT files. The core copies the native system font set into
an `IDWriteFontSetBuilder`; for each configured source it reads the replacement
through the documented DirectWrite file stream, rewrites the complete OpenType
`name` table to the source alias, repairs every SFNT checksum, and publishes an
immutable content-addressed file in the per-user MacType cache. That directory
carries the read and execute grants of `%WINDIR%\Fonts` (`BUILTIN\Users`,
`ALL APPLICATION PACKAGES`, `ALL RESTRICTED APPLICATION PACKAGES`), inherited by
every file, because a browser GPU process opens the file by path under a
restricted token and DirectWrite spares it that open only while the Font Cache
Service is warm; a cold service after a reboot otherwise made WebRender draw the
alias's glyph ids with Arial. The builder is
given an ordinary native `IDWriteFontFaceReference` for that file. The resulting
collection can locate `Cambria`, for example, while its name-table identity is
consistently `Cambria` and its metrics, outlines, file, resource, and glyph data
are consistently derived from `Courier New`. No COM proxy presents two
different identities for the same object.

A source face keeps every non-conflicting family spelling exposed by its
localized, typographic, weight/style, and Win32 properties. One Korean rule
may therefore select the replacement while the resulting collection still
answers both `맑은 고딕` and `Malgun Gothic`, including their Semilight names.
If two names for one physical face resolve to different replacements, that
face remains native rather than publishing a partial alias family.

This collection is installed only at the DirectWrite factory acquisition seam:
`GetSystemFontCollection`, `IDWriteFactory3::GetSystemFontSet`, the modern
`IDWriteFactory3::GetSystemFontCollection`, and the null-collection
`CreateTextFormat` path. It is prepared when a factory is hooked, which happens
only in a process that had DirectWrite mapped before injection or that creates
a factory itself; a console tool that never touches DirectWrite pays nothing.
The post-loader-lock worker runs only when DirectWrite was mapped before
injection and covers a shared factory created before injection. The core does not patch family, font, face,
face-reference, file, index, informational-string, or font-table objects, and
does not carry alias state in thread-local storage. A collection already
returned before injection remains an honest older immutable generation;
diagnostics report that timing instead of mutating retained objects.

Until 2026-10-17 the alias collection became the answer only after the whole
build had run, because `HookDirectWriteAliasCollection` captured the alias
source before patching the factory vtables. Measured on this machine with the
renderer mapped before image entry and a warm virtual-font cache (171 files,
806 MB): the stock collection answered for 1,171 to 1,344 ms, and an
application that fetched the collection in its first 62 ms received
`MALGUN.TTF` Regular for good. Chromium-family hosts (Claude Desktop, VS Code)
fetch and retain the collection in their browser process within that time and
hand it to renderers through `DWriteFontProxy`; their renderers held
`malgun.ttf` and `NotoSansKR-VF.ttf` and no alias file. The factory vtables are
now patched first and the build runs inside the intercepted call: the first
answer is the alias generation (first call at 78 ms after factory creation
returned the alias `Malgun Gothic / Medium / 500`; the readiness poll reports
94 to 157 ms), a second caller waits on the same build entry for at most 10 s,
and a nested call on the building thread receives the stock object instead of
waiting on itself. A persistent manifest in the font cache keys alias outputs by
replacement file identity, alias names, options, pinned axes and profile digest,
so `BuildAliasedSfnt` and `PersistFont` run only on a miss (warm build 58 to
140 ms). Renderer startup before image entry stayed at about 0.5 s with either
core. The alias file of a variable backing now keeps only the pinned named
instance: a consumer that re-parses the file sees one family with the Medium
instance (and its simulated bold), and `GetFirstMatchingFont(400)` returns
weight 500 instead of the default Regular; `fvar` axes and defaults, `glyf` and
`gvar` are byte-identical, so the outlines still match the coordinates. What a
late injection cannot do is replace a collection the application already
retained; forwarding that retained object's family lookups to the alias
collection is a separate design (shared vtable, identity check on `this`,
wholesale forwarding of four methods).

The review round on that change fixed what a read-only audit found and what
the first pre-entry probe run showed. `HookDWriteFactoryEntry` had taken the
hook mutex before `GetModuleHandleExW`, an inversion against the loader lock
that `DllMain` and `GetProcAddress` callers hold; the module reference is now
acquired first and released outside the mutex. The modern collection and
font-set entries established the canonical source themselves instead of
answering with the native object while the source was still unpublished, the
system-fallback hook moved in front of the build, and `GetCachedForFactory`
waits on a building entry. With that preamble inside every intercepted call,
the first hooked probe died with `STATUS_STACK_OVERFLOW`: DirectWrite answers
an original `GetSystemFontCollection` or `GetSystemFontFallback` call partly
through its own virtual methods, which are the patched slots, so the preamble
re-entered itself. `CanonicalSourceScope` in `directwrite_readiness.h` now refuses
a nested call on the establishing thread the way the build already refused one,
and `directwrite-readiness-tests` covers the nested refusal and its thread
separation alongside the two build races.
Name-record pruning was removed (a default instance shares IDs 2 and 17 with
the family; the alias identity records are still rewritten as before), an
explicit advertised weight wins over the pinned coordinate so
the bold-slot alias keeps its own file, and the manifest became
`manifest-v2.bin` with the backing size in the value: a hit is validated by
size and scaler type and a hit whose file no longer opens is dropped and
rebuilt once. Measured with the final core mapped before image entry and the
service stopped: with the manifest deleted and the alias files still present
the alias collection answered after 265 ms; warm, the poll saw it at 109 to
140 ms and the first call returned the alias at 109 to 141 ms. VS Code
started the same way received the alias in its browser process and drew the
editor's Hangul in Pretendard Variable Medium.

A read-only audit after the merge found four more defects, fixed in the
follow-up. `GetCachedForFactory` waited on a reference to a vector element
while the condition variable released the cache mutex, so another factory's
build growing the vector or a failed build erasing the element left the waiter
reading freed memory; it now waits on an owned copy. The fallback slots became
live before the per-factory build entry existed, so a `MapCharacters` in that
window found nothing to wait on and let the native font through; a
`BuildReservation` now inserts the entry before the fallback hook, the
reserving thread's own `GetOrCreate` adopts it, an unconsumed reservation
completes it as a failure, and a lookup on the reserving thread skips rather
than waits on itself. The fallback lookup waited up to ten seconds per factory
source and discarded the timeout silently; one deadline now spans the whole
mapping call and `alias-collection-wait-timeout` is signalled once. The
manifest was loaded once per process and rewritten whole from that snapshot,
so two processes overwrote each other's entries; the state reloads when the
file changed and a bounded named mutex serializes the read-merge-write across
processes. The pre-entry probe exercises the collection path only; the two
fallback windows are covered by the fake-driven readiness tests and by
reasoning, not by a runtime reproduction.

Windows Settings then froze on launch and the shell closed it as not responding
(field log, 2026-10-17, runtime c41f489); it was fine whenever the service was
stopped. A debugger trace in `SystemSettings.exe` counted 150 alias builds in
30 s for one factory and one source set, all on the UI thread from
`TextLayout::MapCharacters` through `FontFallback::FontFallback` into the
collection hook, 85 to 130 ms each, every one ending `createSetFailed`; the
cache erased a failed entry, so every `GetSystemFontCollection` call built
again. The source was the set the original `GetSystemFontSet` returns, which
`EstablishFactoryAliasSource` falls back to when the modern entry runs first:
735 faces, 225 of them downloadable and not local. `Build` re-added every
native face as a bare `AddFontFaceReference`, so `CreateFontSet` had to extract
each face's properties again and failed with `DWRITE_E_REMOTEFONT` on the
remote ones (225 `RemoteFontException` throws per build; every file open in
the build succeeded). A probe without MacType reproduced the failure in a
normal process and in an AppContainer, and built the legacy collection's set
(510 local faces) without trouble, which is why Notepad kept its alias and
Settings never got one. Native faces, downloadable ones included, now go in as
`IDWriteFontSet1::GetFilteredFonts` subsets through
`IDWriteFontSetBuilder1::AddFontSet` in source order, so DirectWrite keeps the
properties it already has instead of reading them again; a downloadable face is
never aliased. The build keeps two views of the result: the full set answers
`GetSystemFontSet` and `GetSystemFontCollection(includeDownloadableFonts=TRUE)`,
and a local-only subset of it answers the legacy collection,
`includeDownloadableFonts=FALSE` and the fallback mapping, the way the stock
objects split. A system without those interfaces keeps the per-reference path
and leaves downloadable faces out. A failed build stays in the cache with a
retry after 2 s, 15 s and 120 s, after which that factory keeps the stock set
until its source set or the profile generation changes, and the fallback lookup
reads the newest entry for a factory, so a failure retained for a superseded
source set cannot hide the current one. The legacy hook also no longer publishes an alias
collection's set as the factory's source on `checkForUpdates`; doing so would
have stacked a new generation on every such call. Measured with the service
stopped and each core loaded by `MacLoader64` before image entry, in a probe
that asks for `GetSystemFontSet` first the way Settings does: the c41f489 core
answered `Malgun Gothic` with `MALGUN.TTF` at weight 400 and spent 2,301 ms on
50 further legacy collection calls and 429 ms on 20 modern ones; the fixed core
answered with the alias file at weight 500 (Pretendard Variable Medium),
returned a 758-face set that still carries the downloadable faces, and spent
0.3 ms and under 0.1 ms.

An injection that lands after the first frame (the open service's remote
`LoadLibrary`, every elevated target, any child of a parent that was not
hooked) leaves that stock frame on screen until the application repaints on
its own, which is why text used to change only under the mouse. The renderer
therefore schedules one activation repaint after it publishes active
admission: a fire-and-forget worker waits up to three seconds for the
DirectWrite lifecycle to leave its starting phase and for the
pre-existing-factory worker to hook the known factories, then invalidates
every visible top-level window of the process (`RepaintOwnedWindows` in
`renderer/activation_repaint.cpp`: `RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
RDW_ALLCHILDREN`, never `RDW_UPDATENOW`, so no foreign thread is painted
synchronously). Pre-entry injection reaches the worker before any window
exists and does nothing. Because a process that acquired its
`IDWriteGdiInterop` before injection never calls `GetGdiInterop` again,
`hookDirectWrite` also seeds the interop and bitmap-render-target hooks once
per process through a 1x1 render target; a sandboxed process whose GDI calls
fail ends up exactly as before. Measured 2026-09-14 with a GDI probe injected
1.5 s after its first paint: the frame was replaced within about one second
without any application repaint, where the previous renderer left it
untouched until a forced invalidation.

The native x86/x64 contract proves both sides of that generation boundary. A
font retained while substitution is disabled remains the native source after a
new aliased collection is published. A source lookup in the active collection
returns a native object graph whose family, PostScript name, OpenType `name`
table, file/index descriptor, `IDWriteFontFace5` resource, and resource-created
face remain mutually consistent, while its metrics and glyph fingerprint agree
with the independently opened replacement.

The Firefox launch gate remains a deterministic test adapter for the real
late-injection order. It stops the parent before user entry and waits for the
core to become hook-ready; it is not itself a product substitution mechanism.
Firefox 151 has already built and retained its shared font collection before
that boundary, so an open-service injection at this point is reported as
`unsupported-late-collection`. The required lane proves the source remains
unchanged and that the prepared alias snapshot was not consumed by the retained
shared-font-list generation. A later, unrelated factory acquisition may return
the new alias generation and is recorded separately; it does not retroactively
make that acquisition early. The lane must not turn green through retained-object
mutation, a renderer pause, or a compatibility preference. Supporting this
Firefox path requires a browser adapter that runs before shared-font-list
creation.

Chromium's default FontDataService validates embedded font data outside the
DirectWrite collection metadata seam. The coherent virtual SFNT solves that
identity boundary: the bytes, native reference, and collection identity now
agree. It does not make a collection acquired before injection mutable. The WMI
service observer is therefore required to report an ordinary late Chromium
launch as `unsupported-late-collection`. The shipped MacLoader is the product
adapter for Chromium support because it injects before process entry; its
required lane must acquire the immutable alias collection early and match an
independent replacement render semantically or byte-for-byte with the default
feature set. Disabling `FontDataServiceAllWebContents` is not accepted as
product support or as a required-path success proof.


The native probes cover GDI and DirectWrite substitution before factory
creation, an isolated factory created before core load, existing and future
classic/DWriteCore factories, multiple immutable collection generations,
worker-thread creation, child renderer processes, and missing, valid, damaged,
selected-missing, and oversized profiles. Concurrent snapshot reload and
shutdown admission are focused module tests. CI retains the applicable JSON,
module inventory, profile, and exact binary artifacts; screenshot and
application-specific golden evidence remains a lab matrix item.

## Delivery sequence and final disposition

Work proceeded in bounded tranches. Items 2 through 5 are the completed large
renderer code program. Items 1, 6, and 7 include ongoing distribution or lab
evidence and must not be used to reopen the ownership architecture without a
new reproduced defect.

1. **Evidence baseline and alpha release:** Finish the real browser gate,
   capture stock/legacy/open-service comparisons, enable all `main` and core
   CI on this branch, establish the Cppcheck safety ratchet and sanitizer/leak
   job contracts, and publish only `alpha-` prereleases.
2. **RAII and runtime state — complete:** Ownership primitives and explicit
   runtime/capability state were introduced without importing the unrelated
   classic-C++ renderer branch.
3. **Substitution resolver — complete:** Parsing and family resolution use one
   immutable snapshot; chain, cycle, charset, reload, virtual-font identity,
   and variable-axis contracts cover the shared GDI/DirectWrite seam.
4. **DirectWrite lifecycle — complete at the supported collection seam:** The
   provisional shared-factory fix was replaced by the coordinator and now
   covers pre-existing shared/isolated factories, future classic/DWriteCore
   entry points, collections, and font sets. Retained pre-injection objects and
   application-private fallback/renderers remain explicit boundaries rather
   than mutable proxies.
5. **FreeType modernization — complete without a raster-policy change:**
   Ownership and caches were modernized and the fork ABI is verified. The
   pinned 2.14.3 fork remains because changing it without golden and
   performance evidence would add risk without a reproduced benefit.
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
DLL presence. Performance claims also require before/after CPU, allocation,
working-set, and render-time measurements. This tranche claims bounded
ownership and cache correctness, not a speedup or a preferred raster-output
change.

All branch releases remain GitHub prereleases. Tags, titles, installers,
integration/developer bundles, and checksum downloads use the `alpha-` prefix
so users cannot mistake this personal experimental core for the regular fork
or upstream MacType.
