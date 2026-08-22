#![forbid(unsafe_code)]

use std::ffi::OsString;
use std::fmt;
use std::io;
use std::path::PathBuf;
use std::time::Duration;

use mactype_service_contract::{
    RendererActivationDisposition, RendererActivationEvidenceV1, RendererArchitecture,
    RendererModuleLoad, RendererRuntimeBinding, RENDERER_ACTIVATION_EVIDENCE_V1_SIZE,
};

use crate::observer::{
    BrokerDisposition, BrokerResult, InjectionBroker, InjectionRequest, ProcessArchitecture,
    ProcessIdentity,
};
use crate::protected_renderer_runtime::ProtectedRendererRuntime;
use crate::runtime_assets::ProtectedRuntimeAssets;

const HELPER_TIMEOUT: Duration = Duration::from_secs(20);
pub(crate) const MAX_HELPER_OUTPUT_BYTES: usize = 1536;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HelperInvocation {
    pub executable: PathBuf,
    pub target: ProcessIdentity,
    pub binding: RendererRuntimeBinding,
    pub timeout: Duration,
}

impl HelperInvocation {
    pub fn arguments_for_process_handle(&self, process_handle: usize) -> Vec<OsString> {
        vec![
            "--process-handle".into(),
            process_handle.to_string().into(),
            "--pid".into(),
            self.target.pid.to_string().into(),
            "--creation-time".into(),
            self.target.creation_time.to_string().into(),
            "--session-id".into(),
            self.target.session_id.to_string().into(),
            "--generation-id".into(),
            self.binding.runtime_generation_id().as_str().into(),
            "--profile-digest".into(),
            self.binding.profile_digest().as_str().into(),
        ]
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HelperOutput {
    pub exit_code: i32,
    pub stdout: Vec<u8>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HelperLaunchStage {
    BeforeResume,
    AfterResume,
}

#[derive(Debug)]
pub struct HelperLaunchError {
    stage: HelperLaunchStage,
    source: io::Error,
}

impl HelperLaunchError {
    pub fn new(stage: HelperLaunchStage, source: io::Error) -> Self {
        Self { stage, source }
    }

    pub fn after_resume(source: io::Error) -> Self {
        Self::new(HelperLaunchStage::AfterResume, source)
    }

    pub const fn stage(&self) -> HelperLaunchStage {
        self.stage
    }

    pub fn kind(&self) -> io::ErrorKind {
        self.source.kind()
    }

    pub fn raw_os_error(&self) -> Option<i32> {
        self.source.raw_os_error()
    }
}

impl From<io::Error> for HelperLaunchError {
    fn from(source: io::Error) -> Self {
        Self::new(HelperLaunchStage::BeforeResume, source)
    }
}

impl fmt::Display for HelperLaunchError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.source.fmt(formatter)
    }
}

impl std::error::Error for HelperLaunchError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        Some(&self.source)
    }
}

pub trait HelperLauncher {
    fn launch(&self, invocation: &HelperInvocation) -> Result<HelperOutput, HelperLaunchError>;
}

impl<T> HelperLauncher for &T
where
    T: HelperLauncher + ?Sized,
{
    fn launch(&self, invocation: &HelperInvocation) -> Result<HelperOutput, HelperLaunchError> {
        (**self).launch(invocation)
    }
}

pub struct FixedHelperBroker<L> {
    assets: ProtectedRuntimeAssets,
    binding: RendererRuntimeBinding,
    launcher: L,
}

impl<L> FixedHelperBroker<L> {
    pub fn new(runtime: &ProtectedRendererRuntime, launcher: L) -> Self {
        Self {
            assets: runtime.assets().clone(),
            binding: runtime.binding(),
            launcher,
        }
    }

    fn invocation(&self, request: &InjectionRequest) -> HelperInvocation {
        let executable = match request.identity.architecture {
            ProcessArchitecture::X86 => self.assets.injector32(),
            ProcessArchitecture::X64 => self.assets.injector64(),
        };
        HelperInvocation {
            executable: executable.to_owned(),
            target: request.identity.clone(),
            binding: request.binding,
            timeout: HELPER_TIMEOUT,
        }
    }
}

