use super::{
    super::*,
    common::{open_for, query_configuration, wide, wide_multi, ServiceHandle},
    control::{create_service_configuration, query, stop},
    snapshot::{
        query_extended_configuration, MAX_FAILURE_ACTIONS, MAX_REQUIRED_PRIVILEGES,
        MAX_SECURITY_DESCRIPTOR_BYTES, SERVICE_READ_CONTROL,
    },
};
use windows_sys::Win32::{
    Foundation::GetLastError,
    Security::{
        GetSecurityDescriptorControl, GetSecurityDescriptorLength, IsValidSecurityDescriptor,
        DACL_SECURITY_INFORMATION, GROUP_SECURITY_INFORMATION, OWNER_SECURITY_INFORMATION,
        PROTECTED_DACL_SECURITY_INFORMATION, SE_DACL_PROTECTED, SE_SELF_RELATIVE,
        UNPROTECTED_DACL_SECURITY_INFORMATION,
    },
    System::Services::{
        ChangeServiceConfig2W, ChangeServiceConfigW, SetServiceObjectSecurity, SC_ACTION,
        SC_ACTION_NONE, SC_ACTION_OWN_RESTART, SC_ACTION_REBOOT, SC_ACTION_RESTART,
        SC_ACTION_RUN_COMMAND, SERVICE_CHANGE_CONFIG, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
        SERVICE_CONFIG_DESCRIPTION, SERVICE_CONFIG_FAILURE_ACTIONS,
        SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, SERVICE_CONFIG_PRESHUTDOWN_INFO,
        SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, SERVICE_CONFIG_SERVICE_SID_INFO,
        SERVICE_CONFIG_TRIGGER_INFO, SERVICE_DELAYED_AUTO_START_INFO, SERVICE_DESCRIPTIONW,
        SERVICE_FAILURE_ACTIONSW, SERVICE_FAILURE_ACTIONS_FLAG, SERVICE_PRESHUTDOWN_INFO,
        SERVICE_QUERY_CONFIG, SERVICE_REQUIRED_PRIVILEGES_INFOW, SERVICE_SID_INFO,
        SERVICE_TRIGGER_INFO,
    },
};

const SERVICE_WRITE_DAC: u32 = 0x0004_0000;
const SERVICE_WRITE_OWNER: u32 = 0x0008_0000;

fn validate_snapshot_string(name: &str, value: &str) -> Result<(), String> {
    if value.contains('\0') || value.encode_utf16().count() > 32_767 {
        Err(format!("legacy SCM {name} is not safely restorable"))
    } else {
        Ok(())
    }
}

fn validate_security_descriptor_snapshot(
    snapshot: &SecurityDescriptorSnapshot,
) -> Result<(), String> {
    if snapshot.self_relative.is_empty()
        || snapshot.self_relative.len() > MAX_SECURITY_DESCRIPTOR_BYTES as usize
    {
        return Err("legacy SCM security descriptor size is invalid".to_owned());
    }
    let descriptor = snapshot.self_relative.as_ptr().cast_mut().cast();
    let mut control = 0u16;
    let mut revision = 0u32;
    if unsafe { IsValidSecurityDescriptor(descriptor) } == 0
        || unsafe { GetSecurityDescriptorControl(descriptor, &mut control, &mut revision) } == 0
        || control & SE_SELF_RELATIVE == 0
        || unsafe { GetSecurityDescriptorLength(descriptor) } as usize
            != snapshot.self_relative.len()
    {
        return Err("legacy SCM security descriptor is invalid".to_owned());
    }
    Ok(())
}

pub(super) fn validate_snapshot_for_restore(snapshot: &LegacyScmSnapshot) -> Result<(), String> {
    require_stable_migration_state(snapshot.state)?;
    let configuration = &snapshot.configuration;
    for (name, value) in [
        ("display name", configuration.display_name.as_str()),
        ("binary path", configuration.binary_path.as_str()),
        ("account", configuration.account.as_str()),
    ] {
        validate_snapshot_string(name, value)?;
    }
    if let Some(load_order_group) = &configuration.load_order_group {
        if load_order_group.is_empty() {
            return Err("legacy SCM load-order group must use None for an empty value".to_owned());
        }
        validate_snapshot_string("load-order group", load_order_group)?;
    }
    for dependency in &configuration.dependencies {
        validate_snapshot_string("dependency", dependency)?;
    }

    let extended = &snapshot.extended;
    if let Some(description) = &extended.description {
        if description.is_empty() {
            return Err("legacy SCM description must use None for an empty value".to_owned());
        }
        validate_snapshot_string("description", description)?;
    }
    for (name, value) in [
        (
            "failure reboot message",
            extended.failure_actions.reboot_message.as_deref(),
        ),
        (
            "failure command",
            extended.failure_actions.command.as_deref(),
        ),
    ] {
        if let Some(value) = value {
            validate_snapshot_string(name, value)?;
        }
    }
    if extended.failure_actions.actions.len() > MAX_FAILURE_ACTIONS
        || extended.failure_actions.actions.iter().any(|action| {
            !matches!(
                action.action_type,
                SC_ACTION_NONE
                    | SC_ACTION_RESTART
                    | SC_ACTION_REBOOT
                    | SC_ACTION_RUN_COMMAND
                    | SC_ACTION_OWN_RESTART
            )
        })
    {
        return Err("legacy SCM failure actions are not safely restorable".to_owned());
    }
    if !matches!(extended.service_sid_type, 0 | 1 | 3)
        || extended.required_privileges.len() > MAX_REQUIRED_PRIVILEGES
    {
        return Err("legacy SCM SID or privilege configuration is invalid".to_owned());
    }
    for privilege in &extended.required_privileges {
        validate_snapshot_string("required privilege", privilege)?;
    }
    validate_security_descriptor_snapshot(&extended.security_descriptor)
}

