#![forbid(unsafe_code)]

mod model;

use std::collections::{HashMap, VecDeque};

use mactype_service_contract::{
    InjectionArchitecture, InjectionSuccess, InjectionTelemetry, RendererRuntimeBinding,
    StructuredServiceError, UnityFontHookPolicy,
};

use crate::observer::{
    BrokerDisposition, BrokerResult, InjectionBroker, InjectionRequest, ProcessArchitecture,
    ProcessIdentity,
};
use crate::target_validation::{
    ProcessInspector, ProcessTargetDecision, ProcessTargetValidator, TargetLiveness,
};

pub use model::{
    ProcessAttemptRecord, ProcessOutcome, RetryPolicy, RetryScheduler, SessionChange,
    MAX_TRACKED_PROCESS_RESULTS,
};

/// The bounded target-result code recorded when an untrustworthy cleanup
/// result was re-checked and the verified target provably no longer existed.
pub const TARGET_VANISHED_RESULT_CODE: &str = "injection-target-vanished";

pub struct InjectionOrchestrator<'a> {
    binding: RendererRuntimeBinding,
    target_validator: ProcessTargetValidator<'a>,
    inspector: &'a dyn ProcessInspector,
    broker: &'a dyn InjectionBroker,
    processed: HashMap<(u32, u64), ProcessAttemptRecord>,
    process_order: VecDeque<(u32, u64)>,
    retry_policy: RetryPolicy,
    retry_scheduler: Option<&'a dyn RetryScheduler>,
    last_injected_identity: Option<ProcessIdentity>,
    injection_telemetry: InjectionTelemetry,
}

impl<'a> InjectionOrchestrator<'a> {
    pub fn new(
        service_pid: u32,
        binding: RendererRuntimeBinding,
        inspector: &'a dyn ProcessInspector,
        broker: &'a dyn InjectionBroker,
    ) -> Self {
        Self::build(
            service_pid,
            binding,
            inspector,
            broker,
            RetryPolicy::default(),
            None,
            UnityFontHookPolicy::default(),
        )
    }

    pub fn with_retry_policy(
        service_pid: u32,
        binding: RendererRuntimeBinding,
        inspector: &'a dyn ProcessInspector,
        broker: &'a dyn InjectionBroker,
        retry_policy: RetryPolicy,
        retry_scheduler: &'a dyn RetryScheduler,
    ) -> Self {
        Self::build(
            service_pid,
            binding,
            inspector,
            broker,
            retry_policy,
            Some(retry_scheduler),
            UnityFontHookPolicy::default(),
        )
    }

    pub fn with_retry_policy_and_unity_font_hook(
        service_pid: u32,
        binding: RendererRuntimeBinding,
        inspector: &'a dyn ProcessInspector,
        broker: &'a dyn InjectionBroker,
        retry_policy: RetryPolicy,
        retry_scheduler: &'a dyn RetryScheduler,
        unity_font_hook: UnityFontHookPolicy,
    ) -> Self {
        Self::build(
            service_pid,
            binding,
            inspector,
            broker,
            retry_policy,
            Some(retry_scheduler),
            unity_font_hook,
        )
    }

    fn build(
        service_pid: u32,
        binding: RendererRuntimeBinding,
        inspector: &'a dyn ProcessInspector,
        broker: &'a dyn InjectionBroker,
        retry_policy: RetryPolicy,
        retry_scheduler: Option<&'a dyn RetryScheduler>,
        unity_font_hook: UnityFontHookPolicy,
    ) -> Self {
        Self {
            binding,
            target_validator: ProcessTargetValidator::with_unity_font_hook_policy(
                service_pid,
                inspector,
                unity_font_hook,
            ),
            inspector,
            broker,
            processed: HashMap::new(),
            process_order: VecDeque::new(),
            retry_policy,
            retry_scheduler,
            last_injected_identity: None,
            injection_telemetry: InjectionTelemetry::default(),
        }
    }

    pub fn handle_pid(&mut self, pid: u32) -> Result<ProcessOutcome, StructuredServiceError> {
        let identity = match self.target_validator.validate(pid)? {
            ProcessTargetDecision::Eligible(identity) => identity,
            ProcessTargetDecision::Skipped { identity, reason } => {
                if let Some(identity) = identity {
                    if self
                        .processed
                        .contains_key(&(identity.pid, identity.creation_time))
                    {
                        return Ok(ProcessOutcome::Duplicate);
                    }
                    self.record_result(
                        identity,
                        ProcessOutcome::Skipped,
                        0,
                        BrokerResult::new(BrokerDisposition::Skipped, reason.code(), None),
                    );
                }
                return Ok(ProcessOutcome::Skipped);
            }
        };
        if self
            .processed
            .contains_key(&(identity.pid, identity.creation_time))
        {
            return Ok(ProcessOutcome::Duplicate);
        }
        let request = InjectionRequest {
            identity,
            binding: self.binding,
        };
        let attempts = self.retry_policy.max_attempts.max(1);
        let mut delay = self.retry_policy.initial_delay;
        for attempt in 1..=attempts {
            let result = self.broker.inject(&request).into_bounded_evidence();
            if let Some(outcome) = terminal_outcome(result.disposition, attempt == attempts) {
                if outcome == ProcessOutcome::Injected {
                    self.last_injected_identity = Some(request.identity.clone());
                    self.record_injection_success(&request.identity);
                }
                let (outcome, result) =
                    self.reclassify_vanished_target(&request.identity, outcome, result);
                self.record_result(request.identity.clone(), outcome, attempt, result);
                return Ok(outcome);
            }

            if let Some(scheduler) = self.retry_scheduler {
                if !scheduler.wait(delay) {
                    self.record_result(
                        request.identity.clone(),
                        ProcessOutcome::Cancelled,
                        attempt,
                        result,
                    );
                    return Ok(ProcessOutcome::Cancelled);
                }
            } else {
                std::thread::sleep(delay);
            }
            delay = delay.saturating_mul(2).min(self.retry_policy.max_delay);
        }
        unreachable!("the bounded attempt loop always returns")
    }

