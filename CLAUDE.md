# CLAUDE.md — fork-only operating rules

This file is for AI-assisted sessions working in the 115dkk/mactype_tauri fork.
**This file must NEVER reach the upstream PR branch or snowie2000/mactype.**

## `codex/alpha-plus-dll` branch charter (overrides the contribution funnel)

These rules apply when the current branch is `codex/alpha-plus-dll` or when a
short-lived child branch targets it. They override the contribution funnel
below wherever the two conflict.

- This is a long-lived personal-use distribution branch. It is not an
  upstream contribution branch, and no commit from it may be placed in an
  upstream pull request or cherry-picked onto `codex/upstream-pr-prep`.
- Do not merge this branch wholesale into fork `main`. A generally useful
  change may move to `main` only when the user explicitly chooses it, after it
  has been isolated on a normal `codex/<name>` branch and reviewed under the
  ordinary contribution funnel.
- Run every applicable `main` CI gate and every core build/test gate. Add
  branch-only browser, rendering, compatibility, stress, crash, leak, and
  performance gates whenever they provide useful evidence. Passing upstream
  CI is a floor, not the completion criterion for this branch.
- The tracked x86 and x64 Release rendering translation units are a pinned
  Cppcheck 2.20.0 target. New null-dereference, allocation/resource, leak,
  lifetime, initialization, and output-parameter diagnostics fail CI. Do not
  hide obsolete first-party source files behind lint exclusions: prove they
  are used or delete them. Generated and required third-party code remains a
  documented dependency boundary.
- Aggressive core work is in scope: RAII conversion, hook-lifecycle redesign,
  FreeType work, stronger font substitution, broader DirectWrite support, and
  Windows 11 application compatibility. Upstream patch size and upstream
  acceptance are not design constraints here.
- Never import or cherry-pick `codex/renderer-memory-safety` into this branch.
  That branch deliberately uses a classic-C++/no-RAII paradigm for a separate
  upstream contribution. Alpha renderer ownership changes must be implemented
  and reviewed independently.
- The completed alpha renderer architecture is an invariant, not a suggestion:
  new top-level hook admission and capability state goes through
  `HookCoordinator`, provider vtable registries stay local to their adapter,
  Detours transactions remain process-serialized, GDI and DirectWrite
  substitutions consume the immutable font-substitution snapshot, and
  FreeType manager ownership ends before its library. Do not add an independent
  runtime phase machine or read mutable profile maps from a render hot path.
- Unity font hooking is startup policy owned by `UnityFontHookLifecycle`. It is
  off by default and accepts only exact PE timestamp/image-size, CodeView PDB
  identity, render and face-open ABIs, and target-prefix descriptors. Never
  replace this with wildcard signature scanning. Font substitution runs only
  after Unity has selected its OS `FontRef`: the exact private FreeType
  face-open target receives a copied pathname argument and the replacement's
  checked TTC face index. Memory/stream faces remain untouched, and a failed
  replacement face falls back to the original arguments. Do not restore
  `CreateFileA/W` IAT patching. Installed families come from DirectWrite,
  registry, and bounded per-face SFNT/TTC name parsing. `Most games`
  admission is an explicit process-local skip when the 신식 서비스 finds
  anti-cheat evidence or cannot prove the bounded installation scan; selected
  and all-games modes do not weaken existing protected-process or mitigation
  guards. The renderer's repeated anti-cheat scan is direct-injection defense,
  not a second service-admission authority.
- Exact Unity builds that expose an OS `FontRef` resolver carry
  `nativeFamily` or `mappedFamily` through `ScopedFontRefSelectionContext`.
  A native family forbids a coincidental same-file redirect; a mapped family
  permits it, and nested resolution must restore the preceding context. Older
  exact adapters without that resolver retain the face-index-checked path
  fallback. Diagnostic hooks observe only: they never replace a returned
  `FT_Face`, retain a raw face pointer, or change cache lookup results.
- A DirectWrite source face can expose localized, typographic, weight/style,
  and Win32 family names. If any name selects a substitution, the immutable
  alias collection must preserve every non-conflicting name for that same
  face. Never collapse `Malgun Gothic`, `맑은 고딕`, and their Semilight names
  into the one spelling that matched the profile; conflicting alias rules fail
  closed to the native face.
- `FreeTypeRuntime` is also the sole Interface for logical bitmap rows and
  one-based face IDs. Render Adapters must use `CheckedBitmapRow`; they never
  reconstruct signed-pitch pointer arithmetic. An `FT_Face` receives a
  separate callback-owned stream backing only after successful construction;
  the builder itself is never deleted by an `FT_Stream` callback.
