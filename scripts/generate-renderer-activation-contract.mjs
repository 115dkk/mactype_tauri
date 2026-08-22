import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const schemaPath = path.join(root, 'shared', 'renderer-activation-schema.json');
const schema = JSON.parse(fs.readFileSync(schemaPath, 'utf8'));
const checkOnly = process.argv.slice(2).includes('--check');

if (process.argv.slice(2).some((argument) => argument !== '--check')) {
  throw new Error('usage: node scripts/generate-renderer-activation-contract.mjs [--check]');
}

const identifier = /^[a-z][a-z0-9_]*$/;
const wireCode = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const primitive = {
  u8: { size: 1, alignment: 1 },
  u16: { size: 2, alignment: 2 },
  u32: { size: 4, alignment: 4 },
  u64: { size: 8, alignment: 8 },
  char: { size: 1, alignment: 1 },
  bytes: { size: 1, alignment: 1 },
};

function fail(message) {
  throw new Error(`invalid renderer activation schema: ${message}`);
}

function align(value, alignment) {
  return Math.ceil(value / alignment) * alignment;
}

function pascal(name) {
  return name.split('_').map((part) => part[0].toUpperCase() + part.slice(1)).join('');
}

function screaming(name) {
  return name.toUpperCase();
}

function enumDefinitions() {
  return [
    ['architecture', 'RendererArchitecture', 'MacTypeRendererArchitecture', 'MACTYPE_RENDERER_ARCHITECTURE'],
    ['moduleLoad', 'RendererModuleLoad', 'MacTypeRendererModuleLoad', 'MACTYPE_RENDERER_MODULE_LOAD'],
    ['disposition', 'RendererActivationDisposition', 'MacTypeRendererActivationDisposition', 'MACTYPE_RENDERER_DISPOSITION'],
    ['reason', 'RendererActivationReason', 'MacTypeRendererActivationReason', 'MACTYPE_RENDERER_REASON'],
  ].map(([key, rustName, cName, cPrefix]) => ({
    key,
    rustName,
    cName,
    cPrefix,
    ...schema.enums[key],
  }));
}

function validateSchema() {
  if (schema.$schema !== 'mactype-renderer-activation-contract-v1') {
    fail('unexpected schema identity');
  }
  const contract = schema.contract;
  for (const key of [
    'schemaVersion',
    'evidenceSize',
    'evidenceAlignment',
    'runtimeGenerationCanonicalChars',
    'runtimeGenerationTextBytes',
    'profileDigestCanonicalChars',
    'profileDigestTextBytes',
    'reservedBytes',
  ]) {
    if (!Number.isInteger(contract[key]) || contract[key] <= 0) {
      fail(`contract.${key} must be a positive integer`);
    }
  }
  if (contract.schemaVersion !== 1 || contract.evidenceAlignment !== 8) {
    fail('v1 schema version and eight-byte evidence alignment are fixed');
  }
  if (contract.queryExport !== 'MacTypeQueryActivationEvidenceV1') {
    fail('the v1 query export name is fixed');
  }
  if (contract.runtimeGenerationTextBytes !== contract.runtimeGenerationCanonicalChars + 1) {
    fail('runtime generation storage must include exactly one NUL');
  }
  if (contract.profileDigestTextBytes !== contract.profileDigestCanonicalChars + 1 ||
      contract.profileDigestPrefix !== 'sha256:') {
    fail('profile digest storage or prefix is not canonical');
  }

  for (const definition of enumDefinitions()) {
    if (!['u8', 'u16'].includes(definition.wireType) || !Array.isArray(definition.values) ||
        definition.values.length === 0) {
      fail(`${definition.key} has an unsupported wire definition`);
    }
    const maximum = definition.wireType === 'u8' ? 0xff : 0xffff;
    const values = new Set();
    const codes = new Set();
    for (const value of definition.values) {
      if (!identifier.test(value.name) || !wireCode.test(value.code) ||
          !Number.isInteger(value.value) || value.value < 0 || value.value > maximum ||
          values.has(value.value) || codes.has(value.code)) {
        fail(`${definition.key} contains an invalid or duplicate value`);
      }
      values.add(value.value);
      codes.add(value.code);
    }
  }

  const capabilityBits = new Set();
  const capabilityCodes = new Set();
  for (const capability of schema.capabilities) {
    if (!identifier.test(capability.name) || !wireCode.test(capability.code) ||
        !Number.isInteger(capability.bit) || capability.bit < 0 || capability.bit >= 64 ||
        capabilityBits.has(capability.bit) || capabilityCodes.has(capability.code)) {
      fail('capabilities contain an invalid or duplicate entry');
    }
    capabilityBits.add(capability.bit);
    capabilityCodes.add(capability.code);
  }

  const layouts = {};
  for (const [name, definition] of Object.entries(schema.structs)) {
    let cursor = 0;
    let maximumAlignment = 1;
    for (const field of definition.fields) {
      if (!identifier.test(field.name)) {
        fail(`${name} contains an invalid field name`);
      }
      const type = primitive[field.type] ?? layouts[field.type];
      if (!type) {
        fail(`${name}.${field.name} uses unknown type ${field.type}`);
      }
      if (['bytes', 'char'].includes(field.type) &&
          (!Number.isInteger(field.count) || field.count <= 0)) {
        fail(`${name}.${field.name} requires a positive count`);
      }
      const count = field.count ?? 1;
      cursor = align(cursor, type.alignment);
      if (field.offset !== cursor) {
        fail(`${name}.${field.name} offset ${field.offset} does not match ${cursor}`);
      }
      cursor += type.size * count;
      maximumAlignment = Math.max(maximumAlignment, type.alignment);
    }
    const expectedAlignment = Math.max(maximumAlignment, definition.alignment);
    const size = align(cursor, expectedAlignment);
    if (size !== definition.size) {
      fail(`${name} size ${definition.size} does not match calculated size ${size}`);
    }
    layouts[name] = { size, alignment: expectedAlignment };
  }
  if (schema.structs.activationEvidenceV1.size !== contract.evidenceSize ||
      schema.structs.activationEvidenceV1.alignment !== contract.evidenceAlignment ||
      schema.structs.runtimeBinding.fields[0].count !== contract.runtimeGenerationTextBytes ||
      schema.structs.runtimeBinding.fields[1].count !== contract.profileDigestTextBytes ||
      schema.structs.activationEvidenceV1.fields.at(-1).count !== contract.reservedBytes) {
    fail('contract constants and struct layout disagree');
  }
}

