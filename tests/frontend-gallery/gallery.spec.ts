import { expect, test } from "@playwright/test";
import path from "node:path";
import { galleryLocales, galleryViews } from "./windows";

const galleryRoot = path.resolve(__dirname, "../../artifacts/frontend-gallery");

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

test("profile editor categories and collections remain interactive", async ({ page }) => {
  const failures: string[] = [];
  page.on("console", (message) => {
    if (message.type() === "error") failures.push(`console: ${message.text()}`);
  });
  page.on("pageerror", (error) => failures.push(`pageerror: ${error.message}`));

  await page.goto("/?view=profiles&gallery=1&lang=ko", { waitUntil: "networkidle" });
  await page.getByRole("button", { name: "LCD·픽셀 배열" }).click();
  await expect(page.getByRole("heading", { name: "LCD·픽셀 배열" })).toBeVisible();
  await page.getByRole("checkbox", { name: "고급 설정 표시" }).check();
  await expect(page.getByText("빨강 채널 튜닝", { exact: true })).toBeVisible();

  await page.getByRole("button", { name: "글꼴별 설정" }).click();
  await page.getByRole("textbox", { name: "추가할 글꼴 이름" }).fill("Test Font");
  await page.getByRole("button", { name: "글꼴 추가" }).click();
  await expect(page.getByText("Test Font", { exact: true })).toBeVisible();

  await page.getByRole("button", { name: "포함·제외" }).click();
  await expect(page.getByText("제외 프로그램", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "어두운 배경" }).click();
  await expect(page.getByRole("img", { name: "현재 설정의 글자 렌더링 프리뷰" })).toHaveAttribute("data-dark", "true");

  const horizontalOverflow = await page.evaluate(() => document.documentElement.scrollWidth > document.documentElement.clientWidth);
  expect(horizontalOverflow, "interactive profile editor must not have horizontal scrolling").toBe(false);
  expect(failures, failures.join("\n")).toEqual([]);
});

test("execution mode controls remain interactive without enabling system modes", async ({ page }) => {
  const failures: string[] = [];
  page.on("console", (message) => {
    if (message.type() === "error") failures.push(`console: ${message.text()}`);
  });
  page.on("pageerror", (error) => failures.push(`pageerror: ${error.message}`));

  await page.goto("/?view=execution&gallery=1&lang=ko", { waitUntil: "networkidle" });
  const autostart = page.getByRole("checkbox");
  await autostart.check();
  await expect(page.getByText("로그인할 때 트레이로 시작합니다.")).toBeVisible();
  await page.getByRole("textbox", { name: "실행 파일의 전체 경로" }).fill("C:\\Windows\\System32\\notepad.exe");
  await page.getByRole("button", { name: "MacType로 실행" }).click();
  await expect(page.getByText(/MacLoader를 통해 프로세스 4242/)).toBeVisible();
  await expect(page.getByRole("heading", { name: "시스템 범위 모드" })).toBeVisible();

  const horizontalOverflow = await page.evaluate(() => document.documentElement.scrollWidth > document.documentElement.clientWidth);
  expect(horizontalOverflow, "execution controls must not have horizontal scrolling").toBe(false);
  expect(failures, failures.join("\n")).toEqual([]);
});

test("language setting switches every supported locale and persists", async ({ page }, testInfo) => {
  await page.goto("/?view=overview&gallery=1&lang=ko", { waitUntil: "networkidle" });
  for (const locale of galleryLocales) {
    await page.locator(".language-control select").selectOption(locale.id);
    await expect(page.locator("html")).toHaveAttribute("lang", locale.id);
    await expect(page.locator("html")).toHaveAttribute("dir", locale.direction);
    await expect(page.getByRole("heading", { level: 1, name: galleryViews[0].title[locale.id] })).toBeVisible();
  }

  await page.locator(".language-control select").selectOption("en");
  await expect(page.getByRole("button", { name: "Dark theme" })).toBeVisible();

  await page.goto("/?view=overview&gallery=1", { waitUntil: "networkidle" });
  await expect(page.getByRole("combobox", { name: "Display language" })).toHaveValue("en");
  await expect(page.getByRole("heading", { level: 1, name: "Overview" })).toBeVisible();
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-language-en.png`), fullPage: true });
});
