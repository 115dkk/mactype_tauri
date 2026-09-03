use super::{
    super::*,
    common::{open_for, query_configuration, query_runtime, ServiceHandle},
    control::query,
};
use crate::machine_integration::scm_response::{ScmResponse, ScmResponseError};
use windows_sys::Win32::{
    Foundation::{GetLastError, ERROR_INSUFFICIENT_BUFFER},
    Security::{
        GetSecurityDescriptorControl, GetSecurityDescriptorLength, IsValidSecurityDescriptor,
        DACL_SECURITY_INFORMATION, GROUP_SECURITY_INFORMATION, OWNER_SECURITY_INFORMATION,
        SE_SELF_RELATIVE,
    },
    System::Services::{
        QueryServiceConfig2W, QueryServiceObjectSecurity, SC_ACTION, SC_ACTION_NONE,
        SC_ACTION_OWN_RESTART, SC_ACTION_REBOOT, SC_ACTION_RESTART, SC_ACTION_RUN_COMMAND,
        SERVICE_CONFIG_DELAYED_AUTO_START_INFO, SERVICE_CONFIG_DESCRIPTION,
        SERVICE_CONFIG_FAILURE_ACTIONS, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,
        SERVICE_CONFIG_PRESHUTDOWN_INFO, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO,
        SERVICE_CONFIG_SERVICE_SID_INFO, SERVICE_CONFIG_TRIGGER_INFO,
        SERVICE_DELAYED_AUTO_START_INFO, SERVICE_DESCRIPTIONW, SERVICE_FAILURE_ACTIONSW,
        SERVICE_FAILURE_ACTIONS_FLAG, SERVICE_PRESHUTDOWN_INFO, SERVICE_QUERY_CONFIG,
        SERVICE_QUERY_STATUS, SERVICE_REQUIRED_PRIVILEGES_INFOW, SERVICE_SID_INFO,
        SERVICE_TRIGGER_INFO,
    },
};

pub(super) const MAX_SECURITY_DESCRIPTOR_BYTES: u32 = 64 * 1024;
pub(super) const MAX_FAILURE_ACTIONS: usize = 64;
pub(super) const MAX_REQUIRED_PRIVILEGES: usize = 64;
pub(super) const SERVICE_READ_CONTROL: u32 = 0x0002_0000;

fn query_config2_buffer(
    service: &ServiceHandle,
    information_level: u32,
    minimum_bytes: usize,
) -> Result<ScmResponse, String> {
    ScmResponse::query(minimum_bytes, |buffer, capacity, needed| {
        // SAFETY: `service` holds a live SCM handle; buffer and capacity
        // describe the response allocation or the documented null probe.
        unsafe { QueryServiceConfig2W(service.0, information_level, buffer, capacity, needed) }
    })
    .map_err(|error| match error {
        ScmResponseError::Win32(code) => {
            format!("QueryServiceConfig2W({information_level}) failed with {code}")
        }
        malformed => format!(
            "QueryServiceConfig2W({information_level}) failed: {}",
            malformed.describe()
        ),
    })
}

fn config2_error(information_level: u32, error: ScmResponseError) -> String {
    format!(
        "QueryServiceConfig2W({information_level}) failed: {}",
        error.describe()
    )
}

fn optional_nonempty(value: Option<String>) -> Option<String> {
    value.filter(|value| !value.is_empty())
}

fn query_security_descriptor(
    service: &ServiceHandle,
) -> Result<SecurityDescriptorSnapshot, String> {
    let security_information =
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    let mut needed = 0;
    let initial = unsafe {
        QueryServiceObjectSecurity(
            service.0,
            security_information,
            std::ptr::null_mut(),
            0,
            &mut needed,
        )
    };
    let error = unsafe { GetLastError() };
    if initial != 0
        || error != ERROR_INSUFFICIENT_BUFFER
        || needed == 0
        || needed > MAX_SECURITY_DESCRIPTOR_BYTES
    {
        return Err(format!(
            "QueryServiceObjectSecurity size query failed with {error}"
        ));
    }
    let mut self_relative = vec![0u8; needed as usize];
    if unsafe {
        QueryServiceObjectSecurity(
            service.0,
            security_information,
            self_relative.as_mut_ptr().cast(),
            needed,
            &mut needed,
        )
    } == 0
    {
        return Err(format!(
            "QueryServiceObjectSecurity failed with {}",
            unsafe { GetLastError() }
        ));
    }
    let descriptor = self_relative.as_mut_ptr().cast();
    let mut control = 0u16;
    let mut revision = 0u32;
    if unsafe { IsValidSecurityDescriptor(descriptor) } == 0
        || unsafe { GetSecurityDescriptorControl(descriptor, &mut control, &mut revision) } == 0
        || control & SE_SELF_RELATIVE == 0
    {
        return Err("SCM returned an invalid or non-self-relative security descriptor".to_owned());
    }
    let exact_length = unsafe { GetSecurityDescriptorLength(descriptor) } as usize;
    if exact_length == 0 || exact_length > self_relative.len() {
        return Err("SCM security descriptor length is invalid".to_owned());
    }
    self_relative.truncate(exact_length);
    Ok(SecurityDescriptorSnapshot { self_relative })
}

