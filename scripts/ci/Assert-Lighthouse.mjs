import fs from "node:fs";

const reportPaths = process.argv.slice(2);
if (reportPaths.length === 0) {
  throw new Error("Usage: node Assert-Lighthouse.mjs <lighthouse.json> [...]");
}

const reports = reportPaths.map((reportPath) =>
  JSON.parse(fs.readFileSync(reportPath, "utf8")),
);
const thresholds = {
  performance: 0.9,
  accessibility: 1,
  "best-practices": 0.9,
};

let failed = false;
for (const [category, threshold] of Object.entries(thresholds)) {
  const scores = reports
    .map((report) => report.categories?.[category]?.score)
    .filter((score) => typeof score === "number")
    .sort((left, right) => left - right);
  if (scores.length !== reports.length) {
    console.error(`Missing Lighthouse category: ${category}`);
    failed = true;
    continue;
  }
  const score = scores[Math.floor(scores.length / 2)];
  const percent = Math.round(score * 100);
  const samples = scores.map((sample) => Math.round(sample * 100)).join(", ");
  console.log(
    `${category}: median ${percent} from [${samples}] (required ${Math.round(threshold * 100)})`,
  );
  if (score < threshold) failed = true;
}

if (failed) process.exit(1);
