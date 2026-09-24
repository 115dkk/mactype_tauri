import { expect, test } from "@playwright/test";
import fs from "node:fs";
import path from "node:path";
import { galleryLocales, galleryViews } from "./windows";
import {
  projectServiceCapabilities,
  type ServiceCapabilityInput,
  type ServiceCapabilityProjection,
} from "../../control-center/src/app/runtimeAdapters/serviceCapabilityPolicy";
import {
  galleryExecutionStatus,
  transitionGalleryExecutionStatus,
  transitionGalleryLegacyTrayAutostartDisable,
  transitionGalleryLegacyTrayExit,
  transitionGalleryRunProfile,
} from "../../control-center/src/app/runtimeAdapters/browserGalleryExecution";
import type { ExecutionStatus } from "../../control-center/src/app/model";

const serviceCapabilityCases = JSON.parse(
  fs.readFileSync(path.join(__dirname, "..", "..", "shared", "service-capability-cases.json"), "utf8"),
) as Array<{ name: string; input: ServiceCapabilityInput; expected: ServiceCapabilityProjection }>;

for (const capabilityCase of serviceCapabilityCases) {
  test(`shared service capability projection matches ${capabilityCase.name}`, () => {
    expect(projectServiceCapabilities(capabilityCase.input), capabilityCase.name).toEqual(capabilityCase.expected);
  });
}

function galleryCapabilities(status: ExecutionStatus): ServiceCapabilityProjection {
  const { canInstall, canRemove, canStart, canStop, canRepair, canUpgrade } = status.systemService;
  return {
    canInstall, canRemove, canStart, canStop, canRepair, canUpgrade,
    systemModesSupported: status.systemModesSupported,
    migrationAvailable: status.legacyMacTray?.migrationAvailable ?? false,
  };
}

for (const fixture of [
  { name: "legacy-tray-conflict-present", query: "system-service=ready&service-runtime=stopped&legacy-tray=trusted-current" },
  { name: "foreign-backend", query: "system-service=foreign-service&service-runtime=running" },
  { name: "inaccessible-installation", query: "system-service=inaccessible-service&service-runtime=unknown" },
  { name: "stopped-current-package-not-installed", query: "system-service=ready&service-runtime=stopped&service-package=not-installed" },
  { name: "stopped-current-package-incomplete", query: "system-service=ready&service-runtime=stopped&service-package=incomplete" },
  { name: "stopped-current-package-untrusted", query: "system-service=ready&service-runtime=stopped&service-package=untrusted" },
]) {
  test(`gallery preserves Rust capability gates for ${fixture.name}`, async ({ page }) => {
    const query = `${fixture.query}&legacy=migration-available&legacy-state=stopped`;
    const capabilityCase = serviceCapabilityCases.find((candidate) => candidate.name === fixture.name);
    if (!capabilityCase) throw new Error(`Missing shared capability case: ${fixture.name}`);
    const status = galleryExecutionStatus(new URLSearchParams(query));
    expect(galleryCapabilities(status), fixture.name).toEqual(capabilityCase.expected);
    const designated = transitionGalleryRunProfile(status, "Profiles\\Gallery.ini", false);
    expect(galleryCapabilities(designated), `${fixture.name} after designation`).toEqual(capabilityCase.expected);

    await page.goto(`/?view=execution&gallery=1&lang=en&${query}`, { waitUntil: "networkidle" });
    await openServiceDetails(page);
    const migrate = page.locator('[data-service-backend="legacy-mactray"]').getByRole("button", { name: "Migrate", exact: true });
    if (fixture.name === "legacy-tray-conflict-present") await expect(migrate).toBeDisabled();
    else await expect(migrate).toBeEnabled();
    expect(await overflowingElements(page)).toEqual([]);
  });
}

test("gallery capability projection preserves drift, tray clamps and absent transitions", () => {
  const status = galleryExecutionStatus(new URLSearchParams("system-service=ready&legacy-tray=trusted-current&legacy-startup=hkcu-run&legacy=migration-available"));
  expect(status.systemModesSupported).toBe(true);
  expect(status.legacyMacTray?.migrationAvailable).toBe(true);
  expect(status.systemService.canStop).toBe(true);
  expect(status.systemService.canRemove).toBe(false);
  const process = status.legacyTray.process;
  if (process.state !== "trusted-current-session") throw new Error("Expected a trusted gallery MacTray");
  const exited = transitionGalleryLegacyTrayExit(status, process);
  expect(exited.systemModesSupported).toBe(true);
  expect(exited.systemInjectionActive).toBe(false);
  expect(exited.systemService.canRemove).toBe(false);
  expect(exited.legacyMacTray?.migrationAvailable).toBe(true);
  const cleared = transitionGalleryLegacyTrayAutostartDisable(exited);
  expect(cleared.systemService.canRemove).toBe(true);
  const stopped = transitionGalleryExecutionStatus(cleared, "stop");
  const drifted = transitionGalleryRunProfile({
    ...stopped,
    systemService: { ...stopped.systemService, configurationDrift: true },
  }, "ini\\Default.ini", false);
  expect(drifted.systemService.configurationDrift).toBe(true);
  expect(drifted.systemService.canStart).toBe(false);
  expect(drifted.systemService.canRepair).toBe(true);
  const removed = transitionGalleryExecutionStatus(stopped, "remove");
  expect(removed.systemService.backend).toBe("none");
  expect(removed.systemService.canInstall).toBe(true);
  expect(removed.systemService.canRemove).toBe(false);
  const absent = galleryExecutionStatus(new URLSearchParams("system-service=migration-available"));
  expect(absent.systemService.backend).toBe("none");
  expect(absent.systemService.canInstall).toBe(true);
  expect(absent.systemService.canRemove).toBe(false);
  const deleting = galleryExecutionStatus(new URLSearchParams("system-service=delete-pending"));
  expect(deleting.systemService.runtime).toBe("unknown");
  expect(deleting.systemService.canRemove).toBe(false);
});

test("running legacy migration requires a trusted binary regardless of a declared capability", () => {
  const capabilityCase = serviceCapabilityCases.find((candidate) => candidate.name === "legacy-migration-available");
  if (!capabilityCase) throw new Error("Missing shared legacy migration case");
  const input: ServiceCapabilityInput = {
    ...capabilityCase.input,
    legacy: { presence: "owned", state: "running", migrationAvailable: true },
  };
  expect(projectServiceCapabilities(input).migrationAvailable).toBe(false);
  expect(projectServiceCapabilities({
    ...input,
    legacy: { ...input.legacy, trustedBinaryAvailable: true },
  }).migrationAvailable).toBe(true);
});

const galleryRoot = path.resolve(__dirname, "../../artifacts/frontend-gallery");
/* The docked preview is a property of the shipped window size, so the gallery
   reads that size rather than restating it. */
const defaultWindow = JSON.parse(
  fs.readFileSync(path.resolve(__dirname, "../../control-center/src-tauri/tauri.conf.json"), "utf8"),
).app.windows[0] as { width: number; height: number };

async function overflowingElements(page: import("@playwright/test").Page) {
  return page.evaluate(() => {
    const viewportWidth = document.documentElement.clientWidth;
    const isClippedByAncestor = (element: HTMLElement) => {
      let parent = element.parentElement;
      while (parent && parent !== document.body) {
        const overflowX = window.getComputedStyle(parent).overflowX;
        if (["auto", "scroll", "hidden", "clip"].includes(overflowX)) return true;
        parent = parent.parentElement;
      }
      return false;
    };
    return [...document.querySelectorAll<HTMLElement>("body *")]
      .map((element) => ({ element, rect: element.getBoundingClientRect() }))
      .filter(({ element, rect }) => (rect.right > viewportWidth + 1 || rect.left < -1) && !isClippedByAncestor(element))
      .map(({ element, rect }) => `${element.tagName.toLowerCase()}.${element.className || "-"} [${Math.round(rect.left)}, ${Math.round(rect.right)}]`)
      .slice(0, 12);
  });
}

async function openServiceDetails(page: import("@playwright/test").Page) {
  const rows = page.locator("details.service-row");
  for (let index = 0; index < await rows.count(); index += 1) {
    const row = rows.nth(index);
    if (!await row.evaluate((element) => (element as HTMLDetailsElement).open)) {
      await row.locator("summary").click();
    }
  }
}

const executionStateGallery = [
  { id: "ready", query: "system-service=ready", expected: "Running" },
  { id: "degraded", query: "system-service=degraded", expected: "Running" },
  { id: "initializing", query: "system-service=initializing", expected: "Running" },
  { id: "health-unknown", query: "system-service=unknown-health", expected: "Running" },
  { id: "stopped", query: "system-service=ready&service-runtime=stopped", expected: "Stopped" },
  { id: "stopped-no-profile", query: "system-service=ready&service-runtime=stopped&profile-unapplied=1", expected: "No run profile" },
  { id: "starting", query: "system-service=ready&service-runtime=start-pending", expected: "Starting" },
  { id: "stopping", query: "system-service=ready&service-runtime=stop-pending", expected: "Stopping" },
  { id: "paused", query: "system-service=ready&service-runtime=paused", expected: "Paused" },
  { id: "runtime-unknown", query: "system-service=ready&service-runtime=unknown", expected: "Unknown state" },
  { id: "failed", query: "system-service=failed", expected: "Service configuration needs repair." },
  { id: "outdated", query: "system-service=outdated", expected: "Update required" },
  { id: "profile-mismatch", query: "system-service=profile-mismatch", expected: "Service running with a different profile" },
  { id: "not-installed", query: "system-service=migration-available", expected: "Install service" },
  { id: "foreign-service", query: "system-service=foreign-service", expected: "different configuration" },
  { id: "inaccessible-service", query: "system-service=inaccessible-service", expected: "Inaccessible" },
  { id: "removal-pending", query: "system-service=delete-pending", expected: "Removal pending" },
  { id: "appinit-running", query: "system-service=legacy-conflict&legacy=migration-available&raw-active=1", expected: "Service running while AppInit conflicts" },
  { id: "appinit-stopped", query: "system-service=legacy-conflict&service-runtime=stopped", expected: "AppInit registry mode is active" },
  { id: "legacy-migration-running", query: "system-service=migration-available&legacy=migration-available&legacy-state=running", expected: "Legacy MacTray was detected." },
  { id: "legacy-migration-stopped", query: "system-service=migration-available&legacy=migration-available&legacy-state=stopped", expected: "Legacy MacTray was detected." },
  { id: "legacy-transition", query: "system-service=migration-available&legacy=migration-available&legacy-state=start-pending", expected: "A legacy MacTray service must be resolved first" },
  { id: "legacy-foreign", query: "system-service=migration-available&legacy=foreign", expected: "A foreign legacy MacTray service was detected" },
  { id: "legacy-uncertain", query: "system-service=migration-available&legacy=inaccessible", expected: "Legacy MacTray service status could not be verified" },
  { id: "mactray-current-session", query: "system-service=migration-available&legacy-tray=trusted-current", expected: "Existing MacTray is running" },
  { id: "mactray-other-session", query: "system-service=migration-available&legacy-tray=trusted-other", expected: "MacTray is running in another user session" },
  { id: "mactray-untrusted-process", query: "system-service=migration-available&legacy-tray=untrusted", expected: "Check this copy of MacTray" },
  { id: "mactray-process-unknown", query: "system-service=migration-available&legacy-tray=unknown", expected: "MacTray tray mode status is unavailable" },
  { id: "mactray-autostart", query: "system-service=migration-available&legacy-startup=hkcu-run", expected: "MacTray autostart must be disabled" },
  { id: "mactray-autostart-untrusted", query: "system-service=migration-available&legacy-startup=untrusted", expected: "A MacTray autostart entry could not be trusted" },
  { id: "mactray-autostart-unknown", query: "system-service=migration-available&legacy-startup=unknown", expected: "MacTray autostart status is unavailable" },
  { id: "native-running-with-mactray", query: "system-service=ready&legacy-tray=trusted-current", expected: "Existing MacTray is running" },
] as const;

for (const state of executionStateGallery) {
  test(`execution state gallery captures ${state.id}`, async ({ page }, testInfo) => {
    await page.goto(`/?view=execution&gallery=1&lang=en&${state.query}`, { waitUntil: "networkidle" });

    const summary = page.locator("[data-service-summary]");
    await expect(summary).toContainText(state.expected);
    await expect(page.locator("details.service-row[open]")).toHaveCount(0);
    expect(await overflowingElements(page)).toEqual([]);
    await page.screenshot({
      path: path.join(galleryRoot, `${testInfo.project.name}-execution-state-${state.id}-en.png`),
      fullPage: true,
    });
  });
}

test("gallery publication state follows designation and preserves pending changes across start and stop", () => {
  const pending = galleryExecutionStatus(new URLSearchParams("run-profile-pending=1"));
  expect(pending.runProfilePublication).toBe("pending");
  const stopped = transitionGalleryExecutionStatus(pending, "stop");
  expect(stopped.runProfilePublication).toBe("pending");
  expect(transitionGalleryExecutionStatus(stopped, "start").runProfilePublication).toBe("pending");
  expect(transitionGalleryRunProfile(stopped, "Profiles\\Gallery.ini", false).runProfilePublication).toBe("published");
  const unapplied = galleryExecutionStatus(new URLSearchParams("profile-unapplied=1&run-profile-pending=1"));
  expect(unapplied.runProfilePublication).toBe("unknown");
  expect(transitionGalleryExecutionStatus(unapplied, "start").runProfilePublication).toBe("published");
});

for (const publication of [
  { state: "published", query: "" },
  { state: "unknown", query: "&profile-unapplied=1" },
  { state: "unknown", query: "&profile-runtime-missing=1&run-profile-pending=1" },
] as const) {
  test(`run profile notice stays absent when ${publication.state} (${publication.query || "default"})`, async ({ page }) => {
    expect(galleryExecutionStatus(new URLSearchParams(publication.query)).runProfilePublication).toBe(publication.state);
    await page.goto(`/?view=execution&gallery=1&lang=en${publication.query}`, { waitUntil: "networkidle" });
    await expect(page.locator("[data-service-summary]")).toBeVisible();
    await expect(page.locator("[data-run-profile-pending]")).toHaveCount(0);
    await page.getByRole("button", { name: "Refresh status" }).click();
    await expect(page.locator("[data-run-profile-pending]")).toHaveCount(0);
  });
}

for (const service of [
  { runtime: "running", title: "The settings in use are the previous ones", message: "Restarted the service on the settings you saved. Apps without MacType, open now or later, get them; reopen apps that already had MacType to switch them over." },
  { runtime: "stopped", title: "Starting now would run the previous settings", message: "Saved these settings into the run profile. They take effect when you start the service." },
] as const) {
  test(`pending run profile applies calmly while the service is ${service.runtime}`, async ({ page }, testInfo) => {
    await page.goto(`/?view=execution&gallery=1&lang=en&run-profile-pending=1&service-runtime=${service.runtime}&service-delay=1000`, { waitUntil: "networkidle" });
    const notice = page.locator("[data-run-profile-pending]");
    await expect(notice).toHaveAttribute("role", "status");
    await expect(notice.locator("strong")).toHaveText(service.title);
    await expect(notice).toBeInViewport({ ratio: 1 });
    await expect(page.locator(".page-header + [data-run-profile-pending] + [data-service-summary]")).toHaveCount(1);
    // The service page marks exceptional blocks and warning icons explicitly.
    expect(await notice.evaluate((element) => element.matches('[data-prominent-exception], [data-state="attention"], [data-state="critical"], .warning, .warning-text'))).toBe(false);
    await expect(notice.locator('[data-prominent-exception], .warning, .warning-text, .lucide-triangle-alert')).toHaveCount(0);
    await expect(notice.locator(".lucide-file-clock")).toHaveCount(1);
    expect(await overflowingElements(page)).toEqual([]);
    await page.screenshot({
      path: path.join(galleryRoot, `${testInfo.project.name}-execution-state-run-profile-pending-${service.runtime}-en.png`),
      fullPage: true,
    });
    await notice.getByRole("button", { name: "Apply to service", exact: true }).click();
    await expect(notice.getByRole("button")).toBeDisabled();
    await expect(notice.getByRole("button")).toHaveText("Applying");
    await expect(notice).toHaveCount(0);
    await expect(page.locator(".success-message")).toHaveText(service.message);
    await expect(page.locator("[data-service-summary]")).toContainText(service.runtime === "running" ? "Running" : "Stopped");
    await page.getByRole("button", { name: "Refresh status" }).click();
    await expect(notice).toHaveCount(0);
  });
}

test("pending run profile keeps the notice and reports service operation failures", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&run-profile-pending=1&service-fail=republish-profile", { waitUntil: "networkidle" });
  const notice = page.locator("[data-run-profile-pending]");
  await notice.getByRole("button", { name: "Apply to service", exact: true }).click();
  await expect(page.locator(".inline-error")).toBeVisible();
  await expect(page.locator(".inline-error")).not.toContainText("control-center-internal-operation-failed");
  await expect(page.locator(".success-message")).toHaveCount(0);
  await expect(notice.getByRole("button", { name: "Apply to service", exact: true })).toBeEnabled();
});

for (const locale of galleryLocales) {
  test(`pending run profile notice fits in ${locale.id}`, async ({ page }, testInfo) => {
    await page.goto(`/?view=execution&gallery=1&lang=${locale.id}&run-profile-pending=1`, { waitUntil: "networkidle" });
    const notice = page.locator("[data-run-profile-pending]");
    await expect(notice).toBeInViewport({ ratio: 1 });
    await expect(notice.getByRole("button")).toBeInViewport({ ratio: 1 });
    expect(await overflowingElements(page)).toEqual([]);
    expect(await notice.evaluate((element) => element.scrollWidth > element.clientWidth)).toBe(false);
    await page.screenshot({
      path: path.join(galleryRoot, `${testInfo.project.name}-execution-state-run-profile-pending-${locale.id}.png`),
      fullPage: true,
    });
  });
}