pub(super) fn service_has_triggers(service: &ServiceHandle) -> Result<bool, String> {
    let buffer = query_config2_buffer(
        service,
        SERVICE_CONFIG_TRIGGER_INFO,
        std::mem::size_of::<SERVICE_TRIGGER_INFO>(),
    )?;
    let trigger = buffer
        .header::<SERVICE_TRIGGER_INFO>()
        .map_err(|error| config2_error(SERVICE_CONFIG_TRIGGER_INFO, error))?;
    Ok(trigger.cTriggers != 0 || !trigger.pTriggers.is_null() || !trigger.pReserved.is_null())
}

pub(super) fn query_extended_configuration(
    service: &ServiceHandle,
) -> Result<ServiceExtendedConfiguration, String> {
    let description_buffer = query_config2_buffer(
        service,
        SERVICE_CONFIG_DESCRIPTION,
        std::mem::size_of::<SERVICE_DESCRIPTIONW>(),
    )?;
    let description = description_buffer
        .header::<SERVICE_DESCRIPTIONW>()
        .map_err(|error| config2_error(SERVICE_CONFIG_DESCRIPTION, error))?;
    let failure_buffer = query_config2_buffer(
        service,
        SERVICE_CONFIG_FAILURE_ACTIONS,
        std::mem::size_of::<SERVICE_FAILURE_ACTIONSW>(),
    )?;
    let failure = failure_buffer
        .header::<SERVICE_FAILURE_ACTIONSW>()
        .map_err(|error| config2_error(SERVICE_CONFIG_FAILURE_ACTIONS, error))?;
    let raw_actions = failure_buffer
        .array::<SC_ACTION>(failure.lpsaActions, failure.cActions, MAX_FAILURE_ACTIONS)
        .map_err(|error| config2_error(SERVICE_CONFIG_FAILURE_ACTIONS, error))?;
    let mut actions = Vec::with_capacity(raw_actions.len());
    for action in raw_actions {
        if !matches!(
            action.Type,
            SC_ACTION_NONE
                | SC_ACTION_RESTART
                | SC_ACTION_REBOOT
                | SC_ACTION_RUN_COMMAND
                | SC_ACTION_OWN_RESTART
        ) {
            return Err("legacy service has an unsupported failure action".to_owned());
        }
        actions.push(FailureAction {
            action_type: action.Type,
            delay_ms: action.Delay,
        });
    }
    let failure_flag_buffer = query_config2_buffer(
        service,
        SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,
        std::mem::size_of::<SERVICE_FAILURE_ACTIONS_FLAG>(),
    )?;
    let failure_flag = failure_flag_buffer
        .header::<SERVICE_FAILURE_ACTIONS_FLAG>()
        .map_err(|error| config2_error(SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, error))?;
    let delayed_buffer = query_config2_buffer(
        service,
        SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
        std::mem::size_of::<SERVICE_DELAYED_AUTO_START_INFO>(),
    )?;
    let delayed = delayed_buffer
        .header::<SERVICE_DELAYED_AUTO_START_INFO>()
        .map_err(|error| config2_error(SERVICE_CONFIG_DELAYED_AUTO_START_INFO, error))?;
    let sid_buffer = query_config2_buffer(
        service,
        SERVICE_CONFIG_SERVICE_SID_INFO,
        std::mem::size_of::<SERVICE_SID_INFO>(),
    )?;
    let sid = sid_buffer
        .header::<SERVICE_SID_INFO>()
        .map_err(|error| config2_error(SERVICE_CONFIG_SERVICE_SID_INFO, error))?;
    if !matches!(sid.dwServiceSidType, 0 | 1 | 3) {
        return Err("legacy service has an unsupported service SID type".to_owned());
    }
    let privileges_buffer = query_config2_buffer(
        service,
        SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO,
        std::mem::size_of::<SERVICE_REQUIRED_PRIVILEGES_INFOW>(),
    )?;
    let privileges = privileges_buffer
        .header::<SERVICE_REQUIRED_PRIVILEGES_INFOW>()
        .map_err(|error| config2_error(SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, error))?;
    let preshutdown_buffer = query_config2_buffer(
        service,
        SERVICE_CONFIG_PRESHUTDOWN_INFO,
        std::mem::size_of::<SERVICE_PRESHUTDOWN_INFO>(),
    )?;
    let preshutdown = preshutdown_buffer
        .header::<SERVICE_PRESHUTDOWN_INFO>()
        .map_err(|error| config2_error(SERVICE_CONFIG_PRESHUTDOWN_INFO, error))?;
    let trigger_buffer = query_config2_buffer(
        service,
        SERVICE_CONFIG_TRIGGER_INFO,
        std::mem::size_of::<SERVICE_TRIGGER_INFO>(),
    )?;
    let trigger = trigger_buffer
        .header::<SERVICE_TRIGGER_INFO>()
        .map_err(|error| config2_error(SERVICE_CONFIG_TRIGGER_INFO, error))?;

    let required_privileges = privileges_buffer
        .multi_units(privileges.pmszRequiredPrivileges, MAX_REQUIRED_PRIVILEGES)
        .map_err(|error| config2_error(SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, error))?
        .into_iter()
        .map(|units| {
            String::from_utf16(units).map_err(|_| {
                config2_error(
                    SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO,
                    ScmResponseError::InvalidUtf16,
                )
            })
        })
        .collect::<Result<Vec<_>, _>>()?;

    Ok(ServiceExtendedConfiguration {
        description: optional_nonempty(
            description_buffer
                .wide_string(description.lpDescription)
                .map_err(|error| config2_error(SERVICE_CONFIG_DESCRIPTION, error))?,
        ),
        failure_actions: FailureActionsConfiguration {
            reset_period_seconds: failure.dwResetPeriod,
            reboot_message: optional_nonempty(
                failure_buffer
                    .wide_string(failure.lpRebootMsg)
                    .map_err(|error| config2_error(SERVICE_CONFIG_FAILURE_ACTIONS, error))?,
            ),
            command: optional_nonempty(
                failure_buffer
                    .wide_string(failure.lpCommand)
                    .map_err(|error| config2_error(SERVICE_CONFIG_FAILURE_ACTIONS, error))?,
            ),
            actions,
        },
        failure_actions_on_non_crash: failure_flag.fFailureActionsOnNonCrashFailures != 0,
        delayed_auto_start: delayed.fDelayedAutostart != 0,
        service_sid_type: sid.dwServiceSidType,
        required_privileges,
        preshutdown_timeout_ms: preshutdown.dwPreshutdownTimeout,
        triggers: snapshot_trigger_configuration(
            trigger.cTriggers,
            !trigger.pTriggers.is_null(),
            !trigger.pReserved.is_null(),
        )?,
        security_descriptor: query_security_descriptor(service)?,
    })
}

pub(super) fn migration_snapshot(registry_conflict: bool) -> Result<LegacyScmSnapshot, String> {
    let status = query(registry_conflict);
    if status.registry_conflict
        || !matches!(
            status.presence,
            ServicePresence::Owned | ServicePresence::CompatibleUnquoted
        )
    {
        return Err("legacy SCM service is not exactly owned and migration-safe".to_owned());
    }
    require_stable_migration_state(status.state)?;
    let service = open_for(SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | SERVICE_READ_CONTROL)
        .map_err(|code| format!("OpenServiceW for migration snapshot failed with {code}"))?;
    let configuration = query_configuration(&service)
        .map_err(|code| format!("QueryServiceConfigW for migration snapshot failed with {code}"))?;
    let extended = query_extended_configuration(&service)?;
    let final_state = query_runtime(&service)
        .map_err(|code| format!("QueryServiceStatusEx for snapshot failed with {code}"))?;
    require_stable_migration_state(final_state)?;
    if final_state != status.state {
        return Err("legacy SCM service state changed while taking its snapshot".to_owned());
    }
    Ok(LegacyScmSnapshot {
        presence: status.presence,
        state: status.state,
        configuration,
        extended,
    })
}
