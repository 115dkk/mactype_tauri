use super::operation_log;
use mactype_service_contract::event_log::{
    EventArea, EventRecord, EventSeverity, EventSource, MAX_EVENT_DETAIL_BYTES,
    MAX_EVENT_LOG_BYTES, MAX_EVENT_PARAM_BYTES,
};
use std::{
    backtrace::Backtrace,
    collections::BTreeMap,
    fmt::{self, Write as _},
    fs::{self, OpenOptions},
    io::{self, Write as _},
    path::{Path, PathBuf},
    sync::atomic::{AtomicBool, Ordering},
    time::{SystemTime, UNIX_EPOCH},
};

static PANIC_HOOK_ACTIVE: AtomicBool = AtomicBool::new(false);

pub(super) fn install() {
    std::panic::set_hook(Box::new(|info| {
        if PANIC_HOOK_ACTIVE
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return;
        }

        let payload = info.payload();
        let message = if let Some(message) = payload.downcast_ref::<&str>() {
            bounded_text(message, MAX_EVENT_PARAM_BYTES)
        } else if let Some(message) = payload.downcast_ref::<String>() {
            bounded_text(message, MAX_EVENT_PARAM_BYTES)
        } else {
            "non-string panic payload".to_owned()
        };
        let location = match info.location() {
            Some(location) => bounded_arguments(
                format_args!(
                    "{}:{}:{}",
                    location.file(),
                    location.line(),
                    location.column()
                ),
                MAX_EVENT_PARAM_BYTES,
            ),
            None => "unknown".to_owned(),
        };
        let current_thread = std::thread::current();
        let thread = match current_thread.name() {
            Some(name) => bounded_text(name, MAX_EVENT_PARAM_BYTES),
            None => "unnamed".to_owned(),
        };
        let backtrace = Backtrace::force_capture();
        let _ = record_at_default_root(&message, &location, &thread, &backtrace);
        PANIC_HOOK_ACTIVE.store(false, Ordering::Release);
    }));
}

fn record_at_default_root(
    message: &str,
    location: &str,
    thread: &str,
    backtrace: &dyn fmt::Display,
) -> Option<PathBuf> {
    let root = super::log_root().ok()?;
    record_at(&root, message, location, thread, backtrace)
}

fn record_at(
    root: &Path,
    message: &str,
    location: &str,
    thread: &str,
    backtrace: &dyn fmt::Display,
) -> Option<PathBuf> {
    let params = BTreeMap::from([
        (
            "location".to_owned(),
            bounded_text(location, MAX_EVENT_PARAM_BYTES),
        ),
        (
            "message".to_owned(),
            bounded_text(message, MAX_EVENT_PARAM_BYTES),
        ),
        (
            "thread".to_owned(),
            bounded_text(thread, MAX_EVENT_PARAM_BYTES),
        ),
    ]);
    let mut detail = BoundedText::new(MAX_EVENT_DETAIL_BYTES);
    let _ = detail.write_str("Backtrace:\n");
    let _ = fmt::write(&mut detail, format_args!("{backtrace}"));
    let timestamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .ok()?
        .as_millis();
    let record = EventRecord::new(
        u64::try_from(timestamp).ok()?,
        EventSeverity::Error,
        EventArea::ControlCenter,
        "panic",
        params,
        Some(detail.into_string()),
        EventSource::ControlCenter,
    );

    let mut line = format_record_line(&record)?;
    line.prepend_newline()?;
    let path = root.join(operation_log::LOG_FILE_NAME);

    // Bypass EventLogWriter because a panic may already hold its process-wide append mutex.
    fs::create_dir_all(root).ok()?;
    let mut file = OpenOptions::new()
        .create(true)
        .append(true)
        .open(&path)
        .ok()?;
    let current = file.metadata().ok()?.len();
    if current == 0 {
        line.remove_leading_newline();
    }
    let incoming = u64::try_from(line.len()).ok()?;
    if current.checked_add(incoming)? > MAX_EVENT_LOG_BYTES {
        return None;
    }
    file.write_all(line.as_slice()).ok()?;
    file.flush().ok()?;
    file.sync_data().ok()?;
    Some(path)
}

fn format_record_line(record: &EventRecord) -> Option<BoundedBytes> {
    let mut line = BoundedBytes::new(MAX_EVENT_LOG_BYTES as usize);
    serde_json::to_writer(&mut line, record).ok()?;
    line.push_newline()?;
    Some(line)
}

fn bounded_text(value: &str, maximum_bytes: usize) -> String {
    let mut output = BoundedText::new(maximum_bytes);
    let _ = output.write_str(value);
    output.into_string()
}

fn bounded_arguments(arguments: fmt::Arguments<'_>, maximum_bytes: usize) -> String {
    let mut output = BoundedText::new(maximum_bytes);
    let _ = fmt::write(&mut output, arguments);
    output.into_string()
}

struct BoundedText {
    value: String,
    maximum_bytes: usize,
}

impl BoundedText {
    fn new(maximum_bytes: usize) -> Self {
        Self {
            value: String::with_capacity(maximum_bytes),
            maximum_bytes,
        }
    }

    fn into_string(self) -> String {
        self.value
    }
}

impl fmt::Write for BoundedText {
    fn write_str(&mut self, value: &str) -> fmt::Result {
        let mut remaining = self.maximum_bytes.saturating_sub(self.value.len());
        for character in value.chars() {
            let character_bytes = character.len_utf8();
            if character_bytes > remaining {
                break;
            }
            self.value.push(character);
            remaining -= character_bytes;
        }
        Ok(())
    }
}