for (const theme of ["light", "dark"] as const) {
  test(`pending run profile text meets contrast requirements in ${theme}`, async ({ page }, testInfo) => {
    await page.goto(`/?view=execution&gallery=1&lang=en&run-profile-pending=1&theme=${theme}`, { waitUntil: "networkidle" });
    const notice = page.locator("[data-run-profile-pending]");
    await expect(notice).toBeVisible();
    const contrast = await notice.evaluate((element) => {
      const canvas = document.createElement("canvas");
      canvas.width = canvas.height = 1;
      const context = canvas.getContext("2d")!;
      const rgb = (color: string) => {
        context.clearRect(0, 0, 1, 1);
        context.fillStyle = color;
        context.fillRect(0, 0, 1, 1);
        return Array.from(context.getImageData(0, 0, 1, 1).data).slice(0, 3);
      };
      const luminance = (color: number[]) => color.map((channel) => {
        const value = channel / 255;
        return value <= 0.04045 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4;
      }).reduce((sum, value, index) => sum + value * [0.2126, 0.7152, 0.0722][index], 0);
      const background = rgb(getComputedStyle(element).backgroundColor);
      const title = rgb(getComputedStyle(element.querySelector("strong")!).color);
      const paragraph = rgb(getComputedStyle(element.querySelector("p")!).color);
      const ratio = (color: number[]) => (Math.max(luminance(color), luminance(background)) + 0.05)
        / (Math.min(luminance(color), luminance(background)) + 0.05);
      return { background, title, paragraph, titleRatio: ratio(title), paragraphRatio: ratio(paragraph) };
    });
    expect(contrast.titleRatio).toBeGreaterThanOrEqual(4.5);
    expect(contrast.paragraphRatio).toBeGreaterThanOrEqual(4.5);
    console.log(`Run profile notice contrast (${testInfo.project.name}, ${theme}): ${JSON.stringify(contrast)}`);
    await page.screenshot({
      path: path.join(galleryRoot, `${testInfo.project.name}-execution-state-run-profile-pending-${theme}-en.png`),
      fullPage: true,
    });
  });
}

for (const wording of [
  { locale: "ko", expected: "Control Center 서비스", forbidden: "신식 서비스" },
  { locale: "zh-CN", expected: "Control Center 服务", forbidden: "新式服务" },
  { locale: "zh-TW", expected: "Control Center 服務", forbidden: "新式服務" },
] as const) {
  test(`service terminology is natural in ${wording.locale}`, async ({ page }) => {
    await page.goto(`/?view=execution&gallery=1&lang=${wording.locale}&system-service=ready`, { waitUntil: "networkidle" });
    const main = page.locator("main");
    await expect(main).toContainText(wording.expected);
    await expect(main).not.toContainText(wording.forbidden);
  });
}

test("copy review gallery captures the Korean ready service", async ({ page }, testInfo) => {
  await page.goto("/?view=execution&gallery=1&lang=ko&system-service=ready", { waitUntil: "networkidle" });
  const system = page.locator('details.service-row[data-kind="system"]');
  await system.locator("summary").click();
  await expect(system).toContainText("열려 있는 앱과 새로 여는 앱에 프로파일을 적용하고 있습니다.");
  await page.screenshot({
    path: path.join(galleryRoot, `${testInfo.project.name}-copy-review-ready-ko.png`),
    fullPage: true,
  });
});

test("copy review gallery captures the Korean degraded service", async ({ page }, testInfo) => {
  await page.goto("/?view=execution&gallery=1&lang=ko&system-service=degraded", { waitUntil: "networkidle" });
  const system = page.locator('details.service-row[data-kind="system"]');
  await system.locator("summary").click();
  await expect(system).toContainText("서비스는 실행 중");
  await page.screenshot({
    path: path.join(galleryRoot, `${testInfo.project.name}-copy-review-degraded-ko.png`),
    fullPage: true,
  });
});

test("copy review gallery captures the Korean migration explanation", async ({ page }, testInfo) => {
  await page.goto("/?view=execution&gallery=1&lang=ko&system-service=migration-available&legacy=migration-available", { waitUntil: "networkidle" });
  await page.locator("[data-service-summary]").getByRole("button", { name: "마이그레이션" }).click();
  const dialog = page.getByRole("dialog", { name: "레거시 MacTray를 마이그레이션할까요?" });
  await expect(dialog).toBeVisible();
  await expect(dialog).toContainText("Control Center");
  await page.screenshot({
    path: path.join(galleryRoot, `${testInfo.project.name}-copy-review-migration-ko.png`),
    fullPage: true,
  });
});

for (const view of galleryViews) {
  for (const locale of galleryLocales) {
    test(`${view.id} renders fully in ${locale.id}`, async ({ page }, testInfo) => {
      const failures: string[] = [];
      page.on("console", (message) => {
        if (message.type() === "error") failures.push(`console: ${message.text()}`);
      });
      page.on("pageerror", (error) => failures.push(`pageerror: ${error.message}`));
      page.on("crash", () => failures.push("renderer process crashed"));

      await page.goto(`/?view=${view.id}&gallery=1&lang=${locale.id}`, { waitUntil: "networkidle" });
      await expect(page.locator("html")).toHaveAttribute("lang", locale.id);
      await expect(page.locator("html")).toHaveAttribute("dir", locale.direction);
      await expect(page.locator("body")).toHaveAttribute("data-rendered", "true");
      await expect(page.locator("body")).toHaveAttribute("data-view", view.id);
      await expect(page.locator("body")).toHaveAttribute("data-locale", locale.id);
      await expect(page.getByRole("heading", { level: 1, name: view.title[locale.id] })).toBeVisible();
      expect(await page.locator("main").innerText()).toMatch(locale.script);
      expect(await overflowingElements(page), `${locale.id} view must not overflow horizontally`).toEqual([]);
      expect(failures, failures.join("\n")).toEqual([]);

      await page.screenshot({
        path: path.join(galleryRoot, `${testInfo.project.name}-${view.id}-${locale.id}.png`),
        fullPage: true,
      });
    });
  }
}

test("profile editor categories and collections remain interactive", async ({ page }, testInfo) => {
  const failures: string[] = [];
  page.on("console", (message) => {
    if (message.type() === "error") failures.push(`console: ${message.text()}`);
  });
  page.on("pageerror", (error) => failures.push(`pageerror: ${error.message}`));

  await page.goto("/?view=profiles&gallery=1&lang=ko&profile-unapplied=1", { waitUntil: "networkidle" });
  const undo = page.getByRole("button", { name: "되돌리기", exact: true });
  const redo = page.getByRole("button", { name: "다시 하기", exact: true });
  const discard = page.getByRole("button", { name: "변경 취소", exact: true });
  await expect(undo).toBeDisabled();
  const firstSelect = page.locator(".setting-row select").first();
  const initialOption = await firstSelect.inputValue();
  const nextOption = await firstSelect.locator("option").evaluateAll((options, current) => options.map((option) => (option as HTMLOptionElement).value).find((value) => value !== current), initialOption);
  if (!nextOption) throw new Error("The first profile setting must expose an alternate option");
  await firstSelect.selectOption(nextOption);
  await expect(undo).toBeEnabled();
  await undo.click();
  await expect(redo).toBeEnabled();
  await redo.click();
  await expect(page.locator(".profile-history-actions .designate")).toHaveCount(0);
  await page.getByRole("button", { name: "저장", exact: true }).click();
  await expect(page.locator(".profile-message")).toContainText("저장했습니다");
  await expect(page.locator(".profile-history-actions .designate")).toHaveCount(0);
  await page.getByRole("button", { name: "다른 이름으로 저장", exact: true }).click();
  const dialog = page.getByRole("dialog", { name: "다른 이름으로 저장", exact: true });
  await dialog.getByRole("textbox", { name: "새 프로필 이름" }).fill("Gallery edited copy");
  await dialog.getByRole("button", { name: "저장", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  await expect(page.getByRole("button", { name: "실행 프로필로 지정", exact: true })).toBeEnabled();
  await page.getByRole("button", { name: "실행 프로필로 지정", exact: true }).click();
  await expect(page.locator(".profile-message")).toContainText("실행 프로필로 지정했습니다");
  await firstSelect.selectOption(initialOption);
  await expect(discard).toBeEnabled();
  await discard.click();
  await expect(discard).toBeDisabled();
  await firstSelect.selectOption(initialOption);
  await page.getByRole("button", { name: "저장", exact: true }).click();
  await expect(page.locator(".profile-message")).toContainText("저장했습니다");
  await expect(discard).toBeDisabled();

  // The default stack renders the sample once, because a second sample group
  // would claim the height the settings form needs. Wide layouts dock the
  // preview beside the form; narrower ones keep a bottom panel that no longer
  // grows into the form.
  await expect(page.locator(".preview-strip img")).toHaveCount(1);
  const previewResizer = page.getByRole("separator", { name: "프리뷰 영역 높이 조절" });
  const docked = await page.locator(".settings-workspace").getAttribute("data-preview-docked") === "true";
  if (docked) {
    await expect(previewResizer).toHaveCount(0);
    const formBox = await page.locator(".settings-form").boundingBox();
    const panelBox = await page.locator(".preview-panel").boundingBox();
    if (!formBox || !panelBox) throw new Error("The docked layout must show both columns");
    expect(panelBox.x, "the docked preview sits beside the form, not under it").toBeGreaterThanOrEqual(formBox.x + formBox.width - 1);
  } else {
    const settledHeight = Number(await previewResizer.getAttribute("aria-valuenow"));
    expect(settledHeight, "footer control must stay visible").toBeGreaterThanOrEqual(220);
    expect(settledHeight, "the panel must not grow past its default").toBeLessThanOrEqual(300);
    await previewResizer.press("ArrowDown");
    await expect(previewResizer).toHaveAttribute("aria-valuenow", String(settledHeight - 16));
    await previewResizer.press("Home");
    await expect(previewResizer).toHaveAttribute("aria-valuenow", "128");
  }

  await page.getByRole("button", { name: "LCD·픽셀 배열" }).click();
  await expect(page.getByRole("heading", { name: "LCD·픽셀 배열" })).toBeVisible();
  await expect(page.getByText("빨강 채널 튜닝", { exact: true })).toBeVisible();

  await page.getByRole("button", { name: "고급·실험" }).click();
  await expect(page.getByText("DirectWrite 감마", { exact: true })).toBeVisible();
  const shadow = page.getByRole("group", { name: "글자 그림자" });
  await shadow.getByRole("checkbox").check();
  await shadow.getByRole("spinbutton", { name: "가로 위치" }).fill("-2");
  await shadow.getByRole("spinbutton", { name: "세로 위치" }).fill("3");
  const lcdWeights = page.getByRole("group", { name: "사용자 지정 LCD 필터 가중치" });
  await lcdWeights.getByRole("checkbox").check();
  await expect(lcdWeights.getByRole("spinbutton")).toHaveCount(5);
  const pixelLayout = page.getByRole("group", { name: "사용자 지정 픽셀 배열" });
  await pixelLayout.getByRole("checkbox").check();
  await expect(pixelLayout.getByRole("spinbutton")).toHaveCount(6);
  const substitutionsBefore = await page.getByRole("combobox", { name: "원본 글꼴" }).count();
  await page.getByRole("button", { name: "글꼴 대체 추가" }).click();
  await expect(page.getByRole("combobox", { name: "원본 글꼴" })).toHaveCount(substitutionsBefore + 1);

  await page.getByRole("button", { name: "글꼴별 설정" }).click();
  await page.getByRole("combobox", { name: "설치된 글꼴 선택" }).selectOption("Arial");
  await expect(page.locator(".individual-row > strong").filter({ hasText: "Arial" })).toBeVisible();

  await page.getByRole("button", { name: "포함·제외" }).click();
  await page.getByRole("combobox", { name: "제외 글꼴 · 목록에 글꼴 추가" }).selectOption("Calibri");
  await expect(page.locator(".list-editor li > code").filter({ hasText: "Calibri" })).toBeVisible();
  await expect(page.getByText("제외 프로그램", { exact: true })).toBeVisible();
  await expect(page.getByText("DLL로 앱 제외", { exact: true })).toBeVisible();
  await expect(page.getByText("글꼴 대체 제외 모듈", { exact: true })).toBeVisible();
  await expect(page.getByRole("img", { name: "현재 설정의 글자 렌더링 프리뷰" })).toHaveAttribute("data-dark", "false");
  await page.getByRole("button", { name: "색 반전" }).click();
  await expect(page.getByRole("img", { name: "현재 설정의 글자 렌더링 프리뷰" })).toHaveAttribute("data-dark", "true");
  const horizontalOverflow = await page.evaluate(() => document.documentElement.scrollWidth > document.documentElement.clientWidth);
  expect(horizontalOverflow, "interactive profile editor must not have horizontal scrolling").toBe(false);
  expect(failures, failures.join("\n")).toEqual([]);
});

test("structured list editors add typed entries, reject duplicates, and suggest running processes", async ({ page }, testInfo) => {
  await page.goto("/?view=profiles&gallery=1&lang=ko", { waitUntil: "networkidle" });
  await page.getByRole("button", { name: "포함·제외" }).click();

  const excludePrograms = page.locator(".list-editor").filter({ hasText: "제외 프로그램" });
  const entryInput = excludePrograms.getByRole("combobox", { name: "제외 프로그램 · 추가" });
  await expect(excludePrograms.locator("li > code").filter({ hasText: "fontview.exe" })).toBeVisible();

  await entryInput.fill("notepad.exe");
  await entryInput.press("Enter");
  await expect(excludePrograms.locator("li > code").filter({ hasText: "notepad.exe" })).toBeVisible();
  await expect(entryInput).toHaveValue("");

  await entryInput.fill("NOTEPAD.EXE");
  await excludePrograms.getByRole("button", { name: "제외 프로그램 · 추가" }).click();
  await expect(excludePrograms.getByText("NOTEPAD.EXE은(는) 이미 목록에 있습니다.", { exact: true })).toBeVisible();
  await expect(excludePrograms.locator("li")).toHaveCount(2);

  await excludePrograms.getByRole("button", { name: "notepad.exe 제거" }).click();
  await expect(excludePrograms.locator("li")).toHaveCount(1);
  await expect(excludePrograms.getByText("이미 목록에 있습니다", { exact: false })).toHaveCount(0);

  await expect(entryInput).toHaveAttribute("list", "list-process-suggestions");
  await expect(page.locator("#list-process-suggestions option[value='code.exe']")).toHaveCount(1);

  const unloadDlls = page.locator(".list-editor").filter({ hasText: "DLL로 앱 제외" });
  await expect(unloadDlls.getByText("아직 항목이 없습니다.", { exact: true })).toBeVisible();

  expect(await overflowingElements(page)).toEqual([]);
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-profile-list-editors-ko.png`), fullPage: true });
});

test("profile preview docks as a full-height right column at wide widths", async ({ page }, testInfo) => {
  test.skip(testInfo.project.name !== "desktop-1280", "Docked preview behavior is width-specific");
  await page.setViewportSize({ width: 1680, height: 900 });
  await page.goto("/?view=profiles&gallery=1&lang=ko", { waitUntil: "networkidle" });

  await expect(page.locator('.settings-workspace[data-preview-docked="true"]')).toHaveCount(1);
  await expect(page.locator(".preview-resizer")).toHaveCount(0);
  const formBox = await page.locator(".settings-form").boundingBox();
  const previewBox = await page.locator(".preview-panel").boundingBox();
  expect(formBox).not.toBeNull();
  expect(previewBox).not.toBeNull();
  expect(previewBox!.x, "docked preview must sit right of the settings form").toBeGreaterThanOrEqual(formBox!.x + formBox!.width);
  expect(await overflowingElements(page)).toEqual([]);
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-preview-docked-ko.png`), fullPage: true });
});

