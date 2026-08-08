import assert from 'node:assert/strict';
import test from 'node:test';

import { classifyDirectWriteGeneration } from '../browser_font_evidence.mjs';

function evidence({ aliasReturned, replacementObserved }) {
  return {
    controlsDistinct: true,
    sourceChanged: replacementObserved,
    replacementObserved,
    active: {
      directWriteDiagnostics: {
        main: {
          'hook-ready': true,
          'alias-collection-applied': true,
          'alias-collection-partial': false,
          'legacy-system-collection-alias-returned': false,
          'system-font-set-alias-returned': false,
          'modern-system-collection-alias-returned': aliasReturned,
        },
        renderer: {},
        utility: {},
        gpu: {},
        other: {},
      },
    },
  };
}

test('ordinary late Chromium retains the stock generation', () => {
  assert.deepEqual(
    classifyDirectWriteGeneration(
      evidence({ aliasReturned: false, replacementObserved: false }),
      true,
    ),
    {
      aliasSnapshotPreparedObserved: true,
      aliasCollectionReturnedObserved: false,
      aliasGenerationConsumedObserved: false,
      retainedStockGenerationObserved: true,
    },
  );
});

test('MacLoader Chromium consumes the returned alias generation', () => {
  assert.deepEqual(
    classifyDirectWriteGeneration(
      evidence({ aliasReturned: true, replacementObserved: true }),
      true,
    ),
    {
      aliasSnapshotPreparedObserved: true,
      aliasCollectionReturnedObserved: true,
      aliasGenerationConsumedObserved: true,
      retainedStockGenerationObserved: false,
    },
  );
});

test('a later Firefox alias return does not mutate its retained stock list', () => {
  assert.deepEqual(
    classifyDirectWriteGeneration(
      evidence({ aliasReturned: true, replacementObserved: false }),
      true,
    ),
    {
      aliasSnapshotPreparedObserved: true,
      aliasCollectionReturnedObserved: true,
      aliasGenerationConsumedObserved: false,
      retainedStockGenerationObserved: true,
    },
  );
});

test('stock controls have no injection-generation classification', () => {
  assert.deepEqual(
    classifyDirectWriteGeneration(
      evidence({ aliasReturned: false, replacementObserved: false }),
      false,
    ),
    {
      aliasSnapshotPreparedObserved: null,
      aliasCollectionReturnedObserved: null,
      aliasGenerationConsumedObserved: null,
      retainedStockGenerationObserved: null,
    },
  );
});
