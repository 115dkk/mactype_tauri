#![forbid(unsafe_code)]

use std::{
    collections::VecDeque,
    time::{Duration, Instant},
};

use mactype_service_contract::{
    ComponentReadiness, ConsoleProcessPolicy, HealthState, InjectionTelemetry,
    PrivateFreeTypePolicy, ReadinessReport, RendererRuntimeBinding, StructuredServiceError,
    UnityFontHookPolicy,
};

use crate::injection_orchestrator::{
    DeferralPolicy, InjectionOrchestrator, ProcessAdmissionPolicies, ProcessOutcome, RetryPolicy,
    RetryScheduler,
};
use crate::observer::{
    subscribe_process_creation, InjectionBroker, ProcessArchitecture, ProcessEventSource,
};
use crate::runtime::{InitializedRuntime, RuntimeDriver, RuntimeHealthReporter, StopSignal};
use crate::target_validation::ProcessInspector;

const MAX_TOLERATED_CONSECUTIVE_HEALTH_REPORT_FAILURES: usize = 20;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ObserverRecoveryPolicy {
    /// Delay before the first resubscription attempt.
    pub initial_delay: Duration,
    /// Cap for the doubling delay between attempts.
    pub max_delay: Duration,
    /// Attempts before the driver gives up and fails the service.
    pub max_attempts: u32,
}

impl Default for ObserverRecoveryPolicy {
    fn default() -> Self {
        Self {
            initial_delay: Duration::from_secs(1),
            max_delay: Duration::from_secs(30),
            max_attempts: 12,
        }
    }
}

pub fn initialize_process_orchestration(
    binding: RendererRuntimeBinding,
    service_pid: u32,
    source: Box<dyn ProcessEventSource>,
    inspector: Box<dyn ProcessInspector>,
    broker: Box<dyn InjectionBroker>,
) -> Result<InitializedRuntime, StructuredServiceError> {
    initialize_process_orchestration_with_profile_policies(
        binding,
        UnityFontHookPolicy::default(),
        PrivateFreeTypePolicy::default(),
        ConsoleProcessPolicy::default(),
        ObserverRecoveryPolicy::default(),
        service_pid,
        source,
        inspector,
        broker,
    )
}

pub fn initialize_process_orchestration_with_unity_font_hook(
    binding: RendererRuntimeBinding,
    unity_font_hook: UnityFontHookPolicy,
    service_pid: u32,
    source: Box<dyn ProcessEventSource>,
    inspector: Box<dyn ProcessInspector>,
    broker: Box<dyn InjectionBroker>,
) -> Result<InitializedRuntime, StructuredServiceError> {
    initialize_process_orchestration_with_profile_policies(
        binding,
        unity_font_hook,
        PrivateFreeTypePolicy::default(),
        ConsoleProcessPolicy::default(),
        ObserverRecoveryPolicy::default(),
        service_pid,
        source,
        inspector,
        broker,
    )
}

#[allow(clippy::too_many_arguments)]
pub fn initialize_process_orchestration_with_profile_policies(
    binding: RendererRuntimeBinding,
    unity_font_hook: UnityFontHookPolicy,
    private_freetype: PrivateFreeTypePolicy,
    console_process: ConsoleProcessPolicy,
    observer_recovery: ObserverRecoveryPolicy,
    service_pid: u32,
    mut source: Box<dyn ProcessEventSource>,
    inspector: Box<dyn ProcessInspector>,
    broker: Box<dyn InjectionBroker>,
) -> Result<InitializedRuntime, StructuredServiceError> {
    broker.verify_ready(ProcessArchitecture::X86)?;
    broker.verify_ready(ProcessArchitecture::X64)?;
    subscribe_process_creation(source.as_mut())?;
    let snapshot_pids = source.snapshot_pids()?.into();

    Ok(InitializedRuntime::driven(
        binding,
        ReadinessReport::ready(),
        Box::new(ProcessOrchestrationDriver {
            service_pid,
            binding,
            unity_font_hook,
            private_freetype,
            console_process,
            observer: ObserverState {
                recovery: observer_recovery,
                snapshot_pids,
                source,
            },
            inspector,
            broker,
        }),
    ))
}

struct ProcessOrchestrationDriver {
    service_pid: u32,
    binding: RendererRuntimeBinding,
    unity_font_hook: UnityFontHookPolicy,
    private_freetype: PrivateFreeTypePolicy,
    console_process: ConsoleProcessPolicy,
    observer: ObserverState,
    inspector: Box<dyn ProcessInspector>,
    broker: Box<dyn InjectionBroker>,
}