function cEnum(definition) {
  const underlying = definition.wireType === 'u8' ? 'uint8_t' : 'uint16_t';
  const constants = definition.values.map((value) =>
    `#define ${definition.cPrefix}_${screaming(value.name)} ${value.value}U`,
  ).join('\n');
  const cases = definition.values.map((value) =>
    `    case ${definition.cPrefix}_${screaming(value.name)}:\n` +
    `        return "${value.code}";`,
  ).join('\n');
  return `typedef ${underlying} ${definition.cName};\n${constants}\n\n` +
    `static inline const char* ${definition.cName}Code(${definition.cName} value)\n` +
    `{\n    switch (value)\n    {\n${cases}\n    default:\n        return 0;\n    }\n}\n`;
}

function cField(field) {
  const types = {
    u8: 'uint8_t',
    u16: 'uint16_t',
    u32: 'uint32_t',
    u64: 'uint64_t',
    char: 'char',
    bytes: 'uint8_t',
    runtimeBinding: 'MacTypeRendererRuntimeBindingV1',
  };
  const suffix = field.count ? `[${field.count}]` : '';
  return `    ${types[field.type]} ${field.name}${suffix};`;
}

function cStructAssertions(cName, definition) {
  const fieldAssertions = definition.fields.map((field) =>
    `MACTYPE_RENDERER_STATIC_ASSERT(offsetof(${cName}, ${field.name}) == ${field.offset}U,\n` +
    `                               "${cName}.${field.name} offset changed");`,
  ).join('\n');
  return `MACTYPE_RENDERER_STATIC_ASSERT(sizeof(${cName}) == ${definition.size}U,\n` +
    `                               "${cName} size changed");\n` +
    `MACTYPE_RENDERER_STATIC_ASSERT(MACTYPE_RENDERER_ALIGNOF(${cName}) == ${definition.alignment}U,\n` +
    `                               "${cName} alignment changed");\n${fieldAssertions}`;
}

