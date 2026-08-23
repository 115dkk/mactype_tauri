import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
  assertModelThinking,
  buildPrompt,
  proposalSchema,
  runAntigravity,
  runDeveloperApi,
  selectEntries,
  validateProposals,
  writeExperimentBundle,
} from "./copy-review.mjs";

const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const defaultSample = path.join(repositoryRoot, "scripts/i18n/korean-copy-sample.json");

function parseArgs(argv) {
  const options = {
    provider: "api",
    model: "gemini-3.6-flash",
    thinking: "minimal",
    profile: "all",
    sample: defaultSample,
    output: null,
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

const options = parseArgs(process.argv.slice(2));
assertModelThinking(options.model, options.thinking, options.provider);
const sample = JSON.parse(fs.readFileSync(path.resolve(options.sample), "utf8"));
const entries = selectEntries(sample, options.profile);
if (!entries.length) throw new Error(`No sample entries selected for profile ${options.profile}`);
const prompt = buildPrompt(entries, options);
const schema = proposalSchema(entries.length);
const stamp = new Date().toISOString().replace(/[:.]/g, "-");
const outputPath = path.resolve(options.output ?? path.join(
  repositoryRoot,
  "artifacts/gemini-localization",
  `${options.provider}-${options.model}-${options.thinking}-${options.profile}-${stamp}.json`,
));
const schemaPath = outputPath.replace(/\.json$/u, ".schema.json");
fs.mkdirSync(path.dirname(schemaPath), { recursive: true });
fs.writeFileSync(schemaPath, `${JSON.stringify(schema, null, 2)}\n`, "utf8");

const baseBundle = {
  version: 1,
  createdAt: new Date().toISOString(),
  provider: options.provider,
  model: options.model,
  thinking: options.thinking,
  profile: options.profile,
  sampleVersion: sample.version,
  entries,
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
  const apiKey = process.env.GEMINI_API_KEY;
  if (!apiKey) throw new Error("GEMINI_API_KEY is required for --provider api.");
  result = await runDeveloperApi({ apiKey, model: options.model, thinking: options.thinking, prompt, schema });
} else {
  result = await runAntigravity({ model: options.model, thinking: options.thinking, prompt, schemaPath, cwd: repositoryRoot });
}

const proposals = validateProposals(result.payload, entries);
writeExperimentBundle(outputPath, {
  ...baseBundle,
  status: "SUCCESS",
  providerMetadata: result.providerMetadata,
  proposals,
});
console.log(outputPath);
