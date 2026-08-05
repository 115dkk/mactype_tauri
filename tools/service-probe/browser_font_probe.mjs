import { createHash, randomUUID } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';

const require = createRequire(
  process.env.MACTYPE_PLAYWRIGHT_PACKAGE_ROOT ||
    path.resolve(import.meta.dirname, '../../control-center/package.json'),
);
const { chromium, firefox } = require('@playwright/test');

function parseArguments(argv) {
  const result = {
    engine: 'chromium',
    executable: '',
    output: '',
    injectionHealth: '',
    source: '',
    replacement: '',
    expect: 'substituted',
    timeoutMs: 15000,
  };
  for (let index = 0; index < argv.length; index += 2) {
    const key = argv[index];
    const value = argv[index + 1];
    if (!value) throw new Error(`Missing value for ${key}`);
    if (key === '--engine') result.engine = value;
    else if (key === '--executable') result.executable = value;
    else if (key === '--output') result.output = value;
    else if (key === '--injection-health') result.injectionHealth = value;
    else if (key === '--source') result.source = value;
    else if (key === '--replacement') result.replacement = value;
    else if (key === '--expect') result.expect = value;
    else if (key === '--timeout-ms') result.timeoutMs = Number(value);
    else throw new Error(`Unknown argument: ${key}`);
  }
  if (!result.output || !result.source || !result.replacement) {
    throw new Error('--output, --source, and --replacement are required');
  }
  if (!['chromium', 'firefox'].includes(result.engine)) {
    throw new Error(`Unsupported engine: ${result.engine}`);
  }
  if (!['stock', 'substituted'].includes(result.expect)) {
    throw new Error(`Unsupported expectation: ${result.expect}`);
  }
  if (!Number.isInteger(result.timeoutMs) || result.timeoutMs < 0 || result.timeoutMs > 60000) {
    throw new Error(`Invalid timeout: ${result.timeoutMs}`);
  }
  return result;
}

async function readX64InjectionTelemetry(healthPath) {
  const health = JSON.parse(await readFile(healthPath, 'utf8'));
  return {
    ready: health.health === 'ready',
    telemetry: health.injection?.x64 ?? null,
  };
}

async function waitForBrowserInjection(healthPath, browserPid, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let telemetry;
  do {
    ({ telemetry } = await readX64InjectionTelemetry(healthPath));
    if (telemetry?.lastSuccess?.pid === browserPid) return telemetry.successCount;
    await new Promise((resolve) => setTimeout(resolve, 10));
  } while (Date.now() < deadline);
  throw new Error(
    `Open service did not record injection into browser PID ${browserPid} ` +
      `(last PID ${telemetry?.lastSuccess?.pid ?? 'none'})`,
  );
}

async function waitForInjectionQuiescence(healthPath, initialCount, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let count = initialCount;
  let unchangedSince = Date.now();
  do {
    const health = await readX64InjectionTelemetry(healthPath);
    const telemetry = health.telemetry;
    if (!health.ready || telemetry === null) {
      unchangedSince = Date.now();
      await new Promise((resolve) => setTimeout(resolve, 25));
      continue;
    }
    if (telemetry.successCount !== count) {
      count = telemetry.successCount;
      unchangedSince = Date.now();
    }
    // The current observer uses a one-second WMI creation window. Waiting for
    // two complete quiet windows ensures the browser's already-created helper
    // and renderer processes have had an injection opportunity before the
    // first font lookup can populate Chromium or Firefox caches.
    if (Date.now() - unchangedSince >= 2000) return count;
    await new Promise((resolve) => setTimeout(resolve, 25));
  } while (Date.now() < deadline);
  throw new Error('Open-service browser-process injection did not quiesce');
}

