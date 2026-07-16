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
  await expect(page.locator(".profile-message")).toContainText("실제 MacType 시스템 범위에 적용했습니다");
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

test("settings navigation separates Wizard and Tuner without duplicating the editor", async ({ page }, testInfo) => {
  await page.goto("/?view=overview&gallery=1&lang=ko", { waitUntil: "networkidle" });

  const settingsToggle = page.getByRole("button", { name: "설정", exact: true });
  await expect(settingsToggle).toHaveAttribute("aria-expanded", "true");
  await expect(page.getByRole("button", { name: /설정 파일/ })).toBeVisible();
  await expect(page.getByRole("button", { name: /빠른 설정.*Wizard/ })).toBeVisible();
  await expect(page.getByRole("button", { name: /고급 조정.*Tuner/ })).toBeVisible();

  await page.getByRole("button", { name: /빠른 설정.*Wizard/ }).click();
  await expect(page.locator(".profile-page")).toHaveAttribute("data-mode", "quick");
  await expect(page.getByRole("heading", { level: 1, name: "빠른 설정" })).toBeVisible();
  await expect(page.locator(".settings-index button")).toHaveCount(7);
  await expect(page.locator(".settings-step")).toHaveCount(7);
  await expect(page.locator(".settings-index").getByRole("button", { name: "고급·실험" })).toHaveCount(0);
  const quickActions = page.getByRole("toolbar", { name: "프로필 편집 작업" });
  await expect(quickActions.getByRole("button")).toHaveCount(5);
  expect(await quickActions.getByRole("button").evaluateAll((buttons) => buttons.map((button) => button.textContent?.trim()))).toEqual(["되돌리기", "다시 하기", "초기화", "지금 저장", "지금 적용"]);
  await expect(page.locator(".wizard-quick-actions")).toHaveCount(0);
  const settingsForm = page.locator(".settings-form");
  expect(await settingsForm.evaluate((element) => element.scrollWidth > element.clientWidth), "Wizard settings must not have internal horizontal scrolling").toBe(false);
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-wizard-rendering-ko.png`), fullPage: true });
  await expect(page.getByRole("button", { name: "이전" })).toHaveCount(0);
  await page.getByRole("button", { name: "진행" }).click();
  await expect(page.getByRole("heading", { level: 2, name: "글꼴 품질" })).toBeVisible();
  await expect(page.getByRole("button", { name: "이전" })).toBeVisible();
  await page.locator(".settings-index").getByRole("button", { name: "힌팅" }).click();
  await expect(page.getByRole("heading", { level: 2, name: "힌팅" })).toBeVisible();
  await page.locator(".settings-index").getByRole("button", { name: "적용 및 미리보기" }).click();
  await expect(page.getByRole("button", { name: "진행" })).toHaveCount(0);
  await expect(page.getByRole("button", { name: "MacType에 적용" })).toBeVisible();
  await page.screenshot({ path: path.join(galleryRoot, `${testInfo.project.name}-wizard-apply-ko.png`), fullPage: true });

  await page.getByRole("button", { name: /고급 조정.*Tuner/ }).click();
  await expect(page.locator(".profile-page")).toHaveAttribute("data-mode", "advanced");
  await expect(page.getByRole("heading", { level: 1, name: "고급 조정" })).toBeVisible();
  await expect(page.locator(".settings-index button")).toHaveCount(6);
  expect(await settingsForm.evaluate((element) => element.scrollWidth > element.clientWidth), "Tuner settings must not have internal horizontal scrolling").toBe(false);
  await expect(page.getByRole("checkbox", { name: "고급 설정 표시" })).toBeVisible();

  await settingsToggle.click();
  await expect(settingsToggle).toHaveAttribute("aria-expanded", "false");
  await expect(page.locator("#settings-navigation")).toHaveCount(0);
  await settingsToggle.click();
  await expect(page.locator("#settings-navigation")).toBeVisible();
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
  await expect(page.locator('[data-operation="file-settings"]')).toContainText("시스템 프로필로 적용");

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

  await expect(page.getByText("MacType 시스템 적용 중", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "잠시 적용 끄기" }).click();
  await expect(page.getByText("MacType 시스템 적용 꺼짐", { exact: true })).toBeVisible();
  await expect(page.getByText("MacType 시스템 적용을 잠시 껐습니다.", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "현재 프로필 적용" }).click();
  await expect(page.getByText("MacType 시스템 적용 중", { exact: true })).toBeVisible();
  await expect(page.getByText("현재 프로필을 시스템 범위에 적용했습니다.", { exact: true })).toBeVisible();

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
  await expect(page.locator(".file-selection-grid select")).toHaveValue(recent);
});

test("settings files fall back to the applied profile", async ({ page }) => {
  const applied = "C:\\Program Files\\MacType\\ini\\Default.ini";
  await page.goto("/?view=files&gallery=1&lang=ko&fresh=1", { waitUntil: "networkidle" });
  await expect(page.locator(".file-selection-grid select")).toHaveValue(applied);
});

test("settings files do not claim the already applied legacy profile is different", async ({ page }) => {
  await page.goto("/?view=files&gallery=1&lang=ko&legacy-applied=1", { waitUntil: "networkidle" });
  await expect(page.locator(".legacy-import-banner")).toHaveCount(0);
});