test("the shipped default window docks the preview and never resamples the sample", async ({ page }, testInfo) => {
  test.skip(testInfo.project.name !== "desktop-1280", "The default window is a single size");
  await page.setViewportSize(defaultWindow);
  await page.goto("/?view=profiles&gallery=1&lang=ko", { waitUntil: "networkidle" });

  await expect(page.locator('.settings-workspace[data-preview-docked="true"]')).toHaveCount(1);

  // A strip is a bitmap drawn at the width the panel asked for. Asking for more
  // than the canvas holds makes the browser scale it down, which shrinks the
  // glyphs below the size the reader picked and defeats the preview.
  const strip = page.locator(".preview-strip img").first();
  for (const size of ["12", "14", "18"]) {
    await page.getByRole("combobox", { name: "프리뷰 크기" }).selectOption(size);
    await expect(strip).toHaveJSProperty("complete", true);
    const scale = await strip.evaluate((image) => {
      const rendered = image.getBoundingClientRect().width;
      return rendered / Number((image as HTMLImageElement).getAttribute("width"));
    });
    expect(scale, `the ${size} pt sample must render at its own size in the docked column`).toBeCloseTo(1, 2);
  }

  expect(await overflowingElements(page)).toEqual([]);
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-preview-default-window-ko.png`), fullPage: true });
});

test("native preview display mode dropdown drives the runtime adapter", async ({ page }, testInfo) => {
  test.skip(testInfo.project.name !== "desktop-1280", "The preview footer is hidden on compact layouts");
  await page.goto("/?view=profiles&gallery=1&lang=ko", { waitUntil: "networkidle" });

  const modeSelect = page.getByRole("combobox", { name: "표시 방식" });
  await expect(modeSelect).toBeVisible();
  await expect(modeSelect.locator("option")).toHaveText(["견본", "크기 사다리", "윈도우", "나열 표시"]);

  const nativePreviewState = () => page.evaluate(() => window.sessionStorage.getItem("gallery-native-preview"));
  const nativePreviewBackground = () => page.evaluate(() => window.sessionStorage.getItem("gallery-native-preview-background"));
  await page.getByRole("button", { name: "실시간 미리보기 창 열기" }).click();
  await expect.poll(nativePreviewState).toBe("sample");
  await expect.poll(nativePreviewBackground).toBe("#EEF1F4");
  /* The window draws its own chrome from the strings the Control Center hands over. */
  type NativeOptions = { labels?: Record<string, string>; sizes?: number[]; fontFace?: string; theme?: string; inverted?: boolean; chrome?: { skin?: string } };
  const nativeOptions = () => page.evaluate(() => JSON.parse(window.sessionStorage.getItem("gallery-native-preview-options") ?? "{}") as NativeOptions);
  const options = await nativeOptions();
  expect(options.labels?.title).toBe("MacType 실제 미리보기");
  expect(options.labels?.invert).toBe("색 반전");
  expect(options.sizes?.length).toBeGreaterThan(5);
  expect(options.theme).toBe("light");
  expect(options.inverted).toBe(false);
  expect(options.chrome?.skin).toBe("classic");
  await modeSelect.selectOption("ladder");
  await expect.poll(nativePreviewState).toBe("ladder");
  // The invert choice reaches the open window as a flag over the theme's base
  // colours (the window swaps them itself), and it survives a display-mode change.
  await page.getByRole("button", { name: "색 반전" }).click();
  await expect.poll(async () => (await nativeOptions()).inverted).toBe(true);
  await expect.poll(nativePreviewBackground).toBe("#EEF1F4");
  await modeSelect.selectOption("listing");
  await expect.poll(nativePreviewState).toBe("listing");
  await expect.poll(async () => (await nativeOptions()).inverted).toBe(true);
  await page.getByRole("button", { name: "색 반전" }).click();
  await expect.poll(async () => (await nativeOptions()).inverted).toBe(false);
  await expect.poll(nativePreviewBackground).toBe("#EEF1F4");
  // The window hiding itself (Escape, close button) flips the toggle without a click.
  await expect(page.getByRole("button", { name: "실시간 미리보기 창 닫기" })).toBeVisible();
  await page.evaluate(() => window.dispatchEvent(new CustomEvent("gallery-native-preview-state", { detail: { visible: false, displayMode: "listing", background: "#EEF1F4" } })));
  await expect(page.getByRole("button", { name: "실시간 미리보기 창 열기" })).toBeVisible();
  await expect.poll(nativePreviewState).toBe("hidden");
  await page.getByRole("button", { name: "실시간 미리보기 창 열기" }).click();
  await expect.poll(nativePreviewState).toBe("listing");
  await page.getByRole("button", { name: "실시간 미리보기 창 닫기" }).click();
  await expect.poll(nativePreviewState).toBe("hidden");
  expect(await overflowingElements(page)).toEqual([]);
});

test("preview comparison renders the saved and edited sides only while edits exist", async ({ page }, testInfo) => {
  test.skip(testInfo.project.name !== "desktop-1280", "The preview toolbar wraps on compact layouts");
  await page.goto("/?view=profiles&gallery=1&lang=ko", { waitUntil: "networkidle" });

  const compare = page.getByRole("button", { name: "비교", exact: true });
  const strips = page.locator(".preview-strip");
  await expect(compare).toBeDisabled();
  const baseline = await strips.count();
  expect(baseline).toBeGreaterThan(0);

  const firstSelect = page.locator(".setting-row select").first();
  const initialOption = await firstSelect.inputValue();
  const nextOption = await firstSelect.locator("option").evaluateAll((options, current) => options.map((option) => (option as HTMLOptionElement).value).find((value) => value !== current), initialOption);
  if (!nextOption) throw new Error("The first profile setting must expose an alternate option");
  await firstSelect.selectOption(nextOption);
  await expect(compare).toBeEnabled();

  await compare.click();
  await expect(compare).toHaveAttribute("aria-pressed", "true");
  await expect(strips).toHaveCount(baseline * 2);
  await expect(page.locator(".preview-strip figcaption").first()).toContainText("저장본");
  await expect(page.locator(".preview-strip figcaption").nth(1)).toContainText("편집본");
  expect(await overflowingElements(page)).toEqual([]);
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-preview-compare-ko.png`), fullPage: true });

  // Saving makes both sides identical, so the comparison switches itself off.
  await page.getByRole("button", { name: "저장", exact: true }).click();
  await expect(compare).toBeDisabled();
  await expect(compare).toHaveAttribute("aria-pressed", "false");
  await expect(strips).toHaveCount(baseline);
});

test("RGB comparison replaces the completed preview stack atomically", async ({ page }, testInfo) => {
  test.skip(testInfo.project.name !== "desktop-1280", "The four-channel preview is docked at desktop width");
  await page.goto("/?view=overview&gallery=1&lang=ko&preview-delay=25", { waitUntil: "networkidle" });
  await page.locator(".navigation").getByRole("group", { name: "튜너" }).getByRole("button", { name: "단계별 설정" }).click();
  await page.locator(".settings-index").getByRole("button", { name: "LCD 배열과 색조" }).click();

  const strips = page.locator(".preview-strip");
  await expect(strips).toHaveCount(4);
  const firstSlider = page.locator('.setting-row input[type="range"]').first();
  await firstSlider.focus();
  await firstSlider.press("ArrowRight");
  const compare = page.getByRole("button", { name: "비교", exact: true });
  await expect(compare).toBeEnabled();

  await page.locator(".preview-canvas").evaluate((canvas) => {
    const counts = [canvas.querySelectorAll(".preview-strip").length];
    window.sessionStorage.setItem("gallery-preview-strip-counts", counts.join(","));
    const observer = new MutationObserver(() => {
      counts.push(canvas.querySelectorAll(".preview-strip").length);
      window.sessionStorage.setItem("gallery-preview-strip-counts", counts.join(","));
    });
    observer.observe(canvas, { childList: true, subtree: true });
  });

  await compare.click();
  await expect(strips).toHaveCount(8);
  const observedCounts = await page.evaluate(() => (window.sessionStorage.getItem("gallery-preview-strip-counts") ?? "")
    .split(",")
    .filter(Boolean)
    .map(Number));
  expect(observedCounts, "comparison must keep the old four-line stack until all eight replacements are ready")
    .toEqual(expect.arrayContaining([4, 8]));
  expect(observedCounts.filter((count) => count !== 4 && count !== 8)).toEqual([]);
});

test("settings navigation restores the legacy Wizard and Tuner hierarchy", async ({ page }, testInfo) => {
  await page.goto("/?view=overview&gallery=1&lang=ko", { waitUntil: "networkidle" });

  const wizardGroup = page.locator(".navigation").getByRole("group", { name: "위자드" });
  const tunerGroup = page.locator(".navigation").getByRole("group", { name: "튜너" });
  await expect(wizardGroup.getByRole("button", { name: "프로필" })).toBeVisible();
  await expect(wizardGroup.getByRole("button", { name: "서비스" })).toBeVisible();
  await expect(tunerGroup.getByRole("button", { name: "단계별 설정" })).toBeVisible();
  await expect(tunerGroup.getByRole("button", { name: "전체 설정" })).toBeVisible();
  await expect(page.locator(".navigation").getByRole("button", { name: "위자드", exact: true })).toHaveCount(0);
  await expect(page.locator(".navigation").getByRole("button", { name: "튜너", exact: true })).toHaveCount(0);

  await tunerGroup.getByRole("button", { name: "단계별 설정" }).click();
  await expect(page.locator(".profile-page")).toHaveAttribute("data-mode", "guided");
  await expect(page.getByRole("heading", { level: 1, name: "단계별 설정" })).toBeVisible();
  await expect(page.locator(".profile-mode-title > span")).toHaveText("Tuner");
  expect(await page.locator(".profile-page").innerText()).not.toContain("마법사");
  await expect(page.locator(".settings-index button")).toHaveCount(9);
  await expect(page.locator(".settings-step")).toHaveCount(9);
  await expect(page.locator(".settings-index").getByRole("button", { name: "고급·실험" })).toHaveCount(0);
  await expect(page.getByRole("toolbar", { name: "프로필 편집 작업" })).toHaveCount(0);
  const settingsForm = page.locator(".settings-form");

  await expect(page.getByRole("heading", { level: 2, name: "시작" })).toBeVisible();
  await expect(page.locator(".guided-start-card")).toBeVisible();
  await expect(page.locator(".guided-start-profile code")).toBeVisible();
  await expect(page.locator(".guided-start-font select")).toBeVisible();
  await expect(page.getByRole("button", { name: "이전" })).toHaveCount(0);
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-guided-start-ko.png`), fullPage: true });

  await page.getByRole("button", { name: "진행" }).click();
  await expect(page.getByRole("heading", { level: 2, name: "기본 렌더링" })).toBeVisible();
  await expect(page.getByRole("button", { name: "이전" })).toBeVisible();
  expect(await page.locator(".guided-choice").getByRole("radio").count()).toBeGreaterThanOrEqual(3);
  await expect(page.locator(".setting-actions")).toHaveCount(0);
  await expect(page.getByRole("button", { name: "단계 기본값 복원" })).toBeVisible();
  expect(await settingsForm.evaluate((element) => element.scrollWidth > element.clientWidth), "Guided settings must not have internal horizontal scrolling").toBe(false);
  // The generic overflow gate skips anything inside an overflow-hidden
  // ancestor, so the workspace column needs its own window-bounds check: a
  // wide control in the preview toolbar used to inflate the column past the
  // right edge, clipping the step body instead of scrolling.
  for (const selector of [".settings-form", ".preview-panel", ".guided-step-tools"]) {
    const bounds = await page.locator(selector).first().evaluate((element) => {
      const rect = element.getBoundingClientRect();
      return { left: Math.round(rect.left), right: Math.round(rect.right), viewport: document.documentElement.clientWidth };
    });
    expect(bounds.right, `${selector} must stay inside the window`).toBeLessThanOrEqual(bounds.viewport + 1);
    expect(bounds.left, `${selector} must not start off the left edge`).toBeGreaterThanOrEqual(-1);
  }
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-guided-rendering-ko.png`), fullPage: true });

  await page.getByRole("button", { name: "진행" }).click();
  await expect(page.getByRole("heading", { level: 2, name: "글꼴 품질" })).toBeVisible();
  await expect(page.locator(".guided-scale-words").first()).toContainText("가늘게");

  // Restored legacy Tuner screen: bold and italic together, previewed as
  // bold, italic, and bold italic lines of the same pangram.
  await page.getByRole("button", { name: "진행" }).click();
  await expect(page.getByRole("heading", { level: 2, name: "굵게·기울임" })).toBeVisible();
  const guidedLabels = page.locator(".guided-label label");
  await expect(guidedLabels.nth(0)).toHaveText("굵은 글자 굵기");
  await expect(guidedLabels.nth(1)).toHaveText("굵게 처리 방식");
  await expect(guidedLabels.nth(2)).toHaveText("기울임 정도");
  await expect(page.locator(".preview-strip")).toHaveCount(3);
  await expect(page.locator(".preview-strip figcaption")).toHaveText(["굵게", "기울임", "굵은 기울임"]);
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-guided-bold-italic-ko.png`), fullPage: true });

  // Restored legacy Tuner screen: contrast then gamma sliders before the mode.
  // The step is titled by what it does, not by the core setting it carries.
  await page.locator(".settings-index").getByRole("button", { name: "밝기와 대비" }).click();
  await expect(page.getByRole("heading", { level: 2, name: "밝기와 대비" })).toBeVisible();
  await expect(guidedLabels.nth(0)).toHaveText("대비");
  await expect(guidedLabels.nth(1)).toHaveText("감마 값");
  await expect(guidedLabels.nth(2)).toHaveText("감마 방식");

  // LCD screen gains the RGB text tuning and compares the current method
  // against the red, green, and blue channels without clipping line four.
  await page.locator(".settings-index").getByRole("button", { name: "LCD 배열과 색조" }).click();
  await expect(page.getByRole("heading", { level: 2, name: "LCD 배열과 색조" })).toBeVisible();
  await expect(guidedLabels.nth(2)).toHaveText("빨강 채널 튜닝");
  await expect(page.locator(".preview-strip")).toHaveCount(4);
  await expect(page.locator(".preview-strip figcaption")).toHaveText(["현재 방식", "R", "G", "B"]);
  // Four lines scroll inside the stack instead of stretching the panel into
  // the step body, so line four is reachable and the step keeps its room. A
  // desktop window docks the guided preview beside the step rather than under it.
  await page.locator(".preview-strip").last().scrollIntoViewIfNeeded();
  await expect(page.locator(".preview-strip").last()).toBeInViewport();
  const stepBox = await page.locator(".guided-step-content").boundingBox();
  if (!stepBox) throw new Error("The guided step body must stay visible");
  expect(stepBox.height, "the step body keeps its room beside the four-line stack").toBeGreaterThanOrEqual(240);
  if (testInfo.project.name === "desktop-1280") {
    await expect(page.locator(".settings-workspace")).toHaveAttribute("data-preview-docked", "true");
    await expect(page.getByRole("separator", { name: "프리뷰 영역 높이 조절" })).toHaveCount(0);
  }
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-guided-lcd-channels-ko.png`), fullPage: true });

  await page.locator(".settings-index").getByRole("button", { name: "힌팅" }).click();
  await expect(page.getByRole("heading", { level: 2, name: "힌팅" })).toBeVisible();
  await page.locator(".settings-index").getByRole("button", { name: "실행 프로필 지정", exact: true }).click();
  await expect(page.getByRole("button", { name: "진행" })).toHaveCount(0);
  await expect(page.locator(".guided-apply-card .designate")).toHaveCount(0);
  await expect(page.getByRole("button", { name: "다른 이름으로 저장", exact: true })).toBeEnabled();
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-guided-apply-ko.png`), fullPage: true });

  await tunerGroup.getByRole("button", { name: "전체 설정" }).click();
  await expect(page.locator(".profile-page")).toHaveAttribute("data-mode", "all");
  await expect(page.getByRole("heading", { level: 1, name: "전체 설정" })).toBeVisible();
  await expect(page.locator(".settings-index button")).toHaveCount(6);
  expect(await settingsForm.evaluate((element) => element.scrollWidth > element.clientWidth), "Tuner settings must not have internal horizontal scrolling").toBe(false);
  await expect(page.getByRole("checkbox", { name: "고급 설정 표시" })).toHaveCount(0);

  await wizardGroup.getByRole("button", { name: "프로필" }).click();
  await expect(page.locator("body")).toHaveAttribute("data-view", "files");
  await expect(page.getByRole("heading", { level: 1, name: "프로필" })).toBeVisible();
  await wizardGroup.getByRole("button", { name: "서비스" }).click();
  await expect(page.locator("body")).toHaveAttribute("data-view", "execution");
});

test("guided step undo, redo, and discard stay scoped to the current step", async ({ page }) => {
  await page.goto("/?view=profiles&gallery=1&lang=ko", { waitUntil: "networkidle" });
  await page.locator(".navigation").getByRole("button", { name: "단계별 설정" }).click();
  await expect(page.locator(".profile-page")).toHaveAttribute("data-mode", "guided");

  const undoStep = page.getByRole("button", { name: "되돌리기", exact: true });
  const redoStep = page.getByRole("button", { name: "다시 하기", exact: true });
  const discardStep = page.getByRole("button", { name: "단계 변경 취소", exact: true });

  // Steps without schema settings (start, substitution, apply) expose no step tools.
  await expect(page.getByRole("toolbar", { name: "단계 편집 작업" })).toHaveCount(0);
  await page.locator(".settings-index").getByRole("button", { name: "글꼴 대체" }).click();
  await expect(page.getByRole("toolbar", { name: "단계 편집 작업" })).toHaveCount(0);

  await page.locator(".settings-index").getByRole("button", { name: "글꼴 품질" }).click();
  const weightValue = page.locator("#normal_weight-value");
  await expect(undoStep).toBeDisabled();
  await expect(redoStep).toBeDisabled();
  await expect(discardStep).toBeDisabled();

  await weightValue.fill("24");
  await weightValue.press("Enter");
  await expect(weightValue).toHaveValue("24");
  await expect(discardStep).toBeEnabled();

  await undoStep.click();
  await expect(weightValue).toHaveValue("0");
  await expect(undoStep).toBeDisabled();
  await redoStep.click();
  await expect(weightValue).toHaveValue("24");
  await expect(redoStep).toBeDisabled();

  // Ctrl+Z / Ctrl+Y drive the same step-scoped history from the keyboard.
  await expect(undoStep).toBeEnabled();
  await page.keyboard.press("Control+z");
  await expect(weightValue).toHaveValue("0");
  await expect(redoStep).toBeEnabled();
  await page.keyboard.press("Control+y");
  await expect(weightValue).toHaveValue("24");

  // The brightness step starts with an empty history even though the quality
  // step recorded edits, and its discard leaves the quality step untouched.
  await page.locator(".settings-index").getByRole("button", { name: "밝기와 대비" }).click();
  const contrastValue = page.locator("#contrast-value");
  await expect(undoStep).toBeDisabled();
  await expect(redoStep).toBeDisabled();
  await expect(discardStep).toBeDisabled();

  await contrastValue.fill("2");
  await contrastValue.press("Enter");
  await expect(contrastValue).toHaveValue("2");
  await discardStep.click();
  await expect(contrastValue).toHaveValue("1");
  await expect(discardStep).toBeDisabled();

  // The step discard itself is one more undoable step edit.
  await undoStep.click();
  await expect(contrastValue).toHaveValue("2");

  await page.locator(".settings-index").getByRole("button", { name: "글꼴 품질" }).click();
  await expect(weightValue).toHaveValue("24");
  await expect(undoStep).toBeEnabled();
  await expect(discardStep).toBeEnabled();
});

test("slider drags and exact number edits create one undo revision per interaction", async ({ page }, testInfo) => {
  await page.goto("/?view=profiles&gallery=1&lang=ko", { waitUntil: "networkidle" });
  await page.getByRole("button", { name: "글자 모양", exact: true }).click();

  const weightRow = page.locator(".setting-row").filter({ hasText: "일반 글자 굵기" });
  const weightSlider = weightRow.locator('input[type="range"]');
  const exactWeight = weightRow.locator('input[type="number"]');
  const undo = page.getByRole("button", { name: "되돌리기", exact: true });
  const redo = page.getByRole("button", { name: "다시 하기", exact: true });
  await expect(weightSlider).toHaveCount(1);
  await expect(exactWeight).toHaveValue("0");
  const shapeLayout = await page.locator(".settings-form").evaluate((element) => ({ clientWidth: element.clientWidth, scrollWidth: element.scrollWidth }));
  expect(shapeLayout.scrollWidth, "slider rows must fit without hidden horizontal overflow").toBeLessThanOrEqual(shapeLayout.clientWidth);
  const resetBounds = await weightRow.getByRole("button", { name: /기본값 복원/ }).boundingBox();
  const formBounds = await page.locator(".settings-form").boundingBox();
  if (!resetBounds || !formBounds) throw new Error("Slider reset button and settings form must be visible");
  expect(resetBounds.x + resetBounds.width, "slider reset button must remain inside the visible settings form").toBeLessThanOrEqual(formBounds.x + formBounds.width + 1);
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-profile-exact-input-ko.png`), fullPage: true });

  const sliderBox = await weightSlider.boundingBox();
  if (!sliderBox) throw new Error("Normal weight slider must be visible");
  const y = sliderBox.y + sliderBox.height / 2;
  await page.mouse.move(sliderBox.x + sliderBox.width / 2, y);
  await page.mouse.down();
  await page.mouse.move(sliderBox.x + sliderBox.width * 0.75, y, { steps: 12 });
  await page.mouse.up();

  const draggedValue = await exactWeight.inputValue();
  expect(Number(draggedValue)).toBeGreaterThan(0);
  await undo.click();
  await expect(exactWeight).toHaveValue("0");
  await redo.click();
  await expect(exactWeight).toHaveValue(draggedValue);

  await exactWeight.focus();
  await exactWeight.fill("6");
  await exactWeight.fill("64");
  await exactWeight.press("Tab");
  await expect(exactWeight).toHaveValue("64");
  await undo.click();
  await expect(exactWeight).toHaveValue(draggedValue);

  await exactWeight.focus();
  await exactWeight.fill("6");
  await exactWeight.fill("12");
  await undo.click();
  await expect(exactWeight).toHaveValue(draggedValue);

  await page.getByRole("button", { name: "고급·실험", exact: true }).click();
  const cacheRow = page.locator(".setting-row").filter({ hasText: "글꼴 캐시 개수" });
  const cacheValue = cacheRow.locator('input[type="number"]');
  await expect(cacheRow.locator('input[type="range"]')).toHaveCount(0);
  await expect(cacheValue).toHaveValue("64");
  await cacheValue.fill("128");
  await cacheValue.fill("256");
  await cacheValue.press("Enter");
  await undo.click();
  await expect(cacheValue).toHaveValue("64");
});

