use std::{collections::VecDeque, sync::Arc, time::Duration};

use mactype_service_contract::{
    ComponentReadiness, HealthState, InjectionTelemetry, ReadinessReport, StructuredServiceError,
};

use crate::{
    subscribe_process_creation, HostEvent, HostEventSink, InitializedRuntime, InjectionBroker,
    ProcessArchitecture, ProcessEventSource, ProcessInspector, RetryScheduler, RuntimeDriver,
    RuntimeHealthReporter, StopSignal,
};

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
    active_profile_digest: Option<String>,
    service_pid: u32,
    generation_id: impl Into<String>,
    source: Box<dyn ProcessEventSource>,
    inspector: Box<dyn ProcessInspector>,
    broker: Box<dyn InjectionBroker>,
    events: Arc<dyn HostEventSink>,
) -> Result<InitializedRuntime, StructuredServiceError> {
    initialize_process_orchestration_with_observer_recovery(
        active_profile_digest,
        ObserverRecoveryPolicy::default(),
        service_pid,
        generation_id,
        source,
        inspector,
        broker,
        events,
    )
}

#[allow(clippy::too_many_arguments)]
pub fn initialize_process_orchestration_with_observer_recovery(
    active_profile_digest: Option<String>,
    observer_recovery: ObserverRecoveryPolicy,
    service_pid: u32,
    generation_id: impl Into<String>,
    mut source: Box<dyn ProcessEventSource>,
    inspector: Box<dyn ProcessInspector>,
    broker: Box<dyn InjectionBroker>,
    events: Arc<dyn HostEventSink>,
) -> Result<InitializedRuntime, StructuredServiceError> {
    let profile_digest = active_profile_digest
        .clone()
        .ok_or_else(|| StructuredServiceError {
            code: "active-profile-unavailable".to_owned(),
            message: "process orchestration requires an active protected profile".to_owned(),
            win32_error: None,
        })?;
    broker.verify_ready(ProcessArchitecture::X86)?;
    broker.verify_ready(ProcessArchitecture::X64)?;
    subscribe_process_creation(source.as_mut())?;
    let snapshot_pids = source.snapshot_pids()?.into();

    Ok(InitializedRuntime::driven(
        active_profile_digest,
        ReadinessReport::ready(),
        Box::new(ProcessOrchestrationDriver {
            service_pid,
            generation_id: generation_id.into(),
            profile_digest,
            observer: ObserverState {
                recovery: observer_recovery,
                snapshot_pids,
                source,
            },
            inspector,
            broker,
            events,
        }),
    ))
}

struct ProcessOrchestrationDriver {
    service_pid: u32,
    generation_id: String,
    profile_digest: String,
    observer: ObserverState,
    inspector: Box<dyn ProcessInspector>,
    broker: Box<dyn InjectionBroker>,
    events: Arc<dyn HostEventSink>,
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
    /// Takes the subscription again and folds every process that started while
    /// it was down into the backlog, so a recovery misses no injection target.
    fn resubscribe(&mut self) -> Result<(), StructuredServiceError> {
        subscribe_process_creation(self.source.as_mut())?;
        for pid in self.source.snapshot_pids()? {
            if !self.snapshot_pids.contains(&pid) {
                self.snapshot_pids.push_back(pid);
            }
        }
        Ok(())
    }

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
            match self.resubscribe() {
                Ok(()) => {
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
        let mut orchestrator = crate::InjectionOrchestrator::with_runtime_context(
            self.service_pid,
            &self.generation_id,
            &self.profile_digest,
            self.inspector.as_ref(),
            self.broker.as_ref(),
            crate::RetryPolicy::default(),
            &scheduler,
            self.events.clone(),
        );
        let mut consecutive_health_report_failures = 0;
        loop {
            if stop.wait_timeout(Duration::ZERO)? {
                return Ok(());
            }
            while let Some(change) = stop.take_session_change() {
                orchestrator.handle_session_change(change);
            }
            self.events.record(HostEvent::FlushInjectionSummary);
            let deferred = orchestrator.poll_deferred(std::time::Instant::now());
            let outcome = match deferred {
                Ok(Some(outcome)) if outcome != crate::ProcessOutcome::Deferred => Ok(outcome),
                Err(error) => Err(error),
                _ => {
                    let event_wait = if self.observer.snapshot_pids.is_empty() {
                        Duration::from_millis(250)
                    } else {
                        Duration::ZERO
                    };
                    let pid = match self.observer.source.next_pid(event_wait) {
                        Ok(Some(pid)) => pid,
                        Ok(None) => match self.observer.snapshot_pids.pop_front() {
                            Some(pid) => pid,
                            None => continue,
                        },
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
                    orchestrator.handle_pid(pid)
                }
            };
            match outcome {
                Ok(crate::ProcessOutcome::Injected) => {
                    report_runtime_health(
                        health,
                        &mut consecutive_health_report_failures,
                        HealthState::Ready,
                        ReadinessReport::ready(),
                        orchestrator.injection_telemetry(),
                        None,
                    )?;
                }
                Ok(crate::ProcessOutcome::Rejected | crate::ProcessOutcome::RetryExhausted) => {
                    if let Some(error) = orchestrator.generation_health_error() {
                        report_runtime_health(
                            health,
                            &mut consecutive_health_report_failures,
                            HealthState::Degraded,
                            ReadinessReport::ready(),
                            orchestrator.injection_telemetry(),
                            Some(error),
                        )?;
                    }
                }
                Ok(crate::ProcessOutcome::Cancelled) => return Ok(()),
                Ok(_) => {}
                Err(error) => {
                    report_runtime_health(
                        health,
                        &mut consecutive_health_report_failures,
                        HealthState::Degraded,
                        ReadinessReport::ready(),
                        orchestrator.injection_telemetry(),
                        Some(error),
                    )?;
                }
            }
        }
    }
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

impl crate::RetryScheduler for StopRetryScheduler<'_> {
    fn wait(&self, delay: Duration) -> bool {
        matches!(self.0.wait_timeout(delay), Ok(false))
    }
}
