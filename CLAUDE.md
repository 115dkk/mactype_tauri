# CLAUDE.md — fork-only operating rules

This file is for AI-assisted sessions working in the 115dkk/mactype_tauri fork.
**This file must NEVER reach the upstream PR branch or snowie2000/mactype.**

## Contribution funnel (mandatory order)

1. Work in `.worktrees/<name>` on a `codex/<name>` branch.
2. PR to fork `main` — full CI must be green (Build and package, Lint gates,
   Frontend window gallery, Open service hosted Windows contract).
3. Merge (merge-commit style), verify the push workflows on `main`.
4. Cherry-pick ONLY the product commits onto `codex/upstream-pr-prep`
   (its tree tracks main's frontend, so picks apply clean), then push.
5. Manually dispatch the one-click build:
   `gh workflow run build.yml --ref codex/upstream-pr-prep -f version=0.1.0`.

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

- Never comment on the upstream repo or PR without an explicit user request.
- Screenshots/galleries go to the FORK's issue #3 (images hosted on an orphan
  `gallery-*` branch in the fork; embed raw.githubusercontent URLs).

## Repo quirks worth knowing

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