struct ObserverState {
    recovery: ObserverRecoveryPolicy,
    snapshot_pids: VecDeque<u32>,
    source: Box<dyn ProcessEventSource>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ObserverRecovery {
    Resubscribed,
    Stopped,
}

impl ObserverState {
    fn recover_observer(
        &mut self,
        error: StructuredServiceError,
        stop: &dyn StopSignal,
        health: &dyn RuntimeHealthReporter,
        scheduler: &StopRetryScheduler<'_>,
        consecutive_health_report_failures: &mut usize,
        injection: InjectionTelemetry,
    ) -> Result<ObserverRecovery, StructuredServiceError> {
        let observer_failed = ReadinessReport {
            observer: ComponentReadiness::Failed,
            ..ReadinessReport::ready()
        };
        report_runtime_health(
            health,
            consecutive_health_report_failures,
            HealthState::Degraded,
            observer_failed.clone(),
            injection.clone(),
            Some(error.clone()),
        )?;

        let mut latest_error = error;
        let mut delay = self.recovery.initial_delay;
        for _attempt in 1..=self.recovery.max_attempts {
            if !scheduler.wait(delay) || stop.stop_requested() {
                return Ok(ObserverRecovery::Stopped);
            }
            match subscribe_process_creation(self.source.as_mut()) {
                Ok(()) => match self.source.snapshot_pids() {
                    Ok(pids) => {
                        for pid in pids {
                            if !self.snapshot_pids.contains(&pid) {
                                self.snapshot_pids.push_back(pid);
                            }
                        }
                        report_runtime_health(
                            health,
                            consecutive_health_report_failures,
                            HealthState::Ready,
                            ReadinessReport::ready(),
                            injection,
                            None,
                        )?;
                        return Ok(ObserverRecovery::Resubscribed);
                    }
                    Err(error) => latest_error = error,
                },
                Err(error) => latest_error = error,
            }
            delay = delay.saturating_mul(2).min(self.recovery.max_delay);
        }

        report_runtime_health(
            health,
            consecutive_health_report_failures,
            HealthState::Failed,
            observer_failed,
            injection,
            Some(latest_error.clone()),
        )?;
        Err(latest_error)
    }
}

impl RuntimeDriver for ProcessOrchestrationDriver {
    fn run(
        &mut self,
        stop: &dyn StopSignal,
        health: &dyn RuntimeHealthReporter,
    ) -> Result<(), StructuredServiceError> {
        let scheduler = StopRetryScheduler(stop);
        let mut orchestrator = InjectionOrchestrator::with_retry_policy_and_profile_policies(
            self.service_pid,
            self.binding,
            self.inspector.as_ref(),
            self.broker.as_ref(),
            RetryPolicy::default(),
            &scheduler,
            ProcessAdmissionPolicies::new(
                self.unity_font_hook.clone(),
                self.private_freetype,
                self.console_process,
            ),
            DeferralPolicy::default(),
        );
        let mut consecutive_health_report_failures = 0;
        loop {
            crate::event_log::flush_elapsed_injection_summary();
            if stop.wait_timeout(Duration::ZERO)? {
                return Ok(());
            }
            while let Some(change) = stop.take_session_change() {
                orchestrator.handle_session_change(change);
            }
            let event_wait = if self.observer.snapshot_pids.is_empty() {
                Duration::from_millis(250)
            } else {
                Duration::ZERO
            };
            let pid = match self.observer.source.next_pid(event_wait) {
                Ok(Some(pid)) => Some(pid),
                Ok(None) => self.observer.snapshot_pids.pop_front(),
                Err(error) => match self.observer.recover_observer(
                    error,
                    stop,
                    health,
                    &scheduler,
                    &mut consecutive_health_report_failures,
                    orchestrator.injection_telemetry(),
                ) {
                    Ok(ObserverRecovery::Resubscribed) => continue,
                    Ok(ObserverRecovery::Stopped) => return Ok(()),
                    Err(error) => return Err(error),
                },
            };
            if let Some(pid) = pid {
                if apply_orchestration_outcome(
                    orchestrator.handle_pid(pid).map(Some),
                    &orchestrator,
                    health,
                    &mut consecutive_health_report_failures,
                )? {
                    return Ok(());
                }
            }
            if apply_orchestration_outcome(
                orchestrator.poll_deferred(Instant::now()),
                &orchestrator,
                health,
                &mut consecutive_health_report_failures,
            )? {
                return Ok(());
            }
        }
    }
}

fn apply_orchestration_outcome(
    result: Result<Option<ProcessOutcome>, StructuredServiceError>,
    orchestrator: &InjectionOrchestrator<'_>,
    health: &dyn RuntimeHealthReporter,
    consecutive_health_report_failures: &mut usize,
) -> Result<bool, StructuredServiceError> {
    match result {
        Ok(Some(ProcessOutcome::Injected)) => report_runtime_health(
            health,
            consecutive_health_report_failures,
            HealthState::Ready,
            ReadinessReport::ready(),
            orchestrator.injection_telemetry(),
            None,
        )?,
        Ok(Some(ProcessOutcome::Rejected | ProcessOutcome::RetryExhausted)) => {
            if let Some(error) = orchestrator.generation_health_error() {
                report_runtime_health(
                    health,
                    consecutive_health_report_failures,
                    HealthState::Degraded,
                    ReadinessReport::ready(),
                    orchestrator.injection_telemetry(),
                    Some(error),
                )?;
            }
        }
        Ok(Some(ProcessOutcome::Cancelled)) => return Ok(true),
        Ok(Some(
            ProcessOutcome::Deferred | ProcessOutcome::Skipped | ProcessOutcome::Duplicate,
        ))
        | Ok(None) => {}
        Err(error) => report_runtime_health(
            health,
            consecutive_health_report_failures,
            HealthState::Degraded,
            ReadinessReport::ready(),
            orchestrator.injection_telemetry(),
            Some(error),
        )?,
    }
    Ok(false)
}

fn report_runtime_health(
    health: &dyn RuntimeHealthReporter,
    consecutive_failures: &mut usize,
    state: HealthState,
    readiness: ReadinessReport,
    injection: InjectionTelemetry,
    last_error: Option<StructuredServiceError>,
) -> Result<(), StructuredServiceError> {
    match health.report(state, readiness, injection, last_error) {
        Ok(()) => {
            *consecutive_failures = 0;
            Ok(())
        }
        Err(error) => {
            *consecutive_failures += 1;
            if *consecutive_failures > MAX_TOLERATED_CONSECUTIVE_HEALTH_REPORT_FAILURES {
                Err(error)
            } else {
                Ok(())
            }
        }
    }
}

struct StopRetryScheduler<'a>(&'a dyn StopSignal);

impl RetryScheduler for StopRetryScheduler<'_> {
    fn wait(&self, delay: Duration) -> bool {
        matches!(self.0.wait_timeout(delay), Ok(false))
    }
}
