use super::{
    super::*,
    common::{open_for, query_configuration, query_runtime, ServiceHandle},
    control::query,
};
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

const MAX_CONFIG2_BYTES: u32 = 64 * 1024;
pub(super) const MAX_SECURITY_DESCRIPTOR_BYTES: u32 = 64 * 1024;
pub(super) const MAX_FAILURE_ACTIONS: usize = 64;
pub(super) const MAX_REQUIRED_PRIVILEGES: usize = 64;
pub(super) const SERVICE_READ_CONTROL: u32 = 0x0002_0000;

struct AlignedConfigBuffer {
    words: Vec<usize>,
    byte_length: usize,
}

impl AlignedConfigBuffer {
    fn start(&self) -> usize {
        self.words.as_ptr() as usize
    }

    fn end(&self) -> usize {
        self.start() + self.byte_length
    }

    fn read<T: Copy>(&self) -> Result<T, String> {
        if self.byte_length < std::mem::size_of::<T>() {
            return Err("SCM Config2 response is smaller than its fixed header".to_owned());
        }
        Ok(unsafe { self.words.as_ptr().cast::<T>().read() })
    }

    fn wide_string(&self, pointer: *const u16) -> Result<Option<String>, String> {
        if pointer.is_null() {
            return Ok(None);
        }
        let start = pointer as usize;
        if start < self.start() || start >= self.end() || start % 2 != 0 {
            return Err("SCM Config2 response contains an out-of-buffer string".to_owned());
        }
        let available = (self.end() - start) / std::mem::size_of::<u16>();
        let units = unsafe { std::slice::from_raw_parts(pointer, available) };
        let length = units
            .iter()
            .position(|unit| *unit == 0)
            .ok_or_else(|| "SCM Config2 string is not terminated".to_owned())?;
        String::from_utf16(&units[..length])
            .map(Some)
            .map_err(|_| "SCM Config2 string contains invalid UTF-16".to_owned())
    }

    fn multi_string(&self, pointer: *const u16) -> Result<Vec<String>, String> {
        if pointer.is_null() {
            return Ok(Vec::new());
        }
        let mut current = pointer;
        let mut values = Vec::new();
        loop {
            let value = self.wide_string(current)?;
            let Some(value) = value else {
                return Err("SCM Config2 MULTI_SZ contains a null pointer".to_owned());
            };
            if value.is_empty() {
                return Ok(values);
            }
            if values.len() >= MAX_REQUIRED_PRIVILEGES {
                return Err("SCM Config2 required privilege list is too large".to_owned());
            }
            let advance = value.encode_utf16().count() + 1;
            current = unsafe { current.add(advance) };
            values.push(value);
        }
    }

    fn actions(&self, pointer: *const SC_ACTION, count: u32) -> Result<Vec<SC_ACTION>, String> {
        let count = count as usize;
        if count == 0 {
            return Ok(Vec::new());
        }
        if pointer.is_null() || count > MAX_FAILURE_ACTIONS {
            return Err("SCM failure action list is invalid or too large".to_owned());
        }
        let start = pointer as usize;
        let byte_length = count
            .checked_mul(std::mem::size_of::<SC_ACTION>())
            .ok_or_else(|| "SCM failure action list size overflowed".to_owned())?;
        let end = start
            .checked_add(byte_length)
            .ok_or_else(|| "SCM failure action list pointer overflowed".to_owned())?;
        if start < self.start()
            || end > self.end()
            || start % std::mem::align_of::<SC_ACTION>() != 0
        {
            return Err("SCM failure action list points outside its response".to_owned());
        }
        Ok(unsafe { std::slice::from_raw_parts(pointer, count) }.to_vec())
    }
}

