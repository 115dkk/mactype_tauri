# Vendored audit skills

These three Claude Code skills are what the Alpha Audit workflow installs before
it runs. They live here, on `codex/alpha-plus-dll` only, because the audit is
alpha-only: `main` carries the scheduling entry point and nothing else, since
GitHub schedules workflows from the default branch alone.

They are pinned copies, not a submodule, so an upstream edit cannot change what
a scheduled run analyses without a commit here.

| Skill | Origin | Role |
| --- | --- | --- |
| `improve-codebase-architecture` | [mattpocock/skills](https://github.com/mattpocock/skills) | Part 1. Finds deepening opportunities and writes `ARCHITECTURE_AUDIT.md`. |
| `codebase-design` | [mattpocock/skills](https://github.com/mattpocock/skills) | The architecture vocabulary and principles. The skill above calls it through the Skill tool. |
| `tech-debt-audit` | [ksimback/tech-debt-skill](https://github.com/ksimback/tech-debt-skill) | Part 2. Whole-repo debt survey, writes `TECH_DEBT_AUDIT.md`. |

Two things about them are easy to get wrong.

`codebase-design` is where the deep-module vocabulary lives (module, interface,
depth, seam, adapter, leverage, locality) together with the deletion test.
`improve-codebase-architecture` used to carry its own glossary and no longer
does, so dropping this skill leaves Part 1 running without the words it is
supposed to phrase every finding in.

`improve-codebase-architecture` and `tech-debt-audit` both ship with
`disable-model-invocation: true`, which stops an interactive session from
starting a whole-repo audit by accident. The workflow strips that line from the
installed copies, never from these. `codebase-design` carries no such line and
must not gain one, because the architecture skill has to be able to call it; the
workflow fails if it ever does.

`improve-codebase-architecture/SKILL.md` carries one local change against
upstream: in CI it writes Markdown to `ARCHITECTURE_AUDIT.md` instead of opening
an HTML file in a browser, and it stops before the interactive grilling loop.
Keep that paragraph when refreshing the skill.
