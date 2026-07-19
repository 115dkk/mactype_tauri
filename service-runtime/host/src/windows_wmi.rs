use std::time::Duration;

use mactype_service_contract::StructuredServiceError;
use windows::core::{IUnknown, Interface, BSTR};
use windows::Win32::Foundation::RPC_E_TOO_LATE;
use windows::Win32::System::Com::{
    CoCreateInstance, CoInitializeEx, CoInitializeSecurity, CoSetProxyBlanket, CoUninitialize,
    CLSCTX_INPROC_SERVER, COINIT_MULTITHREADED, EOAC_NONE, RPC_C_AUTHN_LEVEL_CALL,
    RPC_C_IMP_LEVEL_IMPERSONATE,
};
use windows::Win32::System::Rpc::{RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE};
use windows::Win32::System::Variant::VARIANT;
use windows::Win32::System::Wmi::{
    IEnumWbemClassObject, IWbemClassObject, IWbemLocator, IWbemServices, WbemLocator,
    WBEM_FLAG_FORWARD_ONLY, WBEM_FLAG_RETURN_IMMEDIATELY, WBEM_S_TIMEDOUT,
};

use crate::ProcessEventSource;

const PROCESS_SNAPSHOT_QUERY: &str = "SELECT ProcessID FROM Win32_Process";

pub struct WmiProcessEventSource {
    enumerator: Option<IEnumWbemClassObject>,
    services: IWbemServices,
    _apartment: ComApartment,
}

impl WmiProcessEventSource {
    pub fn connect() -> Result<Self, StructuredServiceError> {
        let apartment = ComApartment::initialize()?;
        initialize_com_security()?;
        let locator: IWbemLocator =
            unsafe { CoCreateInstance(&WbemLocator, None::<&IUnknown>, CLSCTX_INPROC_SERVER) }
                .map_err(|error| {
                    com_error(
                        "wmi-locator-unavailable",
                        "WMI locator creation failed",
                        error,
                    )
                })?;
        let empty = BSTR::new();
        let services = unsafe {
            locator.ConnectServer(
                &BSTR::from("ROOT\\CIMV2"),
                &empty,
                &empty,
                &empty,
                0,
                &empty,
                None,
            )
        }
        .map_err(|error| {
            com_error(
                "wmi-namespace-unavailable",
                "the ROOT\\CIMV2 WMI namespace could not be opened",
                error,
            )
        })?;
        unsafe {
            CoSetProxyBlanket(
                &services,
                RPC_C_AUTHN_WINNT,
                RPC_C_AUTHZ_NONE,
                None,
                RPC_C_AUTHN_LEVEL_CALL,
                RPC_C_IMP_LEVEL_IMPERSONATE,
                None,
                EOAC_NONE,
            )
        }
        .map_err(|error| {
            com_error(
                "wmi-proxy-security-failed",
                "WMI proxy security could not be configured for LocalSystem",
                error,
            )
        })?;
        Ok(Self {
            enumerator: None,
            services,
            _apartment: apartment,
        })
    }
}

impl ProcessEventSource for WmiProcessEventSource {
    fn subscribe(&mut self, query: &str) -> Result<(), StructuredServiceError> {
        let flags = WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY;
        let enumerator = unsafe {
            self.services
                .ExecNotificationQuery(&BSTR::from("WQL"), &BSTR::from(query), flags, None)
        }
        .map_err(|error| {
            com_error(
                "wmi-subscription-failed",
                "the temporary Win32_Process creation subscription failed",
                error,
            )
        })?;
        self.enumerator = Some(enumerator);
        Ok(())
    }