test("continuous number-wheel changes preview immediately but commit one undo revision", async ({ page }) => {
  await page.goto("/?view=profiles&gallery=1&lang=ko&preview-delay=20", { waitUntil: "networkidle" });
  await page.getByRole("button", { name: "글자 모양", exact: true }).click();

  const weightRow = page.locator(".setting-row").filter({ hasText: "일반 글자 굵기" });
  const exactWeight = weightRow.locator('input[type="number"]');
  const undo = page.getByRole("button", { name: "되돌리기", exact: true });
  await expect(exactWeight).toHaveValue("0");
  await expect(page.locator(".preview-strip img")).toHaveCount(1);
  await exactWeight.focus();
  await page.evaluate(() => window.sessionStorage.setItem("gallery-preview-started", "0"));

  const requestCounts = await exactWeight.evaluate(async (input: HTMLInputElement) => {
    const counts: number[] = [];
    for (let index = 0; index < 6; index += 1) {
      input.stepUp();
      input.dispatchEvent(new InputEvent("input", { bubbles: true, inputType: "insertText" }));
      await new Promise((resolve) => window.setTimeout(resolve, 10));
      counts.push(Number(window.sessionStorage.getItem("gallery-preview-started") ?? "0"));
    }
    return counts;
  });

  expect(requestCounts.some((count) => count > 0), "preview rendering must begin before continuous wheel input ends").toBe(true);
  await expect(exactWeight).toHaveValue("6");
  await expect(undo).toBeDisabled();

  await exactWeight.press("Tab");
  await expect(undo).toBeEnabled();
  await undo.click();
  await expect(exactWeight).toHaveValue("0");
  await expect(undo).toBeDisabled();
});

test("field revert restores the saved value while default restore and profile-wide reset use core defaults", async ({ page }) => {
  await page.goto("/?view=profiles&gallery=1&lang=ko", { waitUntil: "networkidle" });
  await page.getByRole("button", { name: "글자 모양", exact: true }).click();

  const weightRow = page.locator(".setting-row").filter({ hasText: "일반 글자 굵기" });
  const exactWeight = weightRow.locator('input[type="number"]');
  const revert = weightRow.getByRole("button", { name: /저장된 값으로 되돌리기/ });
  const restoreDefault = weightRow.getByRole("button", { name: /기본값 복원/ });

  // Clean profile: nothing to revert; the factory weight (16) differs from the
  // engine-default 0 the gallery profile starts from, so restore is available.
  await expect(revert).toBeDisabled();
  await expect(restoreDefault).toBeEnabled();

  await exactWeight.fill("12");
  await exactWeight.press("Enter");
  await page.getByRole("button", { name: "저장", exact: true }).click();
  await expect(page.locator(".profile-message")).toContainText("저장했습니다");
  await expect(revert).toBeDisabled();
  await expect(restoreDefault).toBeEnabled();

  await exactWeight.fill("30");
  await exactWeight.press("Enter");
  await expect(revert).toBeEnabled();
  await revert.click();
  await expect(exactWeight).toHaveValue("12");
  await expect(revert).toBeDisabled();

  await restoreDefault.click();
  await expect(exactWeight).toHaveValue("16");
  await expect(restoreDefault).toBeDisabled();
  await page.getByRole("button", { name: "되돌리기", exact: true }).click();
  await expect(exactWeight).toHaveValue("12");

  const gammaRow = page.locator(".setting-row").filter({ hasText: "감마 방식" });
  const gammaSelect = gammaRow.locator("select");
  await expect(gammaSelect).toHaveValue("-1");
  await gammaSelect.selectOption("2");
  await page.getByRole("button", { name: "기본값 초기화", exact: true }).click();
  await expect(exactWeight).toHaveValue("16");
  await expect(gammaSelect).toHaveValue("0");
  await page.getByRole("button", { name: "되돌리기", exact: true }).click();
  await expect(exactWeight).toHaveValue("12");
  await expect(gammaSelect).toHaveValue("2");

  await page.getByRole("button", { name: "변경 취소", exact: true }).click();
  await expect(exactWeight).toHaveValue("12");
  await expect(gammaSelect).toHaveValue("-1");
});

test("a rejected profile mutation requires an explicit snapshot recovery before save or apply", async ({ page }) => {
  await page.goto("/?view=profiles&gallery=1&lang=en&profile-fail-setting=normal_weight", { waitUntil: "networkidle" });
  await page.getByRole("button", { name: "Glyph shape", exact: true }).click();

  const normalWeight = page.locator(".setting-row").filter({ hasText: "Normal weight" }).locator('input[type="number"]');
  const boldWeight = page.locator(".setting-row").filter({ hasText: "Bold weight" }).locator('input[type="number"]');
  const save = page.getByRole("button", { name: "Save", exact: true });
  const saveAs = page.getByRole("button", { name: "Save as", exact: true });
  const followUp = page.locator(".profile-history-actions .designate");

  await normalWeight.fill("12");
  await normalWeight.press("Tab");
  await expect(page.getByText("Gallery profile mutation failed.", { exact: true })).toBeVisible();
  await page.getByTestId("preview-invert").click();
  await expect(page.locator(".preview-canvas")).toHaveAttribute("data-dark", "true");
  await expect(page.getByText("Gallery profile mutation failed.", { exact: true })).toBeVisible();
  await expect(save).toBeDisabled();
  await expect(saveAs).toBeDisabled();
  await expect(followUp).toHaveCount(0);

  await boldWeight.fill("8");
  await boldWeight.press("Tab");
  await expect(save).toBeDisabled();
  await expect(saveAs).toBeDisabled();
  await expect(followUp).toHaveCount(0);

  await page.getByRole("button", { name: "Discard changes", exact: true }).click();
  await expect(normalWeight).toHaveValue("0");
  await expect(boldWeight).toHaveValue("0");
  await expect(saveAs).toBeEnabled();
  await expect(followUp).toHaveCount(0);
});

test("an unmounted profile preview ignores an in-flight completion", async ({ page }) => {
  await page.goto("/?view=profiles&gallery=1&lang=en&ci-smoke=1&preview-delay=1000", { waitUntil: "domcontentloaded" });
  await expect.poll(() => page.evaluate(() => Number(window.sessionStorage.getItem("gallery-preview-started") ?? "0"))).toBeGreaterThan(0);

  await page.getByRole("button", { name: "Overview", exact: true }).click();
  await page.waitForTimeout(1200);

  await expect.poll(() => page.evaluate(() => window.sessionStorage.getItem("gallery-preview-crashes") ?? "0")).toBe("0");
  await expect.poll(() => page.evaluate(() => window.sessionStorage.getItem("gallery-profile-ready") ?? "0")).toBe("0");
});

test("settings files support import, save as, export, reveal, and apply without typing a path", async ({ page }) => {
  const failures: string[] = [];
  page.on("console", (message) => {
    if (message.type() === "error") failures.push(`console: ${message.text()}`);
  });
  page.on("pageerror", (error) => failures.push(`pageerror: ${error.message}`));

  await page.goto("/?view=files&gallery=1&lang=ko", { waitUntil: "networkidle" });
  await expect(page.getByRole("heading", { name: "기존 MacType 설정을 찾았습니다" })).toHaveCount(0);
  await expect(page.locator(".selected-file-summary")).toContainText("ini\\Default.ini");
  await expect(page.getByRole("textbox", { name: /경로|path/i })).toHaveCount(0);

  await page.getByRole("button", { name: "INI 파일 선택" }).click();
  await expect(page.locator('[data-operation="file-settings"]')).toContainText("Community.ini");
  await page.getByRole("button", { name: "다른 이름으로 저장", exact: true }).click();
  const dialog = page.getByRole("dialog", { name: "다른 이름으로 저장", exact: true });
  await dialog.getByRole("textbox", { name: "새 프로필 이름" }).fill("Gallery copy");
  await dialog.getByRole("button", { name: "저장", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  await expect(page.locator('[data-operation="file-settings"]')).toContainText("Gallery copy.ini");
  await page.getByRole("button", { name: "파일 위치 열기" }).click();
  await expect(page.locator('[data-operation="file-settings"]')).toContainText("파일 위치를 열었습니다");
  await page.getByRole("button", { name: "내보낼 위치 선택" }).click();
  await expect(page.locator('[data-operation="file-settings"]')).toContainText("내보냈습니다");
  await page.getByRole("button", { name: "실행 프로필로 지정", exact: true }).click();
  await expect(page.locator('[data-operation="file-settings"]')).toContainText("실행 프로필로 지정했습니다. 실행 중인 서비스에 바로 반영했습니다.");

  const horizontalOverflow = await page.evaluate(() => document.documentElement.scrollWidth > document.documentElement.clientWidth);
  expect(horizontalOverflow, "settings-file controls must not have horizontal scrolling").toBe(false);
  expect(failures, failures.join("\n")).toEqual([]);
});

test("settings files use available width for profile paths", async ({ page }, testInfo) => {
  test.skip(testInfo.project.name !== "desktop-1280", "Desktop width proves the selector can use available space.");

  await page.goto("/?view=files&gallery=1&lang=en", { waitUntil: "networkidle" });
  const pretendardCard = page.locator(".profile-card").filter({ hasText: "Pretendard forever" });
  await pretendardCard.locator(".profile-card-select").click();
  await expect(pretendardCard).toHaveAttribute("data-selected", "true");

  const cardPaths = page.locator(".profile-card-select code");
  for (let index = 0; index < await cardPaths.count(); index += 1) {
    const cardMetrics = await cardPaths.nth(index).evaluate((element) => ({
      clientWidth: element.clientWidth,
      scrollWidth: element.scrollWidth,
    }));
    expect(cardMetrics.scrollWidth).toBeLessThanOrEqual(cardMetrics.clientWidth);
  }

  const displayedPath = page.locator(".selected-file-path code");
  const pathMetrics = await displayedPath.evaluate((element) => ({
    clientWidth: element.clientWidth,
    scrollWidth: element.scrollWidth,
  }));
  expect(pathMetrics.scrollWidth).toBeLessThanOrEqual(pathMetrics.clientWidth);
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-settings-files-responsive-path-en.png`), fullPage: true });
});

test("diagnostics omit internal preview protocol details", async ({ page }) => {
  await page.goto("/?view=diagnostics&gallery=1&lang=en", { waitUntil: "networkidle" });
  await expect(page.getByText("Preview Helper", { exact: true })).toHaveCount(0);
  await expect(page.getByText("IPC protocol", { exact: true })).toHaveCount(0);
  await expect(page.getByText("MTPC v1", { exact: true })).toHaveCount(0);
});

test("saving over the run profile offers applying the saved settings to the service", async ({ page }, testInfo) => {
  await page.goto("/?view=profiles&gallery=1&lang=en", { waitUntil: "networkidle" });
  await expect(page.locator(".profile-editing")).toContainText("Editing:");
  await expect(page.locator(".profile-editing code")).toHaveText("ini\\Default.ini");

  const setting = page.locator(".setting-row select").first();
  const initial = await setting.inputValue();
  const alternate = await setting.locator("option").evaluateAll(
    (options, current) => options.map((option) => (option as HTMLOptionElement).value).find((value) => value !== current),
    initial,
  );
  if (!alternate) throw new Error("A writable profile setting needs an alternate gallery value");
  await setting.selectOption(alternate);

  const save = page.getByRole("button", { name: "Save", exact: true });
  const apply = page.getByRole("button", { name: "Apply to service", exact: true });
  await expect(save).toBeEnabled();
  await expect(page.locator(".profile-history-actions .designate")).toHaveCount(0);
  await save.click();
  await expect(page.locator(".profile-message")).toContainText("Saved Default.ini");
  await expect(apply).toBeEnabled();
  await expect(page.getByRole("button", { name: "Set as run profile", exact: true })).toHaveCount(0);
  await apply.click();
  await expect(page.locator(".profile-message")).toContainText("Restarted the service on the settings you saved. Apps without MacType, open now or later, get them; reopen apps that already had MacType to switch them over.");

  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-profile-direct-save-apply-en.png`), fullPage: true });
});

test("read-only profiles require Save as before apply", async ({ page }, testInfo) => {
  await page.goto("/?view=profiles&gallery=1&lang=en&profile-read-only=1", { waitUntil: "networkidle" });
  await expect(page.locator(".profile-editing code")).toHaveText("ini\\Default.ini");
  await expect(page.getByText("The original file cannot be written.", { exact: false })).toBeVisible();

  const setting = page.locator(".setting-row select").first();
  const initial = await setting.inputValue();
  const alternate = await setting.locator("option").evaluateAll(
    (options, current) => options.map((option) => (option as HTMLOptionElement).value).find((value) => value !== current),
    initial,
  );
  if (!alternate) throw new Error("A read-only profile setting needs an alternate gallery value");
  await setting.selectOption(alternate);

  await expect(page.getByRole("button", { name: "Save", exact: true })).toBeDisabled();
  await expect(page.getByRole("button", { name: "Set as run profile", exact: true })).toHaveCount(0);
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-profile-read-only-save-as-required-en.png`), fullPage: true });
  await page.getByRole("button", { name: "Save as", exact: true }).click();
  await page.getByRole("textbox", { name: "New profile name" }).fill("Review copy");
  await page.getByRole("dialog", { name: "Save as", exact: true }).getByRole("button", { name: "Save", exact: true }).click();

  await expect(page.locator(".profile-editing code")).toHaveText("Profiles\\Review copy.ini");
  await expect(page.locator(".profile-message")).toContainText("Saved as Profiles\\Review copy.ini");
  await expect(page.getByRole("button", { name: "Set as run profile", exact: true })).toBeEnabled();
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-profile-read-only-save-as-en.png`), fullPage: true });
});

