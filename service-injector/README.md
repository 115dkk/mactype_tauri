# MacType service injector

This directory builds the source-owned injection Seam used by the Control
Center service. It does not accept or select arbitrary renderer files.

The two production executables are fixed by architecture:

- `mactype-injector32.exe` loads only the adjacent `MacType.dll` into an x86 target.
- `mactype-injector64.exe` loads only the adjacent `MacType64.dll` into an x64 target.

The broker invocation is deliberately narrow and order-sensitive:

```text
mactype-injector64.exe --process-handle <inherited decimal HANDLE> --pid <u32> --creation-time <u64 FILETIME> --session-id <u32> --generation-id <64 lowercase hexadecimal characters> --profile-digest <sha256: plus 64 lowercase hexadecimal characters>
```

No DLL path, executable path, service name, or other runtime selector is
accepted. The service opens the target with the fixed injection rights,
rechecks its creation time, and passes only that handle through a
`STARTUPINFOEX` handle list. The helper never reopens a PID: it owns and closes
the inherited handle and uses that handle for identity checks and module
enumeration. A fixed MacType module is considered loaded only when its
normalized, case-insensitive full path exactly matches the adjacent DLL; a
same-named DLL from another directory is not accepted. Closed, non-inherited,
mismatched, session-0, protected, critical, and architecture-mismatched targets
fail closed with explicit results. After checking hook compatibility and before
looking up the fixed module, the helper queries the target lifecycle. A deleting
process is intentionally skipped as `process-exiting`, and a frozen process is
intentionally skipped as `process-frozen`. If the lifecycle query is unavailable,
the helper continues with the existing checks. For an already-loaded exact
module, the helper first acquires one reference of its own with the same fixed
adjacent path. It queries activation evidence only while that lease is held and
then releases exactly that reference. A conflicting path is left untouched and
reported as the intentional skip `conflicting-mactype-module-loaded` with exit
code `0`; an uncertain lease release never becomes a verified renderer success.

After a verified load, the helper calls the fixed
`MacTypeQueryActivationEvidenceV1` export in the target. The 312-byte generated
contract binds exact process identity, runtime/profile identity, module-load
origin, renderer admission, lifecycle revision, and capability evidence. The
helper reports injection success only for validated `Active` evidence. A
validated `QuietSkip` is process-local: when the helper loaded the module, it
releases that reference and confirms the module is absent before reporting the
quiet skip. When the module pre-existed, it releases only the extra reference
it acquired for the evidence query; the target's preceding reference remains
owned by the target.

Standard output contains one JSON object of at most 1,536 bytes. Its schema is:

```json
{"schemaVersion":2,"status":"injected","code":"renderer-active","pid":1234,"sessionId":2,"generationId":"<sha256>","module":"MacType64.dll","windowsError":0,"cleanupComplete":true,"rendererEvidence":"<624 lowercase hexadecimal characters>"}
```

`rendererEvidence` is `null` for a result established before a module is
loaded. Exit code `0` means injected or intentionally skipped, `2` means
rejected identity/input, `3` means an injection or evidence failure, and `4`
means a remote operation exceeded its bounded cleanup grace.

After a remote load thread completes, the helper cross-checks its return value
against a fresh inventory of the fixed adjacent module. A load that completes
during cleanup grace may proceed to renderer evidence verification. A zero
return value with no module is a definitive `module-load-failed` result unless
a fresh lifecycle query shows that the target is deleting; that case is
reclassified as an intentional `process-exiting` skip with complete cleanup.
An unavailable post-load lifecycle query preserves `module-load-failed`.

If a remote load or evidence-query thread outlives cleanup grace, its result
cannot be read, module inventory cannot be read, or observations conflict, the
helper returns typed uncertain cleanup or integrity evidence with
`cleanupComplete:false` where cleanup cannot be proved. The service treats that
result as terminal for the process identity; it is never retried automatically.
The helper never terminates a target thread merely to force cleanup.

With `BUILD_TESTING=ON`, CTest builds isolated x86 or x64 marker targets and
validates real DLL loading, activation-evidence round trips, quiet-skip unload,
duplicate ownership, intentional skipping of a same-basename module from
another directory, creation-time/session/architecture rejection, arbitrary
runtime selector rejection, process-lifecycle decoding and querying, bounded
JSON, and verified late loading after the initial deadline.
