import assert from 'node:assert/strict';
import test from 'node:test';

import {
  profileProcessTerminationCommand,
  processIdsForBrowserCleanup,
  processTreeTerminationCommand,
  userDataRemovalRetryPolicy,
} from '../chromium_product_loader.mjs';

test('cleanup binds every current CDP browser process with the browser root first', () => {
  assert.deepEqual(
    processIdsForBrowserCleanup([
      { id: 3003, type: 'renderer' },
      { id: 1001, type: 'browser' },
      { id: 2002, type: 'utility' },
      { id: 2002, type: 'utility' },
      { id: 0, type: 'gpu' },
      { id: '4004', type: 'other' },
    ], 9999),
    [1001, 3003, 2002],
  );
});

test('cleanup retains the verified launch PID when CDP has no browser root', () => {
  assert.deepEqual(
    processIdsForBrowserCleanup([
      { id: 3003, type: 'renderer' },
    ], 9999),
    [9999, 3003],
  );
  assert.deepEqual(processIdsForBrowserCleanup([], 9999), [9999]);
});

test('Windows cleanup binds non-CDP helpers to the exact temporary profile', () => {
  const command = profileProcessTerminationCommand(
    'D:\\a\\_temp\\mactype-chromium-loader-proof',
    'win32',
    'D:\\Windows',
  );
  assert.equal(
    command.executable,
    'D:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe',
  );
  assert.deepEqual(command.arguments.slice(0, 4), [
    '-NoLogo',
    '-NoProfile',
    '-NonInteractive',
    '-Command',
  ]);
  assert.match(command.arguments[4], /Get-CimInstance Win32_Process/u);
  assert.match(command.arguments[4], /IndexOf\(\$target/u);
  assert.match(command.arguments[4], /Stop-Process -Id/u);
  assert.deepEqual(command.environment, {
    MACTYPE_TEST_BROWSER_PROFILE:
      'D:\\a\\_temp\\mactype-chromium-loader-proof',
  });
});

test('temporary-profile cleanup is Windows-only and rejects an unbound path', () => {
  assert.equal(
    profileProcessTerminationCommand('/tmp/proof', 'linux', '/unused'),
    null,
  );
  assert.throws(
    () => profileProcessTerminationCommand('relative-profile', 'win32'),
    /absolute temporary Chromium profile/u,
  );
});

test('locked profile deletion has a bounded linear retry budget', () => {
  assert.deepEqual(userDataRemovalRetryPolicy, {
    maxRetries: 24,
    retryDelay: 100,
  });
  const totalDelay = userDataRemovalRetryPolicy.retryDelay
    * userDataRemovalRetryPolicy.maxRetries
    * (userDataRemovalRetryPolicy.maxRetries + 1)
    / 2;
  assert.ok(totalDelay <= 30_000);
});

test('Windows cleanup targets only the exact browser process tree', () => {
  assert.deepEqual(
    processTreeTerminationCommand(4242, 'win32', 'D:\\Windows'),
    {
      executable: 'D:\\Windows\\System32\\taskkill.exe',
      arguments: ['/PID', '4242', '/T', '/F'],
    },
  );
});

test('non-Windows cleanup keeps the graceful browser path', () => {
  assert.equal(
    processTreeTerminationCommand(4242, 'linux', '/unused'),
    null,
  );
});

test('process-tree cleanup rejects an unbound PID', () => {
  for (const pid of [0, -1, 1.5, Number.NaN]) {
    assert.throws(
      () => processTreeTerminationCommand(pid, 'win32', 'C:\\Windows'),
      /Invalid browser PID/u,
    );
  }
});
