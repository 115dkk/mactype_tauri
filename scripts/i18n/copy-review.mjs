import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";

export const supportedModels = {
  "gemini-3.5-flash-lite": new Set(["minimal", "low", "medium", "high"]),
  "gemini-3.6-flash": new Set(["minimal", "low", "medium", "high"]),
  "gemini-3.7-flash": new Set(["low", "medium", "high"]),
};

const verdicts = new Set(["keep", "rewrite", "remove_clause"]);
const risks = new Set(["low", "medium", "high"]);

export function assertModelThinking(model, thinking, provider = "api") {
  const levels = supportedModels[model];
  if (!levels) throw new Error(`Unsupported experiment model: ${model}`);
  if (!levels.has(thinking)) {
    throw new Error(`${model} does not support thinking level ${thinking}`);
  }
  if (provider === "antigravity" && thinking === "minimal") {
    throw new Error("Antigravity exposes low/medium/high effort, not minimal; use the Developer API for a minimal-thinking comparison.");
  }
}

export function selectEntries(sample, profile = "all") {
  if (profile === "all") return sample.entries;
  if (!new Set(["local", "flow"]).has(profile)) throw new Error(`Unknown reasoning profile: ${profile}`);
  return sample.entries.filter((entry) => entry.reasoningProfile === profile);
}

export function buildPrompt(entries, { profile, thinking }) {
  const scope = profile === "local"
    ? "각 문구는 다른 문구에 의존하지 않는 짧은 UI 카피다. 주어진 화면 맥락만 사용하고 국소적으로 다듬어라."
    : "문구들은 한 서비스 상태 흐름과 마이그레이션 절차를 이룬다. 항목별 문장은 짧게 유지하되, 차단 원인·다음 행동·롤백 계약이 서로 모순되지 않는지 묶음 전체를 확인하라.";

  return [
    "당신은 한국어 Windows 데스크톱 앱의 UX 카피 편집자다.",
    scope,
    `이 실험의 thinking 프로필은 ${thinking}이다. 긴 해설을 만들지 말고 필요한 판단만 하라.`,
    "영어는 의미 확인용이며 번역투를 따라 쓰는 원문이 아니다.",
    "목표 우선순위:",
    "1. 기능, 상태, 실패 원인, 다음 행동을 바꾸지 않는다.",
    "2. 한국어 번역투, 명사 나열, 수동태, 딱딱한 내부 용어를 자연스럽게 고친다.",
    "3. 정상 상태에서 소비자가 요청하지 않은 검증·안전·복구 과시는 없앤다.",
    "4. 실패·충돌·판별 불가 상태에서는 실제 제한과 대응 방법을 반드시 남긴다.",
    "5. 프로그램의 능력 부족이나 원치 않는 제한으로 오해할 수 있는 문구는 실제 제약이 아닐 때만 삭제하거나 바꾼다.",
    "6. requiredFacts와 requiredTokens를 보존하고 새로운 주장을 만들지 않는다.",
    "7. UI 존댓말과 한 줄 JSON 문자열 형식을 유지한다.",
    "verdict는 변경 없음 keep, 문장 수정 rewrite, 불필요한 절만 제거 remove_clause 중 하나다.",
    "모든 입력 키를 정확히 한 번씩 반환하라. keep이면 after는 ko와 완전히 같아야 한다.",
    "입력:",
    JSON.stringify(entries, null, 2),
  ].join("\n");
}

export function proposalSchema(entryCount) {
  return {
    type: "object",
    additionalProperties: false,
    properties: {
      proposals: {
        type: "array",
        ...(Number.isInteger(entryCount) ? { minItems: entryCount, maxItems: entryCount } : {}),
        items: {
          type: "object",
          additionalProperties: false,
          properties: {
            key: { type: "string" },
            verdict: { type: "string", enum: [...verdicts] },
            after: { type: "string" },
            rationale: { type: "string" },
            meaningRisk: { type: "string", enum: [...risks] },
          },
          required: ["key", "verdict", "after", "rationale", "meaningRisk"],
        },
      },
    },
    required: ["proposals"],
  };
}

export function placeholders(value) {
  return [...value.matchAll(/\{(\w+)\}/g)].map((match) => match[1]).sort();
}

