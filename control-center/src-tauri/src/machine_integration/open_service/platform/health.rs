use super::super::{read_bounded_regular_file, HealthReport, LiveHealthReport};
use mactype_service_contract::effective_health_pipe_name;
use std::{fs, io::Read, os::windows::io::AsRawHandle, path::Path};
use windows_sys::Win32::System::Pipes::GetNamedPipeServerProcessId;

const MAX_HEALTH_BYTES: u64 = 16 * 1024;

pub(super) fn read_health() -> Result<LiveHealthReport, String> {
    let file = fs::File::open(effective_health_pipe_name()).map_err(|error| error.to_string())?;
    let mut server_pid = 0;
    if unsafe { GetNamedPipeServerProcessId(file.as_raw_handle().cast(), &mut server_pid) } == 0
        || server_pid == 0
    {
        return Err("service health pipe server PID is unavailable".to_owned());
    }
    let mut bytes = Vec::new();
    file.take(MAX_HEALTH_BYTES + 1)
        .read_to_end(&mut bytes)
        .map_err(|error| error.to_string())?;
    if bytes.is_empty() || bytes.len() > MAX_HEALTH_BYTES as usize {
        return Err("service health response is empty or too large".to_owned());
    }
    let report: HealthReport = serde_json::from_slice(&bytes).map_err(|error| error.to_string())?;
    report.validate().map_err(|error| error.to_string())?;
    Ok(LiveHealthReport { server_pid, report })
}

pub(in crate::machine_integration::open_service) fn read_health_for_scm_process(
    process_id: u32,
) -> Result<HealthReport, String> {
    let live = read_health()?;
    if process_id == 0 || live.server_pid != process_id {
        return Err(format!(
            "service health pipe PID {} does not match SCM PID {process_id}",
            live.server_pid
        ));
    }
    Ok(live.report)
}

pub(super) fn read_persisted_health(service_root: &Path) -> Result<HealthReport, String> {
    let bytes = read_bounded_regular_file(
        &service_root.join("health.json"),
        MAX_HEALTH_BYTES,
        "persisted service health snapshot",
    )?;
    let report: HealthReport = serde_json::from_slice(&bytes).map_err(|error| error.to_string())?;
    report.validate().map_err(|error| error.to_string())?;
    Ok(report)
}
