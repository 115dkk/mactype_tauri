import { expect, test, type Page } from "@playwright/test";
import path from "node:path";
import { gallerySkins } from "./windows";

const galleryRoot = path.resolve(__dirname, "../../artifacts/frontend-gallery");

async function startPaletteAudit(page: Page, selector: string) {
  await page.evaluate((selector) => {
    const audit = { running: true, frames: 0, failures: [] as string[] };
    (window as unknown as { paletteAudit: typeof audit }).paletteAudit = audit;
    const sample = () => {
      if (!audit.running) return;
      for (const board of document.querySelectorAll<HTMLElement>(selector)) {
        const background = getComputedStyle(board).backgroundColor;
        for (const image of board.querySelectorAll<HTMLImageElement>("img")) {
          if (!image.complete || !image.currentSrc.startsWith("data:image/svg+xml")) continue;
          const svg = new DOMParser().parseFromString(decodeURIComponent(image.currentSrc.split(",").slice(1).join(",")), "image/svg+xml");
          const hex = svg.querySelector("rect")?.getAttribute("fill");
          if (!hex || !/^#[0-9a-f]{6}$/i.test(hex)) continue;
          const rgb = `rgb(${[1, 3, 5].map((offset) => parseInt(hex.slice(offset, offset + 2), 16)).join(", ")})`;
          if (rgb !== background) audit.failures.push(`${background} canvas / ${rgb} bitmap`);
        }
      }
      audit.frames++;
      requestAnimationFrame(sample);
    };
    requestAnimationFrame(sample);
  }, selector);
}

async function finishPaletteAudit(page: Page) {
  const audit = await page.evaluate(() => {
    const audit = (window as unknown as { paletteAudit: { running: boolean; frames: number; failures: string[] } }).paletteAudit;
    audit.running = false;
    return audit;
  });
  expect(audit.frames).toBeGreaterThan(0);
  expect(audit.failures).toEqual([]);
}

for (const skin of gallerySkins) {
  test(`${skin} switch thumb stays inside its track in both directions and all states`, async ({ page }, testInfo) => {
    await page.goto(`/?window=preview-studio&gallery=1&lang=en&skin=${skin}`);
    const input = page.getByRole("switch").first();
    await expect(input).toBeVisible();
    for (const direction of ["ltr", "rtl"]) {
      await page.locator("html").evaluate((element, direction) => { element.dir = direction; }, direction);
      for (const checked of [false, true]) {
        await input.setChecked(checked);
        for (const disabled of [false, true]) {
          await input.evaluate((element, disabled) => { (element as HTMLInputElement).disabled = disabled; }, disabled);
          await input.hover({ force: true });
          const geometry = await input.evaluate((element) => {
            const track = getComputedStyle(element);
            const thumb = getComputedStyle(element, "::before");
            const border = parseFloat(track.borderLeftWidth);
            const matrix = thumb.transform === "none" ? new DOMMatrix() : new DOMMatrix(thumb.transform);
            return {
              x: parseFloat(thumb.left) + matrix.m41,
              y: parseFloat(thumb.top) + matrix.m42,
              width: parseFloat(thumb.width), height: parseFloat(thumb.height),
              roomX: element.clientWidth, roomY: element.clientHeight, border,
            };
          });
          expect(geometry.x, `${skin}/${direction}/${checked}/${disabled}`).toBeGreaterThanOrEqual(0);
          expect(geometry.y).toBeGreaterThanOrEqual(0);
          expect(geometry.x + geometry.width).toBeLessThanOrEqual(geometry.roomX);
          expect(geometry.y + geometry.height).toBeLessThanOrEqual(geometry.roomY);
          await input.evaluate((element) => { (element as HTMLInputElement).disabled = false; });
        }
      }
    }
    await input.focus();
    await expect(input).toBeFocused();
    const checked = await input.isChecked();
    await page.keyboard.press("Space");
    await expect(input).toBeChecked({ checked: !checked });
    await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-${skin}-control-geometry.png`) });
  });

  test(`${skin} tuner keeps bitmap and canvas palettes together during inversion and theme changes`, async ({ page }, testInfo) => {
    await page.goto(`/?view=profiles&gallery=1&lang=ko&skin=${skin}&theme=light&preview-delay=35`);
    const canvas = page.locator(".preview-canvas");
    await expect(canvas.locator("img").first()).toBeVisible();
    await startPaletteAudit(page, ".preview-canvas");
    await page.getByTestId("preview-invert").click();
    await expect(canvas).toHaveAttribute("data-dark", "true");
    await page.locator(".theme-toggle").click();
    await expect(canvas).toHaveAttribute("data-dark", "false");
    await finishPaletteAudit(page);
    await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-${skin}-preview-palette.png`), fullPage: true });
    const ranges = page.locator('input[type="range"]:visible');
    if (await ranges.count()) {
      const slider = ranges.first();
      const value = await slider.inputValue();
      await slider.focus();
      await page.keyboard.press("Home");
      await page.keyboard.press("ArrowRight");
      expect(Number(await slider.inputValue())).toBeGreaterThanOrEqual(Number(await slider.getAttribute("min")));
      expect(value).not.toBe("");
    }
  });
}

