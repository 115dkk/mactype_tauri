use std::io;
use std::ptr;

use windows_sys::Win32::Foundation::{LocalFree, ERROR_ACCESS_DENIED, ERROR_SUCCESS, HANDLE};
use windows_sys::Win32::Security::Authorization::{
    ConvertStringSecurityDescriptorToSecurityDescriptorW, ConvertStringSidToSidW, GetSecurityInfo,
    SDDL_REVISION_1, SE_KERNEL_OBJECT,
};
use windows_sys::Win32::Security::{
    AclSizeInformation, EqualSid, GetAce, GetAclInformation, GetSecurityDescriptorControl,
    ACCESS_ALLOWED_ACE, ACL_SIZE_INFORMATION, DACL_SECURITY_INFORMATION,
    OWNER_SECURITY_INFORMATION, PSECURITY_DESCRIPTOR, PSID, SE_DACL_PROTECTED,
};
use windows_sys::Win32::System::Threading::MUTEX_ALL_ACCESS;

use crate::SetupError;

const MACHINE_SETUP_LOCK_SDDL: &str = "O:BAG:BAD:P(A;;0x001F0001;;;SY)(A;;0x001F0001;;;BA)";
const SYSTEM_SID: &str = "S-1-5-18";
const ADMINISTRATORS_SID: &str = "S-1-5-32-544";
const ACCESS_ALLOWED_ACE_KIND: u8 = 0;

pub(super) struct OwnedSecurityDescriptor(PSECURITY_DESCRIPTOR);

impl OwnedSecurityDescriptor {
    pub(super) fn for_machine_setup_lock() -> Result<Self, SetupError> {
        Self::from_sddl(MACHINE_SETUP_LOCK_SDDL)
    }

    #[cfg(test)]
    pub(super) fn from_sddl_for_test(sddl: &str) -> Result<Self, SetupError> {
        Self::from_sddl(sddl)
    }

    fn from_sddl(sddl: &str) -> Result<Self, SetupError> {
        let sddl = wide(sddl);
        let mut descriptor = ptr::null_mut();
        if unsafe {
            ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.as_ptr(),
                SDDL_REVISION_1,
                &mut descriptor,
                ptr::null_mut(),
            )
        } == 0
            || descriptor.is_null()
        {
            return Err(SetupError::Io(io::Error::last_os_error()));
        }
        Ok(Self(descriptor))
    }

    pub(super) fn as_ptr(&self) -> PSECURITY_DESCRIPTOR {
        self.0
    }
}

impl Drop for OwnedSecurityDescriptor {
    fn drop(&mut self) {
        unsafe {
            LocalFree(self.0);
        }
    }
}

pub(super) fn verify_machine_setup_lock(handle: HANDLE) -> Result<(), SetupError> {
    let mut owner = ptr::null_mut();
    let mut dacl = ptr::null_mut();
    let mut descriptor = ptr::null_mut();
    let status = unsafe {
        GetSecurityInfo(
            handle,
            SE_KERNEL_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &mut owner,
            ptr::null_mut(),
            &mut dacl,
            ptr::null_mut(),
            &mut descriptor,
        )
    };
    if status != ERROR_SUCCESS || descriptor.is_null() || owner.is_null() || dacl.is_null() {
        if !descriptor.is_null() {
            unsafe {
                LocalFree(descriptor);
            }
        }
        if status == ERROR_ACCESS_DENIED {
            return Err(foreign_lock_error(
                "security metadata cannot be read from the named object",
            ));
        }
        return Err(SetupError::Io(io::Error::from_raw_os_error(status as i32)));
    }
    let _descriptor = OwnedLocalMemory(descriptor);
    let expected = ExpectedSids::new()?;

    if unsafe { EqualSid(owner, expected.system.0) } == 0
        && unsafe { EqualSid(owner, expected.administrators.0) } == 0
    {
        return Err(foreign_lock_error(
            "owner is neither SYSTEM nor BUILTIN\\Administrators",
        ));
    }

    let mut control = 0u16;
    let mut revision = 0u32;
    if unsafe { GetSecurityDescriptorControl(descriptor, &mut control, &mut revision) } == 0 {
        return Err(SetupError::Io(io::Error::last_os_error()));
    }
    if control & SE_DACL_PROTECTED == 0 {
        return Err(foreign_lock_error("DACL inheritance is enabled"));
    }

    let mut size = ACL_SIZE_INFORMATION::default();
    if unsafe {
        GetAclInformation(
            dacl,
            &mut size as *mut _ as *mut std::ffi::c_void,
            std::mem::size_of::<ACL_SIZE_INFORMATION>() as u32,
            AclSizeInformation,
        )
    } == 0
    {
        return Err(SetupError::Io(io::Error::last_os_error()));
    }
    if size.AceCount != 2 {
        return Err(foreign_lock_error(
            "DACL does not contain exactly the two trusted writer ACEs",
        ));
    }

    let mut saw_system = false;
    let mut saw_administrators = false;
    for index in 0..size.AceCount {
        let mut raw_ace = ptr::null_mut();
        if unsafe { GetAce(dacl, index, &mut raw_ace) } == 0 || raw_ace.is_null() {
            return Err(SetupError::Io(io::Error::last_os_error()));
        }
        let ace = unsafe { &*(raw_ace as *const ACCESS_ALLOWED_ACE) };
        if ace.Header.AceType != ACCESS_ALLOWED_ACE_KIND || ace.Header.AceFlags != 0 {
            return Err(foreign_lock_error("DACL contains an unexpected ACE"));
        }
        let sid = ptr::addr_of!(ace.SidStart) as PSID;
        if unsafe { EqualSid(sid, expected.system.0) } != 0 {
            if saw_system || ace.Mask != MUTEX_ALL_ACCESS {
                return Err(foreign_lock_error("SYSTEM mutex rights are not exact"));
            }
            saw_system = true;
        } else if unsafe { EqualSid(sid, expected.administrators.0) } != 0 {
            if saw_administrators || ace.Mask != MUTEX_ALL_ACCESS {
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
            system: OwnedSid::from_string(SYSTEM_SID)?,
            administrators: OwnedSid::from_string(ADMINISTRATORS_SID)?,
        })
    }
}

struct OwnedSid(PSID);

impl OwnedSid {
    fn from_string(value: &str) -> Result<Self, SetupError> {
        let value = wide(value);
        let mut sid = ptr::null_mut();
        if unsafe { ConvertStringSidToSidW(value.as_ptr(), &mut sid) } == 0 || sid.is_null() {
            return Err(SetupError::Io(io::Error::last_os_error()));
        }
        Ok(Self(sid))
    }
}

impl Drop for OwnedSid {
    fn drop(&mut self) {
        unsafe {
            LocalFree(self.0);
        }
    }
}

struct OwnedLocalMemory(PSECURITY_DESCRIPTOR);

impl Drop for OwnedLocalMemory {
    fn drop(&mut self) {
        unsafe {
            LocalFree(self.0);
        }
    }
}

fn wide(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(Some(0)).collect()
}
