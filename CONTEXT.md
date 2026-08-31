# Project context

This file fixes the domain language used by code, tests, CI, and architecture documents. A name in this glossary is part of the module interface; do not replace it with a near-synonym in a new implementation.

## Domain language

**MachineIntegration**
: The module that presents installation, health, profile publication, migration, repair, and rollback as one user-facing machine-integration model. Its interface is consumed by the frontend; Windows SCM, protected storage, and legacy detection remain implementation details behind its seam.

**신식 서비스**
: The open-source Windows service owned by MacType Control Center. Its fixed production SCM identity is `MacTypeControlCenter`. It observes eligible processes, selects a fixed helper, publishes versioned health, and reads only administrator-protected runtime and profile generations. English documents may add “new service” in parentheses, but the Korean product term is always **신식 서비스**.

**레거시 서비스**
: The original `MacType` Windows service hosted by `MacTray.exe`. It is detected, backed up, stopped, restored, or removed only by the explicit migration flow. It is never a normal dependency of the 신식 서비스. English documents may add “legacy service” in parentheses, but the Korean product term is always **레거시 서비스**.

**generation**
: An immutable, digest- or version-addressed runtime or profile directory under a protected machine root. Activation changes a small durable pointer; it never edits an active generation in place.

**RendererRuntimeBinding**
: The typed pairing of one verified protected runtime generation and one verified protected profile digest. `ProtectedRendererRuntime` constructs it only while both active pointers and their content remain stable. Process observation, helper invocation, telemetry, and renderer evidence carry this value as one Interface; callers never recombine two unrelated strings.

**ProcessInjection**
: The host Module that consumes a verified target decision and `RendererRuntimeBinding`, applies exact-identity de-duplication, retry, cancellation, liveness recheck, bounded result history, telemetry, and terminal health impact. `InjectionOrchestrator` is its orchestration Interface. Helper diagnostic text is evidence only; typed disposition owns policy. Normal target skips and verified pre-injection rejection do not change global service health.

**RendererActivationEvidence**
: The versioned, fixed-width C/Rust evidence returned by the renderer while the helper owns a module reference. It binds exact PID, creation time, session, architecture, `RendererRuntimeBinding`, module-load origin, renderer admission, lifecycle revision, and capability sets. `Active`, `QuietSkip`, and `Failed` are distinct. Health v1 is derived from this evidence and remains wire-compatible.

**ProfileRuntime**
: The renderer Module that publishes only a complete immutable `RendererPolicySnapshot`. It holds the selected profile under a stable read lease through parsing, hashes its exact bytes, binds font substitution to the same revision, and preserves the preceding snapshot when publication fails.

**RendererPolicySnapshot**
: The immutable policy generation consumed by renderer Implementations. It contains the effective profile digest, hook/FreeType/DirectWrite policy, raster policy, font settings, and font-substitution snapshot. One render request retains one snapshot generation and never rereads mutable profile maps.

**ProcessTargetValidator**
: The host module that consumes typed facts for one observed PID and returns a verified eligible identity or an explicit process-local skip. The Windows inspector owns fact collection only; the validator owns self, session-zero, protected, critical, image-name, mitigation, target-race, and PID-mismatch classification. It does not own generation binding, retry, result history, health, or notifications.

**PrivateFreeTypeAdmission**
: The opt-in process-admission policy selected by `SkipPrivateFreeType`. The Windows inspector and `ChildInjectionTransaction` read at most 64 MiB from the exact executable through a fixed-size streaming buffer and recognize only an explicit `windows:fontengine=freetype` marker in ASCII or UTF-16. A detected non-Unity process becomes an explicit process-local skip before injection; unavailable or ambiguous evidence remains eligible. A verified Unity installation selected by `UnityFontHookLifecycle` is exempt.

**explicit process-local skip**
: A normal `InjectionOrchestrator` result for one verified `(pid, creation time)` that cannot or must not receive hooks. Its exact reason is retained in the bounded process-result registry, repeat observation becomes `Duplicate`, and another process with the same executable name remains eligible. It never creates a global health error, warning notification, or health-protocol extension.

**HookCoordinator**
: The process-wide renderer module that owns `Uninitialized`, `Starting`, `Active`, `Failed`, and `Stopping` admission, concrete hook-target attempts, duplicate suppression, first-failure evidence, and transactional stop. Provider-specific DirectWrite, DWriteCore, D2D, GDI, child-injection, and substitution adapters publish capability results through this interface instead of inferring global state independently.

**DirectWriteLifecycle**
: The rendering module that owns classic DirectWrite and app-local DWriteCore discovery, concrete entry hooks, existing shared and isolated factory readiness, direct native-loader coverage for future app-local modules, worker cancellation, module pins, diagnostics, and explicit-unload ordering. Its interface is process-lifetime start plus transactional stop; loader interception and provider-specific state remain implementation details behind its seam. A font collection returned before injection remains an immutable older generation.

**UnityFontHookLifecycle**
: The renderer Module that adapts allowlisted UnityPlayer generations whose text is rendered by Unity's private FreeType. Its startup-only Interface consumes `RendererPolicySnapshot`, validates exact PE/PDB identity, render and face-open ABIs, and target bytes, owns the UnityPlayer module reference, and substitutes only copied pathname-backed FreeType face-open arguments. Exact OS `FontRef` resolver adapters carry a scoped native-or-mapped family selection so a shared font file cannot redirect an unrelated family; older exact adapters use the checked path-and-face-index fallback. It applies the immutable substitution snapshot and Unity coverage lookup tables and participates in transactional renderer drain. Memory and stream faces are never redirected, a failed replacement face retries the original arguments, and diagnostic observation never changes a returned face. Unknown Unity builds fail unavailable without signature scanning. The 신식 서비스 owns anti-cheat process admission; the renderer repeats a bounded fail-closed scan only as direct-injection defense.