export function editDistance(left, right) {
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

export function validateProposals(payload, entries) {
  if (!payload || !Array.isArray(payload.proposals)) throw new Error("Model output must contain a proposals array.");
  const inputs = new Map(entries.map((entry) => [entry.key, entry]));
  const seen = new Set();
  const validated = [];

  for (const proposal of payload.proposals) {
    if (!proposal || typeof proposal !== "object") throw new Error("Every proposal must be an object.");
    const input = inputs.get(proposal.key);
    if (!input) throw new Error(`Unexpected proposal key: ${proposal.key}`);
    if (seen.has(proposal.key)) throw new Error(`Duplicate proposal key: ${proposal.key}`);
    seen.add(proposal.key);
    if (!verdicts.has(proposal.verdict)) throw new Error(`Invalid verdict for ${proposal.key}: ${proposal.verdict}`);
    if (!risks.has(proposal.meaningRisk)) throw new Error(`Invalid meaning risk for ${proposal.key}: ${proposal.meaningRisk}`);
    if (typeof proposal.after !== "string" || !proposal.after.trim()) throw new Error(`Empty after text for ${proposal.key}`);
    if (/\r|\n/u.test(proposal.after)) throw new Error(`UI copy must stay on one line: ${proposal.key}`);
    if (typeof proposal.rationale !== "string" || !proposal.rationale.trim()) throw new Error(`Missing rationale for ${proposal.key}`);
    if (proposal.verdict === "keep" && proposal.after !== input.ko) throw new Error(`keep must preserve the source byte-for-byte: ${proposal.key}`);
    if (proposal.verdict !== "keep" && proposal.after === input.ko) throw new Error(`${proposal.verdict} must change the source: ${proposal.key}`);
    if (JSON.stringify(placeholders(proposal.after)) !== JSON.stringify(placeholders(input.ko))) {
      throw new Error(`Placeholder mismatch for ${proposal.key}`);
    }
    for (const token of input.requiredTokens) {
      if (!proposal.after.includes(token)) throw new Error(`Required token ${token} is missing from ${proposal.key}`);
    }
    const changeRate = editDistance(input.ko, proposal.after) / Math.max(input.ko.length, proposal.after.length, 1);
    if (input.category === "naturalness" && changeRate > 0.5) {
      throw new Error(`Naturalness-only rewrite changed more than 50% of ${proposal.key}`);
    }
    validated.push({ ...proposal, before: input.ko, changeRate: Number(changeRate.toFixed(4)) });
  }

  const missing = entries.map((entry) => entry.key).filter((key) => !seen.has(key));
  if (missing.length) throw new Error(`Missing proposal keys: ${missing.join(", ")}`);
  return validated;
}

function extractGeneratedText(response) {
  return response?.candidates?.[0]?.content?.parts
    ?.map((part) => part.text ?? "")
    .join("")
    .trim();
}

export async function runDeveloperApi({ apiKey, model, thinking, prompt, schema }) {
  const response = await fetch(`https://generativelanguage.googleapis.com/v1beta/models/${encodeURIComponent(model)}:generateContent`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "x-goog-api-key": apiKey,
    },
    body: JSON.stringify({
      contents: [{ role: "user", parts: [{ text: prompt }] }],
      generationConfig: {
        responseFormat: {
          text: {
            mimeType: "application/json",
            schema,
          },
        },
        thinkingConfig: { thinkingLevel: thinking.toUpperCase() },
      },
    }),
  });
  if (!response.ok) throw new Error(`Gemini API failed (${response.status}): ${await response.text()}`);
  const envelope = await response.json();
  const text = extractGeneratedText(envelope);
  if (!text) throw new Error("Gemini API returned no generated text.");
  return { payload: JSON.parse(text), providerMetadata: { usage: envelope.usageMetadata ?? null } };
}

function antigravitySlug(model) {
  return `${model}-medium`;
}

function spawnCapture(command, args, options = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { ...options, shell: false, windowsHide: true });
    let stdout = "";
    let stderr = "";
    child.stdout.on("data", (chunk) => { stdout += chunk; });
    child.stderr.on("data", (chunk) => { stderr += chunk; });
    child.on("error", reject);
    child.on("close", (code) => {
      if (code !== 0) reject(new Error(`Antigravity exited ${code}: ${stderr.trim() || stdout.trim()}`));
      else resolve({ stdout, stderr });
    });
  });
}

export async function runAntigravity({ model, thinking, prompt, schemaPath, cwd }) {
  const command = process.env.AGY_BIN || "agy";
  const { stdout } = await spawnCapture(command, [
    "-p", prompt,
    "--model", antigravitySlug(model),
    "--effort", thinking,
    "--output-format", "json",
    "--json-schema", schemaPath,
    "--print-timeout", "10m",
  ], { cwd });
  const envelope = JSON.parse(stdout);
  if (envelope.status !== "SUCCESS") throw new Error(`Antigravity failed: ${envelope.error ?? envelope.status}`);
  const payload = envelope.structured_output ?? JSON.parse(envelope.response);
  return {
    payload,
    providerMetadata: {
      conversationId: envelope.conversation_id ?? null,
      usage: envelope.usage ?? null,
      durationSeconds: envelope.duration_seconds ?? null,
    },
  };
}

export function writeExperimentBundle(outputPath, bundle) {
  fs.mkdirSync(path.dirname(outputPath), { recursive: true });
  fs.writeFileSync(outputPath, `${JSON.stringify(bundle, null, 2)}\n`, "utf8");
}