test("Console uses the localized replacement without opening that profile in the editor", async ({ page }) => {
  const query = new URLSearchParams({ view: "overview", gallery: "1", lang: "ko", skin: "console", "system-service": "ready", "preview-substitutes": '"맑은 고딕"="Pretendard"' });
  await page.goto(`/?${query}`);
  await expect(page.getByRole("radio", { name: "Pretendard", exact: true })).toHaveAttribute("aria-checked", "true");
  const image = page.locator(".console-specimen-panel img").first();
  await expect(image).toHaveAttribute("src", /Pretendard/);
  await page.getByRole("radio", { name: "Segoe UI", exact: true }).click();
  await expect(image).toHaveAttribute("src", /Segoe%20UI/);
  await page.goto(`/?${query}&preview-substitution-mode=0`);
  await expect(page.getByRole("radio", { name: "Malgun Gothic", exact: true })).toHaveAttribute("aria-checked", "true");
  await expect(page.getByRole("radio", { name: "Pretendard", exact: true })).toHaveCount(0);
});

test("Console large specimens fit their bitmap and remain scrollable at high DPI", async ({ browser }, testInfo) => {
  test.skip(testInfo.project.name !== "desktop-1280", "Explicit DPI and native minimum window bounds");
  const context = await browser.newContext({ viewport: { width: 880, height: 560 }, deviceScaleFactor: 2 });
  const page = await context.newPage();
  await page.goto("http://127.0.0.1:4173/?view=overview&gallery=1&lang=ko&skin=console&system-service=ready");
  const board = page.locator(".console-specimen-panel .specimen-board");
  await expect(board.locator("img")).toHaveCount(6);
  const clips = await board.locator("img").evaluateAll((images) => images.flatMap((element) => {
    const image = element as HTMLImageElement;
    const svg = new DOMParser().parseFromString(decodeURIComponent(image.src.split(",").slice(1).join(",")), "image/svg+xml");
    const height = Number(svg.documentElement.getAttribute("height"));
    return [...svg.querySelectorAll("text")].flatMap((text) => Number(text.getAttribute("y")) + Number(text.getAttribute("font-size")) * 0.35 > height ? [text.textContent] : []);
  }));
  expect(clips).toEqual([]);
  await board.locator("img").last().scrollIntoViewIfNeeded();
  await expect(board.locator("img").last()).toBeInViewport();
  await page.screenshot({ path: path.join(galleryRoot, "console-specimen-192dpi.png"), fullPage: true });
  await context.close();
});

test("Console and Studio publish decoded strips with their matching background", async ({ page }) => {
  await page.goto("/?view=overview&gallery=1&lang=ko&skin=console&system-service=ready&theme=light&preview-delay=35");
  const board = page.locator(".console-specimen-panel .specimen-board");
  await expect(board.locator("img")).toHaveCount(6);
  await startPaletteAudit(page, ".specimen-board");
  await page.getByRole("button", { name: "색 반전", exact: true }).click();
  await expect(board).toHaveAttribute("data-dark", "true");
  await page.locator(".theme-toggle").click();
  await expect(board).toHaveAttribute("data-dark", "false");
  await finishPaletteAudit(page);
  await page.goto("/?window=preview-studio&gallery=1&lang=ko&skin=fluent&theme=light&preview-delay=35");
  await expect(page.locator(".studio-board img").first()).toBeVisible();
  await startPaletteAudit(page, ".studio-board");
  await page.getByRole("button", { name: "색 반전", exact: true }).click();
  await expect(page.locator(".studio-board").nth(1)).toHaveCSS("background-color", "rgb(23, 26, 31)");
  await finishPaletteAudit(page);
});

test("Studio open failure is visible and allows retry and dismissal", async ({ page }) => {
  await page.goto("/?view=overview&gallery=1&lang=ko&skin=console&studio-open-error=1");
  await page.getByRole("button", { name: "프리뷰 창 열기", exact: true }).click();
  const error = page.getByRole("alert");
  await expect(error).toContainText("Preview Studio could not open");
  await error.getByRole("button").first().click();
  await expect(error).toBeVisible();
  await error.getByRole("button").last().click();
  await expect(error).toHaveCount(0);
});
