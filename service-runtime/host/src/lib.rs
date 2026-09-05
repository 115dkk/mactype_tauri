#![forbid(unsafe_code)]

mod control;
mod event_log;
mod file_health;
mod generated_unity_anticheat_catalog;
mod helper_broker;
mod injection_orchestrator;
mod observer;
mod orchestration_runtime;
mod profile_runtime;
mod protected_path;
mod protected_renderer_runtime;
mod runtime;
mod runtime_assets;
mod service_version;
mod session_event_queue;
mod startup_safety;
mod status;
mod target_validation;
#[cfg(windows)]
mod windows_helper_launcher;

#[cfg(windows)]
mod known_folders;
#[cfg(windows)]
mod named_pipe;
#[cfg(windows)]
mod scm;
#[cfg(windows)]
mod windows_process;
#[cfg(windows)]
mod windows_runtime;
#[cfg(windows)]
mod windows_startup_safety;
#[cfg(windows)]
mod windows_wmi;

pub use file_health::{CompositeHealthPublisher, FileHealthPublisher};
#[cfg(windows)]
pub use helper_broker::{
    FixedHelperBroker, HelperInvocation, HelperLaunchError, HelperLaunchStage, HelperLauncher,
    HelperOutput,
};
pub use injection_orchestrator::{
    DeferralPolicy, DeferredTarget, InjectionOrchestrator, ProcessAttemptRecord, ProcessOutcome,
    RetryPolicy, RetryScheduler, SessionChange, MAX_DEFERRED_TARGETS, MAX_TRACKED_PROCESS_RESULTS,
    TARGET_VANISHED_RESULT_CODE,
};
#[cfg(windows)]
pub use named_pipe::{NamedPipeHealthPublisher, HEALTH_PIPE_SECURITY_SDDL};
pub type ProcessOrchestrator<'a> = InjectionOrchestrator<'a>;
pub use observer::{
    subscribe_process_creation, BrokerDisposition, BrokerResult, InjectionBroker, InjectionRequest,
    ProcessArchitecture, ProcessEventSource, ProcessIdentity, MAX_BROKER_DIAGNOSTIC_CODE_BYTES,
    PROCESS_CREATION_QUERY,
};
pub use orchestration_runtime::{
    initialize_process_orchestration, initialize_process_orchestration_with_profile_policies,
    initialize_process_orchestration_with_unity_font_hook,
};
pub use protected_renderer_runtime::ProtectedRendererRuntime;
pub use runtime::{
    HealthPublisher, HostError, InitializedRuntime, RuntimeDriver, RuntimeHealthReporter,
    RuntimeInitializer, ServiceRuntime, StopSignal,
};
pub use runtime_assets::ProtectedRuntimeAssets;
pub use startup_safety::{LegacyServiceRuntimeState, StartupSafetySnapshot};
pub use status::{ScmState, ServiceStatus, StatusReporter, SERVICE_STOP_WAIT_HINT_MS};
pub use target_validation::{
    BinarySignaturePolicy, DeferralReason, DynamicCodePolicy, InspectionEvidence,
    PrivateFreeTypeClassification, ProcessInspection, ProcessInspectionError, ProcessInspector,
    ProcessSkipReason, ProcessTargetDecision, ProcessTargetValidator, TargetLifecycle,
    TargetLiveness, UnityProcessClassification,
};
#[cfg(windows)]
pub use windows_helper_launcher::WindowsHelperLauncher;
#[cfg(windows)]
pub use windows_process::WindowsProcessInspector;
#[cfg(windows)]
pub use windows_runtime::WindowsOpenServiceInitializer;
#[cfg(windows)]
pub use windows_startup_safety::WindowsStartupSafety;
#[cfg(windows)]
pub use windows_wmi::WmiProcessEventSource;

pub fn validate_host_arguments<I, S>(arguments: I) -> Result<(), &'static str>
where
    I: IntoIterator<Item = S>,
    S: AsRef<str>,
{
    let mut arguments = arguments.into_iter();
    match arguments.next() {
        None => Ok(()),
        Some(argument) if argument.as_ref() == "--service" && arguments.next().is_none() => Ok(()),
        Some(_) => Err("the service host accepts only the fixed --service switch"),
    }
}

#[cfg(windows)]
pub fn run_service_process() -> std::io::Result<()> {
    scm::run_dispatcher()
}

#[cfg(not(windows))]
pub fn run_service_process() -> std::io::Result<()> {
    Err(std::io::Error::new(
        std::io::ErrorKind::Unsupported,
        "the service host requires Windows SCM",
    ))
}
pub use control::{ServiceControl, ACCEPTED_CONTROL_MASK};