- PE export lookup is read-only through `PeExportView`. Do not restore a
  manual loader, executable/RWX image copy, relocation pass, foreign
  `DllMain` call, or unchecked RVA arithmetic merely to obtain a function or
  export-table-slot RVA.
- An active renderer owns one `RendererUnloadLifecycle` self-reference. A
  balanced caller may release its own `LoadLibrary` reference without
  unmapping live hook code. Windows references are not owner-tagged, so
  unmatched or repeated `FreeLibrary` calls are outside the supported
  contract. Supported explicit teardown runs the exported `SafeUnload` thread
  procedure, which serializes attempts, drains workers, hooks, FreeType,
  renderer policy, substitution, and settings outside the loader lock, then
  releases the self-reference with `FreeLibraryAndExitThread`. If a caller
  retains another module reference, mutable exports reject work and the
  stopped image remains mapped until that caller releases its own reference.
  Final explicit detach releases only the TLS slot and empty lock storage;
  `QuietSkip` renderers acquire no self-reference, and process termination
  leaves cleanup to Windows.
- `RendererUnloadLifecycle` also owns one provider-drain transaction. SafeUnload
  must not branch on DirectWrite- or Unity-specific preparation enums. New
  renderer providers join that drain seam so partial commit retry and rollback
  ordering remain local to the Module.
- Source comments follow `docs/source-comment-policy.md`. Keep code-local
  safety proofs and platform traps, move repeated cross-Module contracts to
  their canonical document, and do not leave narration, disabled code, model
  provenance, or untracked TODO/FIXME/HACK markers in first-party source.
- Protected runtime and profile identity travel only as one
  `RendererRuntimeBinding`. A helper-owned load is successful only after the
  generated `RendererActivationEvidence` contract binds the exact process,
  effective profile digest, admission, lifecycle revision, and capability
  evidence. Keep helper wire text inside its Adapter; retry, cleanup, health,
  and quiet-skip policy consume typed dispositions. Never query or release an
  already-loaded renderer without a renderer-owned lifetime lease. For an
  exact pre-existing DLL, acquire one fixed-path helper reference, query it as
  `AlreadyLoaded`, and release only that reference after the query completes;
  retain it when query-thread cleanup is uncertain.
- `ProfileRuntime` publishes one complete immutable `RendererPolicySnapshot`
  after the selected profile remains stable through hashing and parsing. A
  failed publication preserves the prior snapshot. FreeType startup, raster,
  reload, and new renderer work consume that snapshot rather than rebuilding
  policy from `CGdippSettings`.
- Renderer startup fails closed when the adjacent profile or its selected
  `AlternativeFile` is missing, malformed, non-regular, empty, or oversized.
  A renderer still mapped from a retired generation must stop child propagation
  as soon as its adjacent `MacType.ini` disappears; it must not inject a child
  that can only fail during process initialization. Do not restore the
  historical implicit defaults. Feature and interface presence, not
  manifest-sensitive Windows version numbers, select modern rendering paths.
- Stopping the 신식 서비스 closes future injection; it does not remotely
  unload renderers from arbitrary live applications. Existing processes keep
  their immutable renderer/profile generation until supported per-process
  `SafeUnload` or process exit, so profile and Unity-policy changes are not
  retroactive. Isolated field tests must stop the service first and accept
  exactly one MacType module from the requested test directory.
- The fixed setup broker materializes DLL-adjacent `MacType.ini` only for an
  active service. A supported stop and every maintenance or profile operation
  that ends stopped remove only the exact verified generated copy while
  retaining the protected profile generation; start restores exact bytes
  before SCM launch. Preserve this old-renderer-compatible liveness lease so a
  long-lived injected parent cannot keep propagating after the user stops the
  service. Do not substitute an invalid profile, executable-name blacklist, or
  global warning for this state transition.
- A process that explicitly blocks hooks or module loading is a quiet
  process-local skip keyed by `(pid, creation time)`. Preserve its exact reason
  in the bounded orchestrator result, deduplicate that identity, keep global
  service health Ready, and never turn an executable name into an image-wide
  ban. Keep the health-v1 wire schema stable unless a separately versioned
  migration and rollback is designed.
- MSVC ASan is mandatory on x86 and x64 for the new renderer ownership,
  lifecycle, checked-PE, FreeType-policy, and substitution modules. Do not call these
  focused tests a sanitized injected core: stock IniParser and other linked
  C++ dependencies currently have incompatible STL annotations. A full-core
  ASan lane requires every linked C++ dependency to use matching sanitizer and
  runtime settings. Sanitized artifacts never enter an installer or bundle.
  Application Verifier/UMDH is a disposable-lab procedure only; do not mutate
  a developer or hosted runner's machine-global verifier state merely to claim
  a pass.