impl<L> InjectionBroker for FixedHelperBroker<L>
where
    L: HelperLauncher,
{
    fn verify_ready(
        &self,
        architecture: ProcessArchitecture,
    ) -> Result<(), mactype_service_contract::StructuredServiceError> {
        let helper = match architecture {
            ProcessArchitecture::X86 => self.assets.injector32(),
            ProcessArchitecture::X64 => self.assets.injector64(),
        };
        if helper.is_file() && helper.parent() == Some(self.assets.root()) {
            Ok(())
        } else {
            Err(mactype_service_contract::StructuredServiceError {
                code: "runtime-helper-unavailable".to_owned(),
                message: "the fixed helper is not ready in the protected runtime generation"
                    .to_owned(),
                win32_error: None,
            })
        }
    }

    fn inject(&self, request: &InjectionRequest) -> BrokerResult {
        if request.binding.runtime_generation_id() != self.assets.generation_id() {
            return invalid_response("runtime-generation-mismatch", None);
        }
        if request.binding.profile_digest() != self.binding.profile_digest() {
            return invalid_response("profile-digest-mismatch", None);
        }
        let invocation = self.invocation(request);
        match self.launcher.launch(&invocation) {
            Ok(output) => parse_output(request, output),
            Err(error)
                if error.stage() == HelperLaunchStage::BeforeResume
                    && error.kind() == io::ErrorKind::Interrupted =>
            {
                BrokerResult::new(
                    BrokerDisposition::Cancelled,
                    "helper-cancelled-service-stop",
                    error.raw_os_error().map(|code| code as u32),
                )
            }
            Err(error) => {
                let (disposition, code) = if error.stage() == HelperLaunchStage::BeforeResume {
                    (
                        BrokerDisposition::Rejected,
                        "helper-launch-failed-before-resume",
                    )
                } else if error.kind() == io::ErrorKind::Interrupted {
                    (
                        BrokerDisposition::UncertainCleanup,
                        "helper-service-stop-cleanup-unknown",
                    )
                } else if error.kind() == io::ErrorKind::TimedOut {
                    (
                        BrokerDisposition::UncertainCleanup,
                        "helper-absolute-timeout-cleanup-unknown",
                    )
                } else {
                    (
                        BrokerDisposition::UncertainCleanup,
                        "helper-launch-failed-cleanup-unknown",
                    )
                };
                BrokerResult::new(
                    disposition,
                    code,
                    error.raw_os_error().map(|code| code as u32),
                )
            }
        }
    }
}

fn parse_output(request: &InjectionRequest, output: HelperOutput) -> BrokerResult {
    if output.stdout.len() > MAX_HELPER_OUTPUT_BYTES {
        return invalid_response("helper-response-too-large", None);
    }
    let value: serde_json::Value = match serde_json::from_slice(&output.stdout) {
        Ok(value) => value,
        Err(_) => return invalid_response("helper-response-invalid", None),
    };
    let object = match value.as_object() {
        Some(object) if object.len() == 10 => object,
        _ => return invalid_response("helper-response-invalid", None),
    };
    let status = object.get("status").and_then(serde_json::Value::as_str);
    let Some(code) = object
        .get("code")
        .and_then(serde_json::Value::as_str)
        .filter(|code| !code.is_empty())
    else {
        return invalid_response("helper-response-invalid", None);
    };
    let pid = object.get("pid").and_then(serde_json::Value::as_u64);
    let session = object.get("sessionId").and_then(serde_json::Value::as_u64);
    let generation = object
        .get("generationId")
        .and_then(serde_json::Value::as_str);
    let module = object.get("module").and_then(serde_json::Value::as_str);
    let windows_error = object
        .get("windowsError")
        .and_then(serde_json::Value::as_u64);
    let cleanup = object
        .get("cleanupComplete")
        .and_then(serde_json::Value::as_bool);
    let renderer_evidence = match object.get("rendererEvidence") {
        Some(value) => match decode_renderer_evidence(value) {
            Ok(evidence) => evidence,
            Err(()) => return invalid_response("renderer-evidence-invalid", None),
        },
        None => return invalid_response("helper-response-invalid", None),
    };
    let expected_module = match request.identity.architecture {
        ProcessArchitecture::X86 => "MacType.dll",
        ProcessArchitecture::X64 => "MacType64.dll",
    };
    if object
        .get("schemaVersion")
        .and_then(serde_json::Value::as_u64)
        != Some(2)
        || pid != Some(u64::from(request.identity.pid))
        || session != Some(u64::from(request.identity.session_id))
        || generation != Some(request.binding.runtime_generation_id().as_str())
        || module != Some(expected_module)
        || windows_error.map_or(true, |value| value > u64::from(u32::MAX))
        || cleanup.is_none()
    {
        return invalid_response("helper-response-invalid", None);
    }

    if let Some(evidence) = &renderer_evidence {
        let expected_architecture = match request.identity.architecture {
            ProcessArchitecture::X86 => RendererArchitecture::X86,
            ProcessArchitecture::X64 => RendererArchitecture::X64,
        };
        let expected_identity = mactype_service_contract::RendererProcessIdentity {
            pid: request.identity.pid,
            creation_time: request.identity.creation_time,
            session_id: request.identity.session_id,
            architecture: expected_architecture,
        };
        if evidence.identity().ok() != Some(expected_identity)
            || evidence.binding != request.binding
        {
            return invalid_response("renderer-evidence-mismatch", None);
        }
    }

    let expected_exit = match status {
        Some("injected" | "skipped") => 0,
        Some("rejected") => 2,
        Some("failed" | "integrity") => 3,
        Some("timeout") => 4,
        _ => return invalid_response("helper-response-invalid", None),
    };
    if output.exit_code != expected_exit {
        return invalid_response("helper-exit-mismatch", None);
    }
    let disposition = match classify_renderer_result(
        status.expect("validated helper status"),
        code,
        cleanup.expect("validated cleanup evidence"),
        renderer_evidence.as_ref(),
    ) {
        Ok(disposition) => disposition,
        Err(()) => return invalid_response("renderer-evidence-inconsistent", None),
    };
    BrokerResult::new(
        disposition,
        code,
        windows_error
            .filter(|value| *value != 0)
            .map(|value| value as u32),
    )
}