function collectDirectWriteDiagnostics(diagnosticNamespace) {
  const roles = ['main', 'renderer', 'utility', 'gpu', 'other'];
  const stages = [
    'hook-entered',
    'system-hook-installed',
    'shared-factory-hook-installed',
    'isolated-factory-hook-installed',
    'find-called',
    'substitution-resolved',
    'face-called',
    'face-resolved',
    'face-created',
    ...['cambria', 'impact', 'courier-new'].flatMap((family) => [
      `find-${family}`,
      `resolved-${family}`,
      `face-${family}`,
      `face-resolved-${family}`,
      `face-created-${family}`,
    ]),
  ];
  const eventNames = roles.flatMap((role) => stages.map(
    (stage) => `Local\\MacType.${diagnosticNamespace}.${role}.${stage}`,
  ));
  const script = [
    '$names = ConvertFrom-Json $env:MACTYPE_DIAGNOSTIC_EVENT_NAMES',
    '$result = @{}',
    'foreach ($name in $names) {',
    '  try {',
    '    $event = [System.Threading.EventWaitHandle]::OpenExisting($name)',
    '    try { $result[$name] = $event.WaitOne(0) } finally { $event.Dispose() }',
    '  } catch { $result[$name] = $false }',
    '}',
    '$result | ConvertTo-Json -Compress',
  ].join('; ');
  const powershell = path.join(
    process.env.SystemRoot || 'C:\\Windows',
    'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe',
  );
  const raw = JSON.parse(execFileSync(
    powershell,
    ['-NoProfile', '-NonInteractive', '-Command', script],
    {
      encoding: 'utf8',
      windowsHide: true,
      env: {
        ...process.env,
        MACTYPE_DIAGNOSTIC_EVENT_NAMES: JSON.stringify(eventNames),
      },
    },
  ));
  return Object.fromEntries(roles.map((role) => [
    role,
    Object.fromEntries(stages.map((stage) => [
      stage,
      raw[`Local\\MacType.${diagnosticNamespace}.${role}.${stage}`] === true,
    ])),
  ]));
}

async function collectChromiumFontDataHistograms(browser, engine) {
  if (engine !== 'chromium') return null;
  const session = await browser.newBrowserCDPSession();
  try {
    const result = await session.send('Browser.getHistograms', {
      query: 'Chrome.FontDataService',
      delta: false,
    });
    return result.histograms;
  } catch (error) {
    return {
      unavailable: error instanceof Error ? error.message : String(error),
    };
  } finally {
    await session.detach();
  }
}

async function capture(browserType, options, disabled, waitForReplacement) {
  const environment = { ...process.env };
  // GitHub's Windows runner launches its test tree with a service token. The
  // existing explicit force-load contract makes the injected DLL initialize
  // its user-mode DirectWrite hooks without changing injector target policy.
  environment.MACTYPE_FORCE_LOAD = '1';
  const diagnosticNamespace = `browser-${randomUUID()}`;
  environment.MACTYPE_DIRECTWRITE_DIAGNOSTICS = diagnosticNamespace;
  if (disabled) environment.MACTYPE_FONTSUBSTITUTES_ENV = '1';
  else delete environment.MACTYPE_FONTSUBSTITUTES_ENV;

  const browserServer = await browserType.launchServer({
    executablePath: options.executable || undefined,
    headless: true,
    env: environment,
  });
  try {
    const browserPid = browserServer.process().pid;
    const browser = await browserType.connect(browserServer.wsEndpoint());
    let injectionSuccessCount = null;
    if (options.injectionHealth) {
      injectionSuccessCount = await waitForBrowserInjection(
        options.injectionHealth,
        browserPid,
        options.timeoutMs,
      );
      injectionSuccessCount = await waitForInjectionQuiescence(
        options.injectionHealth,
        injectionSuccessCount,
        options.timeoutMs,
      );
    }
    const page = await browser.newPage({ viewport: { width: 900, height: 260 } });
    if (options.injectionHealth) {
      injectionSuccessCount = await waitForInjectionQuiescence(
        options.injectionHealth,
        injectionSuccessCount,
        options.timeoutMs,
      );
    }
    // Chromium calls DWriteFontCollectionProxy::FindFamilyName before its
    // first CreateCustomFontCollection call exposes that same proxy through
    // IDWriteFontCollectionLoader. Load a dedicated family first so the hook
    // is installed before the source family's first lookup. Never warm the
    // source or replacement: those remain the independent pixel proof.
    const warmupFamilies = ['Impact'].filter(
      (family) => family !== options.source && family !== options.replacement,
    );
    await page.evaluate(async (families) => {
      const canvas = document.createElement('canvas');
      const context = canvas.getContext('2d');
      for (const family of families) {
        const escaped = family.replaceAll('"', '\\"');
        context.font = `16px "${escaped}"`;
        context.measureText('MacType font pipeline warmup');
        await document.fonts.load(`16px "${escaped}"`);
      }
    }, warmupFamilies);
    const startedAt = Date.now();
    let attempts = 0;
    let observation;
    do {
      attempts += 1;
      observation = await page.evaluate(async ({ source, replacement }) => {
        const sample = 'MacType substitution 0123456789 WMWM iii 한글 中文 日本語';
        const render = async (family) => {
          const canvas = document.createElement('canvas');
          canvas.width = 900;
          canvas.height = 220;
          const context = canvas.getContext('2d', { willReadFrequently: true });
          context.fillStyle = '#fff';
          context.fillRect(0, 0, canvas.width, canvas.height);
          context.fillStyle = '#101820';
          context.textBaseline = 'top';
          context.font = `48px "${family.replaceAll('"', '\\"')}"`;
          context.fillText(sample, 16, 24);
          const metrics = context.measureText(sample);
          return {
            pngDataUrl: canvas.toDataURL('image/png'),
            width: metrics.width,
            font: context.font,
          };
        };
        return {
          source: await render(source),
          replacement: await render(replacement),
          sourceAvailable: document.fonts.check(`48px "${source}"`),
          replacementAvailable: document.fonts.check(`48px "${replacement}"`),
          userAgent: navigator.userAgent,
        };
      }, { source: options.source, replacement: options.replacement });
      if (!waitForReplacement ||
          observation.source.pngDataUrl === observation.replacement.pngDataUrl ||
          Date.now() - startedAt >= options.timeoutMs) {
        break;
      }
      await page.waitForTimeout(250);
    } while (true);
    observation.attempts = attempts;
    observation.elapsedMs = Date.now() - startedAt;
    observation.browserPid = browserPid;
    observation.injectionSuccessCount = injectionSuccessCount;
    observation.directWriteDiagnostics = collectDirectWriteDiagnostics(
      diagnosticNamespace,
    );
    observation.fontDataServiceHistograms =
      await collectChromiumFontDataHistograms(browser, options.engine);
    return observation;
  } finally {
    await browserServer.close();
  }
}

