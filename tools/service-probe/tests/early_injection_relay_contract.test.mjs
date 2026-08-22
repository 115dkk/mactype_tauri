import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

const root = new URL('../../../', import.meta.url);

async function source(path) {
  return readFile(new URL(path, root), 'utf8');
}

test('HookChildProcesses relays only the fixed generation DLL before child entry', async () => {
  const [hookList, hook, relay, relayHeader, spawnTree] = await Promise.all([
    source('renderer/hooklist.h'),
    source('renderer/hook.cpp'),
    source('renderer/child_process_relay.cpp'),
    source('renderer/child_process_relay.h'),
    source('tools/service-probe/spawn_tree.cpp'),
  ]);

  assert.match(hookList, /HOOK_MANUALLY\(BOOL, CreateProcessInternalW,/);
  assert.match(hook, /HookChildProcesses\(\)/);
  assert.match(hook, /hook_demand_CreateProcessInternalW/);
  assert.match(relay, /DetourUpdateProcessWithDll\(/);
  assert.match(relay, /RelayChildProcess/);
  assert.match(relay, /rundll32\.exe/);
  assert.match(relay, /ScopedChildRelayBypass/);
  assert.match(relay, /MacType64(?:\.Core)?\.dll/);
  assert.match(relay, /MacType(?:\.Core)?\.dll/);
  assert.match(relay, /ProcessDynamicCodePolicy/);
  assert.match(relay, /ProcessSignaturePolicy/);
  assert.match(relay, /shared\/hook_compatibility\.h/);
  assert.match(relay, /ClassifyTarget/);
  assert.match(relay, /protectionUnavailable/);
  assert.match(relay, /criticalityUnavailable/);
  assert.match(relay, /CThreadCounter relayLease/);
  assert.match(relay, /SuspendedChildObligation/);
  assert.match(relay, /ChildRelayDisposition::unsafeToResume/);
  assert.match(
    relay,
    /mixedArchitectureHelperTimeout[\s\S]+TerminateAndRelease/,
  );
  assert.match(relayHeader, /ChildInjectionTransactionResult/);
  assert.match(relayHeader, /ExecuteVerifiedChildInjection/);
  assert.doesNotMatch(relayHeader, /RelayCreateProcessWithFixedMacType/);
  assert.match(relay, /CREATE_SUSPENDED/);
  assert.match(relay, /ResumeThread/);
  assert.doesNotMatch(relay, /SetProcessMitigationPolicy/);
  assert.doesNotMatch(relay, /MACTYPE_RELAY_EVIDENCE/);
  assert.match(spawnTree, /--preload-mactype/);
  assert.match(spawnTree, /probe_options\.preload_mactype_path/);
});
