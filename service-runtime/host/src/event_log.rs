#![forbid(unsafe_code)]

use mactype_service_contract::{
    event_log::{
        EventArea, EventLogWriter, EventSeverity, EventSource, EventThrottle,
        MAX_EVENT_DETAIL_BYTES,
    },
    HealthState, StructuredServiceError,
};
use std::{
    collections::BTreeMap,
    path::PathBuf,
    sync::Mutex,
    time::{Duration, Instant},
};

use crate::{ProcessArchitecture, ProcessAttemptRecord, ProcessOutcome};

const SUMMARY_WINDOW: Duration = Duration::from_secs(60);
const DEDUPE_WINDOW: Duration = Duration::from_secs(10 * 60);

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum HostEvent {
    ServiceStarted {
        version: String,
    },
    ServiceStartSkipped {
        error: StructuredServiceError,
    },
    ServiceStopped,
    HealthChanged {
        state: HealthState,
        error: Option<StructuredServiceError>,
    },
    InjectionResult {
        record: ProcessAttemptRecord,
        process: String,
    },
    InjectionSkipped,
    FlushInjectionSummary,
    HelperBrokerFailed {
        architecture: ProcessArchitecture,
        code: String,
        detail: Option<String>,
    },
}

pub trait HostEventSink: Send + Sync {
    fn record(&self, event: HostEvent);
}

pub struct HostEventLogger {
    state: Mutex<HostEventLoggerState>,
}

impl HostEventLogger {
    pub fn new(path: PathBuf) -> Self {
        Self::new_at(path, Instant::now())
    }

    pub fn new_at(path: PathBuf, now: Instant) -> Self {
        Self {
            state: Mutex::new(HostEventLoggerState::new(path, now)),
        }
    }

    pub fn record_at(&self, event: HostEvent, now: Instant) {
        let mut logger = self
            .state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        match event {
            HostEvent::ServiceStarted { version } => logger.service_started(&version),
            HostEvent::ServiceStartSkipped { error } => logger.service_start_skipped(&error),
            HostEvent::ServiceStopped => {
                logger.flush_summary(now);
                logger.write(
                    EventSeverity::Info,
                    EventArea::Service,
                    "service-stopped",
                    BTreeMap::new(),
                    None,
                );
            }
            HostEvent::HealthChanged { state, error } => {
                logger.health_changed(state, error.as_ref());
            }
            HostEvent::InjectionResult { record, process } => {
                let detail = if matches!(
                    record.outcome,
                    ProcessOutcome::Rejected | ProcessOutcome::RetryExhausted
                ) {
                    diagnostic_detail(&record)
                } else {
                    String::new()
                };
                logger.injection_result(&record, process, detail, now);
            }
            HostEvent::InjectionSkipped => logger.injection_skipped(now),
            HostEvent::FlushInjectionSummary => logger.flush_elapsed_summary(now),
            HostEvent::HelperBrokerFailed {
                architecture,
                code,
                detail,
            } => logger.helper_broker_failed(architecture, &code, detail, now),
        }
    }
}

impl HostEventSink for HostEventLogger {
    fn record(&self, event: HostEvent) {
        self.record_at(event, Instant::now());
    }
}

struct HostEventLoggerState {
    writer: EventLogWriter,
    write_error_reported: bool,
    health: Option<HealthState>,
    summary: InjectionSummary,
    injection_throttle: EventThrottle,
    helper_throttle: EventThrottle,
}

#[derive(Default)]
struct InjectionCounts {
    injected: u64,
    failed: u64,
    skipped: u64,
}

struct InjectionSummary {
    window_start: Instant,
    counts: InjectionCounts,
}

impl HostEventLoggerState {
    fn new(path: PathBuf, now: Instant) -> Self {
        Self {
            writer: EventLogWriter::new(path),
            write_error_reported: false,
            health: None,
            summary: InjectionSummary {
                window_start: now,
                counts: InjectionCounts::default(),
            },
            injection_throttle: EventThrottle::default(),
            helper_throttle: EventThrottle::default(),
        }
    }