fn invalid_response(code: &str, win32_error: Option<u32>) -> BrokerResult {
    BrokerResult::new(BrokerDisposition::UncertainIntegrity, code, win32_error)
}

fn safe_to_retry(code: &str) -> bool {
    matches!(
        code,
        "session-unavailable"
            | "identity-unavailable"
            | "architecture-unavailable"
            | "module-inventory-unavailable"
    )
}

fn decode_renderer_evidence(
    value: &serde_json::Value,
) -> Result<Option<RendererActivationEvidenceV1>, ()> {
    if value.is_null() {
        return Ok(None);
    }
    let encoded = value.as_str().ok_or(())?;
    if encoded.len() != RENDERER_ACTIVATION_EVIDENCE_V1_SIZE * 2
        || !encoded
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return Err(());
    }
    let mut bytes = [0_u8; RENDERER_ACTIVATION_EVIDENCE_V1_SIZE];
    for (index, output) in bytes.iter_mut().enumerate() {
        *output = u8::from_str_radix(&encoded[index * 2..index * 2 + 2], 16).map_err(|_| ())?;
    }
    RendererActivationEvidenceV1::from_wire_bytes(&bytes)
        .map(Some)
        .map_err(|_| ())
}

fn classify_renderer_result(
    status: &str,
    code: &str,
    cleanup_complete: bool,
    evidence: Option<&RendererActivationEvidenceV1>,
) -> Result<BrokerDisposition, ()> {
    let evidence_semantics = evidence
        .map(|evidence| {
            Ok((
                evidence.disposition()?,
                RendererModuleLoad::try_from(evidence.module_load)?,
            ))
        })
        .transpose()
        .map_err(|_: mactype_service_contract::RendererActivationContractError| ())?;
    match (status, cleanup_complete, evidence_semantics) {
        (
            "injected",
            true,
            Some((
                RendererActivationDisposition::Active,
                RendererModuleLoad::LoadedByRequest | RendererModuleLoad::AlreadyLoaded,
            )),
        ) if code == "renderer-active" => Ok(BrokerDisposition::Injected),
        (
            "skipped",
            true,
            Some((
                RendererActivationDisposition::QuietSkip,
                RendererModuleLoad::LoadedByRequest | RendererModuleLoad::AlreadyLoaded,
            )),
        ) if code == evidence.ok_or(())?.reason().map_err(|_| ())?.code() => {
            Ok(BrokerDisposition::Skipped)
        }
        (
            "timeout",
            false,
            Some((RendererActivationDisposition::QuietSkip, RendererModuleLoad::LoadedByRequest)),
        ) => Ok(BrokerDisposition::UncertainCleanup),
        (
            "failed",
            false,
            Some((RendererActivationDisposition::Failed, RendererModuleLoad::LoadedByRequest)),
        ) if code == evidence.ok_or(())?.reason().map_err(|_| ())?.code() => {
            Ok(BrokerDisposition::UncertainCleanup)
        }
        (
            "failed",
            true,
            Some((RendererActivationDisposition::Failed, RendererModuleLoad::AlreadyLoaded)),
        ) if code == evidence.ok_or(())?.reason().map_err(|_| ())?.code() => {
            Ok(BrokerDisposition::Rejected)
        }
        (
            "timeout",
            false,
            Some((
                RendererActivationDisposition::Active
                | RendererActivationDisposition::QuietSkip
                | RendererActivationDisposition::Failed,
                RendererModuleLoad::AlreadyLoaded,
            )),
        ) if code == "renderer-reference-release-cleanup-unknown" => {
            Ok(BrokerDisposition::UncertainCleanup)
        }
        ("skipped", true, None) => Ok(BrokerDisposition::Skipped),
        ("rejected", true, None) => Ok(BrokerDisposition::Rejected),
        ("failed", true, None) if safe_to_retry(code) => Ok(BrokerDisposition::Retryable),
        ("failed", true, None) => Ok(BrokerDisposition::Rejected),
        ("integrity", true, None) => Ok(BrokerDisposition::UncertainIntegrity),
        ("failed" | "timeout" | "integrity", false, None) => {
            Ok(BrokerDisposition::UncertainCleanup)
        }
        _ => Err(()),
    }
}
