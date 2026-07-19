use serde::{Deserialize, Serialize};
use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

const LOG_FILE_NAME: &str = "control-center.log";
const MAX_LOG_BYTES: u64 = 512 * 1024;
const LOG_BACKUPS: usize = 4;
const MAX_OPERATION_BYTES: usize = 64;
const MAX_STAGE_BYTES: usize = 256;
const MAX_ERROR_BYTES: usize = 24 * 1024;
const MAX_CHANNEL_ERROR_BYTES: usize = 2 * 1024;
const MAX_ROLLBACK_BYTES: usize = 512;
const MAX_FINAL_STATE_BYTES: usize = 2 * 1024;

#[derive(Clone, Debug)]
pub(crate) struct OperationFailure {
    pub(crate) operation: String,
    pub(crate) stage: String,
    pub(crate) error_chain: String,
    pub(crate) broker_exit_code: Option<u32>,
    pub(crate) channel_failure: Option<String>,
    pub(crate) rollback: String,
    pub(crate) final_state: String,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct OperationLogEntry {
    pub(crate) timestamp_unix_ms: u64,
    pub(crate) operation: String,
    pub(crate) stage: String,
    pub(crate) error_chain: String,
    pub(crate) win32_code: Option<u32>,
    pub(crate) broker_exit_code: Option<u32>,
    pub(crate) channel_failure: Option<String>,
    pub(crate) rollback: String,
    pub(crate) final_state: String,
}

impl OperationLogEntry {
    pub(crate) fn render(&self) -> String {
        let mut value = format!(
            "{} operation={} stage={} error={}",
            self.timestamp_unix_ms, self.operation, self.stage, self.error_chain
        );
        if let Some(code) = self.win32_code {
            value.push_str(&format!(" win32Code={code}"));
        }
        if let Some(code) = self.broker_exit_code {
            value.push_str(&format!(" brokerExitCode={code}"));
        }
        if let Some(channel) = &self.channel_failure {
            value.push_str(&format!(" channelFailure={channel}"));
        }
        value.push_str(&format!(
            " rollback={} finalState={}",
            self.rollback, self.final_state
        ));
        value
    }
}

pub(crate) fn record_operation_failure(
    failure: &OperationFailure,
    redactions: &[&str],
) -> Result<PathBuf, String> {
    record_operation_failure_at(&super::log_root()?, failure, redactions)
}

pub(super) fn record_operation_failure_at(
    root: &Path,
    failure: &OperationFailure,
    redactions: &[&str],
) -> Result<PathBuf, String> {
    fs::create_dir_all(root).map_err(|error| error.to_string())?;
    let error_chain = sanitize(&failure.error_chain, redactions, MAX_ERROR_BYTES);
    let entry = OperationLogEntry {
        timestamp_unix_ms: timestamp_unix_ms()?,
        operation: sanitize(&failure.operation, &[], MAX_OPERATION_BYTES),
        stage: sanitize(&failure.stage, &[], MAX_STAGE_BYTES),
        win32_code: extract_numeric_code(&error_chain, "Win32 "),
        broker_exit_code: failure
            .broker_exit_code
            .or_else(|| extract_numeric_code(&error_chain, "broker exit code ")),
        channel_failure: failure
            .channel_failure
            .as_deref()
            .map(|value| sanitize(value, redactions, MAX_CHANNEL_ERROR_BYTES)),
        rollback: sanitize(&failure.rollback, &[], MAX_ROLLBACK_BYTES),
        final_state: sanitize(&failure.final_state, &[], MAX_FINAL_STATE_BYTES),
        error_chain,
    };
    let mut line = serde_json::to_vec(&entry).map_err(|error| error.to_string())?;
    line.push(b'\n');
    let path = root.join(LOG_FILE_NAME);
    rotate_if_needed(root, line.len() as u64)?;
    let mut file = fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&path)
        .map_err(|error| error.to_string())?;
    file.write_all(&line).map_err(|error| error.to_string())?;
    file.flush().map_err(|error| error.to_string())?;
    Ok(path)
}