    pub fn handle_session_change(&mut self, change: SessionChange) {
        if change.is_overflow() {
            self.processed.clear();
            self.process_order.clear();
            return;
        }
        if matches!(change.event_type, 2 | 4 | 6 | 11) {
            self.processed
                .retain(|_, record| record.identity.session_id != change.session_id);
            self.process_order
                .retain(|identity| self.processed.contains_key(identity));
        }
    }

    pub fn last_injected_identity(&self) -> Option<&ProcessIdentity> {
        self.last_injected_identity.as_ref()
    }

    pub fn last_result(&self, pid: u32, creation_time: u64) -> Option<&ProcessAttemptRecord> {
        self.processed.get(&(pid, creation_time))
    }

    pub fn tracked_process_count(&self) -> usize {
        self.processed.len()
    }

    pub fn most_recent_result(&self) -> Option<&ProcessAttemptRecord> {
        self.process_order
            .back()
            .and_then(|identity| self.processed.get(identity))
    }

    pub fn injection_telemetry(&self) -> InjectionTelemetry {
        self.injection_telemetry.clone()
    }

    pub fn generation_health_error(&self) -> Option<StructuredServiceError> {
        let record = self.most_recent_result()?;
        let code = match record.broker_disposition {
            BrokerDisposition::UncertainCleanup => "injection-cleanup-unknown",
            BrokerDisposition::UncertainIntegrity => "injection-helper-response-integrity-unknown",
            BrokerDisposition::Injected
            | BrokerDisposition::Skipped
            | BrokerDisposition::Rejected
            | BrokerDisposition::Retryable
            | BrokerDisposition::Cancelled => return None,
        };
        Some(StructuredServiceError {
            code: code.to_owned(),
            message: format!(
                "target result is not trustworthy: pid={} creation_time={} session_id={} generation={} broker_code={}",
                record.identity.pid,
                record.identity.creation_time,
                record.identity.session_id,
                record.binding.runtime_generation_id(),
                record.code
            ),
            win32_error: record.win32_error,
        })
    }

    fn reclassify_vanished_target(
        &self,
        identity: &ProcessIdentity,
        outcome: ProcessOutcome,
        result: BrokerResult,
    ) -> (ProcessOutcome, BrokerResult) {
        if !matches!(
            outcome,
            ProcessOutcome::Rejected | ProcessOutcome::RetryExhausted
        ) || result.disposition != BrokerDisposition::UncertainCleanup
        {
            return (outcome, result);
        }
        match self.inspector.probe_target_liveness(identity) {
            TargetLiveness::Vanished => (
                ProcessOutcome::Skipped,
                BrokerResult::new(
                    BrokerDisposition::Skipped,
                    TARGET_VANISHED_RESULT_CODE,
                    result.win32_error,
                ),
            ),
            TargetLiveness::Alive | TargetLiveness::Unknown => (outcome, result),
        }
    }

    fn record_injection_success(&mut self, identity: &ProcessIdentity) {
        self.injection_telemetry.record_success(
            match identity.architecture {
                ProcessArchitecture::X86 => InjectionArchitecture::X86,
                ProcessArchitecture::X64 => InjectionArchitecture::X64,
            },
            InjectionSuccess {
                pid: identity.pid,
                creation_time: identity.creation_time,
                session_id: identity.session_id,
                runtime_generation_id: self.binding.runtime_generation_id().to_string(),
                profile_digest: self.binding.profile_digest().to_string(),
            },
        );
    }

    fn record_result(
        &mut self,
        identity: ProcessIdentity,
        outcome: ProcessOutcome,
        attempts: u8,
        result: BrokerResult,
    ) {
        let result = result.into_bounded_evidence();
        let key = (identity.pid, identity.creation_time);
        if !self.processed.contains_key(&key) {
            while self.processed.len() >= MAX_TRACKED_PROCESS_RESULTS {
                if let Some(oldest) = self.process_order.pop_front() {
                    self.processed.remove(&oldest);
                }
            }
            self.process_order.push_back(key);
        }
        self.processed.insert(
            key,
            ProcessAttemptRecord {
                identity,
                binding: self.binding,
                outcome,
                broker_disposition: result.disposition,
                attempts,
                code: result.code,
                win32_error: result.win32_error,
            },
        );
    }
}

fn terminal_outcome(disposition: BrokerDisposition, final_attempt: bool) -> Option<ProcessOutcome> {
    match disposition {
        BrokerDisposition::Cancelled => Some(ProcessOutcome::Cancelled),
        BrokerDisposition::Injected => Some(ProcessOutcome::Injected),
        BrokerDisposition::Skipped => Some(ProcessOutcome::Skipped),
        BrokerDisposition::Rejected
        | BrokerDisposition::UncertainCleanup
        | BrokerDisposition::UncertainIntegrity => Some(ProcessOutcome::Rejected),
        BrokerDisposition::Retryable if final_attempt => Some(ProcessOutcome::RetryExhausted),
        BrokerDisposition::Retryable => None,
    }
}
