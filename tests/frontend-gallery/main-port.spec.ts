import { expect, test } from "@playwright/test";
import { galleryLocales } from "./windows";

test("the Preview Studio renders a specimen board per source and offers export", async ({ page }, testInfo) => {
  test.skip(testInfo.project.name === "mobile-390", "the studio window is desktop-sized");
  await page.goto("/?window=preview-studio&gallery=1&lang=ko", { waitUntil: "networkidle" });
  await expect(page.getByTestId("preview-studio")).toBeVisible();
  await expect(page.locator(".studio-board")).toHaveCount(2);
  await expect(page.locator(".studio-board").nth(1).locator(".studio-strip")).toHaveCount(7);
  await expect(page.locator(".studio-board").first()).toContainText("Control Center에서 프로파일을 열면 편집 중인 내용과 저장된 설정을 비교할 수 있습니다.");
  await page.locator(".studio-chip", { hasText: /^36$/ }).click();
  await expect(page.locator(".studio-board").nth(1).locator(".studio-strip")).toHaveCount(8);
  await page.getByRole("button", { name: "PNG로 저장" }).click();
  await expect(page.locator(".studio-export-message")).toContainText("mactype-specimen");
  expect(await page.evaluate(() => window.sessionStorage.getItem("gallery-preview-export"))).toContain(".png");
});


test("the main Tuner opens the Studio and reports a failed open", async ({ page }) => {
  await page.goto("/?view=profiles&gallery=1&lang=en", { waitUntil: "networkidle" });
  await page.getByTestId("open-preview-studio").click();
  await expect.poll(() => page.evaluate(() => sessionStorage.getItem("gallery-preview-studio"))).toBe("open");
  await page.goto("/?view=profiles&gallery=1&lang=en&studio-open-error=1", { waitUntil: "networkidle" });
  await page.getByTestId("open-preview-studio").click();
  await expect(page.getByText("Preview Studio could not open", { exact: true })).toBeVisible();
});

test("diagnostics filters unified events and discloses technical details and sources", async ({ page }, testInfo) => {
  await page.goto("/?view=diagnostics&gallery=1&lang=en&events-unreadable=1", { waitUntil: "networkidle" });
  await page.locator("[data-log-disclosure-actions]").getByRole("button", { name: "Expand" }).click();
  const timeline = page.getByTestId("event-timeline");
  await expect(timeline.locator(".event-row")).toHaveCount(12);
  await timeline.getByRole("searchbox").fill("vgtray.exe");
  await expect(timeline.locator(".event-row")).toHaveCount(1);
  await expect(timeline.locator(".event-title")).toContainText("vgtray.exe");
  await expect(timeline.locator(".event-detail")).toHaveCount(0);
  await timeline.getByRole("button", { name: "Details", exact: true }).click();
  await expect(timeline.locator(".event-detail")).toContainText("injection-failed");
  await expect(timeline.locator(".event-detail")).toContainText("pid=4180");
  await timeline.locator('.event-chip[data-severity="warning"]').click();
  await expect(timeline.locator(".event-row")).toHaveCount(0);
  await expect(timeline).toContainText("No events match the filters.");
  await timeline.getByRole("button", { name: "Reset filters" }).click();
  await expect(timeline.locator(".event-row")).toHaveCount(12);
  await page.locator(".event-source-disclosure > summary").click();
  await expect(page.locator('.event-sources [data-readable="false"]')).toContainText("Not readable");
  await page.screenshot({ path: `artifacts/frontend-gallery/${testInfo.project.name}-events-expanded-en.png`, fullPage: true });
});

for (const locale of galleryLocales) {
  test(`standalone preview controls are usable in ${locale.id}`, async ({ page }, testInfo) => {
    test.skip(testInfo.project.name !== "desktop-1280", "the native studio has a desktop minimum size");
    await page.goto(`/?window=preview-studio&gallery=1&lang=${locale.id}`, { waitUntil: "networkidle" });
    await expect(page.locator("html")).toHaveAttribute("dir", locale.direction);
    await expect(page.locator(".studio-board").nth(1).locator(".studio-strip")).toHaveCount(7);
    await page.locator(".studio-source-kind select").first().selectOption("profile");
    await expect(page.locator(".studio-board").first().locator(".studio-strip")).toHaveCount(7);
    await expect(page.locator(".studio-status .button.primary")).toBeEnabled();
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    const clippedControls = await page.locator(".studio-controls button, .studio-controls select").evaluateAll((controls) => controls.filter((control) => control.scrollWidth > control.clientWidth + 2 || control.scrollHeight > control.clientHeight + 2).map((control) => control.textContent));
    expect(clippedControls).toEqual([]);
    expect(await page.locator(".studio-controls").evaluate((element) => element.scrollWidth <= element.clientWidth)).toBe(true);
    await page.locator(".studio-follow input").check();
    await page.locator(".studio-follow label").click();
    await expect(page.locator(".studio-follow input")).not.toBeChecked();
    await page.screenshot({ path: `artifacts/frontend-gallery/${testInfo.project.name}-studio-${locale.id}.png` });
  });
}
