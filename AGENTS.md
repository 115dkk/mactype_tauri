# AGENTS.md — mandatory delivery and CI completion

This file applies to every coding agent working in this repository. Read and
follow `AGENT.md` and `CLAUDE.md` as well; their branch, security, upstream, and
repository-specific rules remain in force.

## A repository change is not complete at `git push`

For every change task, establish the intended delivery path before declaring
the task complete. Obtain the user's explicit authorization for each GitHub
write that requires it, including commit, push, PR creation, and merge. When
end-to-end delivery is authorized, all of the following are mandatory:

1. Commit the confirmed task files and push the exact feature-branch HEAD.
2. Record the local HEAD SHA and confirm that the remote branch points to the
   same SHA.
3. Discover every applicable GitHub Actions run whose `headSha` exactly equals
   that SHA. Do not accept the newest run if it belongs to an older commit, and
   do not treat a temporary “no checks reported” state as success.
4. Watch every required run to completion with `gh run watch <run-id>
   --compact --exit-status --interval 10`. Do not use sleep-based polling, and
   do not report success while any required run is queued or in progress.
5. If a run fails, inspect that same run's failed jobs and logs, make an
   evidence-backed fix, push the new HEAD, and restart exact-SHA discovery.
   Never continue watching a superseded SHA.
6. For ordinary contribution branches, create or reuse the authorized PR,
   confirm its head OID equals the tested SHA, watch `gh pr checks` to a green
   merge gate, merge using the repository's required merge-commit style, and
   then watch every required push workflow for the exact merge commit on
   `main`.
7. Verify the final remote branch/merge SHA and a clean local worktree before
   handing off. The final report must include the commit, PR/merge result, CI
   run results, and any intentionally skipped check with its reason.

Always pass `--repo 115dkk/mactype_tauri` to `gh`. Never target the upstream
repository unless the user explicitly requests upstream contact.

## `codex/alpha-plus-dll` exception

`codex/alpha-plus-dll` is the long-lived delivery branch and must never be
merged wholesale into `main`. A direct authorized push to this branch is the
delivery/merge endpoint; watch all applicable branch push workflows for the
exact pushed SHA. A short-lived child branch may merge only into
`codex/alpha-plus-dll`, followed by exact merge-SHA CI monitoring. Moving a
generally useful change to `main` requires the explicit isolation and review
flow defined in `CLAUDE.md`.