for (const view of ["files", "profiles"]) {
  test(`${view} Save As validates names, traps focus, and restores its trigger`, async ({ page }, testInfo) => {
    await page.goto(`/?view=${view}&gallery=1&lang=ko`, { waitUntil: "networkidle" });
    const trigger = page.getByRole("button", { name: "다른 이름으로 저장", exact: true });
    await trigger.click();
    const dialog = page.getByRole("dialog", { name: "다른 이름으로 저장", exact: true });
    const name = dialog.getByRole("textbox", { name: "새 프로필 이름", exact: true });
    const submit = dialog.getByRole("button", { name: "저장", exact: true });
    const cancel = dialog.getByRole("button", { name: "취소", exact: true });
    await expect(name).toHaveValue("Default");
    await expect(name).toBeFocused();
    expect(await name.evaluate((element: HTMLInputElement) => [element.selectionStart, element.selectionEnd])).toEqual([0, 7]);
    await expect(submit).toBeEnabled();
    await name.press("Shift+Tab");
    await expect(submit).toBeFocused();
    await submit.press("Tab");
    await expect(name).toBeFocused();
    await name.fill("Recent");
    await expect(dialog).toContainText("같은 이름의 프로필이 이미 있습니다.");
    await expect(name).toHaveAttribute("aria-invalid", "true");
    await expect(name).toHaveAttribute("aria-describedby", "profile-name-dialog-message");
    await expect(submit).toBeDisabled();
    await name.press("Enter");
    await expect(dialog).toBeVisible();
    await name.press("Shift+Tab");
    await expect(cancel).toBeFocused();
    await cancel.press("Tab");
    await expect(name).toBeFocused();
    await name.fill("Bad/name");
    await expect(dialog).toContainText("이름에 쓸 수 없는 문자가 들어 있습니다.");
    await expect(submit).toBeDisabled();
    await name.fill("");
    await expect(dialog.locator('[aria-live="polite"]')).toBeEmpty();
    await expect(submit).toBeDisabled();
    await name.fill("Gallery named profile");
    await expect(name).toHaveAttribute("aria-invalid", "false");
    await expect(name).not.toHaveAttribute("aria-describedby");
    await expect(submit).toBeEnabled();
    expect(await overflowingElements(page)).toEqual([]);
    await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-${view}-save-as-dialog-ko.png`), fullPage: true });
    await name.press("Escape");
    await expect(dialog).toHaveCount(0);
    await expect(trigger).toBeFocused();
    await trigger.click();
    await cancel.click();
    await expect(dialog).toHaveCount(0);
    await expect(trigger).toBeFocused();
    await trigger.click();
    await name.fill("Gallery named profile");
    await name.press("Enter");
    await expect(dialog).toHaveCount(0);
    await expect(trigger).toBeFocused();
  });
}

test("Save As advances the suggested name past a managed profile collision", async ({ page }) => {
  await page.goto("/?view=files&gallery=1&lang=en", { waitUntil: "networkidle" });
  await page.locator(".profile-card").filter({ hasText: "Recent" }).locator(".profile-card-select").click();
  await page.getByRole("button", { name: "Save as", exact: true }).click();
  const dialog = page.getByRole("dialog", { name: "Save as", exact: true });
  await expect(dialog.getByRole("textbox", { name: "New profile name" })).toHaveValue("Recent (2)");
  await expect(dialog.getByRole("button", { name: "Save", exact: true })).toBeEnabled();
});

test("guided saving offers only the outstanding follow-up and clears it after an edit", async ({ page }) => {
  await page.goto("/?view=profiles&gallery=1&lang=ko&service-runtime=stopped", { waitUntil: "networkidle" });
  await page.getByRole("button", { name: "단계별 설정", exact: true }).click();
  await page.locator(".settings-index").getByRole("button", { name: "실행 프로필 지정", exact: true }).click();
  const card = page.locator(".guided-apply-card");
  await expect(card.locator(".designate")).toHaveCount(0);
  await card.getByRole("button", { name: "다른 이름으로 저장", exact: true }).click();
  const dialog = page.getByRole("dialog", { name: "다른 이름으로 저장", exact: true });
  await dialog.getByRole("textbox", { name: "새 프로필 이름" }).fill("Guided copy");
  await dialog.getByRole("button", { name: "저장", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  await expect(card.getByRole("button")).toHaveText(["프리뷰", "프로필 저장", "다른 이름으로 저장", "실행 프로필로 지정"]);
  await card.getByRole("button", { name: "실행 프로필로 지정", exact: true }).click();
  await expect(card.getByRole("button", { name: "지금 서비스 시작", exact: true })).toBeEnabled();
  await page.locator(".settings-index").getByRole("button", { name: "기본 렌더링", exact: true }).click();
  const choice = page.locator('.guided-choice input:not(:checked)').first();
  await choice.check();
  await page.locator(".settings-index").getByRole("button", { name: "실행 프로필 지정", exact: true }).click();
  await expect(card.locator(".designate")).toHaveCount(0);
  await card.getByRole("button", { name: "프로필 저장", exact: true }).click();
  const apply = card.getByRole("button", { name: "서비스에 적용", exact: true });
  await expect(apply).toBeEnabled();
  await expect(card.getByRole("button", { name: "실행 프로필로 지정", exact: true })).toHaveCount(0);
  await page.locator(".settings-index").getByRole("button", { name: "기본 렌더링", exact: true }).click();
  await choice.check();
  await page.locator(".settings-index").getByRole("button", { name: "실행 프로필 지정", exact: true }).click();
  await expect(card.locator(".designate")).toHaveCount(0);
  await card.getByRole("button", { name: "프로필 저장", exact: true }).click();
  await expect(apply).toBeEnabled();
  await apply.click();
  await expect(page.locator(".profile-message")).toHaveText("저장한 설정을 실행 프로필에 반영했습니다. 서비스를 시작하면 이 설정으로 실행됩니다.");
  await expect(card.getByRole("button", { name: "지금 서비스 시작", exact: true })).toBeEnabled();
});

test("known legacy-selected profiles open directly without an import detour", async ({ page }, testInfo) => {
  await page.goto("/?view=files&gallery=1&lang=en&fresh=1&profile-runtime-missing=1", { waitUntil: "networkidle" });
  await expect(page.locator(".legacy-import-banner")).toHaveCount(0);
  await expect(page.locator(".selected-file-summary strong")).toHaveText("Editing");
  await expect(page.locator(".selected-file-summary code")).toHaveText("ini\\pretendard forever.ini");
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-profile-direct-open-en.png`), fullPage: true });
});

test("external legacy-selected profiles require an explicit import", async ({ page }, testInfo) => {
  await page.goto("/?view=files&gallery=1&lang=en&legacy-profile=external", { waitUntil: "networkidle" });
  const banner = page.locator(".legacy-import-banner");
  await expect(banner).toContainText("Existing MacType settings found");
  await expect(banner).toContainText("C:\\Users\\Gallery\\Downloads\\External.ini");
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-profile-external-import-required-en.png`), fullPage: true });
  await banner.getByRole("button", { name: "Import these settings" }).click();
  await expect(banner).toHaveCount(0);
  await expect(page.locator(".selected-file-summary code")).toHaveText("Profiles\\External.ini");
  await expect(page.locator('[data-operation="file-settings"]')).toContainText("Imported External.ini");
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-profile-external-import-en.png`), fullPage: true });
});

test("execution and new system service controls remain interactive", async ({ page }) => {
  const failures: string[] = [];
  page.on("console", (message) => {
    if (message.type() === "error") failures.push(`console: ${message.text()}`);
  });
  page.on("pageerror", (error) => failures.push(`pageerror: ${error.message}`));

  await page.goto("/?view=execution&gallery=1&lang=ko", { waitUntil: "networkidle" });
  await openServiceDetails(page);
  const autostart = page.getByRole("switch", { name: "로그인 시 트레이 시작" });
  await autostart.check();
  await expect(page.getByText("로그인할 때 트레이로 시작합니다.")).toBeVisible();
  await expect(page.getByRole("textbox", { name: "실행 파일의 전체 경로" })).toHaveCount(0);
  await page.getByRole("button", { name: "실행 파일 선택" }).click();
  await expect(page.getByTitle("C:\\Windows\\System32\\notepad.exe", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "트레이에 등록" }).click();
  await expect(page.locator(".registered-launchers li code").filter({ hasText: "C:\\Windows\\System32\\notepad.exe" })).toBeVisible();
  await page.getByRole("button", { name: "등록 프로그램 실행" }).click();
  await expect(page.getByText(/등록 프로그램 1개를 MacType로 시작/)).toBeVisible();
  await page.getByRole("button", { name: "MacType로 실행" }).click();
  await expect(page.getByText(/MacType을 적용해 앱을 실행했습니다\(PID 4242\)/)).toBeVisible();
  await expect(page.getByRole("heading", { name: "시스템 범위 모드" })).toBeVisible();

  await expect(page.getByText("MacType 시스템 적용 중", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "새로 여는 앱에 적용 중지" }).click();
  await expect(page.getByText("MacType 시스템 적용 꺼짐", { exact: true })).toBeVisible();
  await expect(page.getByText("MacType 시스템 적용을 잠시 껐습니다.", { exact: true })).toBeVisible();
  await page.locator(".system-injection-action").click();
  await expect(page.getByText("MacType 시스템 적용 중", { exact: true })).toBeVisible();
  await expect(page.getByText("실행 프로필로 서비스를 시작했습니다.", { exact: true })).toBeVisible();

  await expect(page.locator('[data-service-backend="open-source"]')).toContainText("MacType Control Center 서비스");
  await expect(page.locator('[data-service-backend="legacy-mactray"]')).toHaveCount(0);

  const horizontalOverflow = await page.evaluate(() => document.documentElement.scrollWidth > document.documentElement.clientWidth);
  expect(horizontalOverflow, "execution controls must not have horizontal scrolling").toBe(false);
  expect(failures, failures.join("\n")).toEqual([]);
});

test("manual launch offers running processes first and file browsing second", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=ko&system-service=ready", { waitUntil: "networkidle" });
  const manualRow = page.locator('details.service-row[data-kind="manual"]');
  await manualRow.locator("summary").click();

  await expect(manualRow.getByText("실행 중인 앱", { exact: true })).toBeVisible();
  const rows = manualRow.locator(".process-picker-row");
  await expect(rows).toHaveCount(5);
  await expect(rows.nth(0)).toContainText("code.exe");
  await expect(rows.nth(0)).toContainText("Visual Studio Code");
  await expect(rows.nth(0)).toContainText("PID 5678");
  await expect(rows.nth(1)).toContainText("제목 없음 - 메모장");
  await expect(manualRow.getByText("목록에 없는 실행 파일 찾아보기", { exact: true })).toBeVisible();

  const register = manualRow.getByRole("button", { name: "트레이에 등록" });
  await expect(register).toBeDisabled();

  await manualRow.getByLabel("앱 필터").fill("note");
  await expect(rows).toHaveCount(1);
  await expect(rows.first()).toContainText("notepad.exe");

  await manualRow.getByRole("radio", { name: /notepad\.exe/ }).check();
  await expect(manualRow.locator(".target-selection strong")).toHaveText("notepad.exe");
  await expect(manualRow.locator(".target-selection code")).toHaveText("C:\\Tools\\notepad.exe");
  await expect(register).toBeEnabled();

  expect(await overflowingElements(page)).toEqual([]);
});

test("a running legacy service is never claimed as verified system application", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=ko&system-service=ready&legacy=migration-available", { waitUntil: "networkidle" });
  await openServiceDetails(page);

  const openService = page.locator('[data-service-backend="open-source"]');
  await expect(openService).toBeVisible();
  await expect(openService).toContainText("MacType Control Center 서비스");
  await expect(openService).toContainText("정상");
  await expect(page.getByText("MacType 시스템 적용 중", { exact: true })).toHaveCount(0);
  await expect(openService.locator('[data-state="running-unverified"]')).toBeVisible();

  const legacy = page.locator('[data-service-backend="legacy-mactray"]');
  await expect(legacy).toBeVisible();
  await expect(legacy).toContainText("레거시 MacTray");
  await expect(legacy.getByRole("button", { name: "마이그레이션" })).toBeEnabled();
  await expect(legacy.getByRole("button", { name: "레거시 서비스 제거" })).toBeDisabled();

  await page.getByRole("button", { name: "새로 여는 앱에 적용 중지" }).click();
  await expect(openService.locator('[data-state="legacy-service-migrate"]')).toBeVisible();
  await expect(openService).toContainText("레거시 MacTray 서비스를 먼저 정리해야 합니다");
  await expect(page.locator(".system-injection-action")).toBeDisabled();
  await expect(legacy.getByRole("button", { name: "마이그레이션" })).toBeEnabled();

  expect(await page.evaluate(() => document.documentElement.scrollWidth > document.documentElement.clientWidth)).toBe(false);
});

test("a verified migration cannot be started again while the retired legacy service is stopped", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=ko&system-service=ready&legacy=migration-available&legacy-state=stopped&legacy-retired=1", { waitUntil: "networkidle" });
  await openServiceDetails(page);

  const openService = page.locator('[data-service-backend="open-source"]');
  await expect(openService.locator('[data-state="active"]')).toBeVisible();
  await expect(openService).toContainText("MacType 시스템 적용 중");

  const legacy = page.locator('[data-service-backend="legacy-mactray"]');
  await expect(legacy).toContainText("중지됨");
  await expect(legacy.getByRole("button", { name: "마이그레이션" })).toBeDisabled();
});

test("a retired stopped legacy service leaves new-service recovery available", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=ko&system-service=migration-available&legacy=migration-available&legacy-state=stopped&legacy-retired=1", { waitUntil: "networkidle" });
  await openServiceDetails(page);

  const openService = page.locator('[data-service-backend="open-source"]');
  await expect(openService.getByRole("button", { name: "서비스 설치" })).toBeEnabled();
  await expect(openService).not.toContainText("레거시 MacTray 서비스를 먼저 정리해야 합니다");

  const legacy = page.locator('[data-service-backend="legacy-mactray"]');
  await expect(legacy.getByRole("button", { name: "마이그레이션" })).toBeDisabled();
});

test("designating a run profile with no service holds the choice until the service page starts it", async ({ page }) => {
  await page.goto("/?view=files&gallery=1&lang=en&system-service=migration-available", { waitUntil: "networkidle" });
  await page.getByRole("button", { name: "Set as run profile", exact: true }).click();
  await expect(page.getByText(/is now the run profile\. The service will use it when it starts\./)).toBeVisible();
  await expect(page.getByRole("button", { name: "Start the service now" })).toHaveCount(0);

  await page.getByRole("button", { name: "Service" }).click();
  await openServiceDetails(page);
  const openService = page.locator('[data-service-backend="open-source"]');
  await expect(openService.locator('[data-state="unavailable"], [data-state="inactive"]').first()).toBeVisible();
  await expect(openService).not.toContainText("MacType system-wide rendering active");
  await page.locator("[data-service-summary]").getByRole("button", { name: "Install service" }).click();
  await expect(openService.locator('[data-state="active"]')).toBeVisible();
  await expect(openService).toContainText("MacType system-wide rendering active");
});

test("designating a run profile while the service is stopped keeps it stopped and offers one explicit start", async ({ page }, testInfo) => {
  await page.goto("/?view=files&gallery=1&lang=ko&system-service=stopped", { waitUntil: "networkidle" });
  const pretendardCard = page.locator(".profile-card").filter({ hasText: "Pretendard forever" });
  await expect(page.locator('.profile-card[data-run-profile="true"] .profile-card-title strong')).toHaveText("Default");
  await pretendardCard.locator(".profile-card-select").click();
  await page.getByRole("button", { name: "실행 프로필로 지정", exact: true }).click();

  const message = page.locator('[data-operation="file-settings"]');
  await expect(message).toContainText("서비스를 시작하면 이 프로필로 실행됩니다.");
  await expect(pretendardCard).toHaveAttribute("data-run-profile", "true");
  await expect(pretendardCard.locator(".profile-card-badge")).toHaveText("실행 프로필");
  const startNow = message.getByRole("button", { name: "지금 서비스 시작" });
  await expect(startNow).toBeVisible();
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-run-profile-held-ko.png`), fullPage: true });

  const wizardGroup = page.locator(".navigation").getByRole("group", { name: "위자드" });
  await wizardGroup.getByRole("button", { name: "서비스" }).click();
  const summary = page.locator("[data-service-summary]");
  await expect(summary).toContainText("중지됨");
  await expect(summary).toContainText("실행 프로필");
  await expect(summary).toContainText("pretendard forever.ini");
  await openServiceDetails(page);
  await expect(page.getByText("MacType 시스템 적용 꺼짐", { exact: true })).toBeVisible();
  await expect(page.locator(".system-injection-control")).toContainText("실행 프로필(pretendard forever.ini)");

  // The page remounts on return, so the held designation is repeated before
  // the one explicit start it offers is taken.
  await wizardGroup.getByRole("button", { name: "프로필" }).click();
  await expect(page.locator('.profile-card[data-run-profile="true"] .profile-card-title strong')).toHaveText("Pretendard forever");
  await page.getByRole("button", { name: "실행 프로필로 지정", exact: true }).click();
  await page.locator('[data-operation="file-settings"]').getByRole("button", { name: "지금 서비스 시작" }).click();
  await expect(page.locator('[data-operation="file-settings"]')).toContainText("서비스를 시작했습니다.");
  await expect(page.locator('[data-operation="file-settings"]').getByRole("button", { name: "지금 서비스 시작" })).toHaveCount(0);
  await wizardGroup.getByRole("button", { name: "서비스" }).click();
  await expect(summary).toContainText("실행 중");
  await openServiceDetails(page);
  await expect(page.getByText("MacType 시스템 적용 중", { exact: true })).toBeVisible();
});

test("designating a run profile while the service runs switches it live", async ({ page }, testInfo) => {
  await page.goto("/?view=files&gallery=1&lang=ko&system-service=ready", { waitUntil: "networkidle" });
  const pretendardCard = page.locator(".profile-card").filter({ hasText: "Pretendard forever" });
  await pretendardCard.locator(".profile-card-select").click();
  await page.getByRole("button", { name: "실행 프로필로 지정", exact: true }).click();

  const message = page.locator('[data-operation="file-settings"]');
  await expect(message).toContainText("실행 중인 서비스에 바로 반영했습니다.");
  await expect(message.getByRole("button", { name: "지금 서비스 시작" })).toHaveCount(0);
  await expect(pretendardCard).toHaveAttribute("data-run-profile", "true");
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-run-profile-live-ko.png`), fullPage: true });

  await page.locator(".navigation").getByRole("group", { name: "위자드" }).getByRole("button", { name: "서비스" }).click();
  await expect(page.locator("[data-service-summary]")).toContainText("실행 중");
  await expect(page.locator("[data-service-summary]")).toContainText("pretendard forever.ini");
  await openServiceDetails(page);
  await expect(page.getByText("MacType 시스템 적용 중", { exact: true })).toBeVisible();
});

