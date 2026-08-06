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
- Sanitized core binaries are test artifacts only. Run x86 and x64 MSVC ASan
  variants through native, open-service, and browser injection proofs; never
  publish them in an installer or integration bundle. Because MSVC ASan does
  not provide LeakSanitizer, use a separate bounded Application Verifier and
  UMDH lane for leaks, handles, locks, and module-unload cleanup.
- Aggressive core work must not weaken the service, installer, elevation,
  installed-root, ownership, or payload-integrity boundaries. Unsupported or
  protected processes must fail explicitly; do not create a hidden bypass to
  make injection appear successful.
- At the start of each work session, fetch and inspect `origin/main`, compare
  it with this branch, and review new service, Control Center, packaging,
  security, and CI commits. Cherry-pick useful commits into this branch before
  starting conflicting work, then rerun the affected branch gates.
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
