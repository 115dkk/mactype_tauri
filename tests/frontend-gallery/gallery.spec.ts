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
  const undo = page.getByRole("button", { name: "되돌리기" });
  const redo = page.getByRole("button", { name: "다시 하기" });
  const discard = page.getByRole("button", { name: "초기화", exact: true });
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
  await page.getByRole("button", { name: "지금 적용" }).click();
  await expect(page.locator(".profile-message")).toContainText("실제 MacType 실행 경로에 적용했습니다");
  await expect(discard).toBeEnabled();
  await discard.click();
  await expect(discard).toBeDisabled();
  await firstSelect.selectOption(nextOption);
  await page.getByRole("button", { name: "지금 저장" }).click();
  await expect(page.locator(".profile-message")).toContainText("지금 저장했습니다");
  await expect(discard).toBeDisabled();

  const previewResizer = page.getByRole("separator", { name: "프리뷰 영역 높이 조절" });
  await expect(previewResizer).toHaveAttribute("aria-valuenow", "320");
  await previewResizer.press("ArrowDown");
  await expect(previewResizer).toHaveAttribute("aria-valuenow", "304");
  await previewResizer.press("Home");
  await expect(previewResizer).toHaveAttribute("aria-valuenow", "128");

  await page.getByRole("button", { name: "LCD·픽셀 배열" }).click();
  await expect(page.getByRole("heading", { name: "LCD·픽셀 배열" })).toBeVisible();
  await page.getByRole("checkbox", { name: "고급 설정 표시" }).check();
  await expect(page.getByText("빨강 채널 튜닝", { exact: true })).toBeVisible();

  await page.getByRole("button", { name: "고급·실험" }).click();
  await expect(page.getByText("DirectWrite 감마", { exact: true })).toBeVisible();
  const shadow = page.getByRole("group", { name: "그림자 버퍼" });
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
  await expect(page.locator(".font-list-editor li > span").filter({ hasText: "Calibri" })).toBeVisible();
  await expect(page.getByText("제외 프로그램", { exact: true })).toBeVisible();
  await expect(page.getByText("주입 해제 DLL", { exact: true })).toBeVisible();
  await expect(page.getByText("글꼴 대체 제외 모듈", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "어두운 배경" }).click();
  await expect(page.getByRole("img", { name: "현재 설정의 글자 렌더링 프리뷰" })).toHaveAttribute("data-dark", "true");
  const horizontalOverflow = await page.evaluate(() => document.documentElement.scrollWidth > document.documentElement.clientWidth);
  expect(horizontalOverflow, "interactive profile editor must not have horizontal scrolling").toBe(false);
  expect(failures, failures.join("\n")).toEqual([]);
});

test("settings files support import, save as, export, reveal, and apply without typing a path", async ({ page }) => {
  const failures: string[] = [];
  page.on("console", (message) => {
    if (message.type() === "error") failures.push(`console: ${message.text()}`);
  });
  page.on("pageerror", (error) => failures.push(`pageerror: ${error.message}`));

  await page.goto("/?view=files&gallery=1&lang=ko", { waitUntil: "networkidle" });
  await expect(page.getByRole("heading", { name: "기존 MacType 설정을 찾았습니다" })).toBeVisible();
  await expect(page.getByRole("textbox", { name: /경로|path/i })).toHaveCount(0);
  await page.getByRole("button", { name: "이 설정 가져오기" }).click();
  await expect(page.locator('[data-operation="file-settings"]')).toContainText("개인 프로필로 가져왔습니다");

  await page.getByRole("button", { name: "INI 파일 선택" }).click();
  await expect(page.locator('[data-operation="file-settings"]')).toContainText("Community.ini");
  await page.getByRole("textbox", { name: "복제 프로필 이름" }).fill("Gallery copy");
  await page.getByRole("button", { name: "다른 이름으로 저장" }).click();
  await expect(page.locator('[data-operation="file-settings"]')).toContainText("Gallery copy.ini");
  await page.getByRole("button", { name: "파일 위치 열기" }).click();
  await expect(page.locator('[data-operation="file-settings"]')).toContainText("파일 위치를 열었습니다");
  await page.getByRole("button", { name: "내보낼 위치 선택" }).click();
  await expect(page.locator('[data-operation="file-settings"]')).toContainText("내보냈습니다");
  await page.getByRole("button", { name: "실제 적용" }).click();
  await expect(page.locator('[data-operation="file-settings"]')).toContainText("실행 프로필로 적용");

  const horizontalOverflow = await page.evaluate(() => document.documentElement.scrollWidth > document.documentElement.clientWidth);
  expect(horizontalOverflow, "settings-file controls must not have horizontal scrolling").toBe(false);
  expect(failures, failures.join("\n")).toEqual([]);
});

test("execution and verified legacy service controls remain interactive", async ({ page }) => {
  const failures: string[] = [];
  page.on("console", (message) => {
    if (message.type() === "error") failures.push(`console: ${message.text()}`);
  });
  page.on("pageerror", (error) => failures.push(`pageerror: ${error.message}`));

  await page.goto("/?view=execution&gallery=1&lang=ko", { waitUntil: "networkidle" });
  const autostart = page.getByRole("checkbox");
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
  await expect(page.getByText(/MacLoader를 통해 프로세스 4242/)).toBeVisible();
  await expect(page.getByRole("heading", { name: "시스템 범위 모드" })).toBeVisible();

  await expect(page.getByText("검증된 MacTray 서비스 · 실행 중", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "서비스 중지" }).click();
  await expect(page.getByText("검증된 MacTray 서비스 · 중지됨", { exact: true })).toBeVisible();
  await expect(page.getByText("서비스 상태를 갱신했습니다.", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "서비스 시작" }).click();
  await expect(page.getByText("검증된 MacTray 서비스 · 실행 중", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "서비스 제거" }).click();
  await expect(page.getByText("설치되지 않음 · 중지됨", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "서비스 설치" }).click();
  await expect(page.getByText("검증된 MacTray 서비스 · 실행 중", { exact: true })).toBeVisible();

  const horizontalOverflow = await page.evaluate(() => document.documentElement.scrollWidth > document.documentElement.clientWidth);
  expect(horizontalOverflow, "execution controls must not have horizontal scrolling").toBe(false);
  expect(failures, failures.join("\n")).toEqual([]);
});

test("overview and diagnostic actions always produce visible results", async ({ page }) => {
  await page.goto("/?view=overview&gallery=1&lang=ko", { waitUntil: "networkidle" });
  await page.getByRole("button", { name: "설치 위치 다시 찾기" }).click();
  await expect(page.locator('[data-operation="relocate"]')).toBeVisible();
  await page.getByRole("button", { name: "다시 연결" }).click();
  await expect(page.locator('[data-operation="reconnect"]')).toBeVisible();

  await page.getByRole("button", { name: "진단" }).click();
  await page.getByRole("button", { name: "진단 파일 내보내기" }).click();
  await expect(page.locator('[data-operation="export"]')).toContainText("diagnostics-gallery.txt");
  await page.getByRole("button", { name: "진단 정보 복사" }).click();
  await expect(page.locator('[data-operation="copy"]')).toBeVisible();
  await page.getByRole("button", { name: "로그 폴더 열기" }).click();
  await expect(page.locator('[data-operation="folder"]')).toContainText("ControlCenter");
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
