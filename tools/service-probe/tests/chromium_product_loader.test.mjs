import assert from 'node:assert/strict';
import test from 'node:test';

import {
  processIdsForBrowserCleanup,
  processTreeTerminationCommand,
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
