# Project context

This file fixes the domain language used by code, tests, CI, and architecture documents. A name in this glossary is part of the module interface; do not replace it with a near-synonym in a new implementation.

## Domain language

**MachineIntegration**
: The module that presents installation, health, profile publication, migration, repair, and rollback as one user-facing machine-integration model. Its interface is consumed by the frontend; Windows SCM, protected storage, and legacy detection remain implementation details behind its seam.

**신식 서비스**
: The open-source Windows service owned by MacType Control Center. Its fixed production SCM name is `MacTypeControlCenter`; ownership requires the quoted fixed protected service image followed by ` --service`, the own-process service type, and the LocalSystem account. Other SCM registration fields are configuration, not identity. It observes eligible processes, selects a fixed helper, publishes versioned health, and reads only administrator-protected runtime and profile generations. English documents may add “new service” in parentheses, but the Korean product term is always **신식 서비스**.

**ServiceConfigurationDrift**
: A difference between an owned 신식 서비스's SCM configuration and its fixed start type, error control, display name, load order group, tag (only when a load order group is present), or dependencies. It does not make the service foreign. Control Center offers repair or upgrade, and the setup broker restores the fixed configuration through `reconfigure`.

**레거시 서비스**
: The original `MacType` Windows service hosted by `MacTray.exe`. It is detected, backed up, stopped, restored, or removed only by the explicit migration flow. It is never a normal dependency of the 신식 서비스. English documents may add “legacy service” in parentheses, but the Korean product term is always **레거시 서비스**.

**generation**
: An immutable, digest- or version-addressed runtime or profile directory under a protected machine root. Activation changes a small durable pointer; it never edits an active generation in place.

**RendererRuntimeBinding**
: The typed pairing of one verified protected runtime generation and one verified protected profile digest. `ProtectedRendererRuntime` constructs it only while both active pointers and their content remain stable. Process observation, helper invocation, telemetry, and renderer evidence carry this value as one Interface; callers never recombine two unrelated strings.

**ProcessInjection**
: The host Module that consumes a verified target decision and `RendererRuntimeBinding`, applies exact-identity de-duplication, retry, cancellation, liveness recheck, bounded result history, telemetry, and terminal health impact. `InjectionOrchestrator` is its orchestration Interface. Helper diagnostic text is evidence only; typed disposition owns policy. Normal target skips and verified pre-injection rejection do not change global service health. Frozen targets and pre-resume helper launch failures are deferred, re-checked from a bounded set, and never change global health.

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

**ConsoleProcessAdmission**
: The opt-in process-admission policy selected by `SkipConsoleProcesses`. The Windows inspector and `ChildInjectionTransaction` read only the PE optional header of the exact executable and treat `IMAGE_SUBSYSTEM_WINDOWS_CUI` as a console image: a console image becomes an explicit process-local skip before service or child-relay injection, GUI images (including terminal hosts such as conhost and Windows Terminal) stay eligible, and an unreadable header stays eligible. Independently of the option, the 신식 서비스 defers a console image younger than the console grace period and injects it only if it is still alive, so a millisecond-lived tool never receives the renderer. It is never an executable-name list.

**explicit process-local skip**
: A normal `InjectionOrchestrator` result for one verified `(pid, creation time)` that cannot or must not receive hooks. Its exact reason is retained in the bounded process-result registry, repeat observation becomes `Duplicate`, and another process with the same executable name remains eligible. It never creates a global health error, warning notification, or health-protocol extension.

**HookCoordinator**
: The process-wide renderer module that owns `Uninitialized`, `Starting`, `Active`, `Failed`, and `Stopping` admission, concrete hook-target attempts, duplicate suppression, first-failure evidence, and transactional stop. Provider-specific DirectWrite, DWriteCore, D2D, GDI, child-injection, and substitution adapters publish capability results through this interface instead of inferring global state independently.

**DirectWriteLifecycle**
: The rendering module that owns classic DirectWrite and app-local DWriteCore discovery, concrete entry hooks, existing shared and isolated factory readiness, direct native-loader coverage for future app-local modules, worker cancellation, module pins, diagnostics, and explicit-unload ordering. Its interface is process-lifetime start plus transactional stop; loader interception and provider-specific state remain implementation details behind its seam. A font collection returned before injection remains an immutable older generation.

