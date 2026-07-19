use std::time::Duration;

use mactype_service_contract::StructuredServiceError;

pub const PROCESS_CREATION_QUERY: &str =
    "SELECT * FROM __InstanceCreationEvent WITHIN 1 WHERE TargetInstance ISA 'Win32_Process'";
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
    pub protected: bool,
    pub critical: bool,
}

pub trait ProcessInspector {
    fn inspect(&self, pid: u32) -> Result<ProcessIdentity, StructuredServiceError>;
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InjectionRequest {
    pub identity: ProcessIdentity,
    pub generation_id: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BrokerDisposition {
    Injected,
    Skipped,
    Rejected,
    RetryableFailure,
    Cancelled,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BrokerResult {
    pub disposition: BrokerDisposition,
    pub code: String,
    pub win32_error: Option<u32>,
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
