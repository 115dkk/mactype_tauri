import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const args = process.argv.slice(2).filter((value) => value !== "--");
const inputIndex = args.indexOf("--input");
if (inputIndex < 0 || !args[inputIndex + 1]) throw new Error("--input <experiment.json> is required.");
if (!args.includes("--apply")) throw new Error("Refusing to modify catalogs without --apply.");
const bundle = JSON.parse(fs.readFileSync(path.resolve(args[inputIndex + 1]), "utf8"));
if (bundle.status !== "SUCCESS") throw new Error(`Only a successful experiment can be applied; got ${bundle.status}`);
const inputByKey = new Map(bundle.inputs.map((entry) => [entry.key, entry]));

for (const locale of bundle.targetLocales) {
  const catalogPath = path.join(repositoryRoot, `control-center/src/i18n/${locale}.json`);
  const catalog = JSON.parse(fs.readFileSync(catalogPath, "utf8"));
  for (const translated of bundle.translations) {
    const expected = inputByKey.get(translated.key).existing[locale];
    if (catalog[translated.key] !== expected) {
      throw new Error(`Catalog changed after experiment: ${locale}/${translated.key}`);
    }
    catalog[translated.key] = translated.locales[locale];
  }
  fs.writeFileSync(catalogPath, `${JSON.stringify(catalog, null, 2)}\n`, "utf8");
}
console.log(`Applied ${bundle.translations.length} policy translations to ${bundle.targetLocales.length} locale catalogs.`);
