use std::io;
use std::path::{Path, PathBuf};
use std::ptr;

use windows_sys::Win32::Foundation::{LocalFree, ERROR_SUCCESS};
use windows_sys::Win32::Security::Authorization::{
    ConvertStringSecurityDescriptorToSecurityDescriptorW, ConvertStringSidToSidW,
    GetNamedSecurityInfoW, TreeSetNamedSecurityInfoW, SDDL_REVISION_1, SE_FILE_OBJECT,
    TREE_SEC_INFO_RESET,
};
use windows_sys::Win32::Security::{
    AclSizeInformation, EqualSid, GetAce, GetAclInformation, GetSecurityDescriptorControl,
    ACCESS_ALLOWED_ACE, ACL, ACL_SIZE_INFORMATION, DACL_SECURITY_INFORMATION,
    PROTECTED_DACL_SECURITY_INFORMATION, PSECURITY_DESCRIPTOR, PSID, SE_DACL_PROTECTED,
};
use windows_sys::Win32::Storage::FileSystem::{
    FILE_ALL_ACCESS, FILE_GENERIC_EXECUTE, FILE_GENERIC_READ,
};

use crate::storage::reject_reparse_ancestors;
use crate::SetupError;

const MACHINE_TREE_SDDL: &str = "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)";
const SYSTEM_SID: &str = "S-1-5-18";
const ADMINISTRATORS_SID: &str = "S-1-5-32-544";
const USERS_SID: &str = "S-1-5-32-545";
const MAX_PROTECTED_TREE_ENTRIES: usize = 100_000;
const ACCESS_ALLOWED_ACE_KIND: u8 = 0;

pub fn harden_machine_directory(path: &Path) -> Result<(), SetupError> {
    let paths = collect_protected_tree(path)?;
    let descriptor = OwnedSecurityDescriptor::from_sddl(MACHINE_TREE_SDDL)?;
    let dacl = descriptor.dacl()?;
    let path_wide = wide_path(path);
    let status = unsafe {
        TreeSetNamedSecurityInfoW(
            path_wide.as_ptr(),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            ptr::null_mut(),
            ptr::null_mut(),
            dacl,
            ptr::null_mut(),
            TREE_SEC_INFO_RESET,
            None,
            0,
            ptr::null(),
        )
    };
    if status != ERROR_SUCCESS {
        return Err(SetupError::Io(io::Error::from_raw_os_error(status as i32)));
    }

    let sids = ExpectedSids::new()?;
    for entry in paths {
        verify_protected_acl(&entry, &sids)?;
    }
    Ok(())
}

fn collect_protected_tree(root: &Path) -> Result<Vec<PathBuf>, SetupError> {
    reject_reparse_ancestors(root)?;
    let mut pending = vec![root.to_owned()];
    let mut result = Vec::new();
    let mut discovered = 1usize;
    while let Some(path) = pending.pop() {
        reject_reparse_ancestors(&path)?;
        let metadata = std::fs::metadata(&path)?;
        if metadata.is_dir() {
            for entry in std::fs::read_dir(&path)? {
                if discovered == MAX_PROTECTED_TREE_ENTRIES {
                    return Err(SetupError::Runtime(
                        "protected machine tree exceeds the fixed ACL verification bound"
                            .to_owned(),
                    ));
                }
                pending.push(entry?.path());
                discovered += 1;
            }
        }
        result.push(path);
    }
    Ok(result)
}