    fn service_started(&mut self, version: &str) {
        self.write(
            EventSeverity::Info,
            EventArea::Service,
            "service-started",
            BTreeMap::from([("version".to_owned(), version.to_owned())]),
            None,
        );
        self.health = Some(HealthState::Ready);
    }

    fn service_start_skipped(&mut self, error: &StructuredServiceError) {
        self.write(
            EventSeverity::Info,
            EventArea::Service,
            "service-start-skipped",
            BTreeMap::from([
                ("message".to_owned(), error.message.clone()),
                ("reason".to_owned(), error.code.clone()),
            ]),
            None,
        );
    }

    fn health_changed(&mut self, state: HealthState, error: Option<&StructuredServiceError>) {
        if self.health == Some(state) {
            return;
        }
        let recovered = state == HealthState::Ready
            && self.health.is_some_and(|previous| {
                matches!(previous, HealthState::Degraded | HealthState::Failed)
            });
        self.health = Some(state);
        if !matches!(state, HealthState::Degraded | HealthState::Failed) && !recovered {
            return;
        }
        let severity = match state {
            HealthState::Degraded => EventSeverity::Notice,
            HealthState::Failed => EventSeverity::Error,
            HealthState::Ready => EventSeverity::Info,
            _ => return,
        };
        let mut params = BTreeMap::from([("state".to_owned(), health_name(state).to_owned())]);
        if let Some(error) = error {
            params.insert("code".to_owned(), error.code.clone());
            params.insert("message".to_owned(), error.message.clone());
        }
        self.write(
            severity,
            EventArea::Service,
            "service-health-changed",
            params,
            None,
        );
    }

    fn injection_skipped(&mut self, now: Instant) {
        self.flush_elapsed_summary(now);
        self.summary.counts.skipped += 1;
    }

    fn injection_result(
        &mut self,
        record: &ProcessAttemptRecord,
        process: String,
        detail: String,
        now: Instant,
    ) {
        self.flush_elapsed_summary(now);
        match record.outcome {
            ProcessOutcome::Injected => self.summary.counts.injected += 1,
            ProcessOutcome::Skipped => self.summary.counts.skipped += 1,
            ProcessOutcome::Rejected | ProcessOutcome::RetryExhausted => {
                self.summary.counts.failed += 1;
                let key = format!("{process}|{}", record.code);
                if self.injection_throttle.allow(&key, now, DEDUPE_WINDOW) {
                    self.write(
                        EventSeverity::Warning,
                        EventArea::Injection,
                        "injection-failed",
                        BTreeMap::from([
                            ("process".to_owned(), process),
                            ("reason".to_owned(), record.code.clone()),
                        ]),
                        Some(detail),
                    );
                }
            }
            ProcessOutcome::Deferred | ProcessOutcome::Duplicate | ProcessOutcome::Cancelled => {}
        }
    }

    fn helper_broker_failed(
        &mut self,
        architecture: ProcessArchitecture,
        code: &str,
        detail: Option<String>,
        now: Instant,
    ) {
        let architecture = match architecture {
            ProcessArchitecture::X86 => "x86",
            ProcessArchitecture::X64 => "x64",
        };
        if !self.helper_throttle.allow(architecture, now, DEDUPE_WINDOW) {
            return;
        }
        self.write(
            EventSeverity::Error,
            EventArea::Injection,
            "helper-broker-failed",
            BTreeMap::from([
                ("architecture".to_owned(), architecture.to_owned()),
                ("code".to_owned(), code.to_owned()),
            ]),
            detail,
        );
    }

    fn flush_elapsed_summary(&mut self, now: Instant) {
        if now.saturating_duration_since(self.summary.window_start) >= SUMMARY_WINDOW {
            self.flush_summary(now);
        }
    }

    fn flush_summary(&mut self, now: Instant) {
        let counts = std::mem::take(&mut self.summary.counts);
        self.summary.window_start = now;
        if counts.injected == 0 && counts.failed == 0 && counts.skipped == 0 {
            return;
        }
        self.write(
            EventSeverity::Info,
            EventArea::Injection,
            "injection-summary",
            BTreeMap::from([
                ("injected".to_owned(), counts.injected.to_string()),
                ("failed".to_owned(), counts.failed.to_string()),
                ("skipped".to_owned(), counts.skipped.to_string()),
            ]),
            None,
        );
    }

