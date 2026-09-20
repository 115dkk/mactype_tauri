# Alpha-plus refactor cherry-pick and semantic-port rules

This branch has deliberately deepened renderer and service Modules beyond the
file layout on `main`. A clean Git application is no longer evidence that a
change reached the correct owner. These rules apply after the renderer
activation, ProcessInjection, and ProfileRuntime refactor.

## Mandatory intake sequence

1. Fetch `origin/main` and record both the alpha HEAD and fetched main SHA.
2. Run patch-equivalence inspection before looking only at commit subjects.
   An equivalent patch is already present even if its SHA differs.
3. Classify each main-only change as a direct cherry-pick, semantic port,
   generated-contract change, or intentional non-port.
4. Apply product changes before later Unsafe, comment, or formatting passes so
   those passes do not conceal the source intent.
5. Run the affected Module tests after each intake commit, then run the full
   branch gates before delivery.

When the user explicitly schedules intake after a bounded refactor because the
main change is not merged yet, record the initial main SHA and repeat steps 1
through 5 immediately after that refactor. Do not silently treat the initial
inspection as the final intake.

## Direct cherry-pick

A main commit may be cherry-picked with `-x` when it is independent of moved
or deepened Modules and does not recreate a retired Interface. Typical
examples are isolated UI, translation, documentation, packaging, or CI fixes.

After applying it:

- inspect every changed path rather than trusting a conflict-free result;
- verify that no old root renderer source was recreated;
- keep product changes separate from fork-only documentation and CI changes;
- run the narrow affected gates before continuing.

## Semantic port

Treat a main change as a semantic port when it touches an old owner listed in
`docs/renderer-activation-architecture.md`, a renderer source that now lives
under `renderer/`, or host logic now owned by `ProcessInjection`.

For a semantic port:

1. Read the complete main diff and state its behavioral invariant.
2. Find the new Module and Interface that own that invariant.
3. Implement the intent behind that Interface. Do not transplant old global
   reads, raw strings, or duplicated lifecycle state.
4. Port or strengthen the original tests at the new test Interface.
5. Record the source SHA in the commit message with
   `Ported-From-Main: <sha>` and name any deliberately inapplicable hunk.

Never resolve a conflict by accepting one whole side when the file crosses a
deepened Module. A semantic port is reviewed as a new implementation of the
same invariant.

## Generated contracts

For renderer activation or settings contract changes, edit the canonical
schema and generator first, regenerate every Adapter, and commit the generated
outputs together. Never hand-edit a generated C++ or Rust definition.

A main commit that changes only one generated language is incomplete for this
branch. Port its intent to the schema, regenerate all consumers, and run the
drift gate plus cross-language golden tests.

## Path and owner map

| Main-era path or symbol | Alpha-plus destination |
| --- | --- |
| root renderer sources | corresponding source under `renderer/`; a cherry-pick must not recreate the root file |
| `CGdippSettings` reads in render code | `ProfileRuntime` / `RendererPolicySnapshot` Interface |
| helper JSON status parsing | generated HelperProtocol Adapter |
| `BrokerResult` string classification | typed `ProcessInjection` result |
| separate runtime/profile identity strings | `RendererRuntimeBinding` |
| renderer admission inside `DllMain` | `RendererActivation` Module with a thin Windows DLL Adapter |
| DirectWrite hook lifecycle additions | `DirectWriteLifecycle` Interface and the appropriate DirectWrite Adapter |
| FreeType global ownership changes | deep `FreeTypeRuntime` Implementation |

`codex/renderer-memory-safety` is never an intake source. It is a separate
classic-C++/no-RAII upstream experiment and must not be referenced, merged, or
cherry-picked into alpha-plus.

## Intake ledger

Add one row whenever a post-refactor main change is evaluated.

