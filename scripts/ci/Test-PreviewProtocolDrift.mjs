import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const cppPath = path.join(root, "preview-helper", "include", "protocol.h");
const rustPath = path.join(root, "control-center", "src-tauri", "src", "preview", "protocol.rs");
const cpp = fs.readFileSync(cppPath, "utf8");
const rust = fs.readFileSync(rustPath, "utf8");

function evaluateInteger(expression) {
  const normalized = expression
    .replaceAll("'", "")
    .replaceAll(/(?<=\d)(?:ULL|UL|U|L)\b/giu, "")
    .replaceAll(/\b(?:std::)?u?int(?:16|32|64)_t\b/gu, "")
    .replaceAll("_", "")
    .trim();
  if (!/^[0-9a-fx\s*+()]+$/iu.test(normalized)) return undefined;
  return Function(`"use strict"; return Number(${normalized});`)();
}

function cppConstants(source) {
  const result = new Map();
  for (const match of source.matchAll(/constexpr\s+[^=;]+\s+(k\w+)\s*=\s*([^;]+);/gu)) {
    const value = evaluateInteger(match[2]);
    if (value !== undefined) result.set(match[1], value);
  }
  const enumMatch = source.match(/enum class MessageKind[^\{]*\{([\s\S]*?)\};/u);
  if (!enumMatch) throw new Error("C++ MessageKind enum was not found");
  const enumBody = enumMatch[1];
  for (const match of enumBody.matchAll(/(\w+)\s*=\s*([^,]+),/gu)) {
    result.set(`kind.${match[1]}`, evaluateInteger(match[2]));
  }
  return result;
}

function rustConstants(source) {
  const result = new Map();
  for (const match of source.matchAll(/const\s+(\w+)\s*:\s*[^=]+\s*=\s*([^;]+);/gu)) {
    const raw = match[1];
    const value = evaluateInteger(match[2]);
    if (value === undefined) continue;
    const names = {
      MAGIC: "kMagic",
      VERSION: "kVersion",
      MAX_JSON: "kMaxJsonLength",
      MAX_BINARY: "kMaxBinaryLength",
      HEADER_SIZE: "kHeaderSize",
    };
    result.set(names[raw] ?? `kind.${raw.toLowerCase()}`, value);
  }
  const header = source.match(/let mut header = \[0_u8;\s*(\d+)\]/u);
  if (!header) throw new Error("Rust MTPC header size was not found");
  result.set("kHeaderSize", Number(header[1]));
  return result;
}

const left = cppConstants(cpp);
const right = rustConstants(rust);
const names = [...new Set([...left.keys(), ...right.keys()])].sort();
const failures = [];
for (const [side, constants] of [["C++", left], ["Rust", right]]) {
  for (const required of ["kMagic", "kVersion", "kHeaderSize", "kMaxJsonLength", "kMaxBinaryLength"]) {
    if (!constants.has(required)) failures.push(`${required}: not parsed from ${side}`);
  }
  if (![...constants.keys()].some((name) => name.startsWith("kind."))) {
    failures.push(`message kinds: none parsed from ${side}`);
  }
  for (const [name, value] of constants) {
    if (value === undefined || !Number.isFinite(value)) failures.push(`${name}: invalid ${side} value`);
  }
}
for (const name of names) {
  if (!left.has(name)) failures.push(`${name}: present only in Rust (${right.get(name)})`);
  else if (!right.has(name)) failures.push(`${name}: present only in C++ (${left.get(name)})`);
  else if (left.get(name) !== right.get(name)) {
    failures.push(`${name}: C++=${left.get(name)}, Rust=${right.get(name)}`);
  }
}
if (failures.length > 0) {
  console.error("MTPC protocol drift detected:");
  for (const failure of failures) console.error(`- ${failure}`);
  process.exit(1);
}
console.log(`MTPC protocol drift gate passed for ${names.length} constants.`);
