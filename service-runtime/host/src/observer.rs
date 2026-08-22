use std::time::Duration;

use mactype_service_contract::{RendererRuntimeBinding, StructuredServiceError};

pub const PROCESS_CREATION_QUERY: &str = "SELECT * FROM Win32_ProcessStartTrace";
pub const MAX_BROKER_DIAGNOSTIC_CODE_BYTES: usize = 128;
pub trait ProcessEventSource {
    fn subscribe(&mut self, query: &str) -> Result<(), StructuredServiceError>;

    fn snapshot_pids(&mut self) -> Result<Vec<u32>, StructuredServiceError> {
        Ok(Vec::new())
    }

    fn next_pid(&mut self, timeout: Duration) -> Result<Option<u32>, StructuredServiceError>;
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProcessArchitecture {
    X86,
    X64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProcessIdentity {
    pub pid: u32,
    pub creation_time: u64,
    pub session_id: u32,
    pub architecture: ProcessArchitecture,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InjectionRequest {
    pub identity: ProcessIdentity,
    pub binding: RendererRuntimeBinding,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BrokerDisposition {
    Injected,
    Skipped,
    Rejected,
    Retryable,
    UncertainCleanup,
    UncertainIntegrity,
    Cancelled,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BrokerResult {
    pub disposition: BrokerDisposition,
    pub code: String,
    pub win32_error: Option<u32>,
}

impl BrokerResult {
    pub fn new(
        disposition: BrokerDisposition,
        code: impl Into<String>,
        win32_error: Option<u32>,
    ) -> Self {
        Self {
            disposition,
            code: code.into(),
            win32_error,
        }
        .into_bounded_evidence()
    }

    pub(crate) fn into_bounded_evidence(self) -> Self {
        if valid_diagnostic_code(&self.code) {
            self
        } else {
            Self {
                disposition: BrokerDisposition::UncertainIntegrity,
                code: "broker-diagnostic-evidence-invalid".to_owned(),
                win32_error: self.win32_error,
            }
        }
    }
}

fn valid_diagnostic_code(code: &str) -> bool {
    !code.is_empty()
        && code.len() <= MAX_BROKER_DIAGNOSTIC_CODE_BYTES
        && code
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'-'))
}

pub trait InjectionBroker {
    fn verify_ready(
        &self,
        architecture: ProcessArchitecture,
    ) -> Result<(), StructuredServiceError> {
        let _ = architecture;
        Err(service_error(
            "injector-readiness-unverified",
            "the injection broker did not verify a protected helper",
        ))
    }

    fn inject(&self, request: &InjectionRequest) -> BrokerResult;
}

fn service_error(code: &str, message: &str) -> StructuredServiceError {
    StructuredServiceError {
        code: code.to_owned(),
        message: message.to_owned(),
        win32_error: None,
    }
}

pub fn subscribe_process_creation(
    source: &mut dyn ProcessEventSource,
) -> Result<(), StructuredServiceError> {
    source.subscribe(PROCESS_CREATION_QUERY)
}
