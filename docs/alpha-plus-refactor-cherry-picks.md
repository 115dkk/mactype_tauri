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

An intentional non-port needs a concrete reason, such as fork-only behavior,
an invariant already satisfied by a stronger Module, or a change made obsolete
by the refactor. “Conflicted” is not a reason.

## Delivery proof

The intake is not complete until the final alpha commit is pushed and every
applicable GitHub Actions run for that exact SHA completes successfully. The
final report names the main SHA range, classification of every main-only
commit, alpha commit, remote SHA, and exact-SHA CI results.
