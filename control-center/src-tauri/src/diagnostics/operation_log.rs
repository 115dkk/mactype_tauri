use mactype_service_contract::event_log::{
    profile_file_name, sanitize_text, EventArea, EventLogWriter, EventRecord, EventSeverity,
    EventSource, EVENT_LOG_BACKUPS, EVENT_LOG_SCHEMA_VERSION, MAX_EVENT_CODE_BYTES,
    MAX_EVENT_DETAIL_BYTES, MAX_EVENT_LOG_BYTES, MAX_EVENT_PARAMS, MAX_EVENT_PARAM_BYTES,
};
use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeMap,
    fs,
    path::{Path, PathBuf},
    sync::atomic::{AtomicBool, Ordering},
};

const LOG_FILE_NAME: &str = "control-center.log";
static WRITE_ERROR_REPORTED: AtomicBool = AtomicBool::new(false);

#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub(crate) enum ActivityKind {
    ProfileApplied,
    ProfileVerified,
    ServiceStarted,
    ServiceInstalled,
    ServiceStopped,
}

#[derive(Clone, Debug)]
pub(crate) struct OperationFailure {
    pub(crate) operation: String,
    pub(crate) stage: String,
    pub(crate) error_chain: String,
    pub(crate) broker_exit_code: Option<u32>,
    pub(crate) channel_failure: Option<String>,
    pub(crate) rollback: String,
    pub(crate) final_state: String,
    pub(crate) installation_preflight: Option<InstallationPreflightDiagnostics>,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct InstallationPreflightDiagnostics {
    pub(crate) expected_installed_control_center: Option<String>,
    pub(crate) current_executable: Option<String>,
    pub(crate) expected_executable_exists: Option<bool>,
    pub(crate) installed_control_center: String,
    #[serde(default = "not_checked")]
    pub(crate) current_bundle: String,
    #[serde(default = "not_checked")]
    pub(crate) selected_service_package: String,
    pub(crate) setup_broker: String,
    pub(crate) runtime_manifest: String,
    #[serde(default = "not_checked")]
    pub(crate) runtime_payload: String,
    pub(crate) elevation_attempted: bool,
    #[serde(default = "not_checked")]
    pub(crate) elevated_revalidation: String,
    pub(crate) machine_state_changed: bool,
    pub(crate) rollback_required: bool,
}

fn not_checked() -> String {
    "not-checked".to_owned()
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct OperationLogEntry {
    timestamp_unix_ms: u64,
    operation: String,
    stage: String,
    error_chain: String,
    win32_code: Option<u32>,
    broker_exit_code: Option<u32>,
    channel_failure: Option<String>,
    rollback: String,
    final_state: String,
    #[serde(default)]
    installation_preflight: Option<InstallationPreflightDiagnostics>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct ActivityLogEntry {
    timestamp_unix_ms: u64,
    activity: ActivityKind,
    profile: Option<String>,
}

pub(crate) fn record_operation_failure(
    failure: &OperationFailure,
    redactions: &[&str],
) -> Result<PathBuf, String> {
    let result =
        super::log_root().and_then(|root| record_operation_failure_at(&root, failure, redactions));
    if let Err(error) = &result {
        report_write_error(error);
    }
    result
}

pub(super) fn record_operation_failure_at(
    root: &Path,
    failure: &OperationFailure,
    redactions: &[&str],
) -> Result<PathBuf, String> {
    let mut params = BTreeMap::from([
        ("operation".to_owned(), failure.operation.clone()),
        ("stage".to_owned(), failure.stage.clone()),
        ("rollback".to_owned(), failure.rollback.clone()),
    ]);
    if let Some(code) = extract_numeric_code(&failure.error_chain, "Win32 ") {
        params.insert("win32Code".to_owned(), code.to_string());
    }
    if let Some(code) = failure
        .broker_exit_code
        .or_else(|| extract_numeric_code(&failure.error_chain, "broker exit code "))
    {
        params.insert("brokerExitCode".to_owned(), code.to_string());
    }
    if let Some(channel) = &failure.channel_failure {
        params.insert("channelFailure".to_owned(), channel.clone());
    }
    let mut detail = format!(
        "{}\nfinalState={}",
        failure.error_chain, failure.final_state
    );
    if let Some(preflight) = &failure.installation_preflight {
        detail.push('\n');
        detail.push_str(&render_preflight(preflight));
    }
    append_at(
        root,
        EventSeverity::Error,
        EventArea::Setup,
        "operation-failed",
        params,
        Some(detail),
        redactions,
    )
}

pub(crate) fn record_activity(
    activity: ActivityKind,
    profile: Option<&str>,
) -> Result<PathBuf, String> {
    let result = super::log_root().and_then(|root| record_activity_at(&root, activity, profile));
    if let Err(error) = &result {
        report_write_error(error);
    }
    result
}

pub(super) fn record_activity_at(
    root: &Path,
    activity: ActivityKind,
    profile: Option<&str>,
) -> Result<PathBuf, String> {
    let (area, code) = match activity {
        ActivityKind::ProfileApplied => (EventArea::Profile, "profile-applied"),
        ActivityKind::ProfileVerified => (EventArea::Profile, "profile-verified"),
        ActivityKind::ServiceStarted => (EventArea::Service, "service-started"),
        ActivityKind::ServiceInstalled => (EventArea::Service, "service-installed"),
        ActivityKind::ServiceStopped => (EventArea::Service, "service-stopped"),
    };
    let params = profile
        .map(|profile| BTreeMap::from([("profile".to_owned(), profile_file_name(profile))]))
        .unwrap_or_default();
    append_at(root, EventSeverity::Info, area, code, params, None, &[])
}

pub(crate) fn record_control_center_event(
    severity: EventSeverity,
    area: EventArea,
    code: &str,
    params: BTreeMap<String, String>,
) {
    if let Err(error) =
        super::log_root().and_then(|root| append_at(&root, severity, area, code, params, None, &[]))
    {
        report_write_error(&error);
    }
}

fn report_write_error(error: &str) {
    if !WRITE_ERROR_REPORTED.swap(true, Ordering::AcqRel) {
        eprintln!(
            "recording the Control Center event log failed: {}",
            error.replace(['\r', '\n'], " ")
        );
    }
}

fn append_at(
    root: &Path,
    severity: EventSeverity,
    area: EventArea,
    code: &str,
    params: BTreeMap<String, String>,
    detail: Option<String>,
    redactions: &[&str],
) -> Result<PathBuf, String> {
    let path = root.join(LOG_FILE_NAME);
    let params = params
        .iter()
        .map(|(key, value)| (key.as_str(), value.as_str()))
        .collect::<Vec<_>>();
    EventLogWriter::new(path.clone())
        .record(
            EventSource::ControlCenter,
            severity,
            area,
            code,
            &params,
            detail.as_deref(),
            redactions,
        )
        .map_err(|error| error.to_string())?;
    Ok(path)
}

pub(super) fn read_all_at(root: &Path) -> Vec<EventRecord> {
    let base = root.join(LOG_FILE_NAME);
    let mut entries = Vec::new();
    let mut order = 0_u64;
    for path in (1..=EVENT_LOG_BACKUPS)
        .rev()
        .map(|index| PathBuf::from(format!("{}.{index}", base.display())))
        .chain(std::iter::once(base.clone()))
    {
        let bytes = match fs::read(path) {
            Ok(bytes) if bytes.len() as u64 <= MAX_EVENT_LOG_BYTES => bytes,
            _ => continue,
        };
        for line in String::from_utf8_lossy(&bytes).lines() {
            let record = serde_json::from_str::<EventRecord>(line)
                .ok()
                .filter(valid_event)
                .or_else(|| {
                    serde_json::from_str::<OperationLogEntry>(line)
                        .map(convert_operation)
                        .or_else(|_| {
                            serde_json::from_str::<ActivityLogEntry>(line).map(convert_activity)
                        })
                        .ok()
                });
            if let Some(record) = record {
                entries.push((record, order));
            }
            order = order.saturating_add(1);
        }
    }
    entries.sort_by_key(|(record, order)| (record.ts, *order));
    entries.into_iter().map(|(record, _)| record).collect()
}

fn valid_event(record: &EventRecord) -> bool {
    record.v == EVENT_LOG_SCHEMA_VERSION
        && !record.code.is_empty()
        && record.code.len() <= MAX_EVENT_CODE_BYTES
        && record
            .code
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
        && record.params.len() <= MAX_EVENT_PARAMS
        && record.params.iter().all(|(key, value)| {
            key.len() <= MAX_EVENT_PARAM_BYTES && value.len() <= MAX_EVENT_PARAM_BYTES
        })
        && record
            .detail
            .as_ref()
            .map_or(true, |detail| detail.len() <= MAX_EVENT_DETAIL_BYTES)
}

fn convert_operation(entry: OperationLogEntry) -> EventRecord {
    let mut params = BTreeMap::from([
        (
            "operation".to_owned(),
            sanitize_text(&entry.operation, &[], MAX_EVENT_PARAM_BYTES),
        ),
        (
            "stage".to_owned(),
            sanitize_text(&entry.stage, &[], MAX_EVENT_PARAM_BYTES),
        ),
        (
            "rollback".to_owned(),
            sanitize_text(&entry.rollback, &[], MAX_EVENT_PARAM_BYTES),
        ),
    ]);
    if let Some(code) = entry.win32_code {
        params.insert("win32Code".to_owned(), code.to_string());
    }
    if let Some(code) = entry.broker_exit_code {
        params.insert("brokerExitCode".to_owned(), code.to_string());
    }
    if let Some(channel) = entry.channel_failure {
        params.insert(
            "channelFailure".to_owned(),
            sanitize_text(&channel, &[], MAX_EVENT_PARAM_BYTES),
        );
    }
    let mut detail = format!("{}\nfinalState={}", entry.error_chain, entry.final_state);
    if let Some(preflight) = entry.installation_preflight {
        detail.push('\n');
        detail.push_str(&render_preflight(&preflight));
    }
    EventRecord::new(
        entry.timestamp_unix_ms,
        EventSeverity::Error,
        EventArea::Setup,
        "operation-failed",
        params,
        Some(sanitize_text(&detail, &[], MAX_EVENT_DETAIL_BYTES)),
        EventSource::ControlCenter,
    )
}

fn convert_activity(entry: ActivityLogEntry) -> EventRecord {
    let (area, code) = match entry.activity {
        ActivityKind::ProfileApplied => (EventArea::Profile, "profile-applied"),
        ActivityKind::ProfileVerified => (EventArea::Profile, "profile-verified"),
        ActivityKind::ServiceStarted => (EventArea::Service, "service-started"),
        ActivityKind::ServiceInstalled => (EventArea::Service, "service-installed"),
        ActivityKind::ServiceStopped => (EventArea::Service, "service-stopped"),
    };
    let params = entry
        .profile
        .map(|profile| BTreeMap::from([("profile".to_owned(), profile_file_name(&profile))]))
        .unwrap_or_default();
    EventRecord::new(
        entry.timestamp_unix_ms,
        EventSeverity::Info,
        area,
        code,
        params,
        None,
        EventSource::ControlCenter,
    )
}

fn render_preflight(preflight: &InstallationPreflightDiagnostics) -> String {
    let yes_no = |value| if value { "yes" } else { "no" };
    format!(
        "Expected installed Control Center: {}\nCurrent executable: {}\nExpected executable exists: {:?}\nInstalled Control Center: {}\nCurrent bundle: {}\nSelected service package: {}\nSetup broker: {}\nRuntime manifest: {}\nRuntime payload: {}\nElevation attempted: {}\nElevated revalidation: {}\nMachine state changed: {}\nRollback required: {}",
        preflight.expected_installed_control_center.as_deref().unwrap_or("not registered"),
        preflight.current_executable.as_deref().unwrap_or("unavailable"),
        preflight.expected_executable_exists,
        preflight.installed_control_center,
        preflight.current_bundle.replace('-', " "),
        preflight.selected_service_package.replace('-', " "),
        preflight.setup_broker.replace('-', " "),
        preflight.runtime_manifest.replace('-', " "),
        preflight.runtime_payload.replace('-', " "),
        yes_no(preflight.elevation_attempted),
        preflight.elevated_revalidation.replace('-', " "),
        yes_no(preflight.machine_state_changed),
        yes_no(preflight.rollback_required),
    )
}

fn extract_numeric_code(value: &str, marker: &str) -> Option<u32> {
    let suffix = value.split_once(marker)?.1;
    let digits = suffix
        .bytes()
        .take_while(u8::is_ascii_digit)
        .collect::<Vec<_>>();
    (!digits.is_empty())
        .then(|| std::str::from_utf8(&digits).ok()?.parse().ok())
        .flatten()
}