for (const entry of [
  { view: "files", failing: "designate-profile" },
  { view: "files", failing: "publish-profile" },
  { view: "profiles", failing: "publish-profile" },
] as const) {
  test(`${entry.view} hides internal ${entry.failing} details behind the diagnostics message`, async ({ page }) => {
    await page.goto(`/?view=${entry.view}&gallery=1&lang=en&service-fail=${entry.failing}`, { waitUntil: "networkidle" });
    if (entry.view === "profiles") {
      await page.getByRole("button", { name: "Save as", exact: true }).click();
      const dialog = page.getByRole("dialog", { name: "Save as", exact: true });
      await dialog.getByRole("textbox", { name: "New profile name" }).fill("Publish failure copy");
      await dialog.getByRole("button", { name: "Save", exact: true }).click();
      await expect(dialog).toHaveCount(0);
    }
    await page.getByRole("button", { name: "Set as run profile", exact: true }).first().click();

    await expect(page.getByText("The operation failed. Check the diagnostics log for details.", { exact: true })).toBeVisible();
    await expect(page.getByText(/control-center-internal-operation-failed/)).toHaveCount(0);
  });
}

test("a foreign legacy MacType service blocks activation and offers no migration", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=migration-available&legacy=foreign", { waitUntil: "networkidle" });
  await openServiceDetails(page);

  const openService = page.locator('[data-service-backend="open-source"]');
  await expect(openService.locator('[data-state="legacy-service-migrate"]')).toBeVisible();
  await expect(openService).toContainText("A foreign legacy MacTray service was detected");
  await expect(openService).toContainText("A different service is using the MacType name");
  await expect(openService).not.toContainText("Use Migrate below");
  await expect(openService.locator(".system-injection-action")).toBeDisabled();
  await expect(openService.locator(".service-actions").getByRole("button", { name: "Install service" })).toBeDisabled();
  await expect(openService.locator(".service-actions").getByRole("button", { name: "Start service" })).toBeDisabled();

  const legacy = page.locator('[data-service-backend="legacy-mactray"]');
  await expect(legacy).toBeVisible();
  await expect(legacy.getByRole("button", { name: "Migrate" })).toBeDisabled();
  await expect(legacy.getByRole("button", { name: "Remove legacy service" })).toBeDisabled();
});

test("a verified legacy service funnels activation through Migrate until it is removed", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=migration-available&legacy=migration-available", { waitUntil: "networkidle" });
  await openServiceDetails(page);

  const openService = page.locator('[data-service-backend="open-source"]');
  await expect(openService.locator('[data-state="legacy-service-migrate"]')).toBeVisible();
  await expect(openService).toContainText("A legacy MacTray service must be resolved first");
  await expect(openService).toContainText("An older MacTray service is installed");
  await expect(openService).not.toContainText("foreign legacy MacTray service");
  await expect(openService).not.toContainText("status could not be verified");
  await expect(openService.locator(".system-injection-action")).toBeDisabled();
  await expect(openService.locator(".service-actions").getByRole("button", { name: "Install service" })).toBeDisabled();
  await expect(openService.locator(".service-actions").getByRole("button", { name: "Start service" })).toBeDisabled();

  const legacy = page.locator('[data-service-backend="legacy-mactray"]');
  await expect(legacy).toContainText("Verified MacTray service");
  await expect(legacy.getByRole("button", { name: "Migrate" })).toBeEnabled();

  await legacy.getByRole("button", { name: "Migrate" }).click();
  await page.getByRole("dialog", { name: "Migrate legacy MacTray?" }).getByRole("button", { name: "Continue migration" }).click();
  await expect(page.getByText("Migration to the new service completed.", { exact: true })).toBeVisible();

  await expect(legacy).toContainText("Stopped");
  await expect(openService.getByRole("button", { name: "Stop applying to new apps" })).toBeEnabled();
  await expect(openService.getByRole("button", { name: "Install service" })).toBeDisabled();

  await legacy.getByRole("button", { name: "Remove legacy service" }).click();
  await expect(page.getByText("The legacy service was removed.", { exact: true })).toBeVisible();
  await expect(page.locator('[data-service-backend="legacy-mactray"]')).toHaveCount(0);
});

test("internal migration failures show only the localized diagnostics instruction", async ({ page }) => {
  for (const locale of [
    { lang: "en", title: "Migrate legacy MacTray?", continue: "Continue migration", message: "Migration failed. Check the diagnostics log for details." },
    { lang: "ko", title: "레거시 MacTray를 마이그레이션할까요?", continue: "마이그레이션 계속", message: "마이그레이션에 실패했습니다. 자세한 내용은 진단 로그를 확인하세요." },
  ]) {
    await page.goto(`/?view=execution&gallery=1&lang=${locale.lang}&system-service=migration-available&legacy=migration-available&service-fail=migrate-from-legacy`, { waitUntil: "networkidle" });
    await openServiceDetails(page);
    const legacy = page.locator('[data-service-backend="legacy-mactray"]');
    await legacy.getByRole("button", { name: locale.lang === "ko" ? "마이그레이션" : "Migrate" }).click();
    await page.getByRole("dialog", { name: locale.title }).getByRole("button", { name: locale.continue }).click();
    await expect(page.getByText(locale.message, { exact: true })).toBeVisible();
    await expect(page.getByText(/control-center-internal-operation-failed|broker exit code|strict Ready/)).toHaveCount(0);
  }
});

test("legacy migration explains concrete actions and rollback before it can continue", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=migration-available&legacy=migration-available", { waitUntil: "networkidle" });

  const migrationTrigger = page.locator("[data-service-summary]").getByRole("button", { name: "Migrate" });
  await migrationTrigger.click();

  const dialog = page.getByRole("dialog", { name: "Migrate legacy MacTray?" });
  const cancel = dialog.getByRole("button", { name: "Cancel" });
  const continueMigration = dialog.getByRole("button", { name: "Continue migration" });
  await expect(dialog).toBeVisible();
  await expect(cancel).toBeFocused();
  await expect(dialog).toContainText("Checks whether another MacType mode must be turned off first");
  await expect(dialog).toContainText("Backs up the current settings and profile before switching");
  await expect(dialog).toContainText("Stops the legacy service");
  await expect(dialog).toContainText("copies the current profile to the new service");
  await expect(dialog).toContainText("installs and starts it");
  await expect(dialog).toContainText("restores the services and profile to their previous state");
  await expect(dialog).not.toContainText(/verif|safe|Ready|digest|smoke|does not remove/i);

  await page.keyboard.press("Shift+Tab");
  await expect(continueMigration).toBeFocused();
  await page.keyboard.press("Tab");
  await expect(cancel).toBeFocused();
  await page.keyboard.press("Escape");
  await expect(dialog).toHaveCount(0);
  await expect(migrationTrigger).toBeFocused();
  await expect(page.getByText("Migration to the new service completed.", { exact: true })).toHaveCount(0);

  await migrationTrigger.click();
  await cancel.click();
  await expect(migrationTrigger).toBeFocused();

  await migrationTrigger.click();
  await continueMigration.click();
  await expect(page.getByText("Migration to the new service completed.", { exact: true })).toBeVisible();
  await expect(migrationTrigger).toHaveCount(0);
});

test("system service path is read-only and can reveal its installed location", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=ko&system-service=ready", { waitUntil: "networkidle" });
  await openServiceDetails(page);

  const openService = page.locator('[data-service-backend="open-source"]');
  const servicePath = "C:\\Program Files\\MacType Control Center\\Service\\mactype-service.exe";
  await expect(openService.locator("code", { hasText: servicePath })).toBeVisible();
  await expect(openService.getByRole("textbox")).toHaveCount(0);

  const reveal = openService.getByRole("button", { name: "서비스 위치 열기" });
  const bounds = await reveal.boundingBox();
  if (!bounds) throw new Error("The reveal service location button must be visible");
  expect(bounds.width).toBeGreaterThanOrEqual(40);
  expect(bounds.height).toBeGreaterThanOrEqual(40);
  await reveal.click();
  await expect(page.getByText("서비스 파일 위치를 열었습니다.", { exact: true })).toBeVisible();
});

test("an absent service never exposes a binary path or location action", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=migration-available", { waitUntil: "networkidle" });
  await openServiceDetails(page);

  const openService = page.locator('[data-service-backend="open-source"]');
  await expect(openService).toContainText("Not installed");
  await expect(openService.locator(".service-path")).toHaveCount(0);
  await expect(openService.getByRole("button", { name: "Open service location" })).toHaveCount(0);
});

test("the service page keeps its normal state to one summary and one action", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=ready", { waitUntil: "networkidle" });

  const summary = page.locator("[data-service-summary]");
  await expect(summary).toContainText("Run profile");
  await expect(summary).toContainText("Default.ini");
  await expect(summary).toContainText("Control Center service");
  await expect(summary).toContainText("Running");
  await expect(summary.getByRole("button", { name: "Stop" })).toBeEnabled();
  await expect(summary.getByRole("button")).toHaveCount(1);
  await expect(page.getByRole("heading", { name: "System-wide modes" })).toBeVisible();
  await expect(page.locator('details.service-row[data-kind="system"]')).toContainText("Current installation · Running · Healthy");
  await expect(page.getByRole("button", { name: "Remove service" })).toBeHidden();
  await expect(page.locator("details.service-row[open]")).toHaveCount(0);
});

test("small degraded states stay in Details while failed configuration is actionable", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=degraded", { waitUntil: "networkidle" });
  const summary = page.locator("[data-service-summary]");
  await expect(summary).toContainText("Running");
  await expect(summary).not.toContainText("Degraded");
  await expect(summary.getByRole("button", { name: "Repair service" })).toHaveCount(0);
  await expect(page.getByText("Could not confirm that MacType is applying", { exact: true })).toBeHidden();
  await openServiceDetails(page);
  await expect(page.locator('[data-service-backend="open-source"]')).toContainText("Degraded");

  await page.goto("/?view=execution&gallery=1&lang=en&system-service=failed", { waitUntil: "networkidle" });
  await expect(summary).toContainText("Service configuration needs repair.");
  await expect(summary.getByRole("button", { name: "Repair service" })).toBeEnabled();
});

test("outdated services upgrade while only failed current services repair", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=outdated", { waitUntil: "networkidle" });
  const summary = page.locator("[data-service-summary]");
  await expect(summary).toContainText("Update required");
  await expect(summary.getByRole("button", { name: "Upgrade service" })).toBeEnabled();
  await openServiceDetails(page);
  const outdated = page.locator('[data-service-backend="open-source"]');
  await expect(outdated.getByRole("button", { name: "Upgrade service" })).toBeEnabled();
  await expect(outdated.getByRole("button", { name: "Repair service" })).toHaveCount(0);

  await page.goto("/?view=execution&gallery=1&lang=en&system-service=degraded", { waitUntil: "networkidle" });
  await openServiceDetails(page);
  const degraded = page.locator('[data-service-backend="open-source"]');
  await expect(degraded.getByRole("button", { name: "Repair service" })).toHaveCount(0);
  await expect(degraded.getByRole("button", { name: "Upgrade service" })).toHaveCount(0);

  await page.goto("/?view=execution&gallery=1&lang=en&system-service=failed", { waitUntil: "networkidle" });
  await openServiceDetails(page);
  const failed = page.locator('[data-service-backend="open-source"]');
  await expect(failed.getByRole("button", { name: "Repair service" })).toBeEnabled();
  await expect(failed.getByRole("button", { name: "Upgrade service" })).toHaveCount(0);
});

test("a running unverified service remains stoppable without claiming it is inactive", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=degraded", { waitUntil: "networkidle" });
  await openServiceDetails(page);

  const openService = page.locator('[data-service-backend="open-source"]');
  await expect(openService.getByRole("button", { name: "Stop applying to new apps" })).toBeEnabled();
  await expect(openService).toContainText("Could not confirm that MacType is applying");
  await expect(openService).toContainText("The service is running, but its effect on apps could not be checked. You can still stop it.");
  await expect(openService).not.toContainText("Start it to apply the run profile");
  await openService.getByRole("button", { name: "Stop applying to new apps" }).click();
  await expect(page.getByText("MacType system application is temporarily off.", { exact: true })).toBeVisible();
});

test("a running profile mismatch remains stoppable and identifies the selected profile mismatch", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=profile-mismatch", { waitUntil: "networkidle" });
  const summary = page.locator("[data-service-summary]");
  await expect(summary).toContainText("Service running with a different profile");
  await expect(summary).toContainText("The service is running with a profile other than the run profile.");
  await expect(summary.getByRole("button", { name: "Stop" })).toBeEnabled();
  await openServiceDetails(page);

  const openService = page.locator('[data-service-backend="open-source"]');
  await expect(openService.getByRole("button", { name: "Stop applying to new apps" })).toBeEnabled();
  await expect(openService).toContainText("Service running with a different profile");
  await expect(openService).toContainText("The service is running with a profile other than the run profile. Setting the run profile again on the Profiles page switches it immediately.");
  await expect(openService).toContainText("Profile mismatch");
  await expect(openService).not.toContainText("or not yet verified");
});

test("AppInit conflict preserves the backend-authorized recovery stop", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=legacy-conflict&legacy=migration-available&raw-active=1", { waitUntil: "networkidle" });
  const summary = page.locator("[data-service-summary]");
  await expect(summary).toContainText("Service running while AppInit conflicts");
  await expect(summary).toContainText("AppInit mode is also enabled.");
  await expect(summary.getByRole("button", { name: "Stop" })).toBeEnabled();
  await openServiceDetails(page);

  const openService = page.locator('[data-service-backend="open-source"]');
  await expect(openService.getByRole("button", { name: "Stop applying to new apps" })).toBeEnabled();
  await expect(openService).toContainText("Service running while AppInit conflicts");
  await expect(openService).toContainText("AppInit mode is also enabled. Turn it off in your previous MacType setup before changing this service. You can still stop the service.");
  await expect(openService).not.toContainText("MacType system-wide rendering active");
  const statusRow = openService.locator(".detail-list > div").filter({ hasText: "Service status" });
  await expect(statusRow.locator(".warning")).toBeVisible();
  await expect(statusRow.locator(".success")).toHaveCount(0);
  for (const name of ["Install service", "Start service", "Remove service"]) {
    await expect(openService.locator(".service-actions").getByRole("button", { name })).toBeDisabled();
  }
  const legacy = page.locator('[data-service-backend="legacy-mactray"]');
  await expect(legacy.getByRole("button", { name: "Migrate" })).toBeDisabled();
  await expect(legacy.getByRole("button", { name: "Remove legacy service" })).toBeDisabled();
});

test("AppInit remains prominent when there is no safe automatic recovery action", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=legacy-conflict&service-runtime=stopped", { waitUntil: "networkidle" });

  const summary = page.locator("[data-service-summary]");
  await expect(summary).toContainText("AppInit registry mode is active, so service installation and startup are blocked.");
  await expect(summary.locator("[data-prominent-exception]")).toHaveAttribute("data-kind", "appinit-conflict");
  await expect(summary.getByRole("button")).toHaveCount(0);
});

test("trusted MacTray and autostart conflicts are resolved in the required order", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=migration-available&legacy-tray=trusted-current&legacy-startup=hkcu-run", { waitUntil: "networkidle" });

  const conflict = page.locator("[data-legacy-tray-conflict]");
  const summaryInstall = page.locator("[data-service-summary]").getByRole("button", { name: "Install service" });
  await expect(conflict).toContainText("Existing MacTray is running");
  await expect(conflict.getByRole("button", { name: "Exit MacTray" })).toBeEnabled();
  await expect(conflict.getByRole("button", { name: "Check again" })).toBeEnabled();
  await expect(conflict.getByRole("button", { name: "Disable MacTray autostart" })).toHaveCount(0);
  await expect(summaryInstall).toHaveCount(0);

  await conflict.getByRole("button", { name: "Exit MacTray" }).click();
  await expect(conflict).toContainText("MacTray autostart must be disabled");
  await expect(conflict.getByRole("button", { name: "Exit MacTray" })).toHaveCount(0);
  await expect(conflict.getByRole("button", { name: "Disable MacTray autostart" })).toBeEnabled();
  await expect(summaryInstall).toHaveCount(0);

  await conflict.getByRole("button", { name: "Disable MacTray autostart" }).click();
  await expect(page.locator("[data-legacy-tray-conflict]")).toHaveCount(0);
  await expect(summaryInstall).toBeEnabled();
});