function generateCpp() {
  const contract = schema.contract;
  const binding = schema.structs.runtimeBinding;
  const evidence = schema.structs.activationEvidenceV1;
  const enums = enumDefinitions().map(cEnum).join('\n');
  const capabilityConstants = schema.capabilities.map((capability) =>
    `#define MACTYPE_RENDERER_CAPABILITY_${screaming(capability.name)} ` +
    `(UINT64_C(1) << ${capability.bit}U)`,
  ).join('\n');
  const capabilityMask = schema.capabilities
    .map((capability) => `MACTYPE_RENDERER_CAPABILITY_${screaming(capability.name)}`)
    .join(' | ');
  const capabilityCases = schema.capabilities.map((capability) =>
    `    case MACTYPE_RENDERER_CAPABILITY_${screaming(capability.name)}:\n` +
    `        return "${capability.code}";`,
  ).join('\n');
  const bindingFields = binding.fields.map(cField).join('\n');
  const evidenceFields = evidence.fields.map(cField).join('\n');
  return `// Generated by scripts/generate-renderer-activation-contract.mjs. Do not edit.\n` +
`#pragma once\n\n` +
`#include <stddef.h>\n` +
`#include <stdint.h>\n` +
`#include <string.h>\n\n` +
`#define MACTYPE_RENDERER_ACTIVATION_SCHEMA_VERSION ${contract.schemaVersion}U\n` +
`#define MACTYPE_RENDERER_ACTIVATION_QUERY_EXPORT "${contract.queryExport}"\n` +
`#define MACTYPE_RENDERER_ACTIVATION_EVIDENCE_V1_SIZE ${contract.evidenceSize}U\n` +
`#define MACTYPE_RENDERER_RUNTIME_GENERATION_CHARS ${contract.runtimeGenerationCanonicalChars}U\n` +
`#define MACTYPE_RENDERER_RUNTIME_GENERATION_BYTES ${contract.runtimeGenerationTextBytes}U\n` +
`#define MACTYPE_RENDERER_PROFILE_DIGEST_CHARS ${contract.profileDigestCanonicalChars}U\n` +
`#define MACTYPE_RENDERER_PROFILE_DIGEST_BYTES ${contract.profileDigestTextBytes}U\n\n` +
`${enums}\n` +
`${capabilityConstants}\n` +
`#define MACTYPE_RENDERER_CAPABILITY_KNOWN_MASK (${capabilityMask})\n\n` +
`static inline const char* MacTypeRendererCapabilityCode(uint64_t capability)\n` +
`{\n    switch (capability)\n    {\n${capabilityCases}\n    default:\n        return 0;\n    }\n}\n\n` +
`typedef struct MacTypeRendererRuntimeBindingV1\n` +
`{\n${bindingFields}\n} MacTypeRendererRuntimeBindingV1;\n\n` +
`#if defined(_MSC_VER)\n` +
`#define MACTYPE_RENDERER_ALIGN_8 __declspec(align(8))\n` +
`#elif defined(__GNUC__)\n` +
`#define MACTYPE_RENDERER_ALIGN_8 __attribute__((aligned(8)))\n` +
`#else\n` +
`#define MACTYPE_RENDERER_ALIGN_8\n` +
`#endif\n\n` +
`typedef struct MACTYPE_RENDERER_ALIGN_8 MacTypeRendererActivationEvidenceV1\n` +
`{\n${evidenceFields}\n} MacTypeRendererActivationEvidenceV1;\n\n` +
`#if defined(_WIN32)\n` +
`#define MACTYPE_RENDERER_CALL __stdcall\n` +
`#else\n` +
`#define MACTYPE_RENDERER_CALL\n` +
`#endif\n\n` +
`typedef uint32_t (MACTYPE_RENDERER_CALL *MacTypeRendererActivationQueryV1)(\n` +
`    MacTypeRendererActivationEvidenceV1* in_out_evidence);\n\n` +
`#if defined(__cplusplus)\n` +
`#define MACTYPE_RENDERER_STATIC_ASSERT(condition, message) static_assert(condition, message)\n` +
`#define MACTYPE_RENDERER_ALIGNOF(type) alignof(type)\n` +
`#elif defined(_MSC_VER)\n` +
`#define MACTYPE_RENDERER_JOIN_INNER(left, right) left##right\n` +
`#define MACTYPE_RENDERER_JOIN(left, right) MACTYPE_RENDERER_JOIN_INNER(left, right)\n` +
`#define MACTYPE_RENDERER_STATIC_ASSERT(condition, message) \\\n` +
`    typedef char MACTYPE_RENDERER_JOIN(mactype_renderer_static_assert_, __LINE__)[(condition) ? 1 : -1]\n` +
`#define MACTYPE_RENDERER_ALIGNOF(type) __alignof(type)\n` +
`#else\n` +
`#define MACTYPE_RENDERER_STATIC_ASSERT(condition, message) _Static_assert(condition, message)\n` +
`#define MACTYPE_RENDERER_ALIGNOF(type) _Alignof(type)\n` +
`#endif\n\n` +
`${cStructAssertions('MacTypeRendererRuntimeBindingV1', binding)}\n` +
`${cStructAssertions('MacTypeRendererActivationEvidenceV1', evidence)}\n\n` +
`static inline int MacTypeRendererIsLowerHex(char value)\n` +
`{\n    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');\n}\n\n` +
`static inline int MacTypeRendererRuntimeGenerationIsCanonical(\n` +
`    const char value[MACTYPE_RENDERER_RUNTIME_GENERATION_BYTES])\n` +
`{\n    size_t index;\n    for (index = 0; index < MACTYPE_RENDERER_RUNTIME_GENERATION_CHARS; ++index)\n` +
`    {\n        if (!MacTypeRendererIsLowerHex(value[index]))\n            return 0;\n    }\n    return value[MACTYPE_RENDERER_RUNTIME_GENERATION_CHARS] == '\\0';\n}\n\n` +
`static inline int MacTypeRendererProfileDigestIsCanonical(\n` +
`    const char value[MACTYPE_RENDERER_PROFILE_DIGEST_BYTES])\n` +
`{\n    static const char prefix[] = "${contract.profileDigestPrefix}";\n    size_t index;\n    for (index = 0; index < sizeof(prefix) - 1U; ++index)\n` +
`    {\n        if (value[index] != prefix[index])\n            return 0;\n    }\n    for (; index < MACTYPE_RENDERER_PROFILE_DIGEST_CHARS; ++index)\n` +
`    {\n        if (!MacTypeRendererIsLowerHex(value[index]))\n            return 0;\n    }\n    return value[MACTYPE_RENDERER_PROFILE_DIGEST_CHARS] == '\\0';\n}\n\n` +
`static inline int MacTypeRendererBytesAreZero(const uint8_t* value, size_t size)\n` +
`{\n    size_t index;\n    for (index = 0; index < size; ++index)\n` +
`    {\n        if (value[index] != 0U)\n            return 0;\n    }\n    return 1;\n}\n\n` +
`static inline int MacTypeRendererBindingIsCanonical(\n` +
`    const MacTypeRendererRuntimeBindingV1* binding)\n` +
`{\n    return binding != 0 &&\n` +
`           MacTypeRendererRuntimeGenerationIsCanonical(binding->runtime_generation_id) &&\n` +
`           MacTypeRendererProfileDigestIsCanonical(binding->profile_digest);\n}\n\n` +
`static inline int MacTypeRendererActivationCommonIsValid(\n` +
`    const MacTypeRendererActivationEvidenceV1* evidence)\n` +
`{\n    return evidence != 0 &&\n` +
`           evidence->struct_size == MACTYPE_RENDERER_ACTIVATION_EVIDENCE_V1_SIZE &&\n` +
`           evidence->schema_version == MACTYPE_RENDERER_ACTIVATION_SCHEMA_VERSION &&\n` +
`           MacTypeRendererArchitectureCode(evidence->architecture) != 0 &&\n` +
`           evidence->pid != 0U && evidence->creation_time != 0U &&\n` +
`           evidence->session_id != 0U && evidence->reserved0 == 0U &&\n` +
`           evidence->reserved1 == 0U &&\n` +
`           MacTypeRendererBytesAreZero(evidence->reserved_alignment,\n` +
`                                       sizeof(evidence->reserved_alignment)) &&\n` +
`           MacTypeRendererBytesAreZero(evidence->reserved, sizeof(evidence->reserved)) &&\n` +
`           MacTypeRendererBindingIsCanonical(&evidence->binding);\n}\n\n` +
`static inline int MacTypeValidateRendererActivationRequestV1(\n` +
`    const MacTypeRendererActivationEvidenceV1* request)\n` +
`{\n    return MacTypeRendererActivationCommonIsValid(request) &&\n` +
`           request->reason == MACTYPE_RENDERER_REASON_NONE &&\n` +
`           (request->module_load == MACTYPE_RENDERER_MODULE_LOAD_LOADED_BY_REQUEST ||\n` +
`            request->module_load == MACTYPE_RENDERER_MODULE_LOAD_ALREADY_LOADED) &&\n` +
`           request->disposition == 0U && request->lifecycle_revision == 0U &&\n` +
`           request->capability_active == 0U && request->capability_unavailable == 0U &&\n` +
`           request->capability_failed == 0U &&\n` +
`           MacTypeRendererBytesAreZero((const uint8_t*)request->effective_profile_digest,\n` +
`                                       sizeof(request->effective_profile_digest));\n}\n\n` +
`static inline int MacTypeValidateRendererActivationEvidenceV1(\n` +
`    const MacTypeRendererActivationEvidenceV1* evidence)\n` +
`{\n    uint64_t all_capabilities;\n    int effective_is_empty;\n    int effective_is_canonical;\n    if (!MacTypeRendererActivationCommonIsValid(evidence) ||\n` +
`        MacTypeRendererModuleLoadCode(evidence->module_load) == 0 ||\n` +
`        MacTypeRendererActivationDispositionCode(evidence->disposition) == 0 ||\n` +
`        MacTypeRendererActivationReasonCode(evidence->reason) == 0)\n` +
`        return 0;\n` +
`    all_capabilities = evidence->capability_active | evidence->capability_unavailable |\n` +
`                       evidence->capability_failed;\n` +
`    if ((all_capabilities & ~MACTYPE_RENDERER_CAPABILITY_KNOWN_MASK) != 0U ||\n` +
`        (evidence->capability_active & evidence->capability_unavailable) != 0U ||\n` +
`        (evidence->capability_active & evidence->capability_failed) != 0U ||\n` +
`        (evidence->capability_unavailable & evidence->capability_failed) != 0U)\n` +
`        return 0;\n` +
`    effective_is_empty = MacTypeRendererBytesAreZero(\n` +
`        (const uint8_t*)evidence->effective_profile_digest,\n` +
`        sizeof(evidence->effective_profile_digest));\n` +
`    effective_is_canonical = !effective_is_empty &&\n` +
`        MacTypeRendererProfileDigestIsCanonical(evidence->effective_profile_digest);\n` +
`    if (!effective_is_empty && !effective_is_canonical)\n` +
`        return 0;\n` +
`    if (evidence->disposition == MACTYPE_RENDERER_DISPOSITION_ACTIVE)\n` +
`    {\n        return (evidence->module_load == MACTYPE_RENDERER_MODULE_LOAD_LOADED_BY_REQUEST ||\n` +
`                evidence->module_load == MACTYPE_RENDERER_MODULE_LOAD_ALREADY_LOADED) &&\n` +
`               evidence->reason == MACTYPE_RENDERER_REASON_NONE &&\n` +
`               evidence->lifecycle_revision != 0U &&\n` +
`               effective_is_canonical &&\n` +
`               memcmp(evidence->effective_profile_digest, evidence->binding.profile_digest,\n` +
`                      MACTYPE_RENDERER_PROFILE_DIGEST_BYTES) == 0;\n    }\n` +
`    if (evidence->disposition == MACTYPE_RENDERER_DISPOSITION_QUIET_SKIP)\n` +
`    {\n        return (evidence->module_load == MACTYPE_RENDERER_MODULE_LOAD_LOADED_BY_REQUEST ||\n` +
`                evidence->module_load == MACTYPE_RENDERER_MODULE_LOAD_ALREADY_LOADED) &&\n` +
`               evidence->reason != MACTYPE_RENDERER_REASON_NONE &&\n` +
`               evidence->lifecycle_revision != 0U && evidence->capability_active == 0U &&\n` +
`               evidence->capability_failed == 0U && effective_is_canonical &&\n` +
`               memcmp(evidence->effective_profile_digest, evidence->binding.profile_digest,\n` +
`                      MACTYPE_RENDERER_PROFILE_DIGEST_BYTES) == 0;\n    }\n` +
`    return evidence->disposition == MACTYPE_RENDERER_DISPOSITION_FAILED &&\n` +
`           evidence->reason != MACTYPE_RENDERER_REASON_NONE;\n}\n\n` +
`#undef MACTYPE_RENDERER_ALIGN_8\n` +
`#undef MACTYPE_RENDERER_ALIGNOF\n` +
`#undef MACTYPE_RENDERER_CALL\n` +
`#undef MACTYPE_RENDERER_JOIN\n` +
`#undef MACTYPE_RENDERER_JOIN_INNER\n` +
`#undef MACTYPE_RENDERER_STATIC_ASSERT\n`;
}