    fn write(
        &mut self,
        severity: EventSeverity,
        area: EventArea,
        code: &str,
        params: BTreeMap<String, String>,
        detail: Option<String>,
    ) {
        let params = params
            .iter()
            .map(|(key, value)| (key.as_str(), value.as_str()))
            .collect::<Vec<_>>();
        if let Err(error) = self.writer.record(
            EventSource::ServiceHost,
            severity,
            area,
            code,
            &params,
            detail.as_deref(),
            &[],
        ) {
            if !self.write_error_reported {
                let error = error.to_string().replace(['\r', '\n'], " ");
                eprintln!("recording the service event log failed: {error}");
                self.write_error_reported = true;
            }
        }
    }
}

fn health_name(state: HealthState) -> &'static str {
    match state {
        HealthState::Unknown => "unknown",
        HealthState::Initializing => "initializing",
        HealthState::Ready => "ready",
        HealthState::Degraded => "degraded",
        HealthState::Failed => "failed",
    }
}

fn diagnostic_detail(record: &ProcessAttemptRecord) -> String {
    let detail = format!(
        "pid={} creation_time={} session_id={} disposition={:?} attempts={} reason={} win32={:?}",
        record.identity.pid,
        record.identity.creation_time,
        record.identity.session_id,
        record.broker_disposition,
        record.attempts,
        record.code,
        record.win32_error
    );
    mactype_service_contract::event_log::sanitize_text(&detail, &[], MAX_EVENT_DETAIL_BYTES)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{BrokerDisposition, ProcessIdentity};
    use mactype_service_contract::event_log::read_events;
    use mactype_service_contract::{ProfileDigest, RendererRuntimeBinding, RuntimeGenerationId};

    fn binding() -> RendererRuntimeBinding {
        RendererRuntimeBinding::new(
            RuntimeGenerationId::parse(&"a".repeat(64)).unwrap(),
            ProfileDigest::parse(&format!("sha256:{}", "b".repeat(64))).unwrap(),
        )
    }

    fn result(outcome: ProcessOutcome, code: &str) -> ProcessAttemptRecord {
        ProcessAttemptRecord {
            identity: ProcessIdentity {
                pid: 7,
                creation_time: 8,
                session_id: 9,
                architecture: ProcessArchitecture::X64,
            },
            binding: binding(),
            outcome,
            broker_disposition: BrokerDisposition::Rejected,
            attempts: 1,
            code: code.to_owned(),
            win32_error: Some(5),
        }
    }

    #[test]
    #[cfg_attr(
        all(miri, windows),
        ignore = "Windows Miri does not implement CreateDirectoryW"
    )]
    fn supported_stop_records_an_info_event_with_the_reason_and_message() {
        let root =
            std::env::temp_dir().join(format!("host-service-start-skipped-{}", std::process::id()));
        let path = root.join("host.log");
        let mut logger = HostEventLoggerState::new(path.clone(), Instant::now());
        let error = StructuredServiceError {
            code: "runtime-profile-absent".to_owned(),
            message: "the generated profile is absent".to_owned(),
            win32_error: None,
        };

        logger.service_start_skipped(&error);

        let events = read_events(&[path], 20);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].severity, EventSeverity::Info);
        assert_eq!(events[0].code, "service-start-skipped");
        assert_eq!(
            events[0].params.get("reason").map(String::as_str),
            Some("runtime-profile-absent")
        );
        assert_eq!(
            events[0].params.get("message").map(String::as_str),
            Some("the generated profile is absent")
        );
        let _ = std::fs::remove_dir_all(root);
    }

    #[test]
    #[cfg_attr(
        all(miri, windows),
        ignore = "Windows Miri does not implement CreateDirectoryW"
    )]
    fn injectable_clock_flushes_summary_and_resets_window() {
        let root = std::env::temp_dir().join(format!("host-summary-{}", std::process::id()));
        let path = root.join("host.log");
        let start = Instant::now();
        let mut logger = HostEventLoggerState::new(path.clone(), start);
        logger.injection_result(
            &result(ProcessOutcome::Injected, "ok"),
            "a.exe".to_owned(),
            String::new(),
            start,
        );
        logger.injection_result(
            &result(ProcessOutcome::Skipped, "quiet"),
            "b.exe".to_owned(),
            String::new(),
            start + SUMMARY_WINDOW,
        );
        logger.flush_summary(start + SUMMARY_WINDOW + Duration::from_secs(1));
        let events = read_events(&[path], 20);
        let summaries = events
            .iter()
            .filter(|event| event.code == "injection-summary")
            .collect::<Vec<_>>();
        assert_eq!(summaries.len(), 2);
        assert_eq!(
            summaries[0].params.get("injected").map(String::as_str),
            Some("1")
        );
        assert_eq!(
            summaries[1].params.get("skipped").map(String::as_str),
            Some("1")
        );
        let _ = std::fs::remove_dir_all(root);
    }

    #[test]
    #[cfg_attr(
        all(miri, windows),
        ignore = "Windows Miri does not implement CreateDirectoryW"
    )]
    fn quiet_skip_is_counted_without_a_warning_event() {
        let root = std::env::temp_dir().join(format!("host-quiet-skip-{}", std::process::id()));
        let path = root.join("host.log");
        let start = Instant::now();
        let mut logger = HostEventLoggerState::new(path.clone(), start);
        logger.injection_result(
            &result(ProcessOutcome::Skipped, "protected-process"),
            String::new(),
            String::new(),
            start,
        );
        logger.flush_summary(start + SUMMARY_WINDOW);
        let events = read_events(&[path], 20);
        assert!(events.iter().all(|event| event.code != "injection-failed"));
        assert_eq!(
            events[0].params.get("skipped").map(String::as_str),
            Some("1")
        );
        let _ = std::fs::remove_dir_all(root);
    }

    #[test]
    #[cfg_attr(
        all(miri, windows),
        ignore = "Windows Miri does not implement CreateDirectoryW"
    )]
    fn helper_failures_are_deduplicated_per_architecture() {
        let root = std::env::temp_dir().join(format!("host-helper-dedupe-{}", std::process::id()));
        let path = root.join("host.log");
        let start = Instant::now();
        let mut logger = HostEventLoggerState::new(path.clone(), start);
        logger.helper_broker_failed(ProcessArchitecture::X86, "first", None, start);
        logger.helper_broker_failed(
            ProcessArchitecture::X86,
            "second",
            None,
            start + Duration::from_secs(1),
        );
        logger.helper_broker_failed(
            ProcessArchitecture::X64,
            "third",
            None,
            start + Duration::from_secs(1),
        );
        let events = read_events(&[path], 20);
        assert_eq!(events.len(), 2);
        assert_eq!(
            events
                .iter()
                .map(|event| event.params["architecture"].as_str())
                .collect::<Vec<_>>(),
            ["x86", "x64"]
        );
        let _ = std::fs::remove_dir_all(root);
    }

    #[test]
    #[cfg_attr(
        all(miri, windows),
        ignore = "Windows Miri does not implement CreateDirectoryW"
    )]
    fn injectable_clock_deduplicates_failures_for_ten_minutes() {
        let root = std::env::temp_dir().join(format!("host-dedupe-{}", std::process::id()));
        let path = root.join("host.log");
        let start = Instant::now();
        let mut logger = HostEventLoggerState::new(path.clone(), start);
        let failure = result(ProcessOutcome::Rejected, "denied");
        logger.injection_result(&failure, "game.exe".to_owned(), "first".to_owned(), start);
        logger.injection_result(
            &failure,
            "game.exe".to_owned(),
            "second".to_owned(),
            start + Duration::from_secs(599),
        );
        logger.injection_result(
            &failure,
            "game.exe".to_owned(),
            "third".to_owned(),
            start + Duration::from_secs(600),
        );
        let events = read_events(&[path], 20);
        assert_eq!(
            events
                .iter()
                .filter(|event| event.code == "injection-failed")
                .count(),
            2
        );
        let _ = std::fs::remove_dir_all(root);
    }
}
