# Hooking compatibility and explicit exclusions

This inventory was refreshed on 2026-08-20 from the upstream
`snowie2000/mactype` issue tracker. It classifies reported failures by mechanism
rather than adding executable-name workarounds. A browser or packaged app can
host several process roles with different policies, so compatibility decisions
are made for an exact `(pid, creation time)` process instance.

## Operating rule

- A confirmed process mitigation or protected boundary skips only that process.
  It is a normal `skipped` outcome: no degraded service health and no user alert.
- `ProhibitDynamicCode` without thread opt-out, and any active Microsoft-only,
  Store-only, or mitigation-opt-in image signature policy, are explicit blocks.
- A failed or unavailable mitigation query is not an implicit global refusal.
- MacType never calls `SetProcessMitigationPolicy` to weaken a target and never
  disables a browser sandbox, Code Integrity Guard, or Arbitrary Code Guard.
- Names such as `chrome.exe`, `firefox.exe`, and `RuntimeBroker.exe` are not
  globally blacklisted. Eligible roles continue through the normal path.

## Upstream report inventory

| Failure class | Representative upstream reports | Branch response |
| --- | --- | --- |
| Packaged/UWP process discovered too late or through an unexpected broker | [#56](https://github.com/snowie2000/mactype/issues/56), [#245](https://github.com/snowie2000/mactype/issues/245), [#467](https://github.com/snowie2000/mactype/issues/467), [#494](https://github.com/snowie2000/mactype/issues/494), [#518](https://github.com/snowie2000/mactype/issues/518), [#541](https://github.com/snowie2000/mactype/issues/541), [#554](https://github.com/snowie2000/mactype/issues/554), [#798](https://github.com/snowie2000/mactype/issues/798), [#814](https://github.com/snowie2000/mactype/issues/814), [#894](https://github.com/snowie2000/mactype/issues/894), [#931](https://github.com/snowie2000/mactype/issues/931), [#1017](https://github.com/snowie2000/mactype/issues/1017) | Prefer immediate `Win32_ProcessStartTrace`; fall back to the one-second intrinsic WMI query only when the trace provider is denied or unavailable. An already hooked eligible parent injects its fixed adjacent generation while each child is still suspended. |
| Same- and mixed-architecture child trees | The packaged and browser reports above include x86/x64 broker changes | A typed child-injection transaction verifies PID/creation-time/session, protection, criticality, image policy, architecture, and explicit mitigation facts against the inherited process handle. Same-bitness children use `DetourUpdateProcessWithDll` directly; mixed trees use a short-lived matching-architecture `rundll32` relay and revalidate before patching the fixed generation. Known skips and pre-mutation failures restore the caller's requested thread state. An externally terminated helper with unknown mutation state causes only that new child to be terminated rather than resuming a potentially partial image. |
| Chromium, Electron, Firefox, or Teams sandbox/code-integrity refusal | [#597](https://github.com/snowie2000/mactype/issues/597), [#614](https://github.com/snowie2000/mactype/issues/614), [#698](https://github.com/snowie2000/mactype/issues/698), [#725](https://github.com/snowie2000/mactype/issues/725), [#769](https://github.com/snowie2000/mactype/issues/769), [#1138](https://github.com/snowie2000/mactype/issues/1138), [#1146](https://github.com/snowie2000/mactype/issues/1146) | Relay eligible renderer children before entry. Quietly skip only an exact process with an explicit CIG/ACG-style block. Utility/network roles are neither force-injected through a security boundary nor globally excluded by basename. |
| Windows App SDK / WinUI 3 uses app-local DWriteCore | [#882](https://github.com/snowie2000/mactype/issues/882) and the packaged-app reports above | Detect existing and future `DWriteCore.dll`, hook `DWriteCoreCreateFactory`, intercept immediate dynamic factory lookup, and apply the shared DirectWrite factory/vtable path. x86 and x64 dynamic-load probes are CI gates. |
| Qt 6.8+ changed its default Windows text backend | [#884](https://github.com/snowie2000/mactype/issues/884), [#950](https://github.com/snowie2000/mactype/issues/950), [#1097](https://github.com/snowie2000/mactype/issues/1097) | A loaded MacType DLL is not proof of a GDI render delta. Qt 6.8 moved its default backend to DirectWrite, where upstream MacType behavior is a parameter/collection intervention rather than the FreeType GDI replacement. Exercise this as a rendering-adapter and pixel/face-identity contract; do not misclassify it as failed injection or force a protected process. |
| Protected, critical, anti-cheat, antivirus, Secure Boot, or sandbox conflict | [#426](https://github.com/snowie2000/mactype/issues/426), [#1059](https://github.com/snowie2000/mactype/issues/1059), [#1106](https://github.com/snowie2000/mactype/issues/1106) | Keep the protection boundary. Protected/critical/session-0 or explicitly incompatible instances are quiet skips; external software that blocks injection remains external evidence, not a reason to weaken system policy. |
| Application owns the renderer instead of using GDI/DirectWrite | [#607](https://github.com/snowie2000/mactype/issues/607), [#1112](https://github.com/snowie2000/mactype/issues/1112), [#1143](https://github.com/snowie2000/mactype/issues/1143) | Classify as a renderer boundary, not an injection failure. Blender's private FreeType/OpenGL path and Windows Terminal's Atlas/custom glyph path cannot be corrected by forcing a Windows font API hook. |
| DLL presence or process-manager status but no verified visual change | [#1022](https://github.com/snowie2000/mactype/issues/1022), [#1054](https://github.com/snowie2000/mactype/issues/1054), [#1138](https://github.com/snowie2000/mactype/issues/1138), [#1139](https://github.com/snowie2000/mactype/issues/1139), [#1144](https://github.com/snowie2000/mactype/issues/1144), [#1146](https://github.com/snowie2000/mactype/issues/1146) | Keep module-loaded, hook-installed, rule-resolved, and pixel/face-substituted as separate claims. Several recent reports resolve to old or incompatible profiles, DirectWrite's narrower behavior, retained browser collections, ClearType state, or a private pixel layout rather than injection itself. The branch probes therefore require the active profile generation and semantic render evidence instead of treating a loaded module as success. |
| Unsupported machine architecture | [#904](https://github.com/snowie2000/mactype/issues/904), [#1085](https://github.com/snowie2000/mactype/issues/1085) | Native ARM64 remains explicitly unsupported until a native core/helper exists. It is not sent to an x64 helper and does not degrade global health. |

## Evidence and remaining boundary

CI now proves same-bitness x86 and x64 parent/child/grandchild propagation, an
x64-to-x86-to-x64 tree, x86/x64 app-local DWriteCore factory interception, and
that a child with an explicit dynamic-code prohibition is quietly left
uninjected while the next same-name compatible child still receives the fixed
generation.
The service still cannot promise pre-entry interception when an unhooked,
platform-owned broker creates a process by an undocumented path and the kernel
start notification arrives after user code. The immediate trace narrows that
window; the parent relay closes it wherever a compatible parent is already
hooked. This limitation must remain visible rather than being reported as a
successful hook or “fixed” by weakening a protected broker.