- Aggressive core work must not weaken the service, installer, elevation,
  installed-root, ownership, or payload-integrity boundaries. Unsupported or
  protected processes must fail explicitly; do not create a hidden bypass to
  make injection appear successful.
- At the start of each work session, fetch and inspect `origin/main`, compare
  it with this branch, and review new service, Control Center, packaging,
  security, and CI commits. Cherry-pick useful commits into this branch before
  starting conflicting work, then rerun the affected branch gates. After the
  renderer/service deepening, follow the direct-pick and semantic-port rules in
  `docs/alpha-plus-refactor-cherry-picks.md`; the Module ownership and old-to-new
  symbol map are fixed in `docs/renderer-activation-architecture.md`.
- Releases from this branch are prereleases only. Their tag, release title,
  installer download, integration/developer bundle, and checksum download must
  begin with `alpha-`. Never publish a stable release from this branch.
- Work in `.worktrees/alpha-plus-dll` or a dedicated child worktree. Child
  branches merge only into `codex/alpha-plus-dll`, unless the user explicitly
  requests the normal `main` extraction process described above.

## Contribution funnel (mandatory order)

1. Work in `.worktrees/<name>` on a `codex/<name>` branch.
2. PR to fork `main` — full CI must be green (Build and package, Lint gates,
   Frontend window gallery, Open service hosted Windows contract).
3. Merge (merge-commit style), verify the push workflows on `main`.
4. Cherry-pick ONLY the product commits onto `codex/upstream-pr-prep`
   (its tree tracks main's frontend, so picks apply clean), then push.
5. Manually dispatch the one-click build:
   `gh workflow run build.yml --repo 115dkk/mactype_tauri --ref codex/upstream-pr-prep -f version=0.1.0`.

## Upstream hygiene — what never crosses over

`codex/upstream-pr-prep` (feeds upstream PR snowie2000/mactype#1142) carries
ONLY the `workflow_dispatch` build workflow. NEVER cherry-pick or otherwise
carry over:

- `CLAUDE.md` (this file), `.claude/`, `.worktrees/`
- Fork CI workflows or CI helper scripts beyond the dispatch build workflow
- Fork-only docs, gallery branches, or anything referencing the fork's CI

Corollary: never mix changes to the files above into a commit that will be
cherry-picked. Docs/CI edits get their own commits that simply are not picked.

## Communication rules

- Claude sessions only: Korean replies in this project follow the
  `chegyejeog-chulonja-v4-style`
  skill (`~/.claude/skills/chegyejeog-chulonja-v4-style/SKILL.md`): plain
  Korean without translationese, no em-dashes, no emoji, prose for reasoning
  and conclusions, lists only for genuinely parallel items. The skill's own
  frontmatter says to wait for an explicit request; this project asks for it
  by default, so read it at session start and keep applying it. English
  artifacts (commit messages, PR bodies, code comments) are unaffected.
- Non-Claude agents follow `AGENT.md`. The Claude-only Korean writing rule
  above does not apply to them.
- Never comment on the upstream repo or PR without an explicit user request.
- Screenshots/galleries go to the FORK's issue #3 (images hosted on an orphan
  `gallery-*` branch in the fork; embed raw.githubusercontent URLs).

## Repo quirks worth knowing

- **`gh` defaults to the UPSTREAM repo** (snowie2000/mactype, via the
  `upstream` remote), not the fork. Every `gh pr`/`gh issue`/`gh api`/
  `gh workflow` call MUST pass `--repo 115dkk/mactype_tauri`. A forgotten
  flag aims PRs, comments, or workflow dispatches at the upstream project,
  which the rules above forbid. Never "fix" this by removing or reordering
  the `upstream` remote; pass the flag.
- `pnpm` is not on PATH; use corepack (`corepack pnpm …`) or the shim dir.
- `pnpm generate:settings`/`pnpm build` rewrite three generated files with
  line-ending-only noise (`generated_settings.rs`, `generated/settings.ts`,
  `generated_settings.h`) — `git checkout --` them before committing.
- i18n gate: every non-ASCII char in `ko.json` must exist in
  `control-center/src/assets/fonts/ko-glyphs.txt`; reword Korean strings to
  covered glyphs or regenerate the subset via
  `scripts/generate-ko-font-subset.py` (pinned sha256).
- All ten locale catalogs must keep identical key sets and placeholders
  (`node scripts/ci/Test-I18n.mjs`).
- Domain terms are frozen in `CONTEXT.md`; view ids (`files`, `profiles`,
  `execution`) are interface and never renamed, even when labels change.