    fn snapshot_pids(&mut self) -> Result<Vec<u32>, StructuredServiceError> {
        let flags = WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY;
        let enumerator = unsafe {
            self.services.ExecQuery(
                &BSTR::from("WQL"),
                &BSTR::from(PROCESS_SNAPSHOT_QUERY),
                flags,
                None,
            )
        }
        .map_err(|error| {
            com_error(
                "wmi-snapshot-failed",
                "the initial Win32_Process snapshot could not be opened",
                error,
            )
        })?;
        let mut pids = Vec::new();
        loop {
            let mut objects = [None];
            let mut returned = 0;
            let result = unsafe { enumerator.Next(5_000, &mut objects, &mut returned) };
            if returned == 0 {
                break;
            }
            result.ok().map_err(|error| {
                com_error(
                    "wmi-snapshot-failed",
                    "the initial Win32_Process snapshot could not be read",
                    error,
                )
            })?;
            let process = objects[0].take().ok_or_else(|| {
                service_error(
                    "wmi-snapshot-invalid",
                    "WMI returned an empty Win32_Process snapshot row",
                    None,
                )
            })?;
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
        let timeout_ms = timeout.as_millis().min(i32::MAX as u128) as i32;
        let mut objects = [None];
        let mut returned = 0;
        let result = unsafe { enumerator.Next(timeout_ms, &mut objects, &mut returned) };
        if result.0 == WBEM_S_TIMEDOUT.0 || returned == 0 {
            return Ok(None);
        }
        result.ok().map_err(|error| {
            com_error(
                "wmi-observer-failed",
                "the WMI process observer failed while waiting for an event",
                error,
            )
        })?;
        let event = objects[0].take().ok_or_else(|| {
            service_error(
                "wmi-event-invalid",
                "WMI reported a process event without an event object",
                None,
            )
        })?;
        extract_process_id(&event).map(Some)
    }
}

fn extract_process_id(event: &IWbemClassObject) -> Result<u32, StructuredServiceError> {
    let mut target = VARIANT::default();
    unsafe {
        event.Get(
            windows::core::w!("TargetInstance"),
            0,
            &mut target,
            None,
            None,
        )
    }
    .map_err(|error| {
        com_error(
            "wmi-event-invalid",
            "the WMI process event has no TargetInstance",
            error,
        )
    })?;
    let unknown = IUnknown::try_from(&target).map_err(|error| {
        com_error(
            "wmi-event-invalid",
            "the WMI process event TargetInstance is not an object",
            error,
        )
    })?;
    let target: IWbemClassObject = unknown.cast().map_err(|error| {
        com_error(
            "wmi-event-invalid",
            "the WMI process event TargetInstance is not a Win32_Process object",
            error,
        )
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

fn extract_process_id_property(target: &IWbemClassObject) -> Result<u32, StructuredServiceError> {
    let mut pid = VARIANT::default();
    unsafe { target.Get(windows::core::w!("ProcessID"), 0, &mut pid, None, None) }.map_err(
        |error| {
            com_error(
                "wmi-process-id-invalid",
                "the WMI process object has no ProcessID",
                error,
            )
        },
    )?;
    let pid = u32::try_from(&pid).map_err(|error| {
        com_error(
            "wmi-event-invalid",
            "the WMI process event ProcessID is invalid",
            error,
        )
    })?;
    Ok(pid)
}

struct ComApartment;

impl ComApartment {
    fn initialize() -> Result<Self, StructuredServiceError> {
        unsafe { CoInitializeEx(None, COINIT_MULTITHREADED) }
            .ok()
            .map_err(|error| {
                com_error(
                    "com-initialization-failed",
                    "COM could not be initialized for the WMI observer",
                    error,
                )
            })?;
        Ok(Self)
    }
}

impl Drop for ComApartment {
    fn drop(&mut self) {
        unsafe {
            CoUninitialize();
        }
    }
}

fn initialize_com_security() -> Result<(), StructuredServiceError> {
    match unsafe {
        CoInitializeSecurity(
            None,
            -1,
            None,
            None,
            RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            None,
            EOAC_NONE,
            None,
        )
    } {
        Ok(()) => Ok(()),
        Err(error) if error.code() == RPC_E_TOO_LATE => Ok(()),
        Err(error) => Err(com_error(
            "com-security-initialization-failed",
            "COM security could not be initialized for LocalSystem WMI access",
            error,
        )),
    }
}

fn com_error(code: &str, message: &str, error: windows::core::Error) -> StructuredServiceError {
    service_error(code, message, Some(error.code().0 as u32))
}

fn service_error(code: &str, message: &str, win32_error: Option<u32>) -> StructuredServiceError {
    StructuredServiceError {
        code: code.to_owned(),
        message: message.to_owned(),
        win32_error,
    }
}
