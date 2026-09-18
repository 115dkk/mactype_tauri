// Measures how smoothly the Control Center window moves under each skin.
//
// The app must be running with WebView2 remote debugging enabled
// (WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=<port>).
// For every skin the script switches the page to that skin, starts a frame
// sampler inside the page, and asks Move-Window.ps1 to move the window the
// way a drag does. Two signals come back: the window thread's cost per move
// step (SetWindowPos latency) and the page's animation-frame gaps while the
// window moves. The classic skin is the control; every other skin is
// reported as a ratio against it.
//
// usage: node window-move-probe.mjs --cdp http://127.0.0.1:9333 --pid 1234
//          --out artifacts/window-move-probe [--skins classic,console,fluent,cupertino]
//          [--steps 240] [--interval 8] [--amplitude 120] [--drag] [--rounds 2]
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "../..");
const require = createRequire(path.join(root, "control-center", "package.json"));
const { chromium } = require("@playwright/test");

function argument(name, fallback) {
  const index = process.argv.indexOf(`--${name}`);
  return index >= 0 && index + 1 < process.argv.length ? process.argv[index + 1] : fallback;
}
const flag = (name) => process.argv.includes(`--${name}`);

const cdpUrl = argument("cdp", "http://127.0.0.1:9333");
const pid = Number(argument("pid", "0"));
const outDir = path.resolve(argument("out", path.join(root, "artifacts", "window-move-probe")));
const skins = argument("skins", "classic,console,fluent,cupertino").split(",").map((skin) => skin.trim()).filter(Boolean);
/* Views to measure under every skin; the overview alone by default. A view
   is reached through its navigation entry, so the app itself drives the
   change of page. */
const views = argument("views", "overview").split(",").map((view) => view.trim()).filter(Boolean);
const navigationIds = { overview: "overview", files: "files", profiles: "all", execution: "execution", diagnostics: "diagnostics" };
const steps = Number(argument("steps", "240"));
const interval = Number(argument("interval", "8"));
const amplitude = Number(argument("amplitude", "120"));
const rounds = Number(argument("rounds", "2"));
const withDrag = flag("drag");
if (!pid) throw new Error("--pid is required");
fs.mkdirSync(outDir, { recursive: true });

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function percentile(values, fraction) {
  if (values.length === 0) return 0;
  const sorted = [...values].sort((left, right) => left - right);
  const index = Math.min(sorted.length - 1, Math.max(0, Math.ceil(fraction * sorted.length) - 1));
  return sorted[index];
}
const round = (value, digits = 2) => Number(value.toFixed(digits));

function frameStats(sample) {
  const gaps = [];
  for (let index = 1; index < sample.frames.length; index++) gaps.push(sample.frames[index] - sample.frames[index - 1]);
  const longTaskMs = sample.longTasks.reduce((total, [, duration]) => total + duration, 0);
  return {
    frames: sample.frames.length,
    durationMs: round(sample.frames.length > 1 ? sample.frames[sample.frames.length - 1] - sample.frames[0] : 0),
    gapP50Ms: round(percentile(gaps, 0.5)),
    gapP95Ms: round(percentile(gaps, 0.95)),
    gapMaxMs: round(gaps.length ? Math.max(...gaps) : 0),
    gapsOver33Ms: gaps.filter((gap) => gap > 33).length,
    longTasks: sample.longTasks.length,
    longTaskMs: round(longTaskMs),
  };
}

function moveWindow(mode) {
  const script = path.join(here, "Move-Window.ps1");
  const result = spawnSync("pwsh", ["-NoProfile", "-NonInteractive", "-File", script, "-ProcessId", String(pid), "-Mode", mode, "-Steps", String(steps), "-IntervalMs", String(interval), "-Amplitude", String(amplitude)], { encoding: "utf8", timeout: 120_000 });
  if (result.status !== 0) throw new Error(`Move-Window.ps1 (${mode}) failed: ${result.stderr || result.stdout}`);
  const line = result.stdout.trim().split(/\r?\n/).filter(Boolean).pop();
  return JSON.parse(line);
}