fn change_config2<T>(
    service: &ServiceHandle,
    information_level: u32,
    information: &T,
) -> Result<(), String> {
    if unsafe {
        ChangeServiceConfig2W(
            service.0,
            information_level,
            (information as *const T).cast(),
        )
    } == 0
    {
        Err(format!(
            "ChangeServiceConfig2W({information_level}) failed with {}",
            unsafe { GetLastError() }
        ))
    } else {
        Ok(())
    }
}

struct WindowsServiceConfigurationRestorer<'a> {
    service: &'a ServiceHandle,
    snapshot: &'a LegacyScmSnapshot,
}

impl ServiceConfigurationRestorer for WindowsServiceConfigurationRestorer<'_> {
    fn restore(&mut self, step: ServiceRestoreStep) -> Result<(), String> {
        let configuration = &self.snapshot.configuration;
        let extended = &self.snapshot.extended;
        match step {
            ServiceRestoreStep::Core => {
                let binary_path = wide(&configuration.binary_path);
                let mut load_order_group = configuration.load_order_group.as_deref().map(wide);
                let mut tag_id = 0;
                let dependencies = wide_multi(&configuration.dependencies);
                let account = wide(&configuration.account);
                let display_name = wide(&configuration.display_name);
                if unsafe {
                    ChangeServiceConfigW(
                        self.service.0,
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
                        display_name.as_ptr(),
                    )
                } == 0
                {
                    Err(format!("ChangeServiceConfigW failed with {}", unsafe {
                        GetLastError()
                    }))
                } else {
                    Ok(())
                }
            }
            ServiceRestoreStep::Description => {
                let mut description = extended.description.as_deref().map(wide);
                let information = SERVICE_DESCRIPTIONW {
                    lpDescription: description
                        .as_mut()
                        .map_or(std::ptr::null_mut(), |value| value.as_mut_ptr()),
                };
                change_config2(self.service, SERVICE_CONFIG_DESCRIPTION, &information)
            }
            ServiceRestoreStep::FailureActions => {
                let mut reboot_message =
                    extended.failure_actions.reboot_message.as_deref().map(wide);
                let mut command = extended.failure_actions.command.as_deref().map(wide);
                let mut actions = extended
                    .failure_actions
                    .actions
                    .iter()
                    .map(|action| SC_ACTION {
                        Type: action.action_type,
                        Delay: action.delay_ms,
                    })
                    .collect::<Vec<_>>();
                let information = SERVICE_FAILURE_ACTIONSW {
                    dwResetPeriod: extended.failure_actions.reset_period_seconds,
                    lpRebootMsg: reboot_message
                        .as_mut()
                        .map_or(std::ptr::null_mut(), |value| value.as_mut_ptr()),
                    lpCommand: command
                        .as_mut()
                        .map_or(std::ptr::null_mut(), |value| value.as_mut_ptr()),
                    cActions: actions.len() as u32,
                    lpsaActions: if actions.is_empty() {
                        std::ptr::null_mut()
                    } else {
                        actions.as_mut_ptr()
                    },
                };
                change_config2(self.service, SERVICE_CONFIG_FAILURE_ACTIONS, &information)
            }
            ServiceRestoreStep::FailureActionsFlag => {
                let information = SERVICE_FAILURE_ACTIONS_FLAG {
                    fFailureActionsOnNonCrashFailures: i32::from(
                        extended.failure_actions_on_non_crash,
                    ),
                };
                change_config2(
                    self.service,
                    SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,
                    &information,
                )
            }
            ServiceRestoreStep::DelayedAutoStart => {
                let information = SERVICE_DELAYED_AUTO_START_INFO {
                    fDelayedAutostart: i32::from(extended.delayed_auto_start),
                };
                change_config2(
                    self.service,
                    SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                    &information,
                )
            }
            ServiceRestoreStep::ServiceSidType => {
                let information = SERVICE_SID_INFO {
                    dwServiceSidType: extended.service_sid_type,
                };
                change_config2(self.service, SERVICE_CONFIG_SERVICE_SID_INFO, &information)
            }
            ServiceRestoreStep::RequiredPrivileges => {
                let mut privileges = if extended.required_privileges.is_empty() {
                    vec![0u16, 0]
                } else {
                    wide_multi(&extended.required_privileges)
                };
                let information = SERVICE_REQUIRED_PRIVILEGES_INFOW {
                    pmszRequiredPrivileges: privileges.as_mut_ptr(),
                };
                change_config2(
                    self.service,
                    SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO,
                    &information,
                )
            }
            ServiceRestoreStep::PreshutdownTimeout => {
                let information = SERVICE_PRESHUTDOWN_INFO {
                    dwPreshutdownTimeout: extended.preshutdown_timeout_ms,
                };
                change_config2(self.service, SERVICE_CONFIG_PRESHUTDOWN_INFO, &information)
            }
            ServiceRestoreStep::Triggers => {
                let information = SERVICE_TRIGGER_INFO::default();
                change_config2(self.service, SERVICE_CONFIG_TRIGGER_INFO, &information)
            }
            ServiceRestoreStep::SecurityDescriptor => {
                validate_security_descriptor_snapshot(&extended.security_descriptor)?;
                let descriptor = extended
                    .security_descriptor
                    .self_relative
                    .as_ptr()
                    .cast_mut()
                    .cast();
                let mut control = 0u16;
                let mut revision = 0u32;
                if unsafe { GetSecurityDescriptorControl(descriptor, &mut control, &mut revision) }
                    == 0
                {
                    return Err(format!(
                        "GetSecurityDescriptorControl failed with {}",
                        unsafe { GetLastError() }
                    ));
                }
                let protection = if control & SE_DACL_PROTECTED != 0 {
                    PROTECTED_DACL_SECURITY_INFORMATION
                } else {
                    UNPROTECTED_DACL_SECURITY_INFORMATION
                };
                let security_information = OWNER_SECURITY_INFORMATION
                    | GROUP_SECURITY_INFORMATION
                    | DACL_SECURITY_INFORMATION
                    | protection;
                if unsafe {
                    SetServiceObjectSecurity(self.service.0, security_information, descriptor)
                } == 0
                {
                    Err(format!("SetServiceObjectSecurity failed with {}", unsafe {
                        GetLastError()
                    }))
                } else {
                    Ok(())
                }
            }
        }
    }
}

