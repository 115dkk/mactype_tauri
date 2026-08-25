import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
  assertModelThinking,
  placeholders,
  runAntigravity,
  runDeveloperApi,
  writeExperimentBundle,
} from "./copy-review.mjs";

const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const policyPath = path.join(repositoryRoot, "scripts/i18n/multilingual-copy-policy.json");
const catalogPath = (locale) => path.join(repositoryRoot, `control-center/src/i18n/${locale}.json`);

function parseArgs(argv) {
  const options = {
    provider: "antigravity",
    model: "gemini-3.7-flash",
    thinking: "low",
    output: path.join(repositoryRoot, "artifacts/gemini-localization/multilingual-3.7-low.json"),
    dryRun: false,
  };
  for (let index = 0; index < argv.length; index += 1) {
    const value = argv[index];
    if (value === "--") continue;
    if (value === "--dry-run") options.dryRun = true;
    else if (value.startsWith("--")) {
      const name = value.slice(2);
      if (!(name in options)) throw new Error(`Unknown option: ${value}`);
      const next = argv[++index];
      if (!next) throw new Error(`Missing value for ${value}`);
      options[name] = next;
    } else throw new Error(`Unexpected argument: ${value}`);
  }
  if (!new Set(["api", "antigravity"]).has(options.provider)) throw new Error(`Unknown provider: ${options.provider}`);
  return options;
}

function schemaFor(policy) {
  const localeProperties = Object.fromEntries(policy.targetLocales.map((locale) => [locale, { type: "string" }]));
  return {
    type: "object",
    additionalProperties: false,
    properties: {
      translations: {
        type: "array",
        minItems: policy.entries.length,
        maxItems: policy.entries.length,
        items: {
          type: "object",
          additionalProperties: false,
          properties: {
            key: { type: "string" },
            locales: {
              type: "object",
              additionalProperties: false,
              properties: localeProperties,
              required: policy.targetLocales,
            },
          },
          required: ["key", "locales"],
        },
      },
    },
    required: ["translations"],
  };
}

function validateTranslations(payload, inputs, policy) {
  if (!payload || !Array.isArray(payload.translations)) throw new Error("Model output must contain translations.");
  const inputByKey = new Map(inputs.map((entry) => [entry.key, entry]));
  const policyByKey = new Map(policy.entries.map((entry) => [entry.key, entry]));
  const seen = new Set();
  for (const translated of payload.translations) {
    const input = inputByKey.get(translated.key);
    if (!input) throw new Error(`Unexpected translation key: ${translated.key}`);
    if (seen.has(translated.key)) throw new Error(`Duplicate translation key: ${translated.key}`);
    seen.add(translated.key);
    for (const locale of policy.targetLocales) {
      const value = translated.locales?.[locale];
      if (typeof value !== "string" || !value.trim()) throw new Error(`Missing ${locale} translation for ${translated.key}`);
      if (/\r|\n/u.test(value)) throw new Error(`Translation must stay on one line: ${locale}/${translated.key}`);
      if (JSON.stringify(placeholders(value)) !== JSON.stringify(placeholders(input.en))) {
        throw new Error(`Placeholder mismatch: ${locale}/${translated.key}`);
      }
      for (const token of policyByKey.get(translated.key).requiredTokens) {
        if (!value.includes(token)) throw new Error(`Required token ${token} missing: ${locale}/${translated.key}`);
      }
    }
  }
  const missing = inputs.map((entry) => entry.key).filter((key) => !seen.has(key));
  if (missing.length) throw new Error(`Missing translation keys: ${missing.join(", ")}`);
  return payload.translations;
}

const options = parseArgs(process.argv.slice(2));
assertModelThinking(options.model, options.thinking, options.provider);
const policy = JSON.parse(fs.readFileSync(policyPath, "utf8"));
const catalogs = Object.fromEntries(["ko", "en", ...policy.targetLocales].map((locale) => [
  locale,
  JSON.parse(fs.readFileSync(catalogPath(locale), "utf8")),
]));
const inputs = policy.entries.map((entry) => ({
  key: entry.key,
  intent: entry.intent,
  requiredTokens: entry.requiredTokens,
  ko: catalogs.ko[entry.key],
  en: catalogs.en[entry.key],
  existing: Object.fromEntries(policy.targetLocales.map((locale) => [locale, catalogs[locale][entry.key]])),
}));
const prompt = [
  "Translate the approved Korean and English Windows desktop UX copy into every requested target locale.",
  "The Korean and English strings already express the final product meaning. The existing target strings are context only and may contain claims that must be removed.",
  "Follow each intent exactly. Preserve operational failures, disabled actions, and recovery steps, but remove abstract safety/verification signaling and unintended capability limits described by the intent.",
  "Use concise native UI language and the same level of formality as the existing target locale. Do not add explanations or new claims.",
  "Preserve placeholders and requiredTokens byte-for-byte. Return every key and every target locale exactly once.",
  JSON.stringify({ targetLocales: policy.targetLocales, inputs }, null, 2),
].join("\n");
const schema = schemaFor(policy);
const outputPath = path.resolve(options.output);
const schemaPath = outputPath.replace(/\.json$/u, ".schema.json");
fs.mkdirSync(path.dirname(schemaPath), { recursive: true });
fs.writeFileSync(schemaPath, `${JSON.stringify(schema, null, 2)}\n`, "utf8");
const baseBundle = {
  version: 1,
  createdAt: new Date().toISOString(),
  provider: options.provider,
  model: options.model,
  thinking: options.thinking,
  policyVersion: policy.version,
  targetLocales: policy.targetLocales,
  inputs,
  prompt,
  schema,
};
if (options.dryRun) {
  writeExperimentBundle(outputPath, { ...baseBundle, status: "DRY_RUN" });
  console.log(outputPath);
  process.exit(0);
}

let result;
if (options.provider === "api") {
  if (!process.env.GEMINI_API_KEY) throw new Error("GEMINI_API_KEY is required for --provider api.");
  result = await runDeveloperApi({ apiKey: process.env.GEMINI_API_KEY, model: options.model, thinking: options.thinking, prompt, schema });
} else {
  result = await runAntigravity({ model: options.model, thinking: options.thinking, prompt, schemaPath, cwd: repositoryRoot });
}
const translations = validateTranslations(result.payload, inputs, policy);
writeExperimentBundle(outputPath, {
  ...baseBundle,
  status: "SUCCESS",
  providerMetadata: result.providerMetadata,
  translations,
});
console.log(outputPath);
