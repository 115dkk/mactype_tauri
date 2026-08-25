import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
  assertModelThinking,
  buildPrompt,
  proposalSchema,
  selectEntries,
  validateProposals,
} from "../i18n/copy-review.mjs";
import { auditKoreanCopy } from "../i18n/korean-copy-audit.mjs";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const sample = JSON.parse(fs.readFileSync(path.join(root, "scripts/i18n/korean-copy-sample.json"), "utf8"));
const ko = JSON.parse(fs.readFileSync(path.join(root, "control-center/src/i18n/ko.json"), "utf8"));
const en = JSON.parse(fs.readFileSync(path.join(root, "control-center/src/i18n/en.json"), "utf8"));

assertModelThinking("gemini-3.5-flash-lite", "minimal");
assertModelThinking("gemini-3.5-flash", "low", "antigravity");
assertModelThinking("gemini-3.6-flash", "minimal");
assertModelThinking("gemini-3.7-flash", "low");
assert.throws(() => assertModelThinking("gemini-3.7-flash", "minimal"), /does not support/u);
assert.throws(() => assertModelThinking("gemini-3.6-flash", "minimal", "antigravity"), /not minimal/u);
assert.throws(() => assertModelThinking("gemini-3.5-flash-lite", "low", "antigravity"), /does not expose/u);

for (const entry of sample.entries) {
  assert.equal(typeof ko[entry.key], "string", `Missing live Korean key ${entry.key}`);
  assert.equal(typeof en[entry.key], "string", `Missing live English key ${entry.key}`);
  assert.equal(typeof entry.ko, "string");
  assert.equal(typeof entry.en, "string");
  assert.ok(entry.requiredFacts.length > 0, `Required facts must document ${entry.key}`);
}

const local = selectEntries(sample, "local");
const flow = selectEntries(sample, "flow");
assert.ok(local.length >= 5);
assert.ok(flow.length >= 10);
assert.equal(local.length + flow.length, sample.entries.length);
assert.match(buildPrompt(local, { profile: "local", thinking: "minimal" }), /국소적으로 다듬어라/u);
assert.match(buildPrompt(flow, { profile: "flow", thinking: "low" }), /묶음 전체를 확인하라/u);

const schema = proposalSchema(sample.entries.length);
assert.equal(schema.properties.proposals.type, "array");
assert.equal(schema.properties.proposals.minItems, sample.entries.length);
const kept = {
  proposals: local.map((entry) => ({
    key: entry.key,
    verdict: "keep",
    after: entry.ko,
    rationale: "변경하지 않아도 의미와 문체가 분명합니다.",
    meaningRisk: "low",
  })),
};
assert.equal(validateProposals(kept, local).length, local.length);

const missing = structuredClone(kept);
missing.proposals.pop();
assert.throws(() => validateProposals(missing, local), /Missing proposal keys/u);

const changedKeep = structuredClone(kept);
changedKeep.proposals[0].after += " 변경";
assert.throws(() => validateProposals(changedKeep, local), /keep must preserve/u);

const placeholderEntry = local.find((entry) => entry.key === "files.detectedDescription");
const placeholderPayload = {
  proposals: [{
    key: placeholderEntry.key,
    verdict: "rewrite",
    after: "기존 MacType 설치에서 프로필을 찾았습니다.",
    rationale: "짧게 다듬었습니다.",
    meaningRisk: "low",
  }],
};
assert.throws(() => validateProposals(placeholderPayload, [placeholderEntry]), /Placeholder mismatch/u);

const tokenEntry = local.find((entry) => entry.key === "execution.systemNote");
const tokenPayload = {
  proposals: [{
    key: tokenEntry.key,
    verdict: "rewrite",
    after: "시스템 적용을 바꾸려면 승인이 필요합니다.",
    rationale: "내부 용어를 없앴습니다.",
    meaningRisk: "high",
  }],
};
assert.throws(() => validateProposals(tokenPayload, [tokenEntry]), /Required token/u);

const contextual = auditKoreanCopy(
  "설정을 기록하고, 다음 단계로 이동합니다.",
  "설정을 기록하고, 다음 단계로 이동합니다.",
);
assert.equal(contextual.ok, true);
assert.equal(contextual.patterns.contextual.before["punctuation.connective-comma"], 1);
assert.equal(contextual.patterns.contextual.after["punctuation.connective-comma"], 1);

const unchangedBlocking = auditKoreanCopy(
  "결론적으로, API를 통해 문서를 엽니다.",
  "결론적으로, API를 통해 문서를 엽니다.",
);
assert.equal(unchangedBlocking.ok, false);
assert.match(unchangedBlocking.problems.join(" "), /patterns were not reduced/u);

const approvedRemoval = auditKoreanCopy(
  "Ready 상태를 표시합니다.",
  "상태를 표시합니다.",
  { approvedRemovedProtected: ["Ready"] },
);
assert.equal(approvedRemoval.ok, true);
assert.deepEqual(approvedRemoval.protectedTokens.allowedRemoved, ["Ready"]);
assert.deepEqual(approvedRemoval.protectedTokens.missing, []);

const unapprovedRemoval = auditKoreanCopy("Ready 상태를 표시합니다.", "상태를 표시합니다.");
assert.equal(unapprovedRemoval.ok, false);
assert.deepEqual(unapprovedRemoval.protectedTokens.missing, ["Ready"]);

const unknownApproval = auditKoreanCopy(
  "상태를 표시합니다.",
  "상태를 표시합니다.",
  { approvedRemovedProtected: ["Ready"] },
);
assert.equal(unknownApproval.ok, false);
assert.match(unknownApproval.problems.join(" "), /not a protected source token/u);

const warningChange = auditKoreanCopy(
  "사용자는 문서를 빠르게 확인합니다.",
  "사용자는 파일을 바로 검토합니다.",
);
assert.ok(warningChange.changeRate > 0.3 && warningChange.changeRate <= 0.5);
assert.equal(warningChange.ok, true);
assert.equal(warningChange.grade, "C");
assert.deepEqual(warningChange.warnings, ["Change rate exceeds 30%"]);

console.log(`Gemini copy experiment contract passed for ${sample.entries.length} samples (${local.length} local, ${flow.length} flow).`);
