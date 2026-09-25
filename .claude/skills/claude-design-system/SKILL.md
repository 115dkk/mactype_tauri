---
name: claude-design-system
description: Keep this branch's Claude Design design system in sync with the Control Center. Use when the user asks to update, re-sync or publish the design system, and after a change to control-center/src/styles/**, the skins, DESIGN.md, docs/skin-designs.md, the bundled fonts, the product icon or the lucide icons the frontend imports. Each branch has its own system; never publish one branch's design to the other's.
---

# Claude Design system for this branch

`target.json` names this branch's design system (its `url`, `title` and `branch`). The
Control Center design differs between branches, so each branch keeps a separate system:

| Branch | System | Contents |
| --- | --- | --- |
| `codex/alpha-plus-dll` | MacType Control Center Alpha, https://claude.ai/artifact/WtU5TdF7vAKYavzVBJsgVb | four skins, eight themes |
| `main` | MacType Control Center Main, https://claude.ai/artifact/5Gkn7RvXDYNe6QHYMWWSKp | classic only, light and dark |

Publish only to the url in this branch's `target.json`. A change that exists on only one
branch must never reach the other branch's system. This folder is fork-only tooling like
`CLAUDE.md`: never carry it to `codex/upstream-pr-prep` or an upstream pull request.

## What lives here

- `build.mjs` derives everything that has a source in the repository: `tokens.json` (every
  colour token per theme with its contrast failures flagged, sizes, spacing, radius, shadow,
  opacity), `components/bundle.css` (the app's stylesheets in `main.tsx` import order, token
  declarations moved out, skin selectors keyed on theme ids), the bundled font, the section
  files named in `target.json`, the product icon and every lucide icon the frontend imports.
- `content/` holds what a person writes: `README.md` (the brand book), `token-notes.json`
  (one usage note per token; the build fails when a token has none), `type.json` (type
  families, styles and font sources), `contrast.json` (the pairs checked per theme),
  `marker-classes.json`, the asset group READMEs and `components/<Name>/README.md` and
  `preview.html`. Previews use the real class names; `{{icon:Name:size[:stroke]}}` expands
  to the lucide SVG and `{{logo}}` to the product icon as a data URI.

## Re-sync

1. Read the live system before changing it: Artifact `read` with the target `url` and
   `path` `project/design-system.json`, plus every `project/` file you are about to replace.
   If `lastChange.via` is not a GitHub sync, someone edited the system on its page; carry
   those edits into `content/` first so the publish keeps them.
2. Update `content/` for what changed in the Control Center (new components, copy, notes).
   Values that come from CSS are never typed by hand.
3. Fetch the icon source matching `lucide-react` in `control-center/package.json`:
   `npm pack lucide-static@<version>` in a scratch directory and extract it.
4. `node .claude/skills/claude-design-system/build.mjs files --out <scratch>/ds --lucide <scratch>/package`.
   Resolve every warning; a preview class the stylesheets never style is a bug in the preview.
5. Upload each `uploads.json` entry that the live index does not already record
   (`assetGroups.<Group>.files`): Artifact `publish` with the target `url`, `asset: true` and
   the entry's `localPath` (`file_paths` takes 25 at a time). Write the returned ids to a
   `blobs.json` as `{"Icons/house.svg": "<id>"}`.
6. `node .claude/skills/claude-design-system/build.mjs index --out <scratch>/ds --blobs <blobs.json> --existing <the index read in step 1>`.
7. Publish in one call: Artifact `publish` with the target `url`, `root` `<scratch>/ds`,
   `file_path` `<scratch>/ds/project/design-system.json` and `files` = `<scratch>/ds/files.json`.
   A component or section removed from `content/` must also be sent as `null`.
8. Commit the `content/` and script changes on this branch only, in their own commit.
