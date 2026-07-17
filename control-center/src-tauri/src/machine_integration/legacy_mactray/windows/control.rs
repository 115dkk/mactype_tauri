use super::{
    super::*,
    common::{
        expected_mactray_path, open_for, query_configuration, query_runtime, trusted_mactray_path,
        wide, wide_multi, ServiceHandle,
    },
};
use std::{thread, time::Duration};
use windows_sys::Win32::{
    Foundation::{
        GetLastError, ERROR_DUPLICATE_SERVICE_NAME, ERROR_SERVICE_ALREADY_RUNNING,
        ERROR_SERVICE_DOES_NOT_EXIST, ERROR_SERVICE_EXISTS, ERROR_SERVICE_MARKED_FOR_DELETE,
        ERROR_SERVICE_NOT_ACTIVE,
    },
    System::Services::{
        ControlService, CreateServiceW, DeleteService, OpenSCManagerW, OpenServiceW, StartServiceW,
        SC_MANAGER_CONNECT, SC_MANAGER_CREATE_SERVICE, SERVICE_CONTROL_STOP, SERVICE_QUERY_CONFIG,
        SERVICE_QUERY_STATUS, SERVICE_START, SERVICE_STATUS, SERVICE_STOP,
    },
};

fn inaccessible(code: u32, trusted: bool, registry: bool) -> LegacyServiceStatus {
    with_capabilities(
        ServicePresence::Inaccessible,
        ServiceRuntimeState::Unknown,
        None,
        Some(code),
        trusted,
        registry,
    )
}

pub(super) fn query(registry_conflict: bool) -> LegacyServiceStatus {
    let expected = expected_mactray_path();
    let trusted_available = trusted_mactray_path().is_some();
    let manager = unsafe { OpenSCManagerW(std::ptr::null(), std::ptr::null(), SC_MANAGER_CONNECT) };
    if manager.is_null() {
        return inaccessible(
            unsafe { GetLastError() },
            trusted_available,
            registry_conflict,
        );
    }
    let manager = ServiceHandle(manager);
    let name = wide("MacType");
    let service = unsafe {
        OpenServiceW(
            manager.0,
            name.as_ptr(),
            SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG,
        )
    };
    if service.is_null() {
        let code = unsafe { GetLastError() };
        let presence = match code {
            ERROR_SERVICE_DOES_NOT_EXIST => ServicePresence::Absent,
            ERROR_SERVICE_MARKED_FOR_DELETE => ServicePresence::DeletePending,
            _ => ServicePresence::Inaccessible,
        };
        return with_capabilities(
            presence,
            ServiceRuntimeState::Unknown,
            None,
            (presence == ServicePresence::Inaccessible).then_some(code),
            trusted_available,
            registry_conflict,
        );
    }
    let service = ServiceHandle(service);
    let state = match query_runtime(&service) {
        Ok(state) => state,
        Err(code) => return inaccessible(code, trusted_available, registry_conflict),
    };
    let configuration = match query_configuration(&service) {
        Ok(configuration) => configuration,
        Err(code) => return inaccessible(code, trusted_available, registry_conflict),
    };
    status_from_configuration(
        &configuration,
        state,
        expected.as_deref(),
        trusted_available,
        registry_conflict,
    )
}

fn wait_for(target: ServiceRuntimeState) -> Result<(), String> {
    for _ in 0..120 {
        let status = query(false);
        if status.state == target
            || (target == ServiceRuntimeState::Stopped
                && status.presence == ServicePresence::Absent)
        {
            return Ok(());
        }
        if matches!(
            status.presence,
            ServicePresence::Foreign | ServicePresence::Inaccessible
        ) {
            return Err("legacy service changed to an unsafe state".to_owned());
        }
        thread::sleep(Duration::from_millis(250));
    }
    Err("legacy service operation timed out after 30 seconds".to_owned())
}

fn wait_until_absent() -> Result<(), String> {
    for _ in 0..120 {
        let status = query(crate::machine_integration::registry_conflict_detected());
        match status.presence {
            ServicePresence::Absent => return Ok(()),
            ServicePresence::Owned
            | ServicePresence::CompatibleUnquoted
            | ServicePresence::DeletePending => {}
            ServicePresence::Foreign | ServicePresence::Inaccessible => {
                return Err("legacy service changed to an unsafe state".to_owned());
            }
        }
        thread::sleep(Duration::from_millis(250));
    }
    Err("legacy service removal timed out after 30 seconds".to_owned())
}

pub(super) fn start() -> Result<(), String> {
    let service = open_for(SERVICE_START | SERVICE_QUERY_STATUS)
        .map_err(|code| format!("OpenServiceW failed with {code}"))?;
    if unsafe { StartServiceW(service.0, 0, std::ptr::null()) } == 0 {
        let code = unsafe { GetLastError() };
        if code != ERROR_SERVICE_ALREADY_RUNNING {
            return Err(format!("StartServiceW failed with {code}"));
        }
    }
    drop(service);
    wait_for(ServiceRuntimeState::Running)
}