pub(super) fn restore_service_configuration(snapshot: &LegacyScmSnapshot) -> Result<(), String> {
    validate_snapshot_for_restore(snapshot)?;
    let status = query(false);
    match status.presence {
        ServicePresence::Absent => create_service_configuration(&snapshot.configuration)?,
        ServicePresence::Owned | ServicePresence::CompatibleUnquoted => {
            require_stable_migration_state(status.state)?;
            let current = open_for(SERVICE_QUERY_CONFIG | SERVICE_READ_CONTROL)
                .map_err(|code| format!("OpenServiceW for restore preflight failed with {code}"))?;
            query_extended_configuration(&current)?;
            drop(current);
            if status.state != ServiceRuntimeState::Stopped {
                stop()?;
            }
        }
        ServicePresence::Foreign
        | ServicePresence::DeletePending
        | ServicePresence::Inaccessible => {
            return Err("refusing to overwrite an unsafe MacType SCM service".to_owned());
        }
    }
    let verified = query(false);
    if !matches!(
        verified.presence,
        ServicePresence::Owned | ServicePresence::CompatibleUnquoted
    ) || verified.state != ServiceRuntimeState::Stopped
    {
        return Err("legacy SCM service changed before configuration restore".to_owned());
    }
    let preflight = open_for(SERVICE_QUERY_CONFIG | SERVICE_READ_CONTROL)
        .map_err(|code| format!("OpenServiceW for final restore preflight failed with {code}"))?;
    query_extended_configuration(&preflight)?;
    drop(preflight);
    let service = open_for(
        SERVICE_CHANGE_CONFIG
            | SERVICE_QUERY_CONFIG
            | SERVICE_READ_CONTROL
            | SERVICE_WRITE_DAC
            | SERVICE_WRITE_OWNER,
    )
    .map_err(|code| format!("OpenServiceW for restore failed with {code}"))?;
    let mut restorer = WindowsServiceConfigurationRestorer {
        service: &service,
        snapshot,
    };
    perform_service_configuration_restore(&mut restorer)?;
    let restored_configuration = query_configuration(&service).map_err(|code| {
        format!("QueryServiceConfigW after configuration restore failed with {code}")
    })?;
    let restored_extended = query_extended_configuration(&service)?;
    verify_restored_configuration(snapshot, &restored_configuration, &restored_extended)
}
