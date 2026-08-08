const ALIAS_RETURN_STAGES = Object.freeze([
  'legacy-system-collection-alias-returned',
  'system-font-set-alias-returned',
  'modern-system-collection-alias-returned',
]);

export function classifyDirectWriteGeneration(result, hasInjectionBoundary) {
  if (!hasInjectionBoundary) {
    return {
      aliasSnapshotPreparedObserved: null,
      aliasCollectionReturnedObserved: null,
      aliasGenerationConsumedObserved: null,
      retainedStockGenerationObserved: null,
    };
  }

  const diagnostics = result?.active?.directWriteDiagnostics;
  const main = diagnostics?.main;
  if (main === undefined) {
    throw new TypeError('DirectWrite main-process diagnostics are required');
  }

  const aliasSnapshotPreparedObserved =
    main['alias-collection-applied'] === true ||
    main['alias-collection-partial'] === true;
  const aliasCollectionReturnedObserved = ALIAS_RETURN_STAGES.some(
    (stage) => Object.values(diagnostics).some((role) => role[stage] === true),
  );
  const replacementObserved = result.replacementObserved === true;

  return {
    aliasSnapshotPreparedObserved,
    aliasCollectionReturnedObserved,
    // Publication alone is not consumption. A retained Firefox shared font
    // list may be followed by an unrelated acquisition of the new generation.
    aliasGenerationConsumedObserved:
      aliasCollectionReturnedObserved && replacementObserved,
    retainedStockGenerationObserved:
      result.controlsDistinct === true &&
      result.sourceChanged === false &&
      !replacementObserved &&
      main['hook-ready'] === true &&
      aliasSnapshotPreparedObserved,
  };
}