function rustEnum(definition) {
  const underlying = definition.wireType;
  const variants = definition.values.map((value) =>
    `    ${pascal(value.name)} = ${value.value},`,
  ).join('\n');
  const codes = definition.values.map((value) =>
    `            Self::${pascal(value.name)} => "${value.code}",`,
  ).join('\n');
  const fromValues = definition.values.map((value) =>
    `            ${value.value} => Ok(Self::${pascal(value.name)}),`,
  ).join('\n');
  const fromCodes = definition.values.map((value) =>
    `            "${value.code}" => Some(Self::${pascal(value.name)}),`,
  ).join('\n');
  return `#[repr(${underlying})]\n` +
    `#[derive(Clone, Copy, Debug, PartialEq, Eq)]\n` +
    `pub enum ${definition.rustName} {\n${variants}\n}\n\n` +
    `impl ${definition.rustName} {\n` +
    `    pub const fn code(self) -> &'static str {\n` +
    `        match self {\n${codes}\n        }\n    }\n\n` +
    `    pub fn from_code(code: &str) -> Option<Self> {\n` +
    `        match code {\n${fromCodes}\n            _ => None,\n        }\n    }\n}\n\n` +
    `impl TryFrom<${underlying}> for ${definition.rustName} {\n` +
    `    type Error = RendererActivationContractError;\n\n` +
    `    fn try_from(value: ${underlying}) -> Result<Self, Self::Error> {\n` +
    `        match value {\n${fromValues}\n` +
    `            _ => Err(RendererActivationContractError::Invalid${definition.rustName.slice('Renderer'.length)}),\n` +
    `        }\n    }\n}\n`;
}

