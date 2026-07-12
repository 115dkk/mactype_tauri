import { expect, test } from "@playwright/test";
import path from "node:path";
import { galleryViews } from "./windows";

const galleryRoot = path.resolve(__dirname, "../../artifacts/frontend-gallery");

for (const view of galleryViews) {
  test(`${view.id} renders without crash or console errors`, async ({ page }, testInfo) => {
    const failures: string[] = [];
    page.on("console", (message) => {
      if (message.type() === "error") failures.push(`console: ${message.text()}`);
    });
    page.on("pageerror", (error) => failures.push(`pageerror: ${error.message}`));
    page.on("crash", () => failures.push("renderer process crashed"));

    await page.goto(`/?view=${view.id}&gallery=1`, { waitUntil: "networkidle" });
    await expect(page.locator("body")).toHaveAttribute("data-rendered", "true");
    await expect(page.locator("body")).toHaveAttribute("data-view", view.id);
    await expect(page.getByRole("heading", { level: 1, name: view.title })).toBeVisible();
    const horizontalOverflow = await page.evaluate(() => document.documentElement.scrollWidth > document.documentElement.clientWidth);
    expect(horizontalOverflow, "window must not have horizontal scrolling").toBe(false);
    expect(failures, failures.join("\n")).toEqual([]);

    await page.screenshot({
      path: path.join(galleryRoot, `${testInfo.project.name}-${view.id}.png`),
      fullPage: true,
    });
  });
}
