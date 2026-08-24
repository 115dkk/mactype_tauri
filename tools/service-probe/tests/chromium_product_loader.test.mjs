import assert from 'node:assert/strict';
import test from 'node:test';

import {
  processTreeTerminationCommand,
} from '../chromium_product_loader.mjs';

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
