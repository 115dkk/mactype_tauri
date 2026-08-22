# Source comment policy

Comments are part of the maintenance Interface. They record a constraint that
cannot be made obvious by the type or control flow; they are not a transcript
of how the code was written.

## Keep beside the code

- `SAFETY:` immediately before an `unsafe` operation, stating the concrete
  validity, bounds, alignment, ownership, and lifetime facts that make it safe.
- A local platform or compatibility trap that a nearby edit could violate,
  such as loader-lock restrictions, ABI layout, borrowed-versus-owned handles,
  or a required cleanup order.
- A non-obvious failure invariant, especially where a tempting retry, unload,
  fallback, or alert would be incorrect.
- A narrow workaround whose triggering dependency and removal condition are
  named.

These comments explain why the code has its present shape. They do not restate
the next statement or narrate execution step by step.

## Put repeated contracts in documentation

Rationale shared by more than one Module has one canonical home:

| Contract | Canonical document |
| --- | --- |
| Renderer/service binding, activation evidence, quiet skip, and health impact | `docs/renderer-activation-architecture.md` |
| Hook compatibility classes and explicit process-local exclusions | `docs/hooking-compatibility.md` |
| FreeType ownership, cache policy, and loader-lock teardown | `docs/freetype-runtime.md` |
| Control Center profile selection, service state, preview, and UI projection | `docs/control-center-architecture.md` |
| Branch delivery, merge, and exact-SHA CI completion | `AGENTS.md` and `CLAUDE.md` |

An inline warning may point at a local consequence of one of these contracts,
but it must not reproduce the cross-module design.

## Remove instead of preserving

- progress narration such as “now”, “next”, or a numbered implementation
  diary when the code already expresses the sequence;
- commented-out code, debug calls, abandoned alternatives, and speculative
  optimization notes;
- `TODO`, `FIXME`, `HACK`, or `XXX` task markers without a tracked design or
  issue; source files are not the work queue;
- comments that merely translate an identifier into prose;
- model, prompt, conversation, or authoring-history commentary.

Generated and third-party files retain their upstream notices and are not
rewritten to satisfy first-party prose style. Test fixtures preserve their
source bytes. New first-party comments are English and use the established
domain terms from `CONTEXT.md`.

## Review gate

`scripts/ci/Test-SourceComments.ps1` rejects untracked task markers and model
provenance in first-party source. The semantic part of this policy still
requires review: a regular expression cannot decide whether a loader-lock
warning is essential or whether a paragraph belongs in an architecture
document.
