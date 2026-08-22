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

An intentional non-port needs a concrete reason, such as fork-only behavior,
an invariant already satisfied by a stronger Module, or a change made obsolete
by the refactor. “Conflicted” is not a reason.

## Delivery proof

The intake is not complete until the final alpha commit is pushed and every
applicable GitHub Actions run for that exact SHA completes successfully. The
final report names the main SHA range, classification of every main-only
commit, alpha commit, remote SHA, and exact-SHA CI results.
