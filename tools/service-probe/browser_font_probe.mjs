import { createHash } from 'node:crypto';
import { createRequire } from 'node:module';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';

const require = createRequire(
  process.env.MACTYPE_PLAYWRIGHT_PACKAGE_ROOT ||
    path.resolve(import.meta.dirname, '../../control-center/package.json'),
);
const { chromium, firefox } = require('@playwright/test');

function parseArguments(argv) {
  const result = { engine: 'chromium', executable: '', output: '', source: '', replacement: '' };
  for (let index = 0; index < argv.length; index += 2) {
    const key = argv[index];
    const value = argv[index + 1];
    if (!value) throw new Error(`Missing value for ${key}`);
    if (key === '--engine') result.engine = value;
    else if (key === '--executable') result.executable = value;
    else if (key === '--output') result.output = value;
    else if (key === '--source') result.source = value;
    else if (key === '--replacement') result.replacement = value;
    else throw new Error(`Unknown argument: ${key}`);
  }
  if (!result.output || !result.source || !result.replacement) {
    throw new Error('--output, --source, and --replacement are required');
  }
  if (!['chromium', 'firefox'].includes(result.engine)) {
    throw new Error(`Unsupported engine: ${result.engine}`);
  }
  return result;
}

async function capture(browserType, options, disabled) {
  const environment = { ...process.env };
  if (disabled) environment.MACTYPE_FONTSUBSTITUTES_ENV = '1';
  else delete environment.MACTYPE_FONTSUBSTITUTES_ENV;

  const browser = await browserType.launch({
    executablePath: options.executable || undefined,
    headless: true,
    env: environment,
  });
  try {
    const page = await browser.newPage({ viewport: { width: 900, height: 260 } });
    const observation = await page.evaluate(async ({ source, replacement }) => {
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
    for (const rendered of [observation.source, observation.replacement]) {
      rendered.hash = `sha256:${createHash('sha256')
        .update(rendered.pngDataUrl)
        .digest('hex')}`;
      delete rendered.pngDataUrl;
    }
    return observation;
  } finally {
    await browser.close();
  }
}

const options = parseArguments(process.argv.slice(2));
const browserType = options.engine === 'firefox' ? firefox : chromium;
const disabled = await capture(browserType, options, true);
const active = await capture(browserType, options, false);
const result = {
  schemaVersion: 1,
  engine: options.engine,
  executable: options.executable || null,
  sourceFamily: options.source,
  replacementFamily: options.replacement,
  disabled,
  active,
  controlsDistinct: disabled.source.hash !== disabled.replacement.hash,
  sourceChanged: active.source.hash !== disabled.source.hash,
  replacementObserved: active.source.hash === active.replacement.hash,
};
result.evidenceDigest = `sha256:${createHash('sha256')
  .update(JSON.stringify(result))
  .digest('hex')}`;

await mkdir(path.dirname(path.resolve(options.output)), { recursive: true });
await writeFile(options.output, `${JSON.stringify(result, null, 2)}\n`, 'utf8');
process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);

if (!result.controlsDistinct || !result.sourceChanged || !result.replacementObserved) {
  process.exitCode = 1;
}