**FontSubstitutionSnapshot**
: The immutable, reference-counted rule generation consumed by both GDI and DirectWrite adapters. It owns case-insensitive rule identity, charset precedence, deterministic chains, cycle/depth failure, generation, and digest. Reload publishes a new snapshot; in-flight rendering keeps the old one and no adapter reads the mutable profile rule map.

**FreeTypeRuntime**
: The renderer module that owns the paired FreeType library and cache manager, bounded stream reads, checked top-down logical bitmap rows for either pitch sign, one-based face-ID validation, bitmap byte accounting, typed raster cache keys, and the immutable per-render `RasterPolicy`. Its release order is manager before library. A constructed face receives a separate callback-owned stream backing; its builder is never callback-owned. The pinned fork and private `FT_Glyph_To_BitmapEx` ABI are part of this interface.

**PeExportView**
: The renderer module that resolves a named export function RVA or export-address-table slot RVA from a bounded raw-file or loader-mapped PE view. It validates headers, section and export arrays, strings, forwarders, arithmetic, and readable mapped ranges. It never copies an image into executable memory, relocates it, or calls its entry point.

**RendererUnloadLifecycle**
: The renderer module that serializes explicit unload attempts, owns the active renderer's self-reference, and concentrates provider drain through one transaction shared by the DirectWrite and Unity adapters. A balanced caller release of its own `LoadLibrary` reference cannot unmap live hook code; Windows module references are not owner-tagged, so unmatched or repeated `FreeLibrary` calls are unsupported. The `SafeUnload` thread-procedure Interface drains workers, hook leases, FreeType, policy, substitution, and settings outside the loader lock, then releases the self-reference atomically with thread exit; stopped mutable exports reject work, and failed preparation reopens retry admission. Quiet-skip renderers do not acquire this lease, supported explicit detach retains only final TLS-slot and empty lock-storage release, and process termination leaves cleanup to Windows.

**ChildInjectionTransaction**
: The synchronous core module behind the `CreateProcessInternalW` hook. It creates an eligible child suspended, verifies its exact handle-bound identity and safety facts, binds only the current core's fixed adjacent DLL while that generation's adjacent `MacType.ini` still exists, and then restores the caller-requested thread state. A retired generation stops propagating immediately even when its DLL remains mapped in a long-lived parent. Quiet skips never become image-wide bans or service alerts; an unprovable in-flight mixed-helper mutation terminates the new child rather than resuming uncertain state.

**fixed helper**
: The adjacent x86 or x64 `mactype-injector` executable selected by architecture. Its Interface accepts only an inherited process handle, exact process identity, and `RendererRuntimeBinding`. It cannot accept an arbitrary DLL, executable, command line, service name, or profile path. It reports success only after validating `RendererActivationEvidence`; an explicit `QuietSkip` unloads only the helper-owned module reference and stays process-local.

**ExecutionViewModel**
: The frontend-facing model derived from MachineIntegration state. It chooses user-visible actions and explanations without teaching React about SCM flags, registry layouts, helper processes, or migration receipt internals.

**Wizard 영역**
: The navigation area that answers "what runs, and how": profile selection and apply (view id `files`) plus 구동 방식 and service control (view id `execution`). It inherits the role of the legacy MacWizard. The Korean product term is always **위자드**; never write 마법사. View ids are frozen interface — labels may differ from ids.

**Tuner 영역**
: The navigation area that answers "how the profile content is shaped": profile editing (view id `profiles`, modes guided/all). It inherits the role of the legacy MacTuner. The Korean product term is always **튜너**.

**Guided setup (단계별 설정)**
: The guided editing mode inside the Tuner 영역 (internal `profileMode: "quick"`). It curates a fixed subset of settings by quiet omission and never uses wizard/마법사 vocabulary, which belongs exclusively to the Wizard 영역.

**All settings (전체 설정)**
: The full editing mode inside the Tuner 영역 (internal `profileMode: "advanced"`): every setting group, search, and the revert/restore-default/reset toolbar.

## Responsibility map

- React and `ExecutionViewModel` own presentation and user intent.
- Tauri's MachineIntegration adapter translates fixed user actions into system commands and read-only state.
- Rust setup and host modules own SCM, protected generations, observation, health, recovery, rollback, and bounded process-local skip evidence.
- The fixed helper and public MacType C/C++ code own injection and rendering.
- `renderer/` is the physical Implementation root of the `MacType.Core.dll` Module: hooks, settings, rendering Adapters, lifecycle code, and renderer-owned resources stay local there. Workspace build manifests remain at the repository root, while cross-Module contracts remain under `shared/`.
- The 레거시 서비스 remains a migration subject and fallback only; it is not part of normal operation.

## Architecture language

Architecture work uses **module**, **interface**, **implementation**, **seam**, **adapter**, **depth**, **leverage**, and **locality** in their standard repository meanings. The interface is the test surface. Add a seam only when at least two adapters actually vary, and prefer a deep module whose invariants stay local over pass-through modules that spread Windows knowledge across callers.