fn query_config2_buffer(
    service: &ServiceHandle,
    information_level: u32,
    minimum_bytes: usize,
) -> Result<AlignedConfigBuffer, String> {
    let mut needed = 0;
    let initial = unsafe {
        QueryServiceConfig2W(
            service.0,
            information_level,
            std::ptr::null_mut(),
            0,
            &mut needed,
        )
    };
    let error = unsafe { GetLastError() };
    if initial != 0
        || error != ERROR_INSUFFICIENT_BUFFER
        || needed < minimum_bytes as u32
        || needed > MAX_CONFIG2_BYTES
    {
        return Err(format!(
            "QueryServiceConfig2W({information_level}) size query failed with {error}"
        ));
    }
    let word_size = std::mem::size_of::<usize>();
    let mut words = vec![0usize; (needed as usize).div_ceil(word_size)];
    if unsafe {
        QueryServiceConfig2W(
            service.0,
            information_level,
            words.as_mut_ptr().cast(),
            needed,
            &mut needed,
        )
    } == 0
    {
        return Err(format!(
            "QueryServiceConfig2W({information_level}) failed with {}",
            unsafe { GetLastError() }
        ));
    }
    Ok(AlignedConfigBuffer {
        words,
        byte_length: needed as usize,
    })
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
    let trigger = buffer.read::<SERVICE_TRIGGER_INFO>()?;
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
    let description = description_buffer.read::<SERVICE_DESCRIPTIONW>()?;
    let failure_buffer = query_config2_buffer(
        service,
        SERVICE_CONFIG_FAILURE_ACTIONS,
        std::mem::size_of::<SERVICE_FAILURE_ACTIONSW>(),
    )?;
    let failure = failure_buffer.read::<SERVICE_FAILURE_ACTIONSW>()?;
    let raw_actions = failure_buffer.actions(failure.lpsaActions, failure.cActions)?;
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
    let failure_flag = failure_flag_buffer.read::<SERVICE_FAILURE_ACTIONS_FLAG>()?;
    let delayed_buffer = query_config2_buffer(
        service,
        SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
        std::mem::size_of::<SERVICE_DELAYED_AUTO_START_INFO>(),
    )?;
    let delayed = delayed_buffer.read::<SERVICE_DELAYED_AUTO_START_INFO>()?;
    let sid_buffer = query_config2_buffer(
        service,
        SERVICE_CONFIG_SERVICE_SID_INFO,
        std::mem::size_of::<SERVICE_SID_INFO>(),
    )?;
    let sid = sid_buffer.read::<SERVICE_SID_INFO>()?;
    if !matches!(sid.dwServiceSidType, 0 | 1 | 3) {
        return Err("legacy service has an unsupported service SID type".to_owned());
    }
    let privileges_buffer = query_config2_buffer(
        service,
        SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO,
        std::mem::size_of::<SERVICE_REQUIRED_PRIVILEGES_INFOW>(),
    )?;
    let privileges = privileges_buffer.read::<SERVICE_REQUIRED_PRIVILEGES_INFOW>()?;
    let preshutdown_buffer = query_config2_buffer(
        service,
        SERVICE_CONFIG_PRESHUTDOWN_INFO,
        std::mem::size_of::<SERVICE_PRESHUTDOWN_INFO>(),
    )?;
    let preshutdown = preshutdown_buffer.read::<SERVICE_PRESHUTDOWN_INFO>()?;
    let trigger_buffer = query_config2_buffer(
        service,
        SERVICE_CONFIG_TRIGGER_INFO,
        std::mem::size_of::<SERVICE_TRIGGER_INFO>(),
    )?;
    let trigger = trigger_buffer.read::<SERVICE_TRIGGER_INFO>()?;

    Ok(ServiceExtendedConfiguration {
        description: optional_nonempty(description_buffer.wide_string(description.lpDescription)?),
        failure_actions: FailureActionsConfiguration {
            reset_period_seconds: failure.dwResetPeriod,
            reboot_message: optional_nonempty(failure_buffer.wide_string(failure.lpRebootMsg)?),
            command: optional_nonempty(failure_buffer.wide_string(failure.lpCommand)?),
            actions,
        },
        failure_actions_on_non_crash: failure_flag.fFailureActionsOnNonCrashFailures != 0,
        delayed_auto_start: delayed.fDelayedAutostart != 0,
        service_sid_type: sid.dwServiceSidType,
        required_privileges: privileges_buffer.multi_string(privileges.pmszRequiredPrivileges)?,
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
