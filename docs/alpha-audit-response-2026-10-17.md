# Alpha audit response

This is the fork-only response to
[the automatic alpha audit](https://github.com/115dkk/mactype_tauri/issues/116).
The report inspected an older alpha revision. Findings were checked against
`c41f489` and separately against fork main `9f482d4`; report recommendations are
not evidence that every named behavior is a defect.

The report body defines F001 through F080. Its summary misnumbers the helper
issue (F051), startup failure (F071), and MSRV claim (F061), and refers to F081/F082
without body entries. This response uses the body IDs.

## Correctness changes

| Finding | Alpha change | Main handling |
| --- | --- | --- |
| F001/F002 | Existing renderer snapshot now owns admission, font exclusion, display affinity and remaining GDI policy values; nested calls retain one policy and reload writers serialize publication | Separate container owners, retained font-link data, copied individual settings and guarded name-cache access; legacy scalar/LCD state is not migrated |
| F073 | Bounded, typed control-message parser and exception containment at the window procedure | Same parser and exception policy, without alpha lifecycle code |
| F074 | GetModuleFileName receives the actual buffer capacity | Same correction at the main source location |
| F051 | Existing module presence no longer bypasses the helper's owned-reference activation-evidence query; obsolete host pre-probe removed | Not applicable to main's v1 helper contract |
| F009 | Remove the unreachable duplicate disposition match | Duplicate is not present on main |
| F011 | Remove unused last-injected identity state | Same removal |
| F071/F072 | Startup errors reach the recovery boundary; reporting/listener rejections terminate without recursive reporting; pending installation status contains no fabricated version/path | Common fixes plus a main-style recovery boundary and translated recovery text |
| F047 | A valid selected skin survives persistence failure | Alpha-only skin contract |
| Architecture F2 | Browser simulator requires current installation before claiming active injection | Same predicate correction |
| F061 | Service workspace MSRV is 1.85, matching its locked dependency requirement; check all targets/features for both Windows architectures at that compiler | Same service-workspace contract and CI check |

The MSRV change also enables stable Clippy suggestions for `is_none_or` and
`repeat_n`; those replacements preserve the preceding predicates and quoting.
The service MSRV claim is not a minimum-version claim for every dependency in
the separate Control Center workspace.

Main retains old container owners for in-flight readers, but reload explicitly
publishes empty replacement font tables before delayed initialization. A later
GetDC/registry initialization failure must not preserve a newly disabled old
substitution rule. The main port does not migrate the common-font reference,
shadow/tune/LCD pointers, remaining scalar setters, or global FreeType/cache
refresh state. Concurrent access to those remains a C++ data-race concern; it
is not classified as harmless or marked fully resolved under F002.

## Verification and build changes

- F004: Cppcheck inspects `Rel+Detours|Win32` and `Rel+Detours|x64`, matching the
  shipped renderer. The branch-policy assertion checks these same configurations.
  The pinned analyzer version and safety suppression restrictions are unchanged.
- F005: one explicit CMake list supplies normal tests, ASan instrumentation,
  the build target, and CTest registration. This includes arrival-evidence and
  GDI-font-identity tests previously absent from the gates. Interactive/catalog
  tools are not treated as argumentless unit tests.
- F032: generated source and schema files use LF attributes in both branches.
- F033: remove the repeated native-preview generator invocation in alpha jobs.
- F075/F076/F077: document ETW before WMI fallback, distinguish generated inner
  activation evidence from the hand-parsed helper envelope, and state the actual
  scope of immutable settings rather than claiming all mutable caches are gone.
- Native probe tests use the non-input desktop runner described in
  [native probe isolation](native-probe-isolation.md). Real visible-window and
  paint assertions remain active. Test consoles do not open on the input desktop.
- Injector fixture failures retain bounded metadata/result diagnostics in the
  test output. The five-second metadata deadline and expected exit codes remain
  enforced. Windows PowerShell fixture process handles must stay owned until
  exit-code inspection.

## Recommendations not treated as automatic fixes

- EasyHook and manual build configurations are not deleted merely because the
  current packaging workflow does not select them.
- Safety observations before an action, elevated revalidation after UAC, and
  post-mutation readiness checks are not collapsed into one stale snapshot.
- Renderer-owned module leases, cleanup uncertainty, exact process identity,
  installed-root ownership, and payload-integrity checks remain mandatory.
- Profile publication flows differ in pause, selection, designation and start
  semantics. Similar call sequences do not prove they are interchangeable.
- Wire variants and always-present fields are not removed without a separate
  compatibility decision. Contract duplication alone does not prove drift.
- Path/family case comparison, storage durability/reparse handling, and SCM
  restoration helpers have different contracts. They are not unified by name.
- Sourcemaps, strict compiler flags, dependency pins, raw handle counts and
  large files are not vulnerabilities or leaks without a concrete failure.
- The Unity catalog probe requires explicit input/evidence; invoking it without
  arguments only tests usage failure, not the catalog contract.

F001/F002 do not claim to repair every legacy font-name cache, font-link or
parser API in either branch. Main's borrowed-container interfaces must be
reviewed on their own; alpha is never merged wholesale into main.

## Evidence rules

Local checks, hosted CI and field evidence are different claims. A running
production injector can still affect a private test desktop. Preserve the
observed exit code and output, and do not call interference the root cause
without evidence. Do not increase timeouts, suppress a failing assertion, stop
the user's service or change machine-global verifier state merely to report a
pass. Exact final commands and unresolved checks belong in the pull requests.

Product, test/CI and fork documentation changes are separate commits. Only the
reviewed main product commits are eligible for the upstream preparation branch;
this record and fork CI never cross that boundary.