function generateRust() {
  const contract = schema.contract;
  const evidence = schema.structs.activationEvidenceV1;
  const enums = enumDefinitions().map(rustEnum).join('\n');
  const capabilityVariants = schema.capabilities.map((capability) =>
    `    ${pascal(capability.name)} = 1_u64 << ${capability.bit},`,
  ).join('\n');
  const capabilityCodes = schema.capabilities.map((capability) =>
    `            Self::${pascal(capability.name)} => "${capability.code}",`,
  ).join('\n');
  const capabilityFromCode = schema.capabilities.map((capability) =>
    `            "${capability.code}" => Some(Self::${pascal(capability.name)}),`,
  ).join('\n');
  const knownMask = schema.capabilities.map((capability) =>
    `RendererCapability::${pascal(capability.name)} as u64`,
  ).join(' | ');
  const offsets = Object.fromEntries(evidence.fields.map((field) => [field.name, field.offset]));
  const reservedAlignmentBytes = evidence.fields.find(
    (field) => field.name === 'reserved_alignment',
  ).count;
  const offsetAssertions = evidence.fields.map((field) =>
    `    assert!(core::mem::offset_of!(RendererActivationEvidenceV1, ${field.name}) == ${field.offset});`,
  ).join('\n');
  return `// Generated by scripts/generate-renderer-activation-contract.mjs. Do not edit.\n\n` +
`use std::fmt;\n\n` +
`pub const RENDERER_ACTIVATION_SCHEMA_VERSION: u16 = ${contract.schemaVersion};\n` +
`pub const RENDERER_ACTIVATION_QUERY_EXPORT: &str = "${contract.queryExport}";\n` +
`pub const RENDERER_ACTIVATION_EVIDENCE_V1_SIZE: usize = ${contract.evidenceSize};\n` +
`pub const RUNTIME_GENERATION_CANONICAL_CHARS: usize = ${contract.runtimeGenerationCanonicalChars};\n` +
`pub const RUNTIME_GENERATION_TEXT_BYTES: usize = ${contract.runtimeGenerationTextBytes};\n` +
`pub const PROFILE_DIGEST_CANONICAL_CHARS: usize = ${contract.profileDigestCanonicalChars};\n` +
`pub const PROFILE_DIGEST_TEXT_BYTES: usize = ${contract.profileDigestTextBytes};\n` +
`pub const PROFILE_DIGEST_PREFIX: &str = "${contract.profileDigestPrefix}";\n\n` +
`${enums}\n\n` +
`#[repr(u64)]\n` +
`#[derive(Clone, Copy, Debug, PartialEq, Eq)]\n` +
`pub enum RendererCapability {\n${capabilityVariants}\n}\n\n` +
`impl RendererCapability {\n` +
`    pub const fn bit(self) -> u64 {\n        self as u64\n    }\n\n` +
`    pub const fn code(self) -> &'static str {\n` +
`        match self {\n${capabilityCodes}\n        }\n    }\n\n` +
`    pub fn from_code(code: &str) -> Option<Self> {\n` +
`        match code {\n${capabilityFromCode}\n            _ => None,\n        }\n    }\n}\n\n` +
`pub const RENDERER_CAPABILITY_KNOWN_MASK: u64 = ${knownMask};\n\n` +
`#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]\n` +
`pub struct RendererCapabilitySet(u64);\n\n` +
`impl RendererCapabilitySet {\n` +
`    pub fn from_bits(bits: u64) -> Result<Self, RendererActivationContractError> {\n` +
`        if bits & !RENDERER_CAPABILITY_KNOWN_MASK != 0 {\n` +
`            Err(RendererActivationContractError::InvalidCapabilityMask)\n` +
`        } else {\n            Ok(Self(bits))\n        }\n    }\n\n` +
`    pub const fn bits(self) -> u64 {\n        self.0\n    }\n\n` +
`    pub const fn contains(self, capability: RendererCapability) -> bool {\n` +
`        self.0 & capability.bit() != 0\n    }\n}\n\n` +
`#[repr(transparent)]\n` +
`#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]\n` +
`pub struct RuntimeGenerationId([u8; RUNTIME_GENERATION_TEXT_BYTES]);\n\n` +
`impl RuntimeGenerationId {\n` +
`    pub fn parse(value: &str) -> Result<Self, RendererActivationContractError> {\n` +
`        if !canonical_runtime_generation(value.as_bytes()) {\n` +
`            return Err(RendererActivationContractError::InvalidRuntimeGenerationId);\n` +
`        }\n        let mut wire = [0; RUNTIME_GENERATION_TEXT_BYTES];\n` +
`        wire[..RUNTIME_GENERATION_CANONICAL_CHARS].copy_from_slice(value.as_bytes());\n` +
`        Ok(Self(wire))\n    }\n\n` +
`    pub fn as_str(&self) -> &str {\n` +
`        std::str::from_utf8(&self.0[..RUNTIME_GENERATION_CANONICAL_CHARS])\n` +
`            .expect("a validated runtime generation is ASCII")\n    }\n\n` +
`    pub const fn as_wire_bytes(&self) -> &[u8; RUNTIME_GENERATION_TEXT_BYTES] {\n` +
`        &self.0\n    }\n}\n\n` +
`impl fmt::Display for RuntimeGenerationId {\n` +
`    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {\n` +
`        formatter.write_str(self.as_str())\n    }\n}\n\n` +
`#[repr(transparent)]\n` +
`#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]\n` +
`pub struct ProfileDigest([u8; PROFILE_DIGEST_TEXT_BYTES]);\n\n` +
`impl ProfileDigest {\n` +
`    pub fn parse(value: &str) -> Result<Self, RendererActivationContractError> {\n` +
`        if !canonical_profile_digest(value.as_bytes()) {\n` +
`            return Err(RendererActivationContractError::InvalidProfileDigest);\n` +
`        }\n        let mut wire = [0; PROFILE_DIGEST_TEXT_BYTES];\n` +
`        wire[..PROFILE_DIGEST_CANONICAL_CHARS].copy_from_slice(value.as_bytes());\n` +
`        Ok(Self(wire))\n    }\n\n` +
`    pub fn as_str(&self) -> &str {\n` +
`        std::str::from_utf8(&self.0[..PROFILE_DIGEST_CANONICAL_CHARS])\n` +
`            .expect("a validated profile digest is ASCII")\n    }\n\n` +
`    pub const fn as_wire_bytes(&self) -> &[u8; PROFILE_DIGEST_TEXT_BYTES] {\n` +
`        &self.0\n    }\n}\n\n` +
`impl fmt::Display for ProfileDigest {\n` +
`    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {\n` +
`        formatter.write_str(self.as_str())\n    }\n}\n\n` +
`#[repr(C)]\n` +
`#[derive(Clone, Copy, Debug, PartialEq, Eq)]\n` +
`pub struct RendererRuntimeBinding {\n` +
`    runtime_generation_id: RuntimeGenerationId,\n` +
`    profile_digest: ProfileDigest,\n}\n\n` +
`impl RendererRuntimeBinding {\n` +
`    pub const fn new(runtime_generation_id: RuntimeGenerationId, profile_digest: ProfileDigest) -> Self {\n` +
`        Self {\n            runtime_generation_id,\n            profile_digest,\n        }\n    }\n\n` +
`    pub const fn runtime_generation_id(&self) -> &RuntimeGenerationId {\n` +
`        &self.runtime_generation_id\n    }\n\n` +
`    pub const fn profile_digest(&self) -> &ProfileDigest {\n` +
`        &self.profile_digest\n    }\n\n` +
`    fn validate(&self) -> Result<(), RendererActivationContractError> {\n` +
`        if !canonical_runtime_generation_wire(&self.runtime_generation_id.0) {\n` +
`            return Err(RendererActivationContractError::InvalidRuntimeGenerationId);\n` +
`        }\n        if !canonical_profile_digest_wire(&self.profile_digest.0) {\n` +
`            return Err(RendererActivationContractError::InvalidProfileDigest);\n` +
`        }\n        Ok(())\n    }\n}\n\n` +
`#[derive(Clone, Copy, Debug, PartialEq, Eq)]\n` +
`pub struct RendererProcessIdentity {\n` +
`    pub pid: u32,\n` +
`    pub creation_time: u64,\n` +
`    pub session_id: u32,\n` +
`    pub architecture: RendererArchitecture,\n}\n\n` +
`#[repr(C, align(8))]\n` +
`#[derive(Clone, Copy, Debug, PartialEq, Eq)]\n` +
`pub struct RendererActivationEvidenceV1 {\n` +
`    pub struct_size: u32,\n` +
`    pub schema_version: u16,\n` +
`    pub reason: u16,\n` +
`    pub architecture: u8,\n` +
`    pub module_load: u8,\n` +
`    pub disposition: u8,\n` +
`    pub reserved0: u8,\n` +
`    pub pid: u32,\n` +
`    pub session_id: u32,\n` +
`    pub reserved1: u32,\n` +
`    pub creation_time: u64,\n` +
`    pub binding: RendererRuntimeBinding,\n` +
`    pub effective_profile_digest: [u8; PROFILE_DIGEST_TEXT_BYTES],\n` +
`    pub reserved_alignment: [u8; ${reservedAlignmentBytes}],\n` +
`    pub lifecycle_revision: u64,\n` +
`    pub capability_active: u64,\n` +
`    pub capability_unavailable: u64,\n` +
`    pub capability_failed: u64,\n` +
`    pub reserved: [u8; ${contract.reservedBytes}],\n}\n\n` +
`impl RendererActivationEvidenceV1 {\n` +
`    pub fn request(\n` +
`        identity: RendererProcessIdentity,\n` +
`        binding: RendererRuntimeBinding,\n` +
`        module_load: RendererModuleLoad,\n` +
`    ) -> Self {\n` +
`        Self {\n` +
`            struct_size: RENDERER_ACTIVATION_EVIDENCE_V1_SIZE as u32,\n` +
`            schema_version: RENDERER_ACTIVATION_SCHEMA_VERSION,\n` +
`            reason: RendererActivationReason::None as u16,\n` +
`            architecture: identity.architecture as u8,\n` +
`            module_load: module_load as u8,\n` +
`            disposition: 0,\n            reserved0: 0,\n            pid: identity.pid,\n` +
`            session_id: identity.session_id,\n            reserved1: 0,\n` +
`            creation_time: identity.creation_time,\n            binding,\n` +
`            effective_profile_digest: [0; PROFILE_DIGEST_TEXT_BYTES],\n` +
`            reserved_alignment: [0; ${reservedAlignmentBytes}],\n            lifecycle_revision: 0,\n` +
`            capability_active: 0,\n            capability_unavailable: 0,\n` +
`            capability_failed: 0,\n            reserved: [0; ${contract.reservedBytes}],\n        }\n    }\n\n` +
`    pub fn validate_request(&self) -> Result<(), RendererActivationContractError> {\n` +
`        self.validate_common()?;\n` +
`        if self.reason != RendererActivationReason::None as u16 ||\n` +
`            !matches!(\n` +
`                RendererModuleLoad::try_from(self.module_load),\n` +
`                Ok(RendererModuleLoad::LoadedByRequest) | Ok(RendererModuleLoad::AlreadyLoaded)\n` +
`            ) ||\n` +
`            self.disposition != 0 || self.lifecycle_revision != 0 ||\n` +
`            self.capability_active != 0 || self.capability_unavailable != 0 ||\n` +
`            self.capability_failed != 0 || !all_zero(&self.effective_profile_digest)\n` +
`        {\n            return Err(RendererActivationContractError::NonzeroRequestOutput);\n        }\n` +
`        Ok(())\n    }\n\n` +
`    pub fn validate(&self) -> Result<(), RendererActivationContractError> {\n` +
`        self.validate_common()?;\n` +
`        let module_load = RendererModuleLoad::try_from(self.module_load)?;\n` +
`        let disposition = RendererActivationDisposition::try_from(self.disposition)?;\n` +
`        let reason = RendererActivationReason::try_from(self.reason)?;\n` +
`        let all_capabilities = self.capability_active | self.capability_unavailable |\n` +
`            self.capability_failed;\n` +
`        if all_capabilities & !RENDERER_CAPABILITY_KNOWN_MASK != 0 {\n` +
`            return Err(RendererActivationContractError::InvalidCapabilityMask);\n        }\n` +
`        if self.capability_active & self.capability_unavailable != 0 ||\n` +
`            self.capability_active & self.capability_failed != 0 ||\n` +
`            self.capability_unavailable & self.capability_failed != 0\n` +
`        {\n            return Err(RendererActivationContractError::OverlappingCapabilities);\n        }\n` +
`        let effective = self.effective_profile_digest()?;\n` +
`        let binding_matches = effective.as_ref() == Some(self.binding.profile_digest());\n` +
`        let consistent = match disposition {\n` +
`            RendererActivationDisposition::Active => {\n` +
`                matches!(module_load, RendererModuleLoad::LoadedByRequest | RendererModuleLoad::AlreadyLoaded) &&\n` +
`                    reason == RendererActivationReason::None &&\n` +
`                    self.lifecycle_revision != 0 &&\n` +
`                    binding_matches\n            }\n` +
`            RendererActivationDisposition::QuietSkip => {\n` +
`                matches!(module_load, RendererModuleLoad::LoadedByRequest | RendererModuleLoad::AlreadyLoaded) &&\n` +
`                    reason != RendererActivationReason::None &&\n` +
`                    self.lifecycle_revision != 0 && self.capability_active == 0 &&\n` +
`                    self.capability_failed == 0 && binding_matches\n            }\n` +
`            RendererActivationDisposition::Failed => reason != RendererActivationReason::None,\n` +
`        };\n` +
`        if !consistent {\n` +
`            return Err(RendererActivationContractError::InconsistentEvidence);\n        }\n` +
`        Ok(())\n    }\n\n` +
`    pub fn identity(&self) -> Result<RendererProcessIdentity, RendererActivationContractError> {\n` +
`        Ok(RendererProcessIdentity {\n` +
`            pid: self.pid,\n            creation_time: self.creation_time,\n` +
`            session_id: self.session_id,\n` +
`            architecture: RendererArchitecture::try_from(self.architecture)?,\n        })\n    }\n\n` +
`    pub fn disposition(&self) -> Result<RendererActivationDisposition, RendererActivationContractError> {\n` +
`        RendererActivationDisposition::try_from(self.disposition)\n    }\n\n` +
`    pub fn reason(&self) -> Result<RendererActivationReason, RendererActivationContractError> {\n` +
`        RendererActivationReason::try_from(self.reason)\n    }\n\n` +
`    pub fn effective_profile_digest(\n` +
`        &self,\n    ) -> Result<Option<ProfileDigest>, RendererActivationContractError> {\n` +
`        if all_zero(&self.effective_profile_digest) {\n            return Ok(None);\n        }\n` +
`        if !canonical_profile_digest_wire(&self.effective_profile_digest) {\n` +
`            return Err(RendererActivationContractError::InvalidProfileDigest);\n        }\n` +
`        Ok(Some(ProfileDigest(self.effective_profile_digest)))\n    }\n\n` +
`    pub fn active_capabilities(&self) -> Result<RendererCapabilitySet, RendererActivationContractError> {\n` +
`        RendererCapabilitySet::from_bits(self.capability_active)\n    }\n\n` +
`    pub fn unavailable_capabilities(&self) -> Result<RendererCapabilitySet, RendererActivationContractError> {\n` +
`        RendererCapabilitySet::from_bits(self.capability_unavailable)\n    }\n\n` +
`    pub fn failed_capabilities(&self) -> Result<RendererCapabilitySet, RendererActivationContractError> {\n` +
`        RendererCapabilitySet::from_bits(self.capability_failed)\n    }\n\n` +
`    pub fn to_wire_bytes(&self) -> [u8; RENDERER_ACTIVATION_EVIDENCE_V1_SIZE] {\n` +
`        let mut wire = [0; RENDERER_ACTIVATION_EVIDENCE_V1_SIZE];\n` +
`        put(&mut wire, ${offsets.struct_size}, &self.struct_size.to_le_bytes());\n` +
`        put(&mut wire, ${offsets.schema_version}, &self.schema_version.to_le_bytes());\n` +
`        put(&mut wire, ${offsets.reason}, &self.reason.to_le_bytes());\n` +
`        wire[${offsets.architecture}] = self.architecture;\n` +
`        wire[${offsets.module_load}] = self.module_load;\n` +
`        wire[${offsets.disposition}] = self.disposition;\n` +
`        wire[${offsets.reserved0}] = self.reserved0;\n` +
`        put(&mut wire, ${offsets.pid}, &self.pid.to_le_bytes());\n` +
`        put(&mut wire, ${offsets.session_id}, &self.session_id.to_le_bytes());\n` +
`        put(&mut wire, ${offsets.reserved1}, &self.reserved1.to_le_bytes());\n` +
`        put(&mut wire, ${offsets.creation_time}, &self.creation_time.to_le_bytes());\n` +
`        put(&mut wire, ${offsets.binding}, self.binding.runtime_generation_id.as_wire_bytes());\n` +
`        put(&mut wire, ${offsets.binding + contract.runtimeGenerationTextBytes}, self.binding.profile_digest.as_wire_bytes());\n` +
`        put(&mut wire, ${offsets.effective_profile_digest}, &self.effective_profile_digest);\n` +
`        put(&mut wire, ${offsets.reserved_alignment}, &self.reserved_alignment);\n` +
`        put(&mut wire, ${offsets.lifecycle_revision}, &self.lifecycle_revision.to_le_bytes());\n` +
`        put(&mut wire, ${offsets.capability_active}, &self.capability_active.to_le_bytes());\n` +
`        put(&mut wire, ${offsets.capability_unavailable}, &self.capability_unavailable.to_le_bytes());\n` +
`        put(&mut wire, ${offsets.capability_failed}, &self.capability_failed.to_le_bytes());\n` +
`        put(&mut wire, ${offsets.reserved}, &self.reserved);\n        wire\n    }\n\n` +
`    pub fn request_from_wire_bytes(bytes: &[u8]) -> Result<Self, RendererActivationContractError> {\n` +
`        let request = Self::decode_wire(bytes)?;\n        request.validate_request()?;\n        Ok(request)\n    }\n\n` +
`    pub fn from_wire_bytes(bytes: &[u8]) -> Result<Self, RendererActivationContractError> {\n` +
`        let evidence = Self::decode_wire(bytes)?;\n        evidence.validate()?;\n        Ok(evidence)\n    }\n\n` +
`    fn decode_wire(bytes: &[u8]) -> Result<Self, RendererActivationContractError> {\n` +
`        if bytes.len() != RENDERER_ACTIVATION_EVIDENCE_V1_SIZE {\n` +
`            return Err(RendererActivationContractError::InvalidWireSize);\n        }\n` +
`        let mut runtime_generation_id = [0; RUNTIME_GENERATION_TEXT_BYTES];\n` +
`        runtime_generation_id.copy_from_slice(&bytes[${offsets.binding}..${offsets.binding + contract.runtimeGenerationTextBytes}]);\n` +
`        let mut profile_digest = [0; PROFILE_DIGEST_TEXT_BYTES];\n` +
`        profile_digest.copy_from_slice(&bytes[${offsets.binding + contract.runtimeGenerationTextBytes}..${offsets.binding + contract.runtimeGenerationTextBytes + contract.profileDigestTextBytes}]);\n` +
`        let mut effective_profile_digest = [0; PROFILE_DIGEST_TEXT_BYTES];\n` +
`        effective_profile_digest.copy_from_slice(&bytes[${offsets.effective_profile_digest}..${offsets.effective_profile_digest + contract.profileDigestTextBytes}]);\n` +
`        let mut reserved_alignment = [0; ${reservedAlignmentBytes}];\n` +
`        reserved_alignment.copy_from_slice(&bytes[${offsets.reserved_alignment}..${offsets.reserved_alignment + reservedAlignmentBytes}]);\n` +
`        let mut reserved = [0; ${contract.reservedBytes}];\n` +
`        reserved.copy_from_slice(&bytes[${offsets.reserved}..${offsets.reserved + contract.reservedBytes}]);\n` +
`        Ok(Self {\n` +
`            struct_size: read_u32(bytes, ${offsets.struct_size}),\n` +
`            schema_version: read_u16(bytes, ${offsets.schema_version}),\n` +
`            reason: read_u16(bytes, ${offsets.reason}),\n` +
`            architecture: bytes[${offsets.architecture}],\n` +
`            module_load: bytes[${offsets.module_load}],\n` +
`            disposition: bytes[${offsets.disposition}],\n` +
`            reserved0: bytes[${offsets.reserved0}],\n` +
`            pid: read_u32(bytes, ${offsets.pid}),\n` +
`            session_id: read_u32(bytes, ${offsets.session_id}),\n` +
`            reserved1: read_u32(bytes, ${offsets.reserved1}),\n` +
`            creation_time: read_u64(bytes, ${offsets.creation_time}),\n` +
`            binding: RendererRuntimeBinding {\n` +
`                runtime_generation_id: RuntimeGenerationId(runtime_generation_id),\n` +
`                profile_digest: ProfileDigest(profile_digest),\n            },\n` +
`            effective_profile_digest,\n            reserved_alignment,\n` +
`            lifecycle_revision: read_u64(bytes, ${offsets.lifecycle_revision}),\n` +
`            capability_active: read_u64(bytes, ${offsets.capability_active}),\n` +
`            capability_unavailable: read_u64(bytes, ${offsets.capability_unavailable}),\n` +
`            capability_failed: read_u64(bytes, ${offsets.capability_failed}),\n` +
`            reserved,\n        })\n    }\n\n` +
`    fn validate_common(&self) -> Result<(), RendererActivationContractError> {\n` +
`        if self.struct_size as usize != RENDERER_ACTIVATION_EVIDENCE_V1_SIZE {\n` +
`            return Err(RendererActivationContractError::InvalidStructSize);\n        }\n` +
`        if self.schema_version != RENDERER_ACTIVATION_SCHEMA_VERSION {\n` +
`            return Err(RendererActivationContractError::UnsupportedVersion);\n        }\n` +
`        RendererArchitecture::try_from(self.architecture)?;\n` +
`        if self.pid == 0 || self.creation_time == 0 || self.session_id == 0 {\n` +
`            return Err(RendererActivationContractError::InvalidIdentity);\n        }\n` +
`        if self.reserved0 != 0 || self.reserved1 != 0 ||\n` +
`            !all_zero(&self.reserved_alignment) || !all_zero(&self.reserved)\n` +
`        {\n            return Err(RendererActivationContractError::NonzeroReserved);\n        }\n` +
`        self.binding.validate()\n    }\n}\n\n` +
`#[derive(Clone, Copy, Debug, PartialEq, Eq)]\n` +
`pub enum RendererActivationContractError {\n` +
`    InvalidWireSize,\n    InvalidStructSize,\n    UnsupportedVersion,\n` +
`    InvalidRuntimeGenerationId,\n    InvalidProfileDigest,\n` +
`    InvalidArchitecture,\n    InvalidModuleLoad,\n    InvalidActivationDisposition,\n` +
`    InvalidActivationReason,\n    InvalidIdentity,\n    NonzeroReserved,\n` +
`    NonzeroRequestOutput,\n    InvalidCapabilityMask,\n` +
`    OverlappingCapabilities,\n    InconsistentEvidence,\n}\n\n` +
`impl fmt::Display for RendererActivationContractError {\n` +
`    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {\n` +
`        write!(formatter, "renderer activation contract failed: {self:?}")\n    }\n}\n\n` +
`impl std::error::Error for RendererActivationContractError {}\n\n` +
`fn canonical_runtime_generation(value: &[u8]) -> bool {\n` +
`    value.len() == RUNTIME_GENERATION_CANONICAL_CHARS && value.iter().copied().all(lower_hex)\n}\n\n` +
`fn canonical_runtime_generation_wire(value: &[u8; RUNTIME_GENERATION_TEXT_BYTES]) -> bool {\n` +
`    canonical_runtime_generation(&value[..RUNTIME_GENERATION_CANONICAL_CHARS]) &&\n` +
`        value[RUNTIME_GENERATION_CANONICAL_CHARS] == 0\n}\n\n` +
`fn canonical_profile_digest(value: &[u8]) -> bool {\n` +
`    value.len() == PROFILE_DIGEST_CANONICAL_CHARS &&\n` +
`        value.starts_with(PROFILE_DIGEST_PREFIX.as_bytes()) &&\n` +
`        value[PROFILE_DIGEST_PREFIX.len()..].iter().copied().all(lower_hex)\n}\n\n` +
`fn canonical_profile_digest_wire(value: &[u8; PROFILE_DIGEST_TEXT_BYTES]) -> bool {\n` +
`    canonical_profile_digest(&value[..PROFILE_DIGEST_CANONICAL_CHARS]) &&\n` +
`        value[PROFILE_DIGEST_CANONICAL_CHARS] == 0\n}\n\n` +
`fn lower_hex(value: u8) -> bool {\n` +
`    value.is_ascii_digit() || (b'a'..=b'f').contains(&value)\n}\n\n` +
`fn all_zero(value: &[u8]) -> bool {\n    value.iter().all(|byte| *byte == 0)\n}\n\n` +
`fn put<const SIZE: usize>(wire: &mut [u8; SIZE], offset: usize, value: &[u8]) {\n` +
`    wire[offset..offset + value.len()].copy_from_slice(value);\n}\n\n` +
`fn read_u16(bytes: &[u8], offset: usize) -> u16 {\n` +
`    u16::from_le_bytes(bytes[offset..offset + 2].try_into().expect("fixed wire range"))\n}\n\n` +
`fn read_u32(bytes: &[u8], offset: usize) -> u32 {\n` +
`    u32::from_le_bytes(bytes[offset..offset + 4].try_into().expect("fixed wire range"))\n}\n\n` +
`fn read_u64(bytes: &[u8], offset: usize) -> u64 {\n` +
`    u64::from_le_bytes(bytes[offset..offset + 8].try_into().expect("fixed wire range"))\n}\n\n` +
`const _: () = {\n` +
`    assert!(core::mem::size_of::<RendererRuntimeBinding>() == ${schema.structs.runtimeBinding.size});\n` +
`    assert!(core::mem::align_of::<RendererRuntimeBinding>() == ${schema.structs.runtimeBinding.alignment});\n` +
`    assert!(core::mem::size_of::<RendererActivationEvidenceV1>() == ${evidence.size});\n` +
`    assert!(core::mem::align_of::<RendererActivationEvidenceV1>() == ${evidence.alignment});\n` +
`${offsetAssertions}\n};\n`;
}

function emit(outputPath, content) {
  if (checkOnly) {
    if (!fs.existsSync(outputPath) || fs.readFileSync(outputPath, 'utf8') !== content) {
      fail(`generated output is stale: ${path.relative(root, outputPath)}`);
    }
    return;
  }
  fs.mkdirSync(path.dirname(outputPath), { recursive: true });
  fs.writeFileSync(outputPath, content);
}

function formatRust(content) {
  const formatted = spawnSync(
    'rustfmt',
    ['--edition', '2021', '--emit', 'stdout'],
    { input: content, encoding: 'utf8' },
  );
  if (formatted.status !== 0) {
    fail(`rustfmt failed while generating Rust output: ${formatted.stderr || formatted.error}`);
  }
  return formatted.stdout;
}

validateSchema();
emit(path.join(root, 'shared', 'renderer_activation_contract.h'), generateCpp());
emit(
  path.join(root, 'service-runtime', 'contract', 'src', 'renderer_activation.rs'),
  formatRust(generateRust()),
);