**UnityFontHookLifecycle**
: The renderer Module that adapts allowlisted UnityPlayer generations whose text is rendered by Unity's private FreeType. Its startup-only Interface consumes `RendererPolicySnapshot`, validates exact PE/PDB identity, render and face-open ABIs, and target bytes, owns the UnityPlayer module reference, and substitutes only copied pathname-backed FreeType face-open arguments. Exact OS `FontRef` resolver adapters carry a scoped native-or-mapped family selection so a shared font file cannot redirect an unrelated family; older exact adapters use the checked path-and-face-index fallback. It applies the immutable substitution snapshot and Unity coverage lookup tables and participates in transactional renderer drain. Memory and stream faces are never redirected, a failed replacement face retries the original arguments, and diagnostic observation never changes a returned face. Unknown Unity builds fail unavailable without signature scanning. The 신식 서비스 owns anti-cheat process admission; the renderer repeats a bounded fail-closed scan only as direct-injection defense.

**FontSubstitutionSnapshot**
: The immutable, reference-counted rule generation consumed by both GDI and DirectWrite adapters. It owns case-insensitive rule identity, charset precedence, deterministic chains, cycle/depth failure, the bold substitution method with its explicit bold pairs, generation, and digest. Reload publishes a new snapshot; in-flight rendering keeps the old one and no adapter reads the mutable profile rule map.

**굵은 글꼴 대체 방식 (bold substitution method)**
: The `FontSubstitutesBold` policy inside FontSubstitutionSnapshot that decides what a substituted family does when a bold-class face (weight 600 or more) is requested: `0` ignores the weight, `1` forces a synthetic bold, `2` selects the bold face of the replacement's own typographic family (the nearest heavier weight when no 700 exists; default), and `3` uses the `[FontSubstitutesBold]` pairs keyed by the resolved replacement family. Both GDI and DirectWrite adapters apply the same selection rule; the alias collection gives a source family that has no bold-class face of its own one synthesized bold slot so DirectWrite can honour the method, and a text format created against the native collection reaches the same decision through the GDI request. In mode `3` the default relation of a replacement family is **없음** (no pair line): the replacement face is used as is and the weight is ignored, and a malformed pair line or an uninstalled paired family means the same. A pair never redirects to a family the user did not choose, and 없음 never blocks saving.

**FreeTypeRuntime**
: The renderer module that owns the paired FreeType library and cache manager, bounded stream reads, checked top-down logical bitmap rows for either pitch sign, one-based face-ID validation, bitmap byte accounting, typed raster cache keys, and the immutable per-render `RasterPolicy`. Its release order is manager before library. A constructed face receives a separate callback-owned stream backing; its builder is never callback-owned. The pinned fork and private `FT_Glyph_To_BitmapEx` ABI are part of this interface.

**PeExportView**
: The renderer module that resolves a named export function RVA or export-address-table slot RVA from a bounded raw-file or loader-mapped PE view. It validates headers, section and export arrays, strings, forwarders, arithmetic, and readable mapped ranges. It never copies an image into executable memory, relocates it, or calls its entry point.

**RendererUnloadLifecycle**
: The renderer module that serializes explicit unload attempts, owns the active renderer's self-reference, and concentrates provider drain through one transaction shared by the DirectWrite and Unity adapters. A balanced caller release of its own `LoadLibrary` reference cannot unmap live hook code; Windows module references are not owner-tagged, so unmatched or repeated `FreeLibrary` calls are unsupported. The `SafeUnload` thread-procedure Interface drains workers, hook leases, FreeType, policy, substitution, and settings outside the loader lock, then releases the self-reference atomically with thread exit; stopped mutable exports reject work, and failed preparation reopens retry admission. Quiet-skip renderers do not acquire this lease, supported explicit detach retains only final TLS-slot and empty lock-storage release, and process termination leaves cleanup to Windows.

**ChildInjectionTransaction**
: The synchronous core module behind the `CreateProcessInternalW` hook. It creates an eligible child suspended, verifies its exact handle-bound identity and safety facts, binds only the current core's fixed adjacent DLL while that generation's adjacent `MacType.ini` still exists, and then restores the caller-requested thread state. A retired generation stops propagating immediately even when its DLL remains mapped in a long-lived parent. Quiet skips never become image-wide bans or service alerts; an unprovable in-flight mixed-helper mutation terminates the new child rather than resuming uncertain state. When `SkipConsoleProcesses` is enabled it reads the verified child image's bounded PE headers before entry and quietly skips `IMAGE_SUBSYSTEM_WINDOWS_CUI` children while preserving the caller-requested thread state; GUI and unreadable images remain eligible.