pub(crate) fn read_recent_operation_logs(limit: usize) -> Result<Vec<OperationLogEntry>, String> {
    read_recent_operation_logs_at(&super::log_root()?, limit)
}

pub(super) fn read_recent_operation_logs_at(
    root: &Path,
    limit: usize,
) -> Result<Vec<OperationLogEntry>, String> {
    if limit == 0 || !root.exists() {
        return Ok(Vec::new());
    }
    let mut paths = (1..=LOG_BACKUPS)
        .rev()
        .map(|index| root.join(format!("{LOG_FILE_NAME}.{index}")))
        .collect::<Vec<_>>();
    paths.push(root.join(LOG_FILE_NAME));
    let mut entries = Vec::new();
    for path in paths {
        let bytes = match fs::read(&path) {
            Ok(bytes) => bytes,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => continue,
            Err(error) => return Err(error.to_string()),
        };
        if bytes.len() as u64 > MAX_LOG_BYTES {
            continue;
        }
        for line in String::from_utf8_lossy(&bytes).lines() {
            if let Ok(entry) = serde_json::from_str::<OperationLogEntry>(line) {
                entries.push(entry);
            }
        }
    }
    let start = entries.len().saturating_sub(limit);
    Ok(entries.split_off(start))
}

fn rotate_if_needed(root: &Path, incoming: u64) -> Result<(), String> {
    let current = root.join(LOG_FILE_NAME);
    let current_len = match fs::metadata(&current) {
        Ok(metadata) => metadata.len(),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => 0,
        Err(error) => return Err(error.to_string()),
    };
    if current_len.saturating_add(incoming) <= MAX_LOG_BYTES {
        return Ok(());
    }
    for index in (1..=LOG_BACKUPS).rev() {
        let destination = root.join(format!("{LOG_FILE_NAME}.{index}"));
        if destination.exists() {
            fs::remove_file(&destination).map_err(|error| error.to_string())?;
        }
        let source = if index == 1 {
            current.clone()
        } else {
            root.join(format!("{LOG_FILE_NAME}.{}", index - 1))
        };
        match fs::rename(&source, &destination) {
            Ok(()) => {}
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(error.to_string()),
        }
    }
    Ok(())
}

fn timestamp_unix_ms() -> Result<u64, String> {
    let millis = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|error| error.to_string())?
        .as_millis();
    u64::try_from(millis).map_err(|_| "system timestamp exceeds the log format".to_owned())
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

fn sanitize(value: &str, redactions: &[&str], maximum_bytes: usize) -> String {
    let mut sanitized = value.replace(['\r', '\n'], " ");
    for secret in redactions.iter().filter(|secret| !secret.is_empty()) {
        sanitized = sanitized.replace(secret, "[redacted-profile]");
    }
    sanitized = redact_nonce_candidates(&sanitized);
    bounded_text(&sanitized, maximum_bytes)
}

fn redact_nonce_candidates(value: &str) -> String {
    let bytes = value.as_bytes();
    let mut output = String::with_capacity(value.len());
    let mut cursor = 0;
    while cursor < bytes.len() {
        if bytes[cursor].is_ascii_hexdigit() {
            let start = cursor;
            while cursor < bytes.len() && bytes[cursor].is_ascii_hexdigit() {
                cursor += 1;
            }
            if cursor - start == 32 {
                output.push_str("[redacted-nonce]");
            } else {
                output.push_str(&value[start..cursor]);
            }
        } else {
            let character = value[cursor..]
                .chars()
                .next()
                .expect("cursor is on a character boundary");
            output.push(character);
            cursor += character.len_utf8();
        }
    }
    output
}

fn bounded_text(value: &str, maximum_bytes: usize) -> String {
    if value.len() <= maximum_bytes {
        return value.to_owned();
    }
    let suffix = " [truncated]";
    let mut end = maximum_bytes.saturating_sub(suffix.len());
    while !value.is_char_boundary(end) {
        end = end.saturating_sub(1);
    }
    let mut bounded = value[..end].to_owned();
    bounded.push_str(suffix);
    bounded
}