for (const fixture of [
  ["trusted-other", "MacTray is running in another user session"],
  ["untrusted", "Check this copy of MacTray"],
  ["unknown", "MacTray tray mode status is unavailable"],
] as const) {
  test(`${fixture[0]} MacTray state remains fail-closed without an exit action`, async ({ page }) => {
    await page.goto(`/?view=execution&gallery=1&lang=en&system-service=migration-available&legacy-tray=${fixture[0]}`, { waitUntil: "networkidle" });

    const conflict = page.locator("[data-legacy-tray-conflict]");
    const summaryInstall = page.locator("[data-service-summary]").getByRole("button", { name: "Install service" });
    await expect(conflict).toContainText(fixture[1]);
    await expect(conflict.getByRole("button", { name: "Exit MacTray" })).toHaveCount(0);
    await expect(conflict.getByRole("button", { name: "Check again" })).toBeEnabled();
    await expect(summaryInstall).toHaveCount(0);
  });
}

for (const fixture of [
  ["hkcu-run", "MacTray autostart must be disabled", "Disable MacTray autostart"],
  ["untrusted", "A MacTray autostart entry could not be trusted", null],
  ["unknown", "MacTray autostart status is unavailable", null],
] as const) {
  test(`${fixture[0]} MacTray autostart state is prominent and fail-closed`, async ({ page }) => {
    await page.goto(`/?view=execution&gallery=1&lang=en&system-service=migration-available&legacy-startup=${fixture[0]}`, { waitUntil: "networkidle" });

    const summary = page.locator("[data-service-summary]");
    const conflict = summary.locator("[data-legacy-tray-conflict]");
    await expect(conflict).toContainText(fixture[1]);
    await expect(conflict.getByRole("button", { name: "Check again" })).toBeEnabled();
    await expect(conflict.getByRole("button", { name: "Exit MacTray" })).toHaveCount(0);
    if (fixture[2]) await expect(conflict.getByRole("button", { name: fixture[2] })).toBeEnabled();
    else await expect(conflict.getByRole("button", { name: "Disable MacTray autostart" })).toHaveCount(0);
    await expect(summary.getByRole("button", { name: "Install service" })).toHaveCount(0);
  });
}

test("a running new service with a legacy tray conflict offers only the verified stop", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=ready&legacy-tray=trusted-current", { waitUntil: "networkidle" });
  const summary = page.locator("[data-service-summary]");
  const conflict = summary.locator("[data-legacy-tray-conflict]");
  await expect(conflict).toContainText("Existing MacTray is running");
  await expect(conflict.getByRole("button", { name: "Check again" })).toBeEnabled();
  await expect(conflict.getByRole("button", { name: "Exit MacTray" })).toBeEnabled();
  await expect(summary.getByRole("button", { name: "Stop" })).toHaveCount(0);
  await openServiceDetails(page);

  const openService = page.locator('[data-service-backend="open-source"]');
  await expect(openService.locator('[data-state="running-legacy-tray-conflict"]')).toBeVisible();
  await expect(openService).toContainText("Service running while MacTray conflicts");
  await expect(openService).not.toContainText("MacType system-wide rendering active");
  await expect(openService.getByRole("button", { name: "Stop applying to new apps" })).toBeEnabled();
  for (const name of ["Install service", "Start service", "Repair service", "Upgrade service", "Remove service"]) {
    const button = openService.locator(".service-actions").getByRole("button", { name });
    if (await button.count()) await expect(button).toBeDisabled();
  }
});

for (const legacyState of ["running", "stopped", "start-pending", "stop-pending", "paused", "unknown", "continue-pending", "pause-pending"] as const) {
  test(`legacy migration requires a stable ${legacyState} service`, async ({ page }, testInfo) => {
    await page.goto(`/?view=execution&gallery=1&lang=en&system-service=migration-available&legacy=migration-available&legacy-state=${legacyState}`, { waitUntil: "networkidle" });
    const summary = page.locator("[data-service-summary]");
    if (legacyState === "running" || legacyState === "stopped") {
      await expect(summary).toContainText("Legacy MacTray was detected.");
      await expect(summary.getByRole("button", { name: "Migrate" })).toBeEnabled();
    } else {
      await expect(summary).toContainText("A legacy MacTray service must be resolved first");
      await expect(summary.getByRole("button")).toHaveCount(0);
    }
    await openServiceDetails(page);

    const migrate = page.locator('[data-service-backend="legacy-mactray"]').getByRole("button", { name: "Migrate" });
    if (legacyState === "running" || legacyState === "stopped") await expect(migrate).toBeEnabled();
    else await expect(migrate).toBeDisabled();
    await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-execution-detail-legacy-${legacyState}-en.png`), fullPage: true });
  });
}

test("a foreign same-name service is prominent without exposing an unsafe action", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=foreign-service", { waitUntil: "networkidle" });

  const summary = page.locator("[data-service-summary]");
  await expect(summary).toContainText("A service with the same name has a different configuration");
  await expect(summary.locator("[data-prominent-exception]")).toHaveAttribute("data-kind", "foreign-service");
  await expect(summary.getByRole("button")).toHaveCount(0);
  await expect(page.getByText("Manage service", { exact: true })).toBeHidden();
});

test("a foreign legacy service and pending removal cannot hide in Details", async ({ page }) => {
  const summary = page.locator("[data-service-summary]");

  await page.goto("/?view=execution&gallery=1&lang=en&system-service=migration-available&legacy=foreign", { waitUntil: "networkidle" });
  await expect(summary).toContainText("A foreign legacy MacTray service was detected");
  await expect(summary.locator("[data-prominent-exception]")).toHaveAttribute("data-kind", "legacy-service-foreign");
  await expect(summary.getByRole("button")).toHaveCount(0);

  await page.goto("/?view=execution&gallery=1&lang=en&system-service=delete-pending", { waitUntil: "networkidle" });
  await expect(summary).toContainText("Removal pending");
  await expect(summary.locator("[data-prominent-exception]")).toHaveAttribute("data-kind", "removal-pending");
  await expect(summary.getByRole("button")).toHaveCount(0);
});

const legacyServiceIdentityCases = [
  {
    id: "owned",
    query: "legacy=migration-available",
    kind: "migration",
    title: "Legacy MacTray was detected.",
    description: "An older MacTray service is installed.",
    detailWarning: null,
  },
  {
    id: "foreign",
    query: "legacy=foreign",
    kind: "legacy-service-foreign",
    title: "A foreign legacy MacTray service was detected",
    description: "A different service is using the MacType name",
    detailWarning: "A different service is using the MacType name",
  },
  {
    id: "uncertain",
    query: "legacy=inaccessible",
    kind: "legacy-service-uncertain",
    title: "Legacy MacTray service status could not be verified",
    description: "The existing service could not be identified",
    detailWarning: "The existing service could not be identified",
  },
] as const;

for (const identity of legacyServiceIdentityCases) {
  test(`legacy service ${identity.id} identity has distinct copy`, async ({ page }, testInfo) => {
    await page.goto(`/?view=execution&gallery=1&lang=en&system-service=migration-available&${identity.query}`, { waitUntil: "networkidle" });

    const summary = page.locator("[data-service-summary]");
    await expect(summary).toContainText(identity.title);
    await expect(summary).toContainText(identity.description);
    await expect(summary.locator("[data-prominent-exception]")).toHaveAttribute("data-kind", identity.kind);
    for (const other of legacyServiceIdentityCases.filter((candidate) => candidate.id !== identity.id)) {
      await expect(summary).not.toContainText(other.title);
    }

    await openServiceDetails(page);
    const openService = page.locator('[data-service-backend="open-source"]');
    await expect(openService).toContainText(identity.id === "owned" ? "A legacy MacTray service must be resolved first" : identity.title);
    await expect(openService).toContainText(identity.description);

    const legacy = page.locator('[data-service-backend="legacy-mactray"]');
    if (identity.detailWarning) await expect(legacy).toContainText(identity.detailWarning);
    else {
      await expect(legacy).not.toContainText("A different service is using the MacType name");
      await expect(legacy).not.toContainText("The existing service could not be identified");
    }
    await page.screenshot({
      path: path.join(galleryRoot, `${testInfo.project.name}-execution-detail-legacy-identity-${identity.id}-en.png`),
      fullPage: true,
    });
  });
}

for (const fixture of [
  "ready",
  "degraded",
  "initializing",
  "unknown-health",
  "failed",
  "outdated",
  "profile-mismatch",
  "legacy-conflict",
  "migration-available",
  "foreign-service",
  "inaccessible-service",
  "delete-pending",
]) {
  test(`open service gallery renders ${fixture} without claiming SCM running is ready`, async ({ page }, testInfo) => {
    await page.goto(`/?view=execution&gallery=1&lang=en&system-service=${fixture}`, { waitUntil: "networkidle" });
    await openServiceDetails(page);
    const openService = page.locator('[data-service-backend="open-source"]');
    await expect(openService).toBeVisible();
    if (fixture !== "ready" && fixture !== "legacy-conflict") {
      await expect(page.getByText("MacType system-wide rendering active", { exact: true })).toHaveCount(0);
    }
    if (fixture === "legacy-conflict") {
      const legacy = page.locator('[data-service-backend="legacy-mactray"]');
      await expect(legacy).toContainText("AppInit registry mode is active");
    }
    expect(await overflowingElements(page)).toEqual([]);
    await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-execution-${fixture}-en.png`), fullPage: true });
  });
}

for (const unstableState of ["start-pending", "stop-pending", "paused", "unknown"]) {
  test(`new service mutations stay disabled while ${unstableState}`, async ({ page }, testInfo) => {
    await page.goto(`/?view=execution&gallery=1&lang=en&system-service=ready&service-runtime=${unstableState}`, { waitUntil: "networkidle" });
    await openServiceDetails(page);

    const mutationButtons = page.locator('[data-service-backend="open-source"] .system-injection-action, [data-service-backend="open-source"] .service-actions button');
    await expect(mutationButtons).not.toHaveCount(0);
    for (const button of await mutationButtons.all()) await expect(button).toBeDisabled();
    await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-execution-detail-runtime-${unstableState}-en.png`), fullPage: true });
  });
}

for (const fixture of [
  { state: "not-installed", message: "The service package is unavailable" },
  { state: "incomplete", message: "The service package is incomplete" },
  { state: "untrusted", message: "The service package failed verification" },
] as const) {
  test(`service management stays read-only when its service package is ${fixture.state}`, async ({ page }, testInfo) => {
    await page.goto(`/?view=execution&gallery=1&lang=en&system-service=ready&service-package=${fixture.state}`, { waitUntil: "networkidle" });
    await openServiceDetails(page);

    const openService = page.locator('[data-service-backend="open-source"]');
    const notice = openService.locator(`[data-service-package="${fixture.state}"]`);
    await expect(notice).toBeVisible();
    await expect(notice).toContainText(fixture.message);
    const mutationButtons = openService.locator(".system-injection-action, .service-actions button");
    await expect(mutationButtons).not.toHaveCount(0);
    for (const button of await mutationButtons.all()) await expect(button).toBeDisabled();
    expect(await overflowingElements(page)).toEqual([]);
    await page.screenshot({
      path: path.join(galleryRoot, `${testInfo.project.name}-execution-service-package-${fixture.state}-en.png`),
      fullPage: true,
    });
  });
}

test("a stopped new service with no alternative offers Start and Remove", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=ready&service-runtime=stopped", { waitUntil: "networkidle" });
  const summary = page.locator("[data-service-summary]");
  await expect(summary).toContainText("Stopped");
  await expect(summary.getByRole("button", { name: "Start service" })).toBeEnabled();
  await expect(summary.getByRole("button", { name: "Remove service" })).toBeEnabled();
  await expect(summary.getByRole("button")).toHaveCount(2);
  await expect(summary.locator(".success")).toHaveCount(0);
  await expect(summary.locator(".warning")).toHaveCount(0);
  await expect(summary.locator(".neutral-status")).toHaveCount(1);
});

test("a deliberately stopped service reports neutrally instead of claiming degradation", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=ready&service-runtime=stopped", { waitUntil: "networkidle" });
  await openServiceDetails(page);

  const openService = page.locator('[data-service-backend="open-source"]');
  const statusRow = openService.locator(".detail-list > div").filter({ hasText: "Service status" });
  await expect(statusRow).toContainText("Current installation · Stopped");
  await expect(statusRow).not.toContainText("Not checked");
  await expect(statusRow.locator(".neutral-status")).toBeVisible();
  await expect(statusRow.locator(".warning")).toHaveCount(0);

  const profileRow = openService.locator(".detail-list > div").filter({ hasText: "Profile application status" });
  await expect(profileRow).toContainText("Service is off");
  await expect(profileRow).not.toContainText("mismatch");
  await expect(profileRow.locator(".neutral-status")).toBeVisible();
  await expect(profileRow.locator(".warning")).toHaveCount(0);
});

test("a stopped service surfaces a persisted degradation as a last-run record", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=degraded&service-runtime=stopped", { waitUntil: "networkidle" });
  const summary = page.locator("[data-service-summary]");
  await expect(summary).toContainText("Stopped");
  await expect(summary.locator(".neutral-status")).toHaveCount(1);
  await openServiceDetails(page);

  const openService = page.locator('[data-service-backend="open-source"]');
  const statusRow = openService.locator(".detail-list > div").filter({ hasText: "Service status" });
  await expect(statusRow).toContainText("Degraded during the last run");
  const profileRow = openService.locator(".detail-list > div").filter({ hasText: "Profile application status" });
  await expect(profileRow).toContainText("Service is off");
});

test("starting with no applied profile applies the bundled default and says so", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=ready&service-runtime=stopped&profile-unapplied=1", { waitUntil: "networkidle" });
  const summary = page.locator("[data-service-summary]");
  await expect(summary).toContainText("No run profile");

  await openServiceDetails(page);
  await expect(page.locator(".system-injection-control")).toContainText("Starting uses the bundled default profile (Default.ini)");

  await summary.getByRole("button", { name: "Start service" }).click();
  await expect(page.locator(".success-message")).toContainText("the service started with the default profile (Default.ini)");
  await expect(summary).toContainText("Default.ini");
});

test("the primary service action disables immediately while its mutation is busy", async ({ page }) => {
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=ready&service-runtime=stopped&service-delay=750", { waitUntil: "networkidle" });

  const summary = page.locator("[data-service-summary]");
  const start = summary.getByRole("button", { name: "Start service" });
  await expect(start).toBeEnabled();
  await start.click();
  await expect(summary.getByRole("button", { name: "Working…" })).toBeDisabled();
  await expect(summary.getByRole("button", { name: "Stop" })).toBeEnabled();
});

test("service migration gallery remains usable at low window height", async ({ page }, testInfo) => {
  test.skip(testInfo.project.name !== "desktop-1280", "Low-height desktop behavior is width-specific");
  await page.setViewportSize({ width: 1280, height: 420 });
  await page.goto("/?view=execution&gallery=1&lang=en&system-service=migration-available&legacy=migration-available", { waitUntil: "networkidle" });
  await openServiceDetails(page);

  expect(await page.evaluate(() => document.documentElement.scrollHeight > document.documentElement.clientHeight)).toBe(true);
  expect(await overflowingElements(page)).toEqual([]);
  const legacy = page.locator('[data-service-backend="legacy-mactray"]');
  await legacy.scrollIntoViewIfNeeded();
  await expect(legacy.getByRole("button", { name: "Migrate" })).toBeEnabled();
  await page.screenshot({ path: path.join(galleryRoot, "desktop-execution-migration-low-height-en.png"), fullPage: true });
});

test("overview summarizes the active service and discloses at most five successful activities", async ({ page }) => {
  await page.goto("/?view=overview&gallery=1&lang=ko&system-service=ready", { waitUntil: "networkidle" });
  await expect(page.getByRole("heading", { name: "MacType가 실행 중입니다" })).toBeVisible();
  const summary = page.locator("[data-overview-service]");
  await expect(summary).toContainText("ini\\Default.ini");
  await expect(summary).toContainText("Control Center 서비스");
  await expect(summary).toContainText("정상");
  await expect(summary.getByRole("button", { name: "서비스" })).toHaveCount(0);
  await expect(page.getByRole("heading", { name: "설치 구성" })).toHaveCount(0);
  await expect(page.getByRole("heading", { name: "다음 작업" })).toHaveCount(0);

  const activity = page.locator("[data-recent-activity]");
  await expect(activity).toContainText("프로필 Default.ini 적용을 마쳤습니다.");
  await expect(activity.locator("ol")).toHaveCount(0);
  await activity.getByRole("button", { name: "펼치기" }).click();
  await expect(activity.locator("ol li")).toHaveCount(5);
  await expect(activity).not.toContainText("migrate-from-legacy");
  await activity.getByRole("button", { name: "접기" }).click();
  await expect(activity.locator("ol")).toHaveCount(0);
});

test("overview offers a Service shortcut only when the service needs attention", async ({ page }) => {
  await page.goto("/?view=overview&gallery=1&lang=en&system-service=ready&service-runtime=stopped", { waitUntil: "networkidle" });
  await expect(page.locator("[data-overview-service]").getByRole("button", { name: "Service" })).toBeVisible();
  await page.goto("/?view=overview&gallery=1&lang=en&system-service=failed", { waitUntil: "networkidle" });
  await expect(page.locator("[data-overview-service]").getByRole("button", { name: "Service" })).toBeVisible();
});

test("diagnostics owns installation controls and always shows the localized event timeline", async ({ page }) => {
  await page.goto("/?view=diagnostics&gallery=1&lang=ko", { waitUntil: "networkidle" });
  await expect(page.getByRole("heading", { name: "설치 구성" })).toBeVisible();
  await page.getByRole("button", { name: "설치 위치 다시 찾기" }).click();
  await expect(page.locator('[data-operation="relocate"]')).toBeVisible();
  await page.getByRole("button", { name: "다시 연결" }).click();
  await expect(page.locator('[data-operation="reconnect"]')).toBeVisible();
  await expect(page.getByRole("log")).toBeVisible();
  await expect(page.getByRole("log")).toContainText("프로필 Default.ini 적용을 마쳤습니다.");
  await expect(page.getByRole("log")).not.toContainText("operation=migrate-from-legacy");
  const actions = page.locator("[data-log-disclosure-actions]");
  await expect(actions.getByRole("button")).toHaveCount(1);
  await expect(actions.getByRole("button", { name: "로그 폴더 열기" })).toBeVisible();

  await page.getByRole("button", { name: "진단 파일 내보내기" }).click();
  await expect(page.locator('[data-operation="export"]')).toContainText("diagnostics-gallery.txt");
  await page.getByRole("button", { name: "진단 정보 복사" }).click();
  await expect(page.locator('[data-operation="copy"]')).toBeVisible();
  await page.getByRole("button", { name: "로그 폴더 열기" }).click();
  await expect(page.locator('[data-operation="folder"]')).toContainText("ControlCenter");
});

test("event view options hide summaries, persist, and collapse repeated failures", async ({ page }) => {
  await page.goto("/?view=diagnostics&gallery=1&lang=ko", { waitUntil: "networkidle" });
  const summaries = page.locator('.event-row[data-code="injection-summary"]');
  await expect(summaries).toHaveCount(4);
  await expect(page.getByTestId("event-timeline")).toContainText("최근 1분 동안");
  const hideSummaries = page.getByRole("switch", { name: "적용 요약 숨기기" });
  for (const option of ["hideInjectionSummary", "collapseRepeatedFailures", "hideRoutine"]) {
    await expect(page.locator(`.event-view-option[data-option="${option}"] .switch-control > span`)).toBeVisible();
  }
  await page.locator('.event-view-option[data-option="hideInjectionSummary"] .switch-control > span').click();
  await expect(summaries).toHaveCount(0);
  await page.reload({ waitUntil: "networkidle" });
  await expect(hideSummaries).toBeChecked();
  await expect(summaries).toHaveCount(0);
  await hideSummaries.uncheck();
  await expect(summaries).toHaveCount(4);
  const repeated = page.locator('.event-row[data-code="injection-failed"]').filter({ hasText: "vgtray.exe" });
  await expect(repeated).toHaveCount(3);
  const collapse = page.getByRole("switch", { name: "반복된 적용 실패 접기" });
  await collapse.check();
  await expect(repeated).toHaveCount(1);
  await expect(repeated.locator(".event-repeat")).toHaveText("3회 반복");
  await expect(page.locator('.event-row[data-code="injection-failed"]').filter({ hasText: "firefox.exe" })).toHaveCount(1);
  await collapse.uncheck();
  await expect(repeated).toHaveCount(3);
  const routine = page.locator('.event-row[data-code="app-started"], .event-row[data-code="preview-helper-connected"], .event-row[data-code="profile-verified"]');
  await expect(routine).toHaveCount(3);
  await page.getByRole("switch", { name: "앱 실행·미리보기 기록 숨기기" }).check();
  await expect(routine).toHaveCount(0);
  await page.evaluate(() => localStorage.removeItem("mactype-control-center.event-view"));
});

test("event log sources omit absent files but report existing unreadable files", async ({ page }) => {
  await page.goto("/?view=diagnostics&gallery=1&lang=ko&events-absent=1", { waitUntil: "networkidle" });
  await page.locator("details.event-source-disclosure > summary").click();
  await expect(page.locator(".event-sources > div")).toHaveCount(2);
  await expect(page.locator(".event-sources")).not.toContainText("서비스 설치");
  await page.goto("/?view=diagnostics&gallery=1&lang=ko", { waitUntil: "networkidle" });
  await page.locator("details.event-source-disclosure > summary").click();
  await expect(page.locator(".event-sources > div")).toHaveCount(3);
  await page.goto("/?view=diagnostics&gallery=1&lang=ko&events-unreadable=1", { waitUntil: "networkidle" });
  await page.locator("details.event-source-disclosure > summary").click();
  await expect(page.locator(".event-sources > div").nth(2)).toContainText("읽을 수 없음");
});

test("diagnostics event titles localize activation reasons, broker failures, and panics", async ({ page }) => {
  await page.goto("/?view=diagnostics&gallery=1&lang=ko", { waitUntil: "networkidle" });
  const timeline = page.getByTestId("event-timeline");
  await expect(timeline).toContainText("firefox.exe에 적용하지 못했습니다 (모듈을 불러오지 못함).");
  await expect(timeline).toContainText("x64 앱에 MacType을 적용하지 못했습니다(렌더러 확인 스레드 실패).");
  await expect(timeline).toContainText("Control Center가 예기치 않게 중단되었습니다");
  await expect(timeline).not.toContainText("module-load-failed");
  await expect(timeline).not.toContainText("renderer-evidence-thread-failed");
});

test("language setting switches every supported locale and persists", async ({ page }, testInfo) => {
  await page.goto("/?view=overview&gallery=1&lang=ko", { waitUntil: "networkidle" });
  for (const locale of galleryLocales) {
    await page.getByTestId("language-picker-trigger").click();
    await page.locator(`[data-locale-option="${locale.id}"]`).click();
    await expect(page.locator("html")).toHaveAttribute("lang", locale.id);
    await expect(page.locator("html")).toHaveAttribute("dir", locale.direction);
    await expect(page.getByRole("heading", { level: 1, name: galleryViews[0].title[locale.id] })).toBeVisible();
  }

  await page.getByTestId("language-picker-trigger").click();
  await page.locator('[data-locale-option="en"]').click();
  await expect(page.getByRole("button", { name: "Dark theme" })).toBeVisible();

  await page.goto("/?view=overview&gallery=1", { waitUntil: "networkidle" });
  await expect(page.getByTestId("language-picker-trigger")).toHaveText("English");
  await expect(page.getByRole("heading", { level: 1, name: "Overview" })).toBeVisible();
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-language-en.png`), fullPage: true });
});