**fixed helper**
: The adjacent x86 or x64 `mactype-injector` executable selected by architecture. Its Interface accepts only an inherited process handle, exact process identity, and `RendererRuntimeBinding`. It cannot accept an arbitrary DLL, executable, command line, service name, or profile path. It reports success only after validating `RendererActivationEvidence`; an explicit `QuietSkip` unloads only the helper-owned module reference and stays process-local.

**ExecutionViewModel**
: The frontend-facing model derived from MachineIntegration state. It chooses user-visible actions and explanations without teaching React about SCM flags, registry layouts, helper processes, or migration receipt internals.

**Skin (스킨)**
: One of four selectable presentations of the Control Center, chosen from the navigation preference menu beside the language picker and persisted per user. Skin ids (`classic`, `fluent`, `console`, `cupertino`) are interface and appear as `html[data-skin]`; labels may change. A skin is a shell that arranges the shared page models in its own paradigm plus a scoped stylesheet; it never owns an action, a message, or an IPC call. The classic skin is the default and the gallery baseline.

**Preview Studio (프리뷰 스튜디오)**
: The second window (`preview-studio`) that renders one sample through the preview helper across fonts, sizes and styles and compares two sources at integer zoom. Its sources are the Tuner's edits, the saved profile, a profile file, and Windows' own rendering from the plain helper engine. It follows the Tuner document over the studio bridge and never applies or saves anything.

**Event (이벤트)**
: One typed line of the unified event log (`docs/event-log-policy.md`): a severity, an area, a stable code, short parameters, optional technical detail, and the writing process. The Control Center, the 신식 서비스 host and the setup broker write events; the UI localises codes and shows detail only on request. Health, findings and the helper's diagnostic ring are separate evidence and never become events by themselves.

**Wizard 영역**
: The navigation area that answers "what runs, and how": profile selection and apply (view id `files`) plus 구동 방식 and service control (view id `execution`). It inherits the role of the legacy MacWizard. The Korean product term is always **위자드**; never write 마법사. View ids are frozen interface — labels may differ from ids.

**실행 프로필 (run profile)**
: The profile the 신식 서비스 uses whenever it runs: the current user's applied pointer, published to the protected profile store. Setting it never starts or stops the service: a running service switches to it live, a stopped one holds it for its next start, and an absent service receives it when it is installed and started. The bundled `ini\Default.ini` is the run profile until another is set. The Korean product term is always **실행 프로필**; do not write 활성 프로필 or 적용 프로필 for this concept.

**Tuner 영역**
: The navigation area that answers "how the profile content is shaped": profile editing (view id `profiles`, modes guided/all). It inherits the role of the legacy MacTuner. The Korean product term is always **튜너**.

**Guided setup (단계별 설정)**
: The guided editing mode inside the Tuner 영역 (internal `profileMode: "guided"`). It curates a fixed subset of settings by quiet omission and never uses wizard/마법사 vocabulary, which belongs exclusively to the Wizard 영역.

**All settings (전체 설정)**
: The full editing mode inside the Tuner 영역 (internal `profileMode: "all"`): every setting group, search, and the revert/restore-default/reset toolbar.

## Responsibility map

- React and `ExecutionViewModel` own presentation and user intent.
- Tauri's MachineIntegration adapter translates fixed user actions into system commands and read-only state.
- Rust setup and host modules own SCM, protected generations, observation, health, recovery, rollback, and bounded process-local skip evidence.
- The fixed helper and public MacType C/C++ code own injection and rendering.
- `renderer/` is the physical Implementation root of the `MacType.Core.dll` Module: hooks, settings, rendering Adapters, lifecycle code, and renderer-owned resources stay local there. Workspace build manifests remain at the repository root, while cross-Module contracts remain under `shared/`.
- The 레거시 서비스 remains a migration subject and fallback only; it is not part of normal operation.

## Architecture language

Architecture work uses **module**, **interface**, **implementation**, **seam**, **adapter**, **depth**, **leverage**, and **locality** in their standard repository meanings. The interface is the test surface. Add a seam only when at least two adapters actually vary, and prefer a deep module whose invariants stay local over pass-through modules that spread Windows knowledge across callers.
