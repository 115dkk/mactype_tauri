# Project context

This file fixes the domain language used by code, tests, CI, and architecture documents. A name in this glossary is part of the module interface; do not replace it with a near-synonym in a new implementation.

## Domain language

**MachineIntegration**
: The module that presents installation, health, profile publication, migration, repair, and rollback as one user-facing machine-integration model. Its interface is consumed by the frontend; Windows SCM, protected storage, and legacy detection remain implementation details behind its seam.

**신식 서비스**
: The open-source Windows service owned by MacType Control Center. Its fixed production SCM identity is `MacTypeControlCenter`. It observes eligible processes, selects a fixed helper, publishes versioned health, and reads only administrator-protected runtime and profile generations. English documents may add “new service” in parentheses, but the Korean product term is always **신식 서비스**.

**레거시 서비스**
: The original `MacType` Windows service hosted by `MacTray.exe`. It is detected, backed up, stopped, restored, or removed only by the explicit migration flow. It is never a normal dependency of the 신식 서비스. English documents may add “legacy service” in parentheses, but the Korean product term is always **레거시 서비스**.

**generation**
: An immutable, digest- or version-addressed runtime or profile directory under a protected machine root. Activation changes a small durable pointer; it never edits an active generation in place.

**InjectionOrchestrator**
: The host module that consumes a verified target decision, binds the identity to one runtime generation, applies deduplication/retry/cancellation policy, records a bounded target result, and owns generation-bound injection telemetry and terminal health classification. Normal target skips and pre-injection rejection must not change global service health.

**ProcessTargetValidator**
: The host adapter that inspects one observed PID and returns a verified eligible identity or an explicit skip. It owns self, session-zero, protected, critical, target-race, and PID-mismatch classification; it does not own generation binding, retry, or result history.

**fixed helper**
: The adjacent x86 or x64 `mactype-injector` executable selected by architecture. Its interface accepts only an inherited process handle plus fixed identity fields. It cannot accept an arbitrary DLL, executable, command line, service name, or profile path.

**ExecutionViewModel**
: The frontend-facing model derived from MachineIntegration state. It chooses user-visible actions and explanations without teaching React about SCM flags, registry layouts, helper processes, or migration receipt internals.

## Responsibility map

- React and `ExecutionViewModel` own presentation and user intent.
- Tauri's MachineIntegration adapter translates fixed user actions into system commands and read-only state.
- Rust setup and host modules own SCM, protected generations, observation, health, recovery, and rollback.
- The fixed helper and public MacType C/C++ code own injection and rendering.
- The 레거시 서비스 remains a migration subject and fallback only; it is not part of normal operation.

## Architecture language

Architecture work uses **module**, **interface**, **implementation**, **seam**, **adapter**, **depth**, **leverage**, and **locality** in their standard repository meanings. The interface is the test surface. Add a seam only when at least two adapters actually vary, and prefer a deep module whose invariants stay local over pass-through modules that spread Windows knowledge across callers.
