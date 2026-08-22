#![forbid(unsafe_code)]

use std::{collections::VecDeque, time::Duration};

use mactype_service_contract::{
    ComponentReadiness, HealthState, InjectionTelemetry, ReadinessReport, RendererRuntimeBinding,
    StructuredServiceError,
};

use crate::injection_orchestrator::{
    InjectionOrchestrator, ProcessOutcome, RetryPolicy, RetryScheduler,
};
use crate::observer::{
    subscribe_process_creation, InjectionBroker, ProcessArchitecture, ProcessEventSource,
};
use crate::runtime::{InitializedRuntime, RuntimeDriver, RuntimeHealthReporter, StopSignal};
use crate::target_validation::ProcessInspector;

const MAX_TOLERATED_CONSECUTIVE_HEALTH_REPORT_FAILURES: usize = 20;

pub fn initialize_process_orchestration(
    binding: RendererRuntimeBinding,
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
            snapshot_pids,
            source,
            inspector,
            broker,
        }),
    ))
}

struct ProcessOrchestrationDriver {
    service_pid: u32,
    binding: RendererRuntimeBinding,
    snapshot_pids: VecDeque<u32>,
    source: Box<dyn ProcessEventSource>,
    inspector: Box<dyn ProcessInspector>,
    broker: Box<dyn InjectionBroker>,
}

impl RuntimeDriver for ProcessOrchestrationDriver {
    fn run(
        &mut self,
        stop: &dyn StopSignal,
        health: &dyn RuntimeHealthReporter,
    ) -> Result<(), StructuredServiceError> {
        let scheduler = StopRetryScheduler(stop);
        let mut orchestrator = InjectionOrchestrator::with_retry_policy(
            self.service_pid,
            self.binding,
            self.inspector.as_ref(),
            self.broker.as_ref(),
            RetryPolicy::default(),
            &scheduler,
        );
        let mut consecutive_health_report_failures = 0;
        loop {
            if stop.wait_timeout(Duration::ZERO)? {
                return Ok(());
            }
            while let Some(change) = stop.take_session_change() {
                orchestrator.handle_session_change(change);
            }
            let event_wait = if self.snapshot_pids.is_empty() {
                Duration::from_millis(250)
            } else {
                Duration::ZERO
            };
            let pid = match self.source.next_pid(event_wait) {
                Ok(Some(pid)) => pid,
                Ok(None) => match self.snapshot_pids.pop_front() {
                    Some(pid) => pid,
                    None => continue,
                },
                Err(error) => {
                    let _ = report_runtime_health(
                        health,
                        &mut consecutive_health_report_failures,
                        HealthState::Failed,
                        ReadinessReport {
                            observer: ComponentReadiness::Failed,
                            ..ReadinessReport::ready()
                        },
                        orchestrator.injection_telemetry(),
                        Some(error.clone()),
                    );
                    return Err(error);
                }
            };
            match orchestrator.handle_pid(pid) {
                Ok(ProcessOutcome::Injected) => {
                    report_runtime_health(
                        health,
                        &mut consecutive_health_report_failures,
                        HealthState::Ready,
                        ReadinessReport::ready(),
                        orchestrator.injection_telemetry(),
                        None,
                    )?;
                }
                Ok(ProcessOutcome::Rejected | ProcessOutcome::RetryExhausted) => {
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
                Ok(ProcessOutcome::Cancelled) => return Ok(()),
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

impl RetryScheduler for StopRetryScheduler<'_> {
    fn wait(&self, delay: Duration) -> bool {
        matches!(self.0.wait_timeout(delay), Ok(false))
    }
}
