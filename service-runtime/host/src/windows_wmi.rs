use std::time::Duration;

use mactype_service_contract::StructuredServiceError;
use mactype_service_platform::{
    ComApartment, ComThreading, WmiConnection, WmiEnumerator, WmiError, WmiLocator, WmiObject,
    WmiPropertyStep,
};

use crate::ProcessEventSource;

const PROCESS_SNAPSHOT_QUERY: &str = "SELECT ProcessID FROM Win32_Process";
const SNAPSHOT_ROW_TIMEOUT: Duration = Duration::from_millis(5_000);

pub struct WmiProcessEventSource {
    enumerator: Option<WmiEnumerator>,
    connection: WmiConnection,
    _apartment: ComApartment,
}

impl WmiProcessEventSource {
    pub fn connect() -> Result<Self, StructuredServiceError> {
        let apartment = ComApartment::initialize(ComThreading::Multi).map_err(|hresult| {
            service_error(
                "com-initialization-failed",
                "COM could not be initialized for the WMI observer",
                Some(hresult as u32),
            )
        })?;
        WmiConnection::initialize_security().map_err(|error| {
            wmi_error(
                "com-security-initialization-failed",
                "COM security could not be initialized for LocalSystem WMI access",
                error,
            )
        })?;
        let locator = WmiLocator::create().map_err(|error| {
            wmi_error(
                "wmi-locator-unavailable",
                "WMI locator creation failed",
                error,
            )
        })?;
        let namespace = locator.open_cimv2().map_err(|error| {
            wmi_error(
                "wmi-namespace-unavailable",
                "the ROOT\\CIMV2 WMI namespace could not be opened",
                error,
            )
        })?;
        let connection = namespace.with_service_blanket().map_err(|error| {
            wmi_error(
                "wmi-proxy-security-failed",
                "WMI proxy security could not be configured for LocalSystem",
                error,
            )
        })?;
        Ok(Self {
            enumerator: None,
            connection,
            _apartment: apartment,
        })
    }
}

impl ProcessEventSource for WmiProcessEventSource {
    fn subscribe(&mut self, query: &str) -> Result<(), StructuredServiceError> {
        let enumerator = self.connection.notification_query(query).map_err(|error| {
            wmi_error(
                "wmi-subscription-failed",
                "the temporary Win32_Process creation subscription failed",
                error,
            )
        })?;
        self.enumerator = Some(enumerator);
        Ok(())
    }

    fn snapshot_pids(&mut self) -> Result<Vec<u32>, StructuredServiceError> {
        let enumerator = self
            .connection
            .query(PROCESS_SNAPSHOT_QUERY)
            .map_err(|error| {
                wmi_error(
                    "wmi-snapshot-failed",
                    "the initial Win32_Process snapshot could not be opened",
                    error,
                )
            })?;
        let mut pids = Vec::new();
        loop {
            let process = match enumerator.next(SNAPSHOT_ROW_TIMEOUT) {
                Ok(Some(process)) => process,
                Ok(None) => break,
                Err(error) => {
                    return Err(wmi_error(
                        "wmi-snapshot-failed",
                        "the initial Win32_Process snapshot could not be read",
                        error,
                    ))
                }
            };
            let pid = extract_process_id_property(&process)?;
            if pid != 0 {
                pids.push(pid);
            }
        }
        pids.sort_unstable();
        pids.dedup();
        Ok(pids)
    }

    fn next_pid(&mut self, timeout: Duration) -> Result<Option<u32>, StructuredServiceError> {
        let enumerator = self.enumerator.as_ref().ok_or_else(|| {
            service_error(
                "wmi-not-subscribed",
                "the WMI process observer was not subscribed",
                None,
            )
        })?;
        let event = match enumerator.next(timeout) {
            Ok(Some(event)) => event,
            Ok(None) => return Ok(None),
            Err(error) => {
                return Err(wmi_error(
                    "wmi-observer-failed",
                    "the WMI process observer failed while waiting for an event",
                    error,
                ))
            }
        };
        extract_process_id(&event).map(Some)
    }
}

fn extract_process_id(event: &WmiObject) -> Result<u32, StructuredServiceError> {
    let target = event.target_instance_with_step().map_err(|failure| {
        let message = match failure.step {
            WmiPropertyStep::Get => "the WMI process event has no TargetInstance",
            WmiPropertyStep::Convert => "the WMI process event TargetInstance is not an object",
            WmiPropertyStep::Cast => {
                "the WMI process event TargetInstance is not a Win32_Process object"
            }
        };
        wmi_error("wmi-event-invalid", message, failure.error)
    })?;
    let pid = extract_process_id_property(&target)?;
    if pid == 0 {
        return Err(service_error(
            "wmi-event-invalid",
            "the WMI process event ProcessID is zero",
            None,
        ));
    }
    Ok(pid)
}

fn extract_process_id_property(target: &WmiObject) -> Result<u32, StructuredServiceError> {
    target
        .process_id_with_step()
        .map_err(|failure| match failure.step {
            WmiPropertyStep::Get => wmi_error(
                "wmi-process-id-invalid",
                "the WMI process object has no ProcessID",
                failure.error,
            ),
            WmiPropertyStep::Convert | WmiPropertyStep::Cast => wmi_error(
                "wmi-event-invalid",
                "the WMI process event ProcessID is invalid",
                failure.error,
            ),
        })
}

fn wmi_error(code: &str, message: &str, error: WmiError) -> StructuredServiceError {
    service_error(code, message, Some(error.hresult as u32))
}

fn service_error(code: &str, message: &str, win32_error: Option<u32>) -> StructuredServiceError {
    StructuredServiceError {
        code: code.to_owned(),
        message: message.to_owned(),
        win32_error,
    }
}
