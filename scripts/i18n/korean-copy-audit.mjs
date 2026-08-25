import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const moduleDirectory = path.dirname(fileURLToPath(import.meta.url));
const defaultPolicy = JSON.parse(fs.readFileSync(path.join(moduleDirectory, "korean-copy-audit-policy.json"), "utf8"));

export function auditKoreanCopy(source, final, options = {}) {
  const policy = options.policy ?? defaultPolicy;
  const protectedTokens = collectProtectedTokens(source, options.requiredTokens ?? []);
  const approvedRemovals = new Set(options.approvedRemovedProtected ?? []);
  const invalidApprovals = [...approvedRemovals].filter((token) => !protectedTokens.has(token));
  const removed = [...protectedTokens].filter((token) => !final.includes(token));
  const allowedRemoved = removed.filter((token) => approvedRemovals.has(token));
  const missing = removed.filter((token) => !approvedRemovals.has(token));
  const unusedApprovals = [...approvedRemovals].filter((token) => protectedTokens.has(token) && !allowedRemoved.includes(token));
  const blocking = countPatternGroup(source, final, policy.blockingPatterns);
  const contextual = countPatternGroup(source, final, policy.contextPatterns);
  const changeRate = editDistance(source, final) / Math.max(source.length, final.length, 1);
  const problems = [];

  if (missing.length > 0) problems.push(`Protected tokens changed: ${missing.join(", ")}`);
  if (invalidApprovals.length > 0) problems.push(`Approved removal is not a protected source token: ${invalidApprovals.join(", ")}`);
  if (changeRate > policy.changeRate.failAbove) problems.push("Change rate exceeds 50%");
  if (sumCounts(blocking.before) > 0 && sumCounts(blocking.after) >= sumCounts(blocking.before)) {
    problems.push("Blocking Korean copy patterns were not reduced");
  }

  const warnings = [];
  if (changeRate > policy.changeRate.warnAbove && changeRate <= policy.changeRate.failAbove) {
    warnings.push("Change rate exceeds 30%");
  }
  if (unusedApprovals.length > 0) warnings.push(`Approved removal was not used: ${unusedApprovals.join(", ")}`);

  return {
    ok: problems.length === 0,
    grade: gradeAudit({ blockingAfter: sumCounts(blocking.after), changeRate, problems, policy }),
    changeRate: Number(changeRate.toFixed(4)),
    protectedTokens: {
      total: protectedTokens.size,
      missing,
      allowedRemoved,
      unusedApprovals,
    },
    patterns: { blocking, contextual },
    warnings,
    problems,
  };
}

function collectProtectedTokens(source, requiredTokens) {
  const patterns = [
    /https?:\/\/\S+/gu,
    /`[^`]+`/gu,
    /"[^"]+"/gu,
    /\b[A-Z][A-Za-z0-9.-]*\b/gu,
    /\b[A-Z]{2,}\b/gu,
    /\b\d+(?:\.\d+){1,}\b/gu,
    /\d{4}년\s*\d{1,2}월\s*\d{1,2}일/gu,
    /\d+(?:\.\d+)?\s?(?:%|MB|GB|KB|ms|초|분|시간|원|달러)/gu,
  ];
  return new Set([
    ...patterns.flatMap((pattern) => source.match(pattern) ?? []),
    ...requiredTokens.filter((token) => source.includes(token)),
  ]);
}

function countPatternGroup(source, final, patterns) {
  return {
    before: Object.fromEntries(patterns.map(({ id, pattern }) => [id, countMatches(source, pattern)])),
    after: Object.fromEntries(patterns.map(({ id, pattern }) => [id, countMatches(final, pattern)])),
  };
}

function countMatches(value, pattern) {
  return [...value.matchAll(new RegExp(pattern, "gu"))].length;
}

function sumCounts(counts) {
  return Object.values(counts).reduce((total, count) => total + count, 0);
}

function editDistance(left, right) {
  const previous = Array.from({ length: right.length + 1 }, (_, index) => index);
  for (let row = 1; row <= left.length; row += 1) {
    const current = [row];
    for (let column = 1; column <= right.length; column += 1) {
      current[column] = Math.min(
        current[column - 1] + 1,
        previous[column] + 1,
        previous[column - 1] + (left[row - 1] === right[column - 1] ? 0 : 1),
      );
    }
    previous.splice(0, previous.length, ...current);
  }
  return previous[right.length];
}

function gradeAudit({ blockingAfter, changeRate, problems, policy }) {
  if (problems.length > 0) return "D";
  if (changeRate > policy.changeRate.warnAbove || blockingAfter > 0) return policy.changeRate.warningGrade;
  if (changeRate >= 0.1) return "A";
  return "B";
}
