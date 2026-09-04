use std::io;

use mactype_service_platform::{
    AclValidationError, LocalSecurityDescriptor, OwnedHandle, OwnedSid, SecurityAclError,
    SecurityDescriptor,
};
use windows_sys::Win32::Foundation::ERROR_ACCESS_DENIED;
use windows_sys::Win32::Security::SE_DACL_PROTECTED;
use windows_sys::Win32::System::Threading::MUTEX_ALL_ACCESS;

use crate::SetupError;

const MACHINE_SETUP_LOCK_SDDL: &str = "O:BAG:BAD:P(A;;0x001F0001;;;SY)(A;;0x001F0001;;;BA)";
const SYSTEM_SID: &str = "S-1-5-18";
const ADMINISTRATORS_SID: &str = "S-1-5-32-544";

pub(super) fn machine_setup_lock_descriptor() -> Result<SecurityDescriptor, SetupError> {
    SecurityDescriptor::from_sddl(MACHINE_SETUP_LOCK_SDDL).map_err(SetupError::Io)
}

pub(super) fn verify_machine_setup_lock(handle: &OwnedHandle) -> Result<(), SetupError> {
    let descriptor = match LocalSecurityDescriptor::query_kernel_object(handle) {
        Ok(descriptor) => descriptor,
        Err(ERROR_ACCESS_DENIED) => {
            return Err(foreign_lock_error(
                "security metadata cannot be read from the named object",
            ));
        }
        Err(status) => return Err(SetupError::Io(io::Error::from_raw_os_error(status as i32))),
    };
    let expected = ExpectedSids::new()?;

    let owner = descriptor.owner().map_err(machine_owner_error)?;
    if !owner.matches(&expected.system) && !owner.matches(&expected.administrators) {
        return Err(foreign_lock_error(
            "owner is neither SYSTEM nor BUILTIN\\Administrators",
        ));
    }

    let control = descriptor.control().map_err(SetupError::Io)?;
    if control & SE_DACL_PROTECTED == 0 {
        return Err(foreign_lock_error("DACL inheritance is enabled"));
    }

    let dacl = descriptor.dacl().map_err(machine_dacl_error)?;
    if dacl.len() != 2 {
        return Err(foreign_lock_error(
            "DACL does not contain exactly the two trusted writer ACEs",
        ));
    }

    let mut saw_system = false;
    let mut saw_administrators = false;
    for ace in dacl.allowed_aces() {
        if ace.flags() != 0 {
            return Err(foreign_lock_error("DACL contains an unexpected ACE"));
        }
        if ace.trustee_matches(&expected.system) {
            if saw_system || ace.mask() != MUTEX_ALL_ACCESS {
                return Err(foreign_lock_error("SYSTEM mutex rights are not exact"));
            }
            saw_system = true;
        } else if ace.trustee_matches(&expected.administrators) {
            if saw_administrators || ace.mask() != MUTEX_ALL_ACCESS {
                return Err(foreign_lock_error(
                    "BUILTIN\\Administrators mutex rights are not exact",
                ));
            }
            saw_administrators = true;
        } else {
            return Err(foreign_lock_error("DACL grants access to an untrusted SID"));
        }
    }
    if !saw_system || !saw_administrators {
        return Err(foreign_lock_error(
            "DACL is missing a required trusted writer ACE",
        ));
    }
    Ok(())
}

fn machine_owner_error(error: SecurityAclError) -> SetupError {
    match error {
        SecurityAclError::Io(error) => SetupError::Io(error),
        SecurityAclError::Invalid(error) => invalid_machine_owner(error),
    }
}

fn invalid_machine_owner(_error: AclValidationError) -> SetupError {
    foreign_lock_error("owner SID is missing or malformed")
}

fn machine_dacl_error(error: SecurityAclError) -> SetupError {
    match error {
        SecurityAclError::Io(error) => SetupError::Io(error),
        SecurityAclError::Invalid(error) => invalid_machine_dacl(error),
    }
}

fn invalid_machine_dacl(_error: AclValidationError) -> SetupError {
    foreign_lock_error("DACL contains an unexpected ACE")
}

fn foreign_lock_error(detail: &str) -> SetupError {
    SetupError::Runtime(format!("foreign machine setup lock rejected: {detail}"))
}

struct ExpectedSids {
    system: OwnedSid,
    administrators: OwnedSid,
}

impl ExpectedSids {
    fn new() -> Result<Self, SetupError> {
        Ok(Self {
            system: OwnedSid::from_string(SYSTEM_SID).map_err(SetupError::Io)?,
            administrators: OwnedSid::from_string(ADMINISTRATORS_SID).map_err(SetupError::Io)?,
        })
    }
}
