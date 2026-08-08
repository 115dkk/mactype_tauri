import { createHash, randomUUID } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { mkdir, readFile, unlink, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import process from 'node:process';

const require = createRequire(
  process.env.MACTYPE_PLAYWRIGHT_PACKAGE_ROOT ||
    path.resolve(import.meta.dirname, '../../control-center/package.json'),
);
const { chromium, firefox } = require('@playwright/test');
const FIREFOX_CHILD_PAUSE_SECONDS = 10;

function parseArguments(argv) {
  const result = {
    engine: 'chromium',
    executable: '',
    output: '',
    injectionHealth: '',
    source: '',
    replacement: '',
    expect: 'substituted',
    firefoxLaunchGate: '',
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
    else if (key === '--firefox-launch-gate') result.firefoxLaunchGate = value;
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
  if (result.engine !== 'firefox' && result.firefoxLaunchGate) {
    throw new Error('--firefox-launch-gate is only valid for Firefox');
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

async function waitForBrowserInjection(
  healthPath,
  browserPid,
  initialSuccessCount,
  diagnosticNamespace,
  timeoutMs,
) {
  const deadline = Date.now() + timeoutMs;
  let telemetry;
  let browserPidObserved = false;
  do {
    ({ telemetry } = await readX64InjectionTelemetry(healthPath));
    browserPidObserved ||= telemetry?.lastSuccess?.pid === browserPid;
    if (telemetry?.successCount > initialSuccessCount) {
      const diagnostics = collectDirectWriteDiagnostics(diagnosticNamespace);
      const targetTreeHooked = Object.values(diagnostics).some(
        (role) => role['hook-entered'],
      );
      if (targetTreeHooked) {
        return {
          successCount: telemetry.successCount,
          browserPidObserved,
        };
      }
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  } while (Date.now() < deadline);
  throw new Error(
    `Open service did not hook browser tree ${browserPid} ` +
      `(initial count ${initialSuccessCount}, current count ` +
      `${telemetry?.successCount ?? 'none'}, last PID ` +
      `${telemetry?.lastSuccess?.pid ?? 'none'})`,
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

async function waitForBrowserRoleInjection(
  healthPath,
  initialSuccessCount,
  diagnosticNamespace,
  role,
  timeoutMs,
) {
  const deadline = Date.now() + timeoutMs;
  let telemetry;
  do {
    ({ telemetry } = await readX64InjectionTelemetry(healthPath));
    if (telemetry?.successCount > initialSuccessCount) {
      const diagnostics = collectDirectWriteDiagnostics(diagnosticNamespace);
      if (diagnostics[role]?.['hook-entered']) return telemetry.successCount;
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  } while (Date.now() < deadline);
  throw new Error(
    `Open service did not hook browser role ${role} ` +
      `(initial count ${initialSuccessCount}, current count ` +
      `${telemetry?.successCount ?? 'none'}, last PID ` +
      `${telemetry?.lastSuccess?.pid ?? 'none'})`,
  );
}

function collectDirectWriteDiagnostics(diagnosticNamespace) {
  const roles = ['main', 'renderer', 'utility', 'gpu', 'other'];
  const stages = [
    'hook-entered',
    'hook-ready',
    'legacy-system-collection-called',
    'system-font-set-called',
    'system-font-set-alias-returned',
    'modern-system-collection-called',
    'modern-system-collection-alias-returned',
    'alias-collection-applied',
    'alias-collection-partial',
    'alias-collection-no-rules',
    'alias-collection-unsupported-factory',
    'alias-collection-system-set-unavailable',
    'alias-collection-builder-unavailable',
    'alias-collection-add-font-failed',
    'alias-collection-create-set-failed',
    'alias-collection-create-collection-failed',
    'find-called',
    'substitution-resolved',
    ...['cambria', 'impact', 'courier-new'].flatMap((family) => [
      `find-${family}`,
      `resolved-${family}`,
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

async function collectChromiumFontDataHistograms(browser, engine, delta = false) {
  if (engine !== 'chromium') return null;
  const session = await browser.newBrowserCDPSession();
  try {
    const result = await session.send('Browser.getHistograms', {
      query: 'Chrome.FontDataService',
      delta,
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
  const diagnosticNamespace = `browser-${randomUUID()}`;
  environment.MACTYPE_DIRECTWRITE_DIAGNOSTICS = diagnosticNamespace;
  if (options.engine === 'firefox' && options.injectionHealth) {
    const outputPath = path.resolve(options.output);
    await mkdir(path.dirname(outputPath), { recursive: true });
    const phase = disabled ? 'disabled' : 'active';
    environment.MOZ_LOG = 'fontlist:5';
    environment.MOZ_LOG_FILE = path.join(
      path.dirname(outputPath),
      `${path.basename(outputPath, path.extname(outputPath))}.${phase}.fontlist.log`,
    );
  }
  delete environment.MOZ_DEBUG_CHILD_PAUSE;
  if (disabled) environment.MACTYPE_FONTSUBSTITUTES_ENV = '1';
  else delete environment.MACTYPE_FONTSUBSTITUTES_ENV;

  const firefoxUserPrefs = options.engine === 'firefox'
    ? { 'dom.ipc.processPrelaunch.enabled': false }
    : undefined;
  let initialInjectionSuccessCount = null;
  if (options.injectionHealth) {
    const initialHealth = await readX64InjectionTelemetry(options.injectionHealth);
    initialInjectionSuccessCount = initialHealth.telemetry?.successCount ?? 0;
  }
  const usesFirefoxLaunchGate = options.engine === 'firefox' &&
    Boolean(options.firefoxLaunchGate);
  const gatePidPath = usesFirefoxLaunchGate
    ? path.join(
        process.env.RUNNER_TEMP || tmpdir(),
        `mactype-browser-gate-${randomUUID()}.pid`,
      )
    : null;
  if (usesFirefoxLaunchGate) {
    environment.MACTYPE_BROWSER_GATE_TARGET =
      options.executable || browserType.executablePath();
    environment.MACTYPE_BROWSER_GATE_PID_FILE = gatePidPath;
    environment.MACTYPE_BROWSER_GATE_TIMEOUT_MS = String(options.timeoutMs);
    environment.MOZ_DEBUG_CHILD_PAUSE = String(FIREFOX_CHILD_PAUSE_SECONDS);
  }
  let browserServer = null;
  try {
    browserServer = await browserType.launchServer({
      executablePath: usesFirefoxLaunchGate
        ? options.firefoxLaunchGate
        : options.executable || undefined,
      headless: true,
      env: environment,
      firefoxUserPrefs,
    });
    const browserPid = usesFirefoxLaunchGate
      ? Number((await readFile(gatePidPath, 'utf8')).trim())
      : browserServer.process().pid;
    if (!Number.isInteger(browserPid) || browserPid <= 0) {
      throw new Error(`Firefox launch gate returned an invalid PID: ${browserPid}`);
    }
    const browser = await browserType.connect(browserServer.wsEndpoint());
    let injectionSuccessCount = null;
    let browserPidInjectionObserved = null;
    if (options.injectionHealth) {
      const injection = await waitForBrowserInjection(
        options.injectionHealth,
        browserPid,
        initialInjectionSuccessCount,
        diagnosticNamespace,
        options.timeoutMs,
      );
      injectionSuccessCount = injection.successCount;
      browserPidInjectionObserved = usesFirefoxLaunchGate ||
        injection.browserPidObserved;
      injectionSuccessCount = await waitForInjectionQuiescence(
        options.injectionHealth,
        injectionSuccessCount,
        options.timeoutMs,
      );
    }
    const page = await browser.newPage({ viewport: { width: 900, height: 260 } });
    if (options.injectionHealth) {
      // Firefox can reuse a content process that the service hooked during
      // launch, so prove this browser tree added the renderer after the
      // pre-launch baseline instead of requiring another post-page injection.
      injectionSuccessCount = await waitForBrowserRoleInjection(
        options.injectionHealth,
        initialInjectionSuccessCount,
        diagnosticNamespace,
        'renderer',
        options.timeoutMs,
      );
      injectionSuccessCount = await waitForInjectionQuiescence(
        options.injectionHealth,
        injectionSuccessCount,
        options.timeoutMs,
      );
    }
    await collectChromiumFontDataHistograms(browser, options.engine, true);
    const probeFamily = async (family) => {
      await page.evaluate(async (name) => {
        const escaped = name.replaceAll('"', '\\"');
        const canvas = document.createElement('canvas');
        const context = canvas.getContext('2d');
        context.font = `48px "${escaped}"`;
        context.measureText('MacType FontDataService diagnostic');
        await document.fonts.load(`48px "${escaped}"`);
      }, family);
      return collectChromiumFontDataHistograms(
        browser, options.engine, true,
      );
    };
    const fontDataServiceFamilyDeltas = {
      source: await probeFamily(options.source),
      replacement: await probeFamily(options.replacement),
    };
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
    observation.browserPidInjectionObserved = browserPidInjectionObserved;
    observation.injectionSuccessCount = injectionSuccessCount;
    observation.directWriteDiagnostics = collectDirectWriteDiagnostics(
      diagnosticNamespace,
    );
    observation.fontDataServiceFamilyDeltas = fontDataServiceFamilyDeltas;
    observation.fontDataServiceHistograms =
      await collectChromiumFontDataHistograms(browser, options.engine);
    return observation;
  } finally {
    if (browserServer) await browserServer.close();
    if (gatePidPath) {
      await unlink(gatePidPath).catch((error) => {
        if (error?.code !== 'ENOENT') throw error;
      });
    }
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
  firefoxLaunchGate: options.engine === 'firefox'
    ? (options.firefoxLaunchGate ? path.basename(options.firefoxLaunchGate) : null)
    : null,
  firefoxChildPauseSeconds: options.engine === 'firefox'
    ? (options.firefoxLaunchGate ? FIREFOX_CHILD_PAUSE_SECONDS : 0)
    : null,
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