test("sidebar preferences stay at the bottom and yield to scrolling when height is tight", async ({ page }, testInfo) => {
  test.skip(testInfo.project.name !== "desktop-1280", "Desktop sidebar behavior is width-specific");
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto("/?view=overview&gallery=1&lang=ko", { waitUntil: "networkidle" });
  const sidebar = page.locator(".navigation");
  const preferences = page.locator(".navigation-preferences");

  const roomyGap = await page.evaluate(() => {
    const sidebarRect = document.querySelector<HTMLElement>(".navigation")!.getBoundingClientRect();
    const preferencesRect = document.querySelector<HTMLElement>(".navigation-preferences")!.getBoundingClientRect();
    return sidebarRect.bottom - preferencesRect.bottom;
  });
  expect(roomyGap).toBeCloseTo(16, 0);
  await page.screenshot({ path: path.join(galleryRoot, "desktop-sidebar-preferences-roomy.png"), fullPage: true });

  await page.setViewportSize({ width: 1280, height: 300 });
  const tightMetrics = await sidebar.evaluate((element) => {
    element.scrollTop = 0;
    const preferencesRect = element.querySelector<HTMLElement>(".navigation-preferences")!.getBoundingClientRect();
    return {
      overflows: element.scrollHeight > element.clientHeight,
      preferencesBelowFold: preferencesRect.bottom > element.getBoundingClientRect().bottom,
    };
  });
  expect(tightMetrics).toEqual({ overflows: true, preferencesBelowFold: true });

  await sidebar.evaluate((element) => element.scrollTo({ top: element.scrollHeight }));
  await expect.poll(async () => {
    const sidebarBox = await sidebar.boundingBox();
    const preferencesBox = await preferences.boundingBox();
    return Math.round((sidebarBox?.y ?? 0) + (sidebarBox?.height ?? 0) - ((preferencesBox?.y ?? 0) + (preferencesBox?.height ?? 0)));
  }).toBe(16);
  await page.screenshot({ path: path.join(galleryRoot, "desktop-sidebar-preferences-tight.png") });
  await page.getByRole("button", { name: "어두운 테마" }).click();
  await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
  await page.getByTestId("language-picker-trigger").click();
  await page.locator('[data-locale-option="en"]').click();
  await expect(page.getByRole("heading", { level: 1, name: "Overview" })).toBeVisible();
});

test("dark language menu and custom titlebar follow the application theme", async ({ page }, testInfo) => {
  await page.goto("/?view=overview&gallery=1&lang=ko", { waitUntil: "networkidle" });
  await expect(page.getByRole("button", { name: "창 최소화" })).toBeVisible();
  await expect(page.getByRole("button", { name: "창 최대화 또는 복원" })).toBeVisible();
  await expect(page.getByRole("button", { name: "창 닫기" })).toBeVisible();

  await page.getByRole("button", { name: "어두운 테마" }).click();
  await page.getByTestId("language-picker-trigger").click();
  const menu = page.getByRole("listbox", { name: "표시 언어" });
  await expect(menu).toBeVisible();
  const themeColors = await page.evaluate(() => ({
    menu: getComputedStyle(document.querySelector<HTMLElement>(".language-menu")!).backgroundColor,
    titlebar: getComputedStyle(document.querySelector<HTMLElement>(".window-titlebar")!).backgroundColor,
  }));
  expect(themeColors).toEqual({ menu: "rgb(25, 32, 39)", titlebar: "rgb(25, 32, 39)" });
  expect(await menu.evaluate((element) => element.scrollHeight > element.clientHeight)).toBe(true);
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-dark-language-titlebar.png`), fullPage: true });
});

for (const failure of ["access", "read", "write"] as const) {
  test(`preferences remain usable when localStorage ${failure} throws`, async ({ page }) => {
    const failures: string[] = [];
    page.on("pageerror", (error) => failures.push(error.message));
    await page.addInitScript((failure) => {
      Object.defineProperty(navigator, "language", { get: () => "en-US" });
      const unavailable = () => { throw new DOMException("Storage unavailable", "SecurityError"); };
      if (failure === "access") {
        Object.defineProperty(window, "localStorage", { get: unavailable });
      } else {
        Object.defineProperty(Storage.prototype, failure === "read" ? "getItem" : "setItem", { value: unavailable });
      }
    }, failure);

    await page.goto("/?view=overview&gallery=1", { waitUntil: "networkidle" });
    await expect(page.locator("body")).toHaveAttribute("data-rendered", "true");
    await expect(page.locator("html")).toHaveAttribute("lang", "en");
    await expect(page.locator("html")).toHaveAttribute("data-theme", "light");
    await expect(page.getByRole("heading", { level: 1, name: "Overview" })).toBeVisible();

    await page.getByRole("button", { name: "Dark theme" }).click();
    await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
    await page.getByTestId("language-picker-trigger").click();
    await page.locator('[data-locale-option="ko"]').click();
    await expect(page.locator("html")).toHaveAttribute("lang", "ko");

    await page.goto("/?view=files&gallery=1&fresh=1&lang=fr&theme=dark", { waitUntil: "networkidle" });
    await expect(page.locator("html")).toHaveAttribute("lang", "fr");
    await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
    await expect(page.locator('.profile-card[data-selected="true"] .profile-card-title strong')).toHaveText("Default");

    await page.goto("/?view=diagnostics&gallery=1&lang=en&theme=dark", { waitUntil: "networkidle" });
    const summaries = page.locator('.event-row[data-code="injection-summary"]');
    const hideSummaries = page.locator('.event-view-option[data-option="hideInjectionSummary"]').getByRole("switch");
    await expect(hideSummaries).not.toBeChecked();
    await expect(summaries).toHaveCount(4);
    await hideSummaries.check();
    await expect(summaries).toHaveCount(0);
    expect(failures).toEqual([]);
  });
}

test("preference query overrides persist and invalid values use stored or default choices", async ({ page }) => {
  await page.addInitScript(() => {
    Object.defineProperty(navigator, "language", { get: () => "zh-HK" });
  });
  await page.goto("/?view=overview&gallery=1&lang=fr&theme=dark", { waitUntil: "networkidle" });
  await expect(page.locator("html")).toHaveAttribute("lang", "fr");
  await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
  expect(await page.evaluate(() => ({
    locale: localStorage.getItem("mactype-control-center.locale"),
    theme: localStorage.getItem("mactype-control-center.theme"),
  }))).toEqual({ locale: "fr", theme: "dark" });

  await page.goto("/?view=overview&gallery=1&lang=invalid&theme=invalid", { waitUntil: "networkidle" });
  await expect(page.locator("html")).toHaveAttribute("lang", "fr");
  await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");

  await page.goto("/?view=overview&gallery=1&lang=de&theme=light", { waitUntil: "networkidle" });
  await expect(page.locator("html")).toHaveAttribute("lang", "de");
  await expect(page.locator("html")).toHaveAttribute("data-theme", "light");
  await page.goto("/?view=overview&gallery=1", { waitUntil: "networkidle" });
  await expect(page.locator("html")).toHaveAttribute("lang", "de");
  await expect(page.locator("html")).toHaveAttribute("data-theme", "light");

  await page.evaluate(() => {
    localStorage.setItem("mactype-control-center.locale", "invalid");
    localStorage.setItem("mactype-control-center.theme", "invalid");
    localStorage.setItem("mactype-control-center.event-view", "{invalid");
  });
  await page.goto("/?view=diagnostics&gallery=1&lang=invalid&theme=invalid", { waitUntil: "networkidle" });
  await expect(page.locator("html")).toHaveAttribute("lang", "zh-TW");
  await expect(page.locator("html")).toHaveAttribute("data-theme", "light");
  await expect(page.locator('.event-view-option input:checked')).toHaveCount(0);
  await expect(page.locator('.event-row[data-code="injection-summary"]')).toHaveCount(4);
});

test("theme setting persists across launches", async ({ page }) => {
  await page.goto("/?view=overview&gallery=1&lang=ko", { waitUntil: "networkidle" });
  await page.getByRole("button", { name: "어두운 테마" }).click();
  await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");

  await page.reload({ waitUntil: "networkidle" });
  await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
  await expect(page.getByRole("button", { name: "밝은 테마" })).toBeVisible();

  await page.getByRole("button", { name: "밝은 테마" }).click();
  await page.reload({ waitUntil: "networkidle" });
  await expect(page.locator("html")).toHaveAttribute("data-theme", "light");
  await expect(page.getByRole("button", { name: "어두운 테마" })).toBeVisible();
});

test("settings files prefer the most recently worked profile", async ({ page }) => {
  const recent = "C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\profiles\\Recent.ini";
  await page.addInitScript(({ key, value }) => window.localStorage.setItem(key, value), {
    key: "mactype-control-center.recent-profile",
    value: recent,
  });
  await page.goto("/?view=files&gallery=1&lang=ko&fresh=1", { waitUntil: "networkidle" });
  await expect(page.locator('.profile-card[data-selected="true"] .profile-card-title strong')).toHaveText("Recent");
  await expect(page.locator(".selected-file-summary code")).toHaveText("Profiles\\Recent.ini");
});

test("settings files fall back to the applied profile", async ({ page }) => {
  await page.goto("/?view=files&gallery=1&lang=ko&fresh=1", { waitUntil: "networkidle" });
  await expect(page.locator('.profile-card[data-selected="true"] .profile-card-title strong')).toHaveText("Default");
  await expect(page.locator(".selected-file-summary code")).toHaveText("ini\\Default.ini");
});

test("settings files do not claim the already applied legacy profile is different", async ({ page }) => {
  await page.goto("/?view=files&gallery=1&lang=ko&legacy-applied=1", { waitUntil: "networkidle" });
  await expect(page.locator(".legacy-import-banner")).toHaveCount(0);
});

test("settings files present profile cards with thumbnails, apply ownership, and a tuner hand-off", async ({ page }, testInfo) => {
  await page.goto("/?view=files&gallery=1&lang=ko", { waitUntil: "networkidle" });

  const cards = page.locator(".profile-card");
  await expect(cards).toHaveCount(3);
  await expect(page.locator(".profile-card-thumb img")).toHaveCount(3);

  const appliedCard = page.locator('.profile-card[data-applied="true"]');
  await expect(appliedCard).toHaveCount(1);
  await expect(appliedCard.locator(".profile-card-title strong")).toHaveText("Default");
  await expect(appliedCard.locator(".profile-card-badge")).toHaveText("실행 프로필");

  const pretendardCard = cards.filter({ hasText: "Pretendard forever" });
  await pretendardCard.locator(".profile-card-select").click();
  await expect(pretendardCard).toHaveAttribute("data-selected", "true");
  await expect(page.locator(".selected-file-summary code")).toHaveText("ini\\pretendard forever.ini");

  const details = page.locator("details.file-details");
  await expect(details.locator("summary")).toContainText("파일 상세");
  await expect(details.locator("summary")).toContainText("UTF-8 · CRLF");
  await expect(details.locator(".detail-list")).toBeHidden();
  await details.locator("summary").click();
  await expect(details.locator(".detail-list")).toBeVisible();
  await expect(details).toContainText("문자 인코딩");

  await page.getByRole("button", { name: "실행 프로필로 지정", exact: true }).click();
  await expect(page.locator('[data-operation="file-settings"]')).toContainText("실행 프로필로 지정했습니다");
  await expect(appliedCard).toHaveCount(1);
  await expect(appliedCard.locator(".profile-card-title strong")).toHaveText("Pretendard forever");

  expect(await overflowingElements(page)).toEqual([]);
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-profile-card-selector-ko.png`), fullPage: true });

  const editInTuner = page.getByRole("button", { name: "튜너에서 편집" });
  await expect(editInTuner).toHaveCount(3);
  await editInTuner.first().click();
  await expect(page.locator("body")).toHaveAttribute("data-view", "profiles");
  await expect(page.locator("body")).toHaveAttribute("data-profile-mode", "all");
});

