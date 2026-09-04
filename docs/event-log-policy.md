# Event log policy

This document fixes what the Control Center, the 신식 서비스 host, and the setup broker record, where they record it, and how the Control Center shows it. The renderer DLLs record nothing; their evidence reaches the host as typed dispositions and the host decides what becomes an event.

## One schema, three writers

Every record is one JSON line of the `EventRecord` schema defined in `mactype_service_contract::event_log` (`v` 1). Fields: `ts` (unix milliseconds), `severity`, `area`, `code`, `params`, `detail`, `source`.

- `severity` is one of `info` (something the user asked for completed), `notice` (a state change the user did not request but should know about), `warning` (degraded but working), `error` (an operation failed or a component stopped working).
- `area` names the module that owns the fact: `service`, `setup`, `profile`, `preview`, `injection`, `control-center`, `tray`.
- `code` is a stable kebab-case identifier from the registry below. The UI localises codes through `event.<code>` catalog keys; a code the catalog does not know is shown as the code with its parameters, never hidden.
- `params` holds at most eight short strings used only as localisation placeholders. Profile references are file names, never paths or contents. Counts are decimal strings.
- `detail` is optional technical text (error chains, Win32 codes, helper diagnostics), sanitised and bounded to 24 KiB. It is shown only behind a disclosure and in the exported report.
- `source` is the writing process: `control-center`, `service-host`, `service-setup`.

Files: the Control Center writes `%LOCALAPPDATA%\MacType\ControlCenter\logs\control-center.log`; the host writes `%ProgramData%\MacType\ControlCenter\logs\service-host.log`; the broker writes `service-setup.log` beside it. Each file rotates at 512 KiB into four backups. The ProgramData directory is readable by Users and writable only by SYSTEM and Administrators, so the user-session Control Center can read what the service wrote. Legacy `control-center.log` lines from before the schema are converted on read.

## Redaction and bounds

Profile bodies are never logged; the writer receives them only as redaction keys and replaces them with `[redacted-profile]`. Thirty-two-character hexadecimal runs become `[redacted-nonce]`. Every field is truncated on a character boundary with a `[truncated]` suffix. Writers never block their caller: a failed append is reported once on stderr and dropped.

## Rate limits

The host summarises injection once per sixty seconds (`injection-summary` with injected, failed and skipped counts) and only when a counter is non-zero. A per-process failure (`injection-failed`) is written at most once per process name and reason in ten minutes; a quiet process-local skip is counted, never written, because a normal skip is not a health condition (see `CONTEXT.md`, explicit process-local skip). Health transitions are written on change only.

## Registry

| code | severity | area | params | writer |
|---|---|---|---|---|
| `app-started` | info | control-center | `version` | control-center |
| `profile-applied` | info | profile | `profile` | control-center |
| `profile-verified` | info | profile | `profile` | control-center |
| `service-installed` | info | service | | control-center |
| `service-started` | info | service | `version`? | control-center, service-host |
| `service-stopped` | info | service | | control-center, service-host |
| `service-health-changed` | notice / error / info | service | `state`, `code`?, `message`? | service-host |
| `injection-summary` | info | injection | `injected`, `failed`, `skipped` | service-host |
| `injection-failed` | warning | injection | `process`, `reason` | service-host |
| `helper-broker-failed` | error | injection | `architecture`, `code` | service-host |
| `operation-failed` | error | setup | `operation`, `stage`, `rollback`, `win32Code`?, `brokerExitCode`?, `channelFailure`? | control-center |
| `setup-command-failed` | error | setup | `command`, `stage` | service-setup |
| `setup-rollback-completed` | notice | setup | `command` | service-setup |
| `legacy-tray-detected` | notice | tray | | control-center |
| `preview-helper-connected` | info | preview | `architecture`, `coreVersion` | control-center |
| `preview-helper-restarted` | warning | preview | | control-center |
| `preview-helper-failed` | error | preview | `reason` | control-center |

A new code needs a row here, an `event.<code>` string in all ten catalogs, and a gallery fixture line.

## What the user sees

The user never reads a raw line unless they ask for it.

- The **overview** shows the recent activity feed: the last eight `info` and `notice` events from the profile, service, tray and preview areas, localised, newest first, collapsed to the latest entry. Errors do not appear there.
- The **diagnostics** page shows the event timeline: every event of the last three hundred, grouped by day, with severity as a filled dot, the area as a caption, and the localised title as the line. Chips filter by severity and area; a search box matches codes, parameters and detail. A disclosure on each row reveals the source, the code, the parameters and the technical detail. The page also lists the three log files with their size and whether the Control Center could read them.
- **Operations** that fail report inline where the user acted, through the existing coded prefixes; the same failure is recorded as an event so the diagnostics page keeps it.
- **Live refresh**: the Control Center polls the three files every two seconds and emits `event-log:changed`; the feed and the timeline refresh without a reload.
- **Export and copy** render the last two hundred events as text lines with their detail, after the installation and component report.
- No toasts and no modal alarms come from the log. A background event changes a count in the diagnostics page, nothing else.

## Non-goals

The event log is not a trace. Per-call renderer hooks, per-frame timings and per-process helper chatter stay out of it; the host's bounded in-memory result registry and the preview helper's transient stderr ring remain the places for that evidence.
