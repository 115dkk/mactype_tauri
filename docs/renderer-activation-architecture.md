# Renderer activation architecture

This document fixes the Module and Interface direction between the protected
Rust service runtime, the fixed injector, and the renderer. The canonical
domain terms remain in `CONTEXT.md`; this document records where each decision
is owned after the alpha-plus refactor.

## Module direction

```text
ProtectedRendererRuntime
  -> RendererRuntimeBinding
  -> ProcessInjection
  -> HelperProtocol Adapter
  -> RendererActivation Adapter
  -> RendererActivationEvidence

RendererActivation
  -> ProfileRuntime
  -> RendererPolicySnapshot
     -> FreeTypeRuntime
     -> PeExportView
     -> DirectWriteLifecycle
     -> font-substitution Adapters
  -> RendererUnloadLifecycle
```

`RendererRuntimeBinding` is the only Interface that pairs a protected runtime
generation with a protected profile digest. Callers must not carry the two
identities as unrelated strings and recombine them later.

`ProcessInjection` owns target validation, exact-identity de-duplication,
retry, cancellation, bounded result history, telemetry, liveness recheck, and
global health impact. The helper protocol is an Adapter. Its status text and
diagnostic code are evidence, not policy inputs.

`RendererActivation` owns renderer admission and produces one of three
semantic results:

- `Active`: required startup completed; individual optional capabilities may
  still be unavailable and are reported separately.
- `QuietSkip`: the exact process or profile policy explicitly declines
  rendering. This result is process-local and does not degrade service health.
- `Failed`: renderer startup could not establish its required invariants.

`ProfileRuntime` publishes a complete immutable `RendererPolicySnapshot` only
after parsing and validation succeed. A failed reload preserves the previously
published snapshot. One render request retains one snapshot revision, so it
cannot observe a mixture of policy generations.

## Process-lifetime and unload policy

Process-owned renderer registries use explicit teardown rather than C++ static
destruction because Windows may run the latter while holding the loader lock.
An active renderer owns one module self-reference through
`RendererUnloadLifecycle`. A raw `FreeLibrary` can release only the caller's
reference and cannot unmap live hook code. The exported `SafeUnload` thread
procedure is the supported explicit teardown Interface: one atomic gate admits
the attempt, it drains hook leases and DirectWrite workers, commits stop, and
releases the self-reference with `FreeLibraryAndExitThread`. Preparation
failure reopens admission for a later retry. If another caller reference is
still present, the stopped image remains mapped until that caller releases its
own reference. A caller that is about to terminate leaves process cleanup to
Windows. The helper may
release only the reference it created for a verified `QuietSkip`, after the
renderer has confirmed that no hook capability is active or failed and has
cleared its policy, substitution, and lifecycle snapshots. Failure to prove
that state is cleanup uncertainty, not a successful skip. A quiet-skip
renderer never acquires the active-image self-reference.

The same rule governs FreeType and DirectWrite owners. Explicit teardown runs
outside the loader lock in dependency order; process termination may leave a
small owner container for the operating system rather than execute library or
COM release paths from static destructors. Code-local comments retain only the
specific ordering or loader-lock warning needed at each owner.

`FreeTypeRuntime` presents logical top-down bitmap rows through one checked
Interface for both pitch signs. Raster Adapters do not perform their own row
pointer arithmetic. Its face builder retains a separate stream backing until
construction succeeds, then transfers only that backing to the stream close
callback. `PeExportView` similarly keeps PE knowledge local: raw-file and
loader-mapped views validate every header, section, export array, string, and
RVA conversion without creating an executable image or invoking foreign code.

## Evidence Interface

The renderer-activation contract is versioned independently of health v1. Its
fixed-width evidence contains:

- exact PID, creation time, session, and architecture;
- protected runtime generation and profile digest;
- module-load and renderer-admission results;
- a bounded admission reason;
- renderer lifecycle revision;
- bounded capability active, unavailable, and failed sets.

No raw address, C++ standard-library type, Rust allocation, unbounded text, or
internal renderer class crosses this Interface. C++ and Rust definitions are
generated from one canonical schema and checked for size, offset, enum, and
code drift.

The service may continue deriving health-v1 telemetry from the richer result,
but health-v1 serialization is unchanged. Detailed renderer evidence remains
a separately versioned diagnostic Interface.

## Trust and failure rules