struct BoundedBytes {
    value: Vec<u8>,
    maximum_bytes: usize,
}

impl BoundedBytes {
    fn new(maximum_bytes: usize) -> Self {
        Self {
            value: Vec::with_capacity(maximum_bytes.min(4096)),
            maximum_bytes,
        }
    }

    fn push_newline(&mut self) -> Option<()> {
        if self.value.len() >= self.maximum_bytes {
            return None;
        }
        self.value.push(b'\n');
        Some(())
    }

    fn prepend_newline(&mut self) -> Option<()> {
        if self.value.len() >= self.maximum_bytes {
            return None;
        }
        self.value.insert(0, b'\n');
        Some(())
    }

    fn remove_leading_newline(&mut self) {
        if self.value.first() == Some(&b'\n') {
            self.value.remove(0);
        }
    }

    fn len(&self) -> usize {
        self.value.len()
    }

    fn as_slice(&self) -> &[u8] {
        &self.value
    }
}

impl io::Write for BoundedBytes {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if bytes.len() > self.maximum_bytes.saturating_sub(self.value.len()) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "panic event exceeds the event-log byte limit",
            ));
        }
        self.value.extend_from_slice(bytes);
        Ok(bytes.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use mactype_service_contract::event_log::{MAX_EVENT_CODE_BYTES, MAX_EVENT_PARAMS};
    use std::{env, time::SystemTime};

    fn unique_root(name: &str) -> PathBuf {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|duration| duration.as_nanos())
            .unwrap_or_default();
        env::temp_dir().join(format!("mactype-{name}-{}-{unique}", std::process::id()))
    }

    #[test]
    fn panic_formatter_escapes_fields_into_one_json_line() {
        let record = EventRecord::new(
            7,
            EventSeverity::Error,
            EventArea::ControlCenter,
            "panic",
            BTreeMap::from([
                ("location".to_owned(), "fixture.rs:7:3".to_owned()),
                (
                    "message".to_owned(),
                    "quote=\" slash=\\ newline=\n".to_owned(),
                ),
                ("thread".to_owned(), "fixture-thread".to_owned()),
            ]),
            Some("Backtrace:\nfixture\\trace".to_owned()),
            EventSource::ControlCenter,
        );

        let line = format_record_line(&record).unwrap();

        assert_eq!(
            line.as_slice()
                .iter()
                .filter(|byte| **byte == b'\n')
                .count(),
            1
        );
        assert_eq!(line.as_slice().last(), Some(&b'\n'));
        let decoded: EventRecord =
            serde_json::from_slice(&line.as_slice()[..line.len() - 1]).unwrap();
        assert_eq!(decoded, record);
    }

    #[test]
    fn panic_record_is_well_formed_and_bounded() {
        let root = unique_root("panic-log");
        let message = format!("fixture-message-{}", "m".repeat(MAX_EVENT_PARAM_BYTES * 2));
        let location = format!("fixture.rs:17:9-{}", "l".repeat(MAX_EVENT_PARAM_BYTES * 2));
        let thread = format!("fixture-thread-{}", "t".repeat(MAX_EVENT_PARAM_BYTES * 2));
        let backtrace = format!(
            "fixture-backtrace\n{}",
            "b".repeat(MAX_EVENT_DETAIL_BYTES * 2)
        );

        record_at(&root, &message, &location, &thread, &backtrace).unwrap();

        let events = operation_log::read_all_at(&root);
        assert_eq!(events.len(), 1);
        let event = &events[0];
        assert_eq!(event.code, "panic");
        assert_eq!(event.severity, EventSeverity::Error);
        assert_eq!(event.area, EventArea::ControlCenter);
        assert_eq!(event.source, EventSource::ControlCenter);
        assert!(event.params["message"].starts_with("fixture-message-"));
        assert!(event.params["location"].starts_with("fixture.rs:17:9"));
        assert!(event.params["thread"].starts_with("fixture-thread-"));
        assert!(event
            .detail
            .as_deref()
            .is_some_and(|detail| detail.contains("fixture-backtrace")));
        assert!(event.code.len() <= MAX_EVENT_CODE_BYTES);
        assert!(event.params.len() <= MAX_EVENT_PARAMS);
        assert!(event
            .params
            .iter()
            .all(|(key, value)| key.len() <= MAX_EVENT_PARAM_BYTES
                && value.len() <= MAX_EVENT_PARAM_BYTES));
        assert!(event
            .detail
            .as_ref()
            .is_some_and(|detail| detail.len() <= MAX_EVENT_DETAIL_BYTES));
        assert!(fs::metadata(root.join(operation_log::LOG_FILE_NAME))
            .is_ok_and(|metadata| metadata.len() <= MAX_EVENT_LOG_BYTES));
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn panic_writer_skips_a_full_log_without_rotation() {
        let root = unique_root("panic-full-log");
        fs::create_dir_all(&root).unwrap();
        let path = root.join(operation_log::LOG_FILE_NAME);
        fs::write(&path, vec![b'x'; MAX_EVENT_LOG_BYTES as usize]).unwrap();

        assert!(record_at(&root, "message", "file.rs:1:1", "thread", &"trace").is_none());
        assert_eq!(fs::metadata(&path).unwrap().len(), MAX_EVENT_LOG_BYTES);
        assert!(!PathBuf::from(format!("{}.1", path.display())).exists());
        let _ = fs::remove_dir_all(root);
    }
}
