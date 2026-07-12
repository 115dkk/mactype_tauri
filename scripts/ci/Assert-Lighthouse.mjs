import fs from "node:fs";

const [reportPath] = process.argv.slice(2);
if (!reportPath) {
  throw new Error("Usage: node Assert-Lighthouse.mjs <lighthouse.json>");
}

const report = JSON.parse(fs.readFileSync(reportPath, "utf8"));
const thresholds = {
  performance: 0.9,
  accessibility: 1,
  "best-practices": 0.9,
  seo: 0.9,
};

let failed = false;
for (const [category, threshold] of Object.entries(thresholds)) {
  const score = report.categories?.[category]?.score;
  if (typeof score !== "number") {
    console.error(`Missing Lighthouse category: ${category}`);
    failed = true;
    continue;
  }
  const percent = Math.round(score * 100);
  console.log(`${category}: ${percent} (required ${Math.round(threshold * 100)})`);
  if (score < threshold) failed = true;
}

if (failed) process.exit(1);
