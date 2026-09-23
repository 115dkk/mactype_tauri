import { expect, test } from "@playwright/test";

test("diagnostics filters unified events and discloses technical details and sources", async ({ page }, testInfo) => {
  await page.goto("/?view=diagnostics&gallery=1&lang=en&events-unreadable=1", { waitUntil: "networkidle" });
  const timeline = page.getByTestId("event-timeline");
  await expect(page.getByRole("log")).toBeVisible();
  await expect(timeline.locator(".event-row")).toHaveCount(20);
  await timeline.getByRole("searchbox").fill("vgtray.exe");
  await expect(timeline.locator(".event-row")).toHaveCount(3);
  await expect(timeline.locator(".event-title").first()).toContainText("vgtray.exe");
  await expect(timeline.locator(".event-detail")).toHaveCount(0);
  await timeline.getByRole("button", { name: "Details", exact: true }).first().click();
  await expect(timeline.locator(".event-detail")).toContainText("injection-failed");
  await expect(timeline.locator(".event-detail")).toContainText("pid=4180");
  await timeline.locator('.event-chip[data-severity="warning"]').click();
  await expect(timeline.locator(".event-row")).toHaveCount(0);
  await expect(timeline).toContainText("No events match the filters.");
  await timeline.getByRole("button", { name: "Reset filters" }).click();
  await expect(timeline.locator(".event-row")).toHaveCount(20);
  await page.locator(".event-source-disclosure > summary").click();
  await expect(page.locator('.event-sources [data-readable="false"]')).toContainText("Not readable");
  await page.screenshot({ path: `artifacts/frontend-gallery/${testInfo.project.name}-events-expanded-en.png`, fullPage: true });
});