fn verify_protected_acl(path: &Path, sids: &ExpectedSids) -> Result<(), SetupError> {
    let path_wide = wide_path(path);
    let mut dacl = ptr::null_mut();
    let mut descriptor = ptr::null_mut();
    let status = unsafe {
        GetNamedSecurityInfoW(
            path_wide.as_ptr(),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION,
            ptr::null_mut(),
            ptr::null_mut(),
            &mut dacl,
            ptr::null_mut(),
            &mut descriptor,
        )
    };
    if status != ERROR_SUCCESS || descriptor.is_null() || dacl.is_null() {
        if !descriptor.is_null() {
            unsafe {
                LocalFree(descriptor);
            }
        }
        return Err(SetupError::Io(io::Error::from_raw_os_error(status as i32)));
    }
    let _descriptor = OwnedLocalMemory(descriptor);

    let mut control = 0u16;
    let mut revision = 0u32;
    if unsafe { GetSecurityDescriptorControl(descriptor, &mut control, &mut revision) } == 0
        || control & SE_DACL_PROTECTED == 0
    {
        return Err(SetupError::Runtime(format!(
            "protected machine ACL still permits inheritance: {}",
            path.display()
        )));
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

    let mut saw_system = false;
    let mut saw_administrators = false;
    let mut saw_users = false;
    for index in 0..size.AceCount {
        let mut raw_ace = ptr::null_mut();
        if unsafe { GetAce(dacl, index, &mut raw_ace) } == 0 || raw_ace.is_null() {
            return Err(SetupError::Io(io::Error::last_os_error()));
        }
        let ace = unsafe { &*(raw_ace as *const ACCESS_ALLOWED_ACE) };
        if ace.Header.AceType != ACCESS_ALLOWED_ACE_KIND {
            return Err(SetupError::Runtime(format!(
                "protected machine ACL contains a non-allow ACE: {}",
                path.display()
            )));
        }
        let sid = ptr::addr_of!(ace.SidStart) as PSID;
        if unsafe { EqualSid(sid, sids.system.0) } != 0 {
            if saw_system || ace.Mask != FILE_ALL_ACCESS {
                return Err(invalid_acl(path));
            }
            saw_system = true;
        } else if unsafe { EqualSid(sid, sids.administrators.0) } != 0 {
            if saw_administrators || ace.Mask != FILE_ALL_ACCESS {
                return Err(invalid_acl(path));
            }
            saw_administrators = true;
        } else if unsafe { EqualSid(sid, sids.users.0) } != 0 {
            if saw_users || ace.Mask != FILE_GENERIC_READ | FILE_GENERIC_EXECUTE {
                return Err(invalid_acl(path));
            }
            saw_users = true;
        } else {
            return Err(SetupError::Runtime(format!(
                "protected machine ACL contains an unapproved allow ACE: {}",
                path.display()
            )));
        }
    }
    if !saw_system || !saw_administrators || !saw_users {
        return Err(invalid_acl(path));
    }
    Ok(())
}

fn invalid_acl(path: &Path) -> SetupError {
    SetupError::Runtime(format!(
        "protected machine ACL does not match SYSTEM/Admin Full and Users Read+Execute: {}",
        path.display()
    ))
}

struct ExpectedSids {
    system: OwnedSid,
    administrators: OwnedSid,
    users: OwnedSid,
}

impl ExpectedSids {
    fn new() -> Result<Self, SetupError> {
        Ok(Self {
            system: OwnedSid::from_string(SYSTEM_SID)?,
            administrators: OwnedSid::from_string(ADMINISTRATORS_SID)?,
            users: OwnedSid::from_string(USERS_SID)?,
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

struct OwnedSecurityDescriptor(PSECURITY_DESCRIPTOR);

impl OwnedSecurityDescriptor {
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

    fn dacl(&self) -> Result<*mut ACL, SetupError> {
        let mut present = 0;
        let mut defaulted = 0;
        let mut dacl = ptr::null_mut();
        if unsafe {
            windows_sys::Win32::Security::GetSecurityDescriptorDacl(
                self.0,
                &mut present,
                &mut dacl,
                &mut defaulted,
            )
        } == 0
            || present == 0
            || dacl.is_null()
        {
            return Err(SetupError::Io(io::Error::last_os_error()));
        }
        Ok(dacl)
    }
}

impl Drop for OwnedSecurityDescriptor {
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

fn wide_path(path: &Path) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;

    path.as_os_str().encode_wide().chain(Some(0)).collect()
}
