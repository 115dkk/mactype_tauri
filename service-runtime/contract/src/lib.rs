#![forbid(unsafe_code)]

mod appinit;
mod broker;
mod console_process;
pub mod event_log;
mod health;
mod ini_policy;
mod manifest;
mod migration_pin;
mod paths;
mod private_freetype;
mod profile;
mod renderer_activation;
mod runtime_activation;
mod service_configuration;
mod unity;

pub use broker::{BrokerCommand, BrokerCommandError, parse_broker_command};
pub use console_process::ConsoleProcessPolicy;
pub use health::{
    ArchitectureInjectionTelemetry, ComponentReadiness, HEALTH_PROTOCOL_VERSION,
    HealthContractError, HealthReport, HealthState, InjectionArchitecture, InjectionSuccess,
    InjectionTelemetry, ReadinessReport, StructuredServiceError,
};
pub use manifest::{
    IMMUTABLE_RUNTIME_FILES, MAX_RUNTIME_FILE_BYTES, ManifestError, RUNTIME_MANIFEST_SCHEMA,
    VerifiedRuntimeManifest, runtime_generation_id, sha256_digest, verify_runtime_manifest,
};
pub use migration_pin::{
    MAX_MIGRATION_RUNTIME_PIN_BYTES, MAX_PINNED_RUNTIMES, MIGRATION_RUNTIME_PIN_SCHEMA,
    MigrationPinError, MigrationPinnedRuntime, MigrationRuntimePin,
};
pub use paths::{MachinePathError, MachinePaths};
pub use private_freetype::PrivateFreeTypePolicy;
pub use profile::{
    GenerationId, GenerationPointer, MAX_PROFILE_BYTES, MAX_PROFILE_POINTER_BYTES,
    PROFILE_POINTER_SCHEMA, ProfileCatalog, ProfileError, ProtectedRendererProfileError,
    SourceMetadata, validate_protected_renderer_profile,
};
pub use renderer_activation::{
    PROFILE_DIGEST_CANONICAL_CHARS, PROFILE_DIGEST_PREFIX, PROFILE_DIGEST_TEXT_BYTES,
    ProfileDigest, RENDERER_ACTIVATION_EVIDENCE_V1_SIZE, RENDERER_ACTIVATION_QUERY_EXPORT,
    RENDERER_ACTIVATION_SCHEMA_VERSION, RENDERER_CAPABILITY_KNOWN_MASK,
    RUNTIME_GENERATION_CANONICAL_CHARS, RUNTIME_GENERATION_TEXT_BYTES,
    RendererActivationContractError, RendererActivationDisposition, RendererActivationEvidenceV1,
    RendererActivationReason, RendererArchitecture, RendererCapability, RendererCapabilitySet,
    RendererModuleLoad, RendererProcessIdentity, RendererRuntimeBinding, RuntimeGenerationId,
};
pub use runtime_activation::{
    LEGACY_RUNTIME_ACTIVATION_SCHEMA, MAX_RUNTIME_ACTIVATION_RECEIPT_BYTES,
    MAX_RUNTIME_POINTER_BYTES, ParsedRuntimeActivationReceipt, RUNTIME_ACTIVATION_SCHEMA,
    RUNTIME_POINTER_SCHEMA, RuntimeActivationContractError, RuntimeActivationPhase,
    RuntimeActivationReceipt, RuntimeGenerationPointer, UNCOMMITTED_RUNTIME_ACTIVATION_SCHEMA,
    parse_runtime_activation_receipt, valid_runtime_version_component,
};
pub use service_configuration::{
    FIXED_ACCOUNT, FIXED_ERROR_CONTROL, FIXED_SERVICE_TYPE, FIXED_START_TYPE,
    ObservedServiceConfiguration, SERVICE_DISPLAY_NAME, ServiceConfigurationDrift,
    owned_service_identity, service_configuration_drift, service_image_matches_protected_contract,
    service_image_matches_protected_layout,
};
pub use unity::{UnityFontHookMode, UnityFontHookPolicy};

pub const SERVICE_NAME: &str = "MacTypeControlCenter";
pub const LEGACY_SERVICE_NAME: &str = "MacType";
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
pub use appinit::{AppInitValueError, appinit_mactype_conflict};