pub(super) fn stop() -> Result<(), String> {
    let service = open_for(SERVICE_STOP | SERVICE_QUERY_STATUS)
        .map_err(|code| format!("OpenServiceW failed with {code}"))?;
    let mut status = SERVICE_STATUS::default();
    if unsafe { ControlService(service.0, SERVICE_CONTROL_STOP, &mut status) } == 0 {
        let code = unsafe { GetLastError() };
        if code != ERROR_SERVICE_NOT_ACTIVE {
            return Err(format!("ControlService failed with {code}"));
        }
    }
    drop(service);
    wait_for(ServiceRuntimeState::Stopped)
}

pub(super) fn create_service_configuration(
    configuration: &ServiceConfiguration,
) -> Result<(), String> {
    let manager = unsafe {
        OpenSCManagerW(
            std::ptr::null(),
            std::ptr::null(),
            SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE,
        )
    };
    if manager.is_null() {
        return Err(format!(
            "OpenSCManagerW for service creation failed with {}",
            unsafe { GetLastError() }
        ));
    }
    let manager = ServiceHandle(manager);
    let name = wide("MacType");
    let display_name = wide(&configuration.display_name);
    let binary_path = wide(&configuration.binary_path);
    let mut load_order_group = configuration.load_order_group.as_deref().map(wide);
    let mut tag_id = 0;
    let dependencies = wide_multi(&configuration.dependencies);
    let account = wide(&configuration.account);
    let service = unsafe {
        CreateServiceW(
            manager.0,
            name.as_ptr(),
            display_name.as_ptr(),
            SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP,
            configuration.service_type,
            configuration.start_type,
            configuration.error_control,
            binary_path.as_ptr(),
            load_order_group
                .as_mut()
                .map_or(std::ptr::null(), |value| value.as_ptr()),
            load_order_group
                .as_ref()
                .map_or(std::ptr::null_mut(), |_| &mut tag_id),
            dependencies.as_ptr(),
            account.as_ptr(),
            std::ptr::null(),
        )
    };
    if service.is_null() {
        let code = unsafe { GetLastError() };
        if matches!(code, ERROR_SERVICE_EXISTS | ERROR_DUPLICATE_SERVICE_NAME)
            && matches!(
                query(false).presence,
                ServicePresence::Owned | ServicePresence::CompatibleUnquoted
            )
        {
            return Ok(());
        }
        return Err(format!("CreateServiceW failed with {code}"));
    }
    drop(ServiceHandle(service));
    let created = query(false);
    if matches!(
        created.presence,
        ServicePresence::Owned | ServicePresence::CompatibleUnquoted
    ) {
        Ok(())
    } else {
        Err("Windows created a MacType service with unexpected configuration".to_owned())
    }
}

fn delete_owned_service() -> Result<(), String> {
    const DELETE_ACCESS: u32 = 0x0001_0000;
    let service = open_for(DELETE_ACCESS | SERVICE_QUERY_STATUS)
        .map_err(|code| format!("OpenServiceW for deletion failed with {code}"))?;
    if unsafe { DeleteService(service.0) } == 0 {
        let code = unsafe { GetLastError() };
        if !matches!(
            code,
            ERROR_SERVICE_DOES_NOT_EXIST | ERROR_SERVICE_MARKED_FOR_DELETE
        ) {
            return Err(format!("DeleteService failed with {code}"));
        }
    }
    drop(service);
    wait_until_absent()
}

pub(super) fn migration_stop() -> Result<(), String> {
    let status = query(false);
    if !matches!(
        status.presence,
        ServicePresence::Owned | ServicePresence::CompatibleUnquoted
    ) {
        return Err("only an owned legacy service can be stopped for migration".to_owned());
    }
    require_stable_migration_state(status.state)?;
    match status.state {
        ServiceRuntimeState::Stopped => Ok(()),
        ServiceRuntimeState::Running => stop(),
        _ => unreachable!(),
    }
}

pub(super) fn migration_remove() -> Result<(), String> {
    let status = query(false);
    if !matches!(
        status.presence,
        ServicePresence::Owned | ServicePresence::CompatibleUnquoted
    ) || status.state != ServiceRuntimeState::Stopped
    {
        return Err("legacy service must be owned and stopped before removal".to_owned());
    }
    delete_owned_service()
}

pub(super) fn migration_restore_running_state(snapshot: &LegacyScmSnapshot) -> Result<(), String> {
    require_stable_migration_state(snapshot.state)?;
    let current = query(false);
    if !matches!(
        current.presence,
        ServicePresence::Owned | ServicePresence::CompatibleUnquoted
    ) {
        return Err("only an owned legacy service can have its runtime state restored".to_owned());
    }
    require_stable_migration_state(current.state)?;
    match (snapshot.state, current.state) {
        (ServiceRuntimeState::Running, ServiceRuntimeState::Stopped) => start(),
        (ServiceRuntimeState::Stopped, ServiceRuntimeState::Running) => migration_stop(),
        (ServiceRuntimeState::Running, ServiceRuntimeState::Running)
        | (ServiceRuntimeState::Stopped, ServiceRuntimeState::Stopped) => Ok(()),
        _ => unreachable!(),
    }
}
