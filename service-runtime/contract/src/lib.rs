#![forbid(unsafe_code)]

mod appinit;
mod broker;
mod health;
mod manifest;
mod migration_pin;
mod paths;
mod private_freetype;
mod profile;
mod renderer_activation;
mod runtime_activation;
mod unity;

pub use broker::{parse_broker_command, BrokerCommand, BrokerCommandError};
pub use health::{
    ArchitectureInjectionTelemetry, ComponentReadiness, HealthContractError, HealthReport,
    HealthState, InjectionArchitecture, InjectionSuccess, InjectionTelemetry, ReadinessReport,
    StructuredServiceError, HEALTH_PROTOCOL_VERSION,
};
pub use manifest::{
    runtime_generation_id, sha256_digest, verify_runtime_manifest, ManifestError,
    VerifiedRuntimeManifest, IMMUTABLE_RUNTIME_FILES, MAX_RUNTIME_FILE_BYTES,
    RUNTIME_MANIFEST_SCHEMA,
};
pub use migration_pin::{
    MigrationPinError, MigrationPinnedRuntime, MigrationRuntimePin,
    MAX_MIGRATION_RUNTIME_PIN_BYTES, MAX_PINNED_RUNTIMES, MIGRATION_RUNTIME_PIN_SCHEMA,
};
pub use paths::{MachinePathError, MachinePaths};
pub use private_freetype::PrivateFreeTypePolicy;
pub use profile::{
    validate_protected_renderer_profile, GenerationId, GenerationPointer, ProfileCatalog,
    ProfileError, ProtectedRendererProfileError, SourceMetadata, MAX_PROFILE_BYTES,
    PROFILE_POINTER_SCHEMA,
};
pub use renderer_activation::{
    ProfileDigest, RendererActivationContractError, RendererActivationDisposition,
    RendererActivationEvidenceV1, RendererActivationReason, RendererArchitecture,
    RendererCapability, RendererCapabilitySet, RendererModuleLoad, RendererProcessIdentity,
    RendererRuntimeBinding, RuntimeGenerationId, PROFILE_DIGEST_CANONICAL_CHARS,
    PROFILE_DIGEST_PREFIX, PROFILE_DIGEST_TEXT_BYTES, RENDERER_ACTIVATION_EVIDENCE_V1_SIZE,
    RENDERER_ACTIVATION_QUERY_EXPORT, RENDERER_ACTIVATION_SCHEMA_VERSION,
    RENDERER_CAPABILITY_KNOWN_MASK, RUNTIME_GENERATION_CANONICAL_CHARS,
    RUNTIME_GENERATION_TEXT_BYTES,
};
pub use runtime_activation::{
    parse_runtime_activation_receipt, valid_runtime_version_component,
    ParsedRuntimeActivationReceipt, RuntimeActivationContractError, RuntimeActivationPhase,
    RuntimeActivationReceipt, RuntimeGenerationPointer, LEGACY_RUNTIME_ACTIVATION_SCHEMA,
    MAX_RUNTIME_ACTIVATION_RECEIPT_BYTES, MAX_RUNTIME_POINTER_BYTES, RUNTIME_ACTIVATION_SCHEMA,
    RUNTIME_POINTER_SCHEMA, UNCOMMITTED_RUNTIME_ACTIVATION_SCHEMA,
};
pub use unity::{UnityFontHookMode, UnityFontHookPolicy};

pub const SERVICE_NAME: &str = "MacTypeControlCenter";
pub const CI_TEST_SERVICE_NAME: &str = "MacTypeControlCenterTest";
pub const HEALTH_PIPE_NAME: &str = r"\\.\pipe\MacTypeControlCenter.health.v1";
pub const CI_TEST_HEALTH_PIPE_NAME: &str = r"\\.\pipe\MacTypeControlCenterTest.health.v1";

pub const fn effective_service_name() -> &'static str {
    if cfg!(feature = "ci-test-adapter") {
        CI_TEST_SERVICE_NAME
    } else {
        SERVICE_NAME
    }
}

pub const fn effective_health_pipe_name() -> &'static str {
    if cfg!(feature = "ci-test-adapter") {
        CI_TEST_HEALTH_PIPE_NAME
    } else {
        HEALTH_PIPE_NAME
    }
}
pub use appinit::{appinit_mactype_conflict, AppInitValueError};