| Main SHA | Classification | Alpha commit | Tests | Notes |
| --- | --- | --- | --- | --- |
| `a55ed9cfabd1b585f00a465169a0701e921c53ab` | patch-equivalent baseline at first-phase start | existing history | branch baseline | Re-fetch required after first phase because the requested main change had not merged yet. |
| `2bca83fb53af241456c42e894b1ea1e6c83bea5f` | semantic partial port | `a9f1326` | i18n/settings and renderer relay gates | Ported the generic orphaned-setting translation gate. The setting removal is intentionally inapplicable because alpha's `ChildInjectionTransaction` actively consumes `HookChildProcesses`, and the default profile must keep it enabled for early child relay. |
| `d949dace4a946f3a254dbbcc20e3d8c3da43e031` | patch-equivalence baseline at 2026-09-04 intake start (alpha `ff23cf3`, merge-base `14eba4f`) | existing history | branch baseline | 24 main commits already had equivalents; the 22 rows below are the remainder. |
| `6e6dbded9b27cd50e2c6f4dccfb83f90235721af` | already present | `a4b0579` | i18n experiment gate | Alpha's copy of the Gemini harness differs only by alpha-only lint steps and the Unity catalog script; intentional non-pick. |
| `2cdd76fb0a4c8ee626ffd534baa119214cb45e1e` | already present | `3a02db8` | i18n experiment gate | Same as above. |
| `42955d90092d3fc2e557d781c370d6bb949039c3` | direct cherry-pick | `6d124c5` | frontend lint, i18n, settings, gallery 548/548 | Korean font subset regenerated from the merged catalog so alpha-only strings stay covered. |
| `ad6cad73d3e392f7fb67fddf97fbd063ac93dd81` | direct cherry-pick | `539521c` | gallery | Alpha follow-up `65bd479` repoints the child-process hooking proof at the switch role. |
| `7b6e38612123751106305957b8d461993f204470` | semantic port | `eccff44` (merge `ee663cd`) | x86 and x64 injector build and ctest, contract policy scripts | Alpha's inventory readers (`fixed_module_base`, path-name `remote_module_base`) were rewritten around main's retry driver instead of transplanted. |
| `f1ec23fa2a5678cb934b5a2b3519e68989eae297` | direct cherry-pick | `3f7b9c4` | platform tests 58/58 | Platform crate introduced. |
| `d66009428a85cd462467fcbf39658cb457db7286` | direct cherry-pick | `4664923` | platform tests | |
| `80a361983b14f94a87d4a620478ad6499f143759` | direct cherry-pick | `269bfa3` | platform tests | |
| `c15c3348db6aa7e8fe934ec6f9ddbf635103a184` | direct cherry-pick | `14237e7` | platform tests | |
| `cfb6b97a3f9088c01a7115af30d53371c342ded8` | direct (tree snapshot) | `0b9c7c2` | platform tests | Alpha never edited `platform/`, so the remaining platform hunks were taken as one snapshot identical to main's tree. |
| `92c941efb9b6915c0f495a33ade8b87e2751ddcf` | direct (tree snapshot) | `0b9c7c2` | platform tests | |
| `1be0beefb0db31d22baa18c7d57df6a011ce368a` | semantic port | `99bb07b` (merge `fcbc8fd`) | workspace fmt, clippy, test; relay audit | Platform hunks live in `0b9c7c2`. Host keeps known-versus-unavailable mitigation evidence through new fallible platform queries and the 1536-byte helper bound. A WMI row returned without an object now reads as exhaustion, as on main. |
| `870c1f487ba34113bafb28a84b19bd32c1e9125f` | semantic port | `99bb07b` | launcher tests | Exact-process-object wait ported with alpha's RendererRuntimeBinding fixtures. |
| `ec3b9edc8900adef80879f4f1dded67cd8f88584` | semantic port | `9aebf6a` (merge `7dce6ce`) | workspace fmt, clippy, test; relay audit | Alpha-only `security_acl.rs` deleted in favour of the platform readers; bounded tree check, machine-lock DACL policy, and the profile liveness lease unchanged. |
| `cbc05d3e5985015439a983196279180ebfade43f` | direct cherry-pick | `1f39eff` | docs | |
| `112b7fdfb6d8f57e1763b3d7f7e7bdbc5be86646` | semantic port (end state) | `ea066df` (merge `17d4392`) | src-tauri fmt, clippy, test 239/239; policy scripts; relay audit | The crate-local `scm_response.rs` main later removed was never recreated; bounded reads use the platform `ScmResponse`. |
| `8b5b2a3dbc2418772f8ef6048aa4cdb508738aef` | semantic port | `ea066df` | as above | |
| `594fbcae25fc5c4cd24b1c3bcbbae851e87c3f3d` | direct (inside `ea066df`) | `ea066df` | distribution policy script | |
| `c6044f35f9d350648c0cb66ae2b2ed26c11a5d55` | semantic port | `ea066df` | as above | Legacy MacTray control, snapshot, restore, and picker routed; alpha error classification kept. |
| `449459d6bdbc7776f28cb3c255f2a3c894825985` | semantic port | `ea066df` | as above | Profile-transfer pipes, nonce, fonts, clipboard routed; `#![forbid(unsafe_code)]` on the Tauri crate. |
| `44739a7081889703ea8bacc7600f9ab5354afc60` | direct cherry-pick | `852929c` | docs | |
| `f84999b7a958f6daa74e91a8cd98d54c7d4d2d25` | intentional non-port | (none) | n/a | The Preview Studio (`src/studio/*`, `preview_studio.rs`, the `preview-studio` capability, `studio.*` catalog keys) left `main` on 2026-09-13 (fork PR #60, merge `678720d`) by user decision; alpha keeps the Studio as an experiment. The `nativePreview.*` key renames are not needed here because the Studio keys stay. |
| `b706ac59cb0e79435ebbfa5493f07d9baffc76bf` | intentional non-port | (none) | n/a | Fork-only Studio gallery tests dropped on `main`; alpha keeps them with the Studio. |
| `54cf411a88cf446c6f9e4b0685f84c251930c123` | intentional non-port | (none) | n/a | The Tauri smoke gate on `main` no longer launches the Studio; alpha's gate keeps the `preview-studio` view. |
| `12dd0b7b650cf5e07d7b02faef631e263c319426` | already present | `7b8ec3c8415a1f4ed87fd1b2579a58e7c04fee92` | service-runtime fmt/clippy/test, three driver recovery tests, `interpret_next_result` unit test | WMI observer recovery (failed `Next` surfaced, in-process resubscription with snapshot reconciliation) originated on alpha (`codex/alpha-paint-race`, merged as `5458deb`) and was ported to `main` by user choice (fork PR #61, merge `1acd331`); main's driver has no deferral policies, so the port carries an `initialize_process_orchestration_with_observer_recovery` constructor instead of the extra policy parameter. |
| `75b1e318dc727637e5e6be389e7f4b8701bea68c` | semantic port | `771cf6e` + `1b37074` | setup fmt/clippy/test (53 unit, 23 install-bootstrap, 26 profile-store), installer script byte-identical to the copy ISCC-checked on `main` | Installer start-choice page and the `bootstrap-install-preserve-run-state` verb. Alpha's bootstrap used to reach Ready on every install and then stop an observed-stopped service, which also cleared the DLL-adjacent relay lease. The preserve policy no longer starts a stopped service, but `PreserveExisting` still re-materialises that lease, so `1b37074` clears it again in the no-start branch; a stopped service therefore stays the supported stopped state at boot. `bootstrap-install` keeps main's ensure-Running meaning. |
| `38ea9ec0118b02b80b1e5a5d468958a44e68beab` | direct cherry-pick | `1869a01` | hosted installer E2E (Build and package on push) | Never-started install and opt-in `/STARTSERVICE=1` scenarios; alpha's open-core lineage assertion is untouched. |
| `c246b17b37592c85db7af1a3fe4cb5647343df0a` | semantic port | `09cb866` | i18n gate (10 locales), settings gate, eslint, tsc/vite build, Control Center fmt/clippy/test, browser gallery | Run profile (실행 프로필) designation without starting the service. Alpha's Files page is a thin view over `features/files/useFileSettingsModel`, so the designate and start-now logic lives in that hook, and the Console, Cupertino, and Fluent Files pages carry the run-profile badge, the designate button, and the start-now text action. The Cupertino and Fluent execution descriptions pass `{name}` like the Classic page. The overview badges that used the retired `files.appliedBadge` use an alpha-only `files.inUseBadge`. |
| `b973303d8ad853c28308eea2a4e99c234807e666` | direct cherry-pick | `c6f3361` | browser gallery | Held, live, and no-service designation coverage. |
| `d801c2a626bd823944618f29bff6d7838b7e30f7` | direct cherry-pick | `25d1b5b` | i18n gate | The service page's start switch says "Start service" again. |
| `91d662a80509839dcb56c57c4ac71ca18b5bc037` | direct cherry-pick | `25eef8b` | browser gallery | Addresses the start switch by class now that three buttons share the label. |
| `10c72b2f563dd8e26a11aaed4c5b16fe9fbe8a63` | semantic port | `b1a33c4` | workflow-model contract; five workflow policies; open-service policy modules | One bounded `WorkflowModel` now owns workflow parsing. The five function-based policies keep alpha's FreeType ABI, payload-lineage, alpha snapshot-release, and browser-renderer proof rules. |
| `a1245b72881b9768012bd5108466319210e12467` | intentional non-port | `e577130` | workflow-model contract | The requested work scope forbids `.github/**` edits. The coordinator must add the `Test-WorkflowModelContract.ps1` step to alpha's `open-core-policy` lint job. |
| `c0dc90d5cabc7e9e356b44955e319aed9f515ac8` | generated-contract change | `4a198f9` | preview helper Win32 build; ctest 3/3; preview protocol drift | Ported the runtime split and generated native-preview contract through alpha's Preview Studio owner: the plain engine and second helper slot remain, native profile settings are re-applied after strip renders, and all four alpha skin palettes and metrics now live in the canonical JSON. |
| `f439ae191b716f757ca2161125445f3b94f12813` | semantic port | `b1a33c4` + `e577130` | preview protocol drift | Ported `Test-PreviewProtocolDrift.mjs`. The coordinator must add the generation, generated-output drift, and MTPC drift steps to alpha's build and lint workflows because this worker was forbidden to edit `.github/**`. |
| `d42e6f8de3f99fd976e94defbe380b3915910ee6` | direct cherry-pick | `a5ad3cc` | injector x64 and Win32 build; ctest 2/2 on each | Marker metadata is written to a sibling partial file and atomically renamed into place. |
| `55c4c8ee67c4bf8c6e9f9c36985ae98c1b06caf1` | semantic port | `7fac14a` | host target-validation 13/13; complete host suite | Alpha already had the stronger invariant: `WindowsProcessInspector` returns `ProcessInspection` facts and `ProcessTargetValidator` alone owns `ProcessSkipReason`, including Unity, private-FreeType, console, frozen, and exiting outcomes. This intake retained that owner and moved the expanded decision table onto the shared scripted inspector fixture. |
| `7b262fe110b284a0c5b8e10e1d21eaaa4ef81d45` | semantic port | `7fac14a` | contract, setup, and host crate tests with all features | `mactype-service-contract` now owns service configuration, broker verbs, pointer bounds, and rollback exit semantics. Alpha keeps `RendererRuntimeBinding`, Unity/profile exports, the stopped-service relay lease, and the 1536-byte renderer-evidence helper bound. |
| `f31d07b5e5913adb51fad226d11aa8cfc412467f` | semantic port | `7fac14a` | platform file tests 8/8; legacy migration 29/29; open-service 74/74 | `mactype-service-platform::validate_path_chain` is the owner for every reparse check. Alpha's legacy-migration pure test uses `validate_path_chain_with`; its existing final-handle reparse checks remain. |
| `3af852fcd642419907fad71901d5e45604826113` | direct cherry-pick | `7fac14a` | MachineIntegration 170 passed, 8 live-only ignored | Removed only the test-only `StartupDisableBackend` seam; alpha's production startup coordinator and receipt flow remain. |
| `0b395fd2f5399ca66353fbbf8e7f8af3a1f9ed91` | semantic port | `7fac14a` | MachineIntegration 170 passed, 8 live-only ignored | `ActionFailure`, `ActionBlocker`, and `RollbackOutcome` now carry policy across alpha's migration, relay-lease, paused-runtime, broker, and publication flows; user prose is rendered at one boundary. |
| `8df03e458c70ef169ebe97ca1988991c768ad5fa` | direct cherry-pick | `7fac14a` | contract tests with all features | The four filesystem-backed service-configuration tests are skipped only under Windows Miri; pure identity and drift rows still run. |
| `596133931bf0e47e2d257ba28d475546dfe48c87` | semantic port | `7fac14a` | complete host suite; event integration 30/30; deferred 11/11; validator 13/13 | `HostEventSink` is injected from SCM through lifecycle, observer recovery, alpha admission, helper broker, and orchestrator. Shared fixtures were adapted to `RendererRuntimeBinding` and also cover alpha's image-subsystem, Unity, private-FreeType, and console-grace behavior. |
| `20a73e515a4963fcc82bc161bb2ef59c6b74e13f` | direct cherry-pick | `7fac14a` | platform tests 84/84 | `bounded_read` now owns probe/fill and retry/grow reads for SCM security/configuration, token information, and registry readers without changing public signatures. |
| `3e4eece77a2986bfc8550e7e1b5af733d72a6ea8` | semantic port | `7fac14a` | designation plan 1/1; designation flows 4/4; designation serialization 1/1 | `MachineIntegration::designation_plan` decides live, next-start, or local-only once. Alpha's pause marker changes only after a live publication, while stopped designation keeps the setup broker's relay-lease semantics. |
| `54a867d5aebc3940404aa3a9c3f36ca9fe7b3f52` | direct cherry-pick (patch-equivalent after semantic port) | `7fac14a` | event integration 30/30 | The three file-backed event integration tests already received the Windows Miri exclusions while porting `5961339`; applying this commit produced no additional behavior change. |
| `44041bc37917a41ba28eb640e0385ebb024bfd05` | semantic port | `2160941` | i18n (10 locales, 799 messages), settings, eslint, tsc/vite build, source comments, alpha branch policy, full gallery 1119 (1060 passed, 59 skipped) | `runtime()` replaces the forwarding `app/tauri.ts` in every alpha consumer, the skins, the Unity picker and the Studio included; `features/profiles/useProfileDocument.ts` owns the document, the snapshots and the guarded save/designate/start-now flow with `app/profilePreference.ts` holding the one precedence including the managed-legacy fallback; `app/executionViewModel.ts` owns the presentation edges for all four skins. Named page-model groups, Unity lists and the Studio stay. |
| `811cf1c44d5a8199c00bfb43cf497cb54a599008` | semantic port | `2160941` | catalog identity audit against 841e4a8, i18n gate, full gallery as above, completeness grep | The guided vocabulary and the `guided`/`all` modes reach alpha's models, four Tuner skins, `app.css` and the skin stylesheets, all ten catalogs (38 keys renamed, values and order unchanged) and the gallery specs; `nav.wizardGroup` and the Wizard navigation area keep their names. CONTEXT.md carries the new mode values. |

An intentional non-port needs a concrete reason, such as fork-only behavior,
an invariant already satisfied by a stronger Module, or a change made obsolete
by the refactor. “Conflicted” is not a reason.

## Delivery proof

The intake is not complete until the final alpha commit is pushed and every
applicable GitHub Actions run for that exact SHA completes successfully. The
final report names the main SHA range, classification of every main-only
commit, alpha commit, remote SHA, and exact-SHA CI results.