- The host validates the protected runtime/profile pairing before observing a
  target. The fixed helper revalidates exact identity, architecture,
  mitigations, module inventory, and its fixed adjacent DLL at the later trust
  point. That repeated verification is intentional TOCTOU defense.
- The renderer verifies the effective profile it actually consumes. Success
  cannot be recorded when its digest differs from the requested binding.
- Activation evidence is requested only while the helper owns a module
  reference. For an exact module that was already loaded, the helper acquires
  one additional reference through the same fixed adjacent path, reports the
  evidence origin as `AlreadyLoaded`, and releases exactly that reference after
  the query completes. It never queries or releases a pre-existing renderer
  without this lease; an uncertain query thread keeps the lease rather than
  risking a use-after-unload.
- A known dynamic-code or binary-signature refusal is a `QuietSkip` for the
  exact `(pid, creation time)`. It is de-duplicated, produces no user alert,
  and never becomes an executable-name ban.
- Unknown post-injection cleanup is terminal for that identity, is not retried
  automatically, and triggers an exact-identity liveness recheck. A proven
  vanished target becomes a quiet skip; an alive or unknown target retains the
  uncertain result and may affect generation health.
- A long-lived renderer may remain mapped after its runtime is removed. Its
  child-injection Adapter treats the adjacent `MacType.ini` as part of the
  fixed generation boundary and stops propagation once that profile is gone,
  so a later child cannot fail during process initialization on a retired DLL.
- Diagnostic integrity failure is distinct from renderer startup failure.
  Retry and health decisions are exhaustive typed mappings, never code-prefix
  or code-suffix tests.
- `HookCoordinator` remains the sole renderer lifecycle state owner. The
  activation Module projects its state; it does not introduce another phase
  machine.

## Old symbol to new Module map

| Previous location or behavior | Refactored owner |
| --- | --- |
| `ProtectedProfileInitializer` and `ProtectedRuntimeAssets` returned unrelated identities | `ProtectedRendererRuntime` constructs `RendererRuntimeBinding` |
| `observer::BrokerResult { disposition, code }` | typed result in `ProcessInjection`; code retained as diagnostic evidence |
| `helper_broker::parse_output` hand-parsed wire policy | HelperProtocol Adapter backed by the generated contract |
| `InjectionOrchestrator` parsed retry codes and `-cleanup-unknown` | exhaustive typed outcome and health-impact mapping in `ProcessInjection` |
| `DllMain` treated profile exclusion as a loaded but otherwise invisible state | `RendererActivation` reports `QuietSkip(reason)` |
| render paths repeatedly queried `CGdippSettings` | immutable `RendererPolicySnapshot` from `ProfileRuntime` |
| `freetype_runtime.h` exposed helpers while ownership remained global | deep `FreeTypeRuntime` owns startup/stop order and consumes policy snapshots |
| repeated signed `FT_Bitmap::pitch` pointer arithmetic | `FreeTypeRuntime::CheckedBitmapRow` Interface |
| `CMemLoadDll` copied a disk DLL to RWX memory to obtain an RVA | read-only `PeExportView` |
| `SafeUnload` used a plain static reentry flag and arbitrary `FreeLibrary` could unmap live hooks | `RendererUnloadLifecycle` atomic admission plus active-image self-reference |
| DirectWrite lifecycle declarations mixed with hook Implementation declarations | narrow lifecycle Interface with classic DirectWrite and DWriteCore Adapters |

## Required verification

- Every typed injection result has table coverage for retry, health impact,
  and liveness recheck.
- C++ golden frames are accepted by the Rust parser, while unknown versions,
  unknown fields, invalid identity, mismatched binding, and oversized input are
  rejected.
- Explicit skip is quiet, exact-identity-scoped, and leaves Ready health.
- Already-loaded renderer state without a safe lease is integrity uncertainty,
  not a quiet skip and not verified activation.
- Failed policy publication preserves the preceding snapshot and revision.
- FreeType partial-start tests prove manager and caches end before the library.
- Positive and negative bitmap pitch fixtures resolve every logical row within
  the computed backing extent; zero, negative, and out-of-range face IDs fail.
- Truncated and malformed PE fixtures fail under x86 and x64 ASan, while the
  fixed cross-architecture loader RVA still supports mixed child relay.
- Plain `FreeLibrary` leaves an active renderer mapped, while `SafeUnload`
  drains DWriteCore and removes the final self-reference on a dedicated thread.
- x86 and x64 injected probes bind marker identity, runtime generation, profile
  digest, and renderer evidence to the same process attempt.