const startSampler = () => {
  const probe = { frames: [], longTasks: [], running: true, observer: null };
  window.__moveProbe = probe;
  try {
    const observer = new PerformanceObserver((list) => {
      for (const entry of list.getEntries()) probe.longTasks.push([entry.startTime, entry.duration]);
    });
    observer.observe({ entryTypes: ["longtask"] });
    probe.observer = observer;
  } catch {
    /* long tasks are optional evidence */
  }
  const tick = (time) => {
    probe.frames.push(time);
    if (probe.running) requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
};
const stopSampler = () => {
  const probe = window.__moveProbe;
  probe.running = false;
  probe.observer?.disconnect();
  return { frames: probe.frames, longTasks: probe.longTasks };
};

/* Compositor evidence from a Chromium trace: frames the compositor drew
   and pipeline reports it marked dropped while the window moved. */
function traceStats(buffer) {
  let events;
  try {
    const parsed = JSON.parse(buffer.toString("utf8"));
    events = Array.isArray(parsed) ? parsed : parsed.traceEvents ?? [];
  } catch {
    return null;
  }
  let drawn = 0;
  let presented = 0;
  let dropped = 0;
  for (const event of events) {
    if (event.name === "DrawFrame") drawn++;
    if (event.name === "PipelineReporter") {
      const state = event.args?.chrome_frame_reporter?.state ?? "";
      if (state.includes("DROPPED")) dropped++;
      else if (state.includes("PRESENTED")) presented++;
    }
  }
  return { drawn, presented, dropped };
}

async function measure(page, mode, traceFile) {
  let tracing = false;
  try {
    await browser.startTracing(page, { screenshots: false, path: traceFile, categories: ["cc", "benchmark", "disabled-by-default-devtools.timeline", "disabled-by-default-devtools.timeline.frame"] });
    tracing = true;
  } catch {
    /* tracing is optional evidence; the samplers still measure */
  }
  await page.evaluate(startSampler);
  await sleep(150);
  const move = moveWindow(mode);
  await sleep(150);
  const sample = await page.evaluate(stopSampler);
  let trace = null;
  if (tracing) {
    try {
      trace = traceStats(await browser.stopTracing());
    } catch {
      trace = null;
    }
  }
  return { move, page: frameStats(sample), trace };
}

async function connect() {
  let lastError;
  for (let attempt = 0; attempt < 60; attempt++) {
    try {
      return await chromium.connectOverCDP(cdpUrl, { timeout: 5000 });
    } catch (error) {
      lastError = error;
      await sleep(1000);
    }
  }
  throw new Error(`could not connect to ${cdpUrl}: ${lastError}`);
}

async function findMainPage(browser) {
  for (let attempt = 0; attempt < 60; attempt++) {
    const pages = browser.contexts().flatMap((context) => context.pages());
    const page = pages.find((candidate) => /tauri\.localhost|index\.html/.test(candidate.url()) && !/preview-studio/.test(candidate.url()));
    if (page) return page;
    await sleep(500);
  }
  throw new Error("the main window page did not appear over CDP");
}

async function settle(page) {
  /* Let the page settle: specimen strips arrive from the helper and the
     status polls answer; a move measured during that would blame the skin
     for start-up work. */
  await page.waitForSelector(".specimen-board img, .preview-strip img", { timeout: 15_000 }).catch(() => undefined);
  await sleep(2500);
}

async function showSkin(page, skin) {
  const url = new URL(page.url());
  url.search = `?skin=${skin}`;
  await page.goto(url.toString(), { waitUntil: "load", timeout: 60_000 });
  await page.waitForSelector(`html[data-skin="${skin}"]`, { timeout: 30_000 });
  await page.waitForSelector('body[data-rendered="true"]', { timeout: 60_000 });
  await settle(page);
}

async function showView(page, view) {
  const current = await page.evaluate(() => document.body.dataset.view);
  if (current === view) return;
  const id = navigationIds[view];
  if (!id) throw new Error(`unknown view ${view}`);
  await page.click(`button[data-nav="${id}"]`);
  await page.waitForSelector(`body[data-view="${view}"]`, { timeout: 30_000 });
  await settle(page);
}

const browser = await connect();
const page = await findMainPage(browser);
const report = { startedAt: new Date().toISOString(), pid, cdpUrl, steps, interval, amplitude, rounds, withDrag, views, cases: {} };

for (const skin of skins) {
  await showSkin(page, skin);
  for (const view of views) {
    await showView(page, view);
    const name = `${skin}/${view}`;
    const entry = { skin, view, push: [], drag: [] };
    for (let roundIndex = 0; roundIndex < rounds; roundIndex++) {
      entry.push.push(await measure(page, "push", path.join(outDir, `trace-${skin}-${view}-push-${roundIndex}.json`)));
      await sleep(500);
      if (withDrag) {
        entry.drag.push(await measure(page, "drag", path.join(outDir, `trace-${skin}-${view}-drag-${roundIndex}.json`)));
        await sleep(500);
      }
    }
    report.cases[name] = entry;
    console.log(`measured ${name}`);
  }
}
await browser.close().catch(() => undefined);

/* The worst round per skin is the reported one: a lag the user sees once is
   still a lag, and the rounds otherwise agree closely. */
function summarise(entry) {
  const worstPush = entry.push.reduce((worst, run) => (!worst || run.move.p95Ms > worst.move.p95Ms ? run : worst), null);
  const worstDrag = entry.drag.reduce((worst, run) => (!worst || run.move.p95LagPx > worst.move.p95LagPx ? run : worst), null);
  return {
    pushP95Ms: worstPush.move.p95Ms,
    pushMaxMs: worstPush.move.maxMs,
    pushOver16: worstPush.move.over16Ms,
    gapP95Ms: worstPush.page.gapP95Ms,
    gapMaxMs: worstPush.page.gapMaxMs,
    gapsOver33: worstPush.page.gapsOver33Ms,
    frames: worstPush.page.frames,
    longTaskMs: worstPush.page.longTaskMs,
    drawn: worstPush.trace?.drawn ?? null,
    dropped: worstPush.trace?.dropped ?? null,
    dragSupported: worstDrag ? worstDrag.move.inputAccepted && worstDrag.move.windowMoved : null,
    dragP95LagPx: worstDrag ? worstDrag.move.p95LagPx : null,
    dragMaxLagPx: worstDrag ? worstDrag.move.maxLagPx : null,
    dragPositions: worstDrag ? worstDrag.move.distinctPositions : null,
    dragGapP95Ms: worstDrag ? worstDrag.page.gapP95Ms : null,
  };
}
const summary = Object.fromEntries(Object.entries(report.cases).map(([name, entry]) => [name, { skin: entry.skin, view: entry.view, ...summarise(entry) }]));
const ratio = (value, base) => (base > 0 && value !== null ? round(value / base, 2) : null);
for (const [name, values] of Object.entries(summary)) {
  /* The control is the classic skin on the same view, or the first case measured on that view. */
  const control = summary[`classic/${values.view}`] ?? Object.values(summary).find((candidate) => candidate.view === values.view);
  values.control = control === values;
  values.pushP95Ratio = ratio(values.pushP95Ms, control.pushP95Ms);
  values.gapP95Ratio = ratio(values.gapP95Ms, control.gapP95Ms);
  values.dragLagRatio = values.dragP95LagPx === null ? null : ratio(values.dragP95LagPx, control.dragP95LagPx);
  values.notSmooth = !values.control && ((values.pushP95Ratio ?? 0) > 1.5 || (values.gapP95Ratio ?? 0) > 1.5 || values.gapsOver33 > Math.max(3, values.frames * 0.1) || (values.dragLagRatio ?? 0) > 1.5);
  if (values.control && (values.pushP95Ms > 16 || values.gapsOver33 > Math.max(3, values.frames * 0.1))) values.notSmooth = true;
  void name;
}
report.summary = summary;
fs.writeFileSync(path.join(outDir, "report.json"), JSON.stringify(report, null, 2));

const lines = [
  "| skin / view | push p95 ms | push max ms | steps > 16 ms | rAF gap p95 ms | gap max ms | gaps > 33 ms / frames | long tasks ms | compositor drawn / dropped | drag p95 lag px | drag max lag px | ratio push / gap / drag | verdict |",
  "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |",
];
for (const [name, v] of Object.entries(summary)) {
  const drag = v.dragSupported === null ? "n/a | n/a" : v.dragSupported ? `${v.dragP95LagPx} | ${v.dragMaxLagPx}` : "input not accepted | -";
  const compositor = v.drawn === null ? "n/a" : `${v.drawn} / ${v.dropped}`;
  const verdict = v.control ? (v.notSmooth ? "control, NOT SMOOTH" : "control") : v.notSmooth ? "NOT SMOOTH" : "smooth";
  lines.push(`| ${name} | ${v.pushP95Ms} | ${v.pushMaxMs} | ${v.pushOver16} | ${v.gapP95Ms} | ${v.gapMaxMs} | ${v.gapsOver33} / ${v.frames} | ${v.longTaskMs} | ${compositor} | ${drag} | ${v.pushP95Ratio ?? "-"} / ${v.gapP95Ratio ?? "-"} / ${v.dragLagRatio ?? "-"} | ${verdict} |`);
}
const markdown = `## Window move probe\n\nsteps ${steps} × ${interval} ms, amplitude ${amplitude} px, rounds ${rounds}, worst round reported; the control is the classic skin on the same view.\n\n${lines.join("\n")}\n`;
fs.writeFileSync(path.join(outDir, "summary.md"), markdown);
console.log(markdown);
if (process.env.GITHUB_STEP_SUMMARY) fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, markdown);