async function persistPngEvidence(observation, phase, outputPath) {
  const outputDirectory = path.dirname(outputPath);
  const outputStem = path.basename(outputPath, path.extname(outputPath));
  for (const role of ['source', 'replacement']) {
    const rendered = observation[role];
    const prefix = 'data:image/png;base64,';
    if (!rendered.pngDataUrl.startsWith(prefix)) {
      throw new Error(`Browser returned an invalid PNG data URL for ${phase}-${role}`);
    }
    const png = Buffer.from(rendered.pngDataUrl.slice(prefix.length), 'base64');
    const evidenceName = `${outputStem}.${phase}-${role}.png`;
    await writeFile(path.join(outputDirectory, evidenceName), png);
    rendered.hash = `sha256:${createHash('sha256').update(png).digest('hex')}`;
    rendered.evidenceFile = evidenceName;
    delete rendered.pngDataUrl;
  }
}

const options = parseArguments(process.argv.slice(2));
const browserType = options.engine === 'firefox' ? firefox : chromium;
const disabled = await capture(browserType, options, true, false);
const active = await capture(browserType, options, false, options.expect === 'substituted');
const resolvedOutput = path.resolve(options.output);
await mkdir(path.dirname(resolvedOutput), { recursive: true });
await persistPngEvidence(disabled, 'disabled', resolvedOutput);
await persistPngEvidence(active, 'active', resolvedOutput);
const result = {
  schemaVersion: 1,
  engine: options.engine,
  executable: options.executable || null,
  sourceFamily: options.source,
  replacementFamily: options.replacement,
  expectedState: options.expect,
  timeoutMs: options.timeoutMs,
  disabled,
  active,
  controlsDistinct: disabled.source.hash !== disabled.replacement.hash,
  sourceChanged: active.source.hash !== disabled.source.hash,
  replacementObserved: active.source.hash === active.replacement.hash,
};
result.expectationMet = result.controlsDistinct && (
  options.expect === 'stock'
    ? !result.sourceChanged && !result.replacementObserved
    : result.sourceChanged && result.replacementObserved
);
result.evidenceDigest = `sha256:${createHash('sha256')
  .update(JSON.stringify(result))
  .digest('hex')}`;

await writeFile(resolvedOutput, `${JSON.stringify(result, null, 2)}\n`, 'utf8');
process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);

if (!result.expectationMet) {
  process.exitCode = 1;
}
