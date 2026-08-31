# Hooking compatibility and explicit exclusions

This inventory was refreshed on 2026-08-24 from the upstream
`snowie2000/mactype` issue tracker. It classifies reported failures by mechanism
rather than adding executable-name workarounds. A browser or packaged app can
host several process roles with different policies, so compatibility decisions
are made for an exact `(pid, creation time)` process instance.

The only issue created after the previous 2026-08-20 inventory was
[#1147](https://github.com/snowie2000/mactype/issues/1147), which asks how to
approximate stock Windows 11 ClearType while changing `PixelLayout`. It does
not report injection or hook failure and therefore adds no compatibility class
or executable-name workaround.

## Operating rule

- A confirmed process mitigation or protected boundary skips only that process.
  It is a normal `skipped` outcome: no degraded service health and no user alert.
- Once an inspected identity is explicitly skipped, the orchestrator retains
  that `(pid, creation time)` and exact reason in its bounded process-result
  registry. A repeat observation is a duplicate rather than another injection
  attempt. PID reuse is safe because a different creation time is a different
  identity.
- `ProhibitDynamicCode` without thread opt-out, and any active Microsoft-only,
  Store-only, or mitigation-opt-in image signature policy, are explicit blocks.
- A failed or unavailable mitigation query is not an implicit global refusal.
- MacType never calls `SetProcessMitigationPolicy` to weaken a target and never
  disables a browser sandbox, Code Integrity Guard, or Arbitrary Code Guard.
- Names such as `chrome.exe`, `firefox.exe`, and `RuntimeBroker.exe` are not
  globally blacklisted. Eligible roles continue through the normal path.
- Quiet skip evidence is process-local and is not added to the health-v1 wire
  schema. This preserves rollback compatibility and prevents a normal target
  policy from becoming a global warning, `lastError`, or “performance
  degradation” notification.

## Upstream report inventory

| Failure class | Representative upstream reports | Branch response |
| --- | --- | --- |
| Packaged/UWP process discovered too late or through an unexpected broker | [#56](https://github.com/snowie2000/mactype/issues/56), [#245](https://github.com/snowie2000/mactype/issues/245), [#467](https://github.com/snowie2000/mactype/issues/467), [#494](https://github.com/snowie2000/mactype/issues/494), [#518](https://github.com/snowie2000/mactype/issues/518), [#541](https://github.com/snowie2000/mactype/issues/541), [#554](https://github.com/snowie2000/mactype/issues/554), [#798](https://github.com/snowie2000/mactype/issues/798), [#814](https://github.com/snowie2000/mactype/issues/814), [#894](https://github.com/snowie2000/mactype/issues/894), [#931](https://github.com/snowie2000/mactype/issues/931), [#1017](https://github.com/snowie2000/mactype/issues/1017) | Prefer immediate `Win32_ProcessStartTrace`; fall back to the one-second intrinsic WMI query only when the trace provider is denied or unavailable. An already hooked eligible parent injects its fixed adjacent generation while each child is still suspended. |
| Same- and mixed-architecture child trees | The packaged and browser reports above include x86/x64 broker changes | A typed child-injection transaction verifies PID/creation-time/session, protection, criticality, image policy, architecture, and explicit mitigation facts against the inherited process handle. Same-bitness children use `DetourUpdateProcessWithDll` directly; mixed trees use a short-lived matching-architecture `rundll32` relay and revalidate before patching the fixed generation. Known skips and pre-mutation failures restore the caller's requested thread state. An externally terminated helper with unknown mutation state causes only that new child to be terminated rather than resuming a potentially partial image. |
| Chromium, Electron, Firefox, or Teams sandbox/code-integrity refusal | [#597](https://github.com/snowie2000/mactype/issues/597), [#614](https://github.com/snowie2000/mactype/issues/614), [#698](https://github.com/snowie2000/mactype/issues/698), [#725](https://github.com/snowie2000/mactype/issues/725), [#769](https://github.com/snowie2000/mactype/issues/769), [#1138](https://github.com/snowie2000/mactype/issues/1138), [#1146](https://github.com/snowie2000/mactype/issues/1146) | Relay eligible renderer children before entry. Quietly skip only an exact process with an explicit CIG/ACG-style block. Utility/network roles are neither force-injected through a security boundary nor globally excluded by basename. |
| Windows App SDK / WinUI 3 uses app-local DWriteCore | [#882](https://github.com/snowie2000/mactype/issues/882) and the packaged-app reports above | Detect existing and future `DWriteCore.dll`, hook its concrete `DWriteCoreCreateFactory`, intercept immediate dynamic lookup, and cover direct `LdrLoadDll` loads beyond the bounded startup window. The same collection seam is installed on shared and isolated factories. x86 and x64 native-load probes are CI gates. |
| Qt 6.8+ changed its default Windows text backend | [#884](https://github.com/snowie2000/mactype/issues/884), [#950](https://github.com/snowie2000/mactype/issues/950), [#1097](https://github.com/snowie2000/mactype/issues/1097) | A loaded MacType DLL is not proof of a GDI render delta. Qt 6.8 moved its default backend to DirectWrite, where upstream MacType behavior is a parameter/collection intervention rather than the FreeType GDI replacement. Exercise this as a rendering-adapter and pixel/face-identity contract; do not misclassify it as failed injection or force a protected process. |
| Protected, critical, anti-cheat, antivirus, Secure Boot, or sandbox conflict | [#426](https://github.com/snowie2000/mactype/issues/426), [#1059](https://github.com/snowie2000/mactype/issues/1059), [#1106](https://github.com/snowie2000/mactype/issues/1106) | Keep the protection boundary. Protected/critical/session-0 or explicitly incompatible instances are quiet skips; external software that blocks injection remains external evidence, not a reason to weaken system policy. |
| Application owns the renderer instead of using GDI/DirectWrite | [#607](https://github.com/snowie2000/mactype/issues/607), [#1112](https://github.com/snowie2000/mactype/issues/1112), [#1143](https://github.com/snowie2000/mactype/issues/1143) | Classify as a renderer boundary, not an injection failure. Blender's private FreeType/OpenGL path and Windows Terminal's Atlas/custom glyph path still need their own adapters. Allowlisted UnityPlayer builds use `UnityFontHookLifecycle`: exact native FreeType render and face-open interception after Unity selects its OS font, never a forced global Windows file or font hook. |
| DLL presence or process-manager status but no verified visual change | [#1022](https://github.com/snowie2000/mactype/issues/1022), [#1054](https://github.com/snowie2000/mactype/issues/1054), [#1138](https://github.com/snowie2000/mactype/issues/1138), [#1139](https://github.com/snowie2000/mactype/issues/1139), [#1144](https://github.com/snowie2000/mactype/issues/1144), [#1146](https://github.com/snowie2000/mactype/issues/1146) | Keep module-loaded, hook-installed, rule-resolved, and pixel/face-substituted as separate claims. Several recent reports resolve to old or incompatible profiles, DirectWrite's narrower behavior, retained browser collections, ClearType state, or a private pixel layout rather than injection itself. The branch probes therefore require the active profile generation and semantic render evidence instead of treating a loaded module as success. |
| Unsupported machine architecture | [#904](https://github.com/snowie2000/mactype/issues/904), [#1085](https://github.com/snowie2000/mactype/issues/1085) | Native ARM64 remains explicitly unsupported until a native core/helper exists. It is not sent to an x64 helper and does not degrade global health. |

## Implemented evidence

The executable contracts now prove same-bitness x86 and x64
parent/child/grandchild propagation, an x64-to-x86-to-x64 tree, x86/x64
app-local DWriteCore factory interception through a direct native loader call,
and that a child with an explicit dynamic-code prohibition is quietly left
uninjected while the next same-name compatible child still receives the fixed
generation. The exact skipped identity is retained and a repeat is
deduplicated without changing global health.

The native x86 and x64 late-injection experiments additionally precreate an
isolated DirectWrite factory, load the new core, and observe replacement
geometry in the legacy collection, indexed collection, font-set collection,
and modern collection paths. The ordinary shared-factory contract remains in
the x86/x64 CI matrix. Missing, empty, malformed, selected-missing, and
oversized renderer profiles are rejected before a renderer can claim
successful configuration; the positive minimal-profile case prevents an
always-rejecting implementation from satisfying that gate.

Windows Explorer and Steam exposed a separate alias-collection failure: one
physical face can advertise `Malgun Gothic`, `맑은 고딕`, and weight-specific
Win32 and typographic names, while the virtual font retained only the spelling
that matched the profile. Callers using another valid name then received no
face and rendered tofu. Alias resolution now preserves every case-insensitively
distinct name for the face when all matching rules agree on one replacement;
conflicting rules retain the native face. Focused x86/x64 tests cover the
localized Semilight name set and the fail-closed conflict case.

The supported setup stop also retires the exact generated DLL-adjacent
profile. The existing early-injection tree test keeps a renderer loaded in a
parent, retires that profile before child creation, and requires both child and
grandchild to remain uninjected. Hosted service lifecycle coverage requires
the profile to stay absent through stopped repair and upgrade, then return with
the exact protected bytes before Ready.

## Unity game evidence

`scripts/lab/Test-UnityGameCompatibility.ps1` records licensed local game
evidence without redistributing a game binary. It hashes the executable and
`UnityPlayer.dll`, records Unity version and private-renderer markers, and also
records the exact loader, renderer, and adjacent profile hashes. The optional
`ExpectedCoreSha256` contract rejects a runtime before launch when it differs
from the intended release payload. The command launches stock, through the
shipped `MacLoader`, or under an explicitly named running service core, verifies
the exact MacType module, observes responsiveness for a bounded interval,
checks WER, and terminates only the exact test process it launched. If an
adapter exposes character-lookup evidence, redirects no longer count as
success unless at least one observed lookup resolves to a real glyph.

A direct executable or `MacLoader` launch can leave Steamworks uninitialized.
It is therefore only a renderer smoke test for a Steam title, not proof that
the shipped game is playable. An explicit `Steamworks is not initialized`,
`SteamManager.Initialized:False`, or later `SteamAPI_Init() failed` in the
bounded Player log now makes the evidence fail. Full compatibility requires a
Steam-launched process, a stock control that reaches the same UI boundary, and
a full-window capture after injection. Hook counters and non-empty glyph
bitmaps cannot promote a splash screen or a failed stock control to success.

`scripts/lab/Get-InstalledRuntimeProvenance.ps1` provides the cross-device
check without installing a development environment. It verifies the protected
runtime pointer and receipt, hashes both renderer architectures and helpers,
verifies the active profile digest and Unity mode, and optionally compares all
of them with a release payload manifest.

The optional Unity adapter is off by default. Selected-games mode consumes
`[UnityInclude]`; most-games mode admits Unity processes unless the 신식 서비스
finds bounded anti-cheat evidence; all-games mode consumes `[UnityExclude]` and
retains the existing protected-process and mitigation guards. Unknown Unity
images and unsupported hook backends report the Unity capability unavailable.
There is no wildcard code scan.

On 2026-08-25 Plague Inc and Rebel Inc disproved the original file-I/O adapter:
replacement file handles succeeded while the selected face remained native or
Rebel lost its text. The renderer no longer patches UnityPlayer's import table.
Exact PE/PDB descriptors cover the private render function and
`ft_open_face_internal`; only pathname-backed `FT_Open_Args` are copied and
redirected, with source and replacement TTC face indices kept explicit. Rebel
then exposed a second ambiguity: unrelated Unity `FontRef` families can share
`malgunsl.ttf`. An exact OS resolver now establishes a nested native-or-mapped
family context, so only the mapped family may redirect the shared file.
Diagnostic lookup observes but never replaces Unity's returned face.

Plague Inc then exposed why a successful face open is not a successful font
substitution. Unity 2019 opens every installed font while building a private
family catalog. Redirecting `malgun*.ttf` during that discovery pass relabelled
the catalog entry as Pretendard and removed `Malgun Gothic` from Unity's own
fallback search. The broken run redirected 5/5 opens and rendered thousands of
bitmaps, yet all 59,762 observed Korean lookups returned glyph zero. Its exact
2019.4.41 descriptor now also identifies the system-catalog entry loader. A
scoped bypass keeps discovery opens native; face-open substitution resumes only
after Unity selects a catalog entry for actual text.

The original isolated Plague run remained responsive with no WER report,
redirected 4/4 selected-face opens with zero fallbacks, rendered 7,533 non-empty
bitmaps, and resolved 10,828 of 37,918 Korean fallback probes. Its last resolved
Korean face was `Pretendard Variable`. That run nevertheless used a local core
and a direct launch, so it did not prove either release identity or Steamworks
health. On 2026-08-31 the final CI x64 core
`b94f69c167e2f1880dc282ea7fb2344e188850939044cbf1a83ad2a4f119f760`
was injected into a Steam-launched Plague process and the full-window capture
showed the Korean main menu. The installed protected service still selected the
older `f3f24daba65e` generation; under the same Steam session and profile, its
core left the same menu text empty. This separated a stale installation from
the final payload rather than treating a local build as release evidence.

The earlier Rebel counters also remain useful adapter diagnostics: 2/2 face
opens and mapped `Malgun Gothic` lookups resolved to `Pretendard Variable`.
They are not a final gameplay pass. A later stock Rebel control remained on its
background while its Player log reported repeated TLS certificate failures, so
that field session could not distinguish renderer behavior from the game's own
startup state. Synthetic TTC and descriptor-prefix tests still reject
cross-face aliases or an inexact catalog boundary; there is no runtime
signature scan.

Stopping the 신식 서비스 is deliberately non-retroactive: it blocks future
injection and retires child propagation but does not force arbitrary live
processes to unload hook code. During diagnosis, 127 live processes still held
the stopped service generation. Restarting an application therefore removes
that old immutable state, which explains why tofu sometimes vanished only
after restart. Isolated `MacLoader` mode refuses to run while the service can
inject and accepts exactly one MacType module from the requested test
directory. Service mode instead requires the service to be running and binds
evidence to the explicitly named protected core path.

## Remaining platform boundary

The service still cannot promise pre-entry interception when an unhooked,
platform-owned broker creates a process by an undocumented path and the kernel
start notification arrives after user code. The immediate trace narrows that
window; the parent relay closes it wherever a compatible parent is already
hooked. This limitation must remain visible rather than being reported as a
successful hook or “fixed” by weakening a protected broker.

Likewise, a DirectWrite collection or browser font list returned before
injection is an immutable older generation. Hooking the existing factory does
not mutate that retained object. The shipped pre-entry loader/child relay is
the supported adapter for consumers that retain font state before the open
service can observe them. Application-private FreeType, GPU atlas, canvas,
bitmap, and pre-rendered asset paths remain renderer boundaries even when the
MacType module is present.
