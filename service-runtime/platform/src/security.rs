//! Security descriptors and the calls that consume or produce them.
//!
//! A descriptor Windows hands back is a single `LocalAlloc` block whose inner
//! pointers (owner, DACL, ACEs, SIDs) all point inside it. The types in the
//! `acl` submodule turn that block into bounds-checked byte views before
//! anything is read, so a malformed or hostile descriptor is rejected by
//! offset arithmetic rather than dereferenced.

mod acl;

use std::ffi::c_void;
use std::io;
use std::mem::size_of;
use std::path::Path;
use std::ptr::{self, NonNull};

use windows_sys::Win32::Foundation::{LocalFree, ERROR_SUCCESS};
use windows_sys::Win32::Security::Authorization::{
    ConvertStringSecurityDescriptorToSecurityDescriptorW, GetNamedSecurityInfoW, GetSecurityInfo,
    TreeSetNamedSecurityInfoW, SDDL_REVISION_1, SE_FILE_OBJECT, SE_KERNEL_OBJECT,
    TREE_SEC_INFO_RESET,
};
use windows_sys::Win32::Security::{
    CheckTokenMembership, GetSecurityDescriptorControl, GetSecurityDescriptorDacl,
    GetSecurityDescriptorLength, GetSecurityDescriptorOwner, DACL_SECURITY_INFORMATION,
    OWNER_SECURITY_INFORMATION, PROTECTED_DACL_SECURITY_INFORMATION, PSECURITY_DESCRIPTOR,
    SECURITY_ATTRIBUTES,
};

pub use acl::{
    AclValidationError, AllowedAce, OwnedSid, SecurityAclError, ValidatedDacl, ValidatedSid,
};

use crate::handle::OwnedHandle;
use crate::wide::{wide_null, wide_path};

/// A descriptor built from SDDL, used to create objects and to reset ACL
/// trees. Windows consumes it whole; it is never read back field by field.
#[derive(Debug)]
pub struct SecurityDescriptor(NonNull<c_void>);

impl SecurityDescriptor {
    pub fn from_sddl(sddl: &str) -> io::Result<Self> {
        let sddl = wide_null(sddl);
        let mut descriptor = ptr::null_mut();
        // SAFETY: `sddl` is NUL-terminated; `descriptor` is a live out pointer
        // that receives a LocalAlloc block owned by the returned value.
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
            return Err(io::Error::last_os_error());
        }
        Ok(Self(
            NonNull::new(descriptor).expect("checked non-null descriptor"),
        ))
    }

    /// A `SECURITY_ATTRIBUTES` block pointing at this descriptor, for object
    /// creation calls. It borrows the descriptor and cannot outlive it.
    pub(crate) fn attributes(&self, inherit_handle: bool) -> SecurityAttributes<'_> {
        SecurityAttributes {
            raw: SECURITY_ATTRIBUTES {
                nLength: size_of::<SECURITY_ATTRIBUTES>() as u32,
                lpSecurityDescriptor: self.0.as_ptr(),
                bInheritHandle: i32::from(inherit_handle),
            },
            _descriptor: std::marker::PhantomData,
        }
    }

    /// Applies this descriptor's DACL to `path` and its whole subtree,
    /// resetting every inherited entry. With `protect` the tree also stops
    /// inheriting from its parent. The error carries the Win32 status.
    pub fn apply_to_tree(&self, path: &Path, protect: bool) -> io::Result<()> {
        let mut present = 0;
        let mut defaulted = 0;
        let mut dacl = ptr::null_mut();
        // SAFETY: `self.0` is a live descriptor and every out pointer is a
        // local; the DACL pointer stays valid while `self` is borrowed.
        if unsafe {
            GetSecurityDescriptorDacl(self.0.as_ptr(), &mut present, &mut dacl, &mut defaulted)
        } == 0
            || present == 0
            || dacl.is_null()
        {
            return Err(io::Error::last_os_error());
        }
        let information = if protect {
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION
        } else {
            DACL_SECURITY_INFORMATION
        };
        let wide = wide_path(path);
        // SAFETY: `wide` is NUL-terminated; `dacl` points into the live
        // descriptor; owner, group, and SACL are null with matching flags
        // absent; no progress callback is installed.
        let status = unsafe {
            TreeSetNamedSecurityInfoW(
                wide.as_ptr(),
                SE_FILE_OBJECT,
                information,
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
            return Err(io::Error::from_raw_os_error(status as i32));
        }
        Ok(())
    }
}

impl Drop for SecurityDescriptor {
    fn drop(&mut self) {
        // SAFETY: the block came from the converter and is freed exactly once.
        unsafe { LocalFree(self.0.as_ptr()) };
    }
}

/// A `SECURITY_ATTRIBUTES` block that borrows its descriptor.
pub(crate) struct SecurityAttributes<'a> {
    raw: SECURITY_ATTRIBUTES,
    _descriptor: std::marker::PhantomData<&'a SecurityDescriptor>,
}

impl SecurityAttributes<'_> {
    pub(crate) fn as_ptr(&self) -> *const SECURITY_ATTRIBUTES {
        &self.raw
    }
}

/// A descriptor Windows returned for an existing object, with the accessors
/// that validate its parts before exposing them.
#[derive(Debug)]
pub struct LocalSecurityDescriptor(NonNull<c_void>);

impl LocalSecurityDescriptor {
    /// The DACL of the file or directory at `path`. The error is the Win32
    /// status, so callers can recognize a vanished path.
    pub fn query_named_file(path: &Path) -> Result<Self, u32> {
        let wide = wide_path(path);
        let mut descriptor = ptr::null_mut();
        // SAFETY: `wide` is NUL-terminated, every optional output is null, and
        // `descriptor` is a live out pointer whose ownership transfers below.
        let status = unsafe {
            GetNamedSecurityInfoW(
                wide.as_ptr(),
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION,
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut(),
                &mut descriptor,
            )
        };
        Self::from_query_result(status, descriptor)
    }

    /// The owner and DACL of the kernel object behind `handle`.
    pub fn query_kernel_object(handle: &OwnedHandle) -> Result<Self, u32> {
        let mut descriptor = ptr::null_mut();
        // SAFETY: the handle is live; all optional outputs are null and
        // `descriptor` is a live out pointer.
        let status = unsafe {
            GetSecurityInfo(
                handle.as_raw(),
                SE_KERNEL_OBJECT,
                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut(),
                &mut descriptor,
            )
        };
        Self::from_query_result(status, descriptor)
    }

    fn from_query_result(status: u32, descriptor: PSECURITY_DESCRIPTOR) -> Result<Self, u32> {
        if status != ERROR_SUCCESS || descriptor.is_null() {
            if !descriptor.is_null() {
                // SAFETY: a non-null descriptor from these APIs is a LocalAlloc
                // block, even on a partial failure; it is freed exactly once.
                unsafe { LocalFree(descriptor) };
            }
            return Err(status);
        }
        Ok(Self(
            NonNull::new(descriptor).expect("checked non-null descriptor"),
        ))
    }

    /// The descriptor control bits (for example `SE_DACL_PROTECTED`).
    pub fn control(&self) -> io::Result<u16> {
        let mut control = 0_u16;
        let mut revision = 0_u32;
        // SAFETY: `self.0` is a live descriptor; both outputs are locals.
        if unsafe { GetSecurityDescriptorControl(self.0.as_ptr(), &mut control, &mut revision) }
            == 0
        {
            return Err(io::Error::last_os_error());
        }
        Ok(control)
    }

    pub fn owner(&self) -> Result<ValidatedSid, SecurityAclError> {
        let mut owner = ptr::null_mut();
        let mut defaulted = 0;
        // SAFETY: `self.0` is a live descriptor; both outputs are locals. The
        // returned owner pointer is range-checked before it is read.
        if unsafe { GetSecurityDescriptorOwner(self.0.as_ptr(), &mut owner, &mut defaulted) } == 0 {
            return Err(io::Error::last_os_error().into());
        }
        if owner.is_null() {
            return Err(AclValidationError::MissingOwner.into());
        }
        ValidatedSid::from_bounded_bytes(self.bytes_from_pointer(owner.cast())?).map_err(Into::into)
    }

    pub fn dacl(&self) -> Result<ValidatedDacl, SecurityAclError> {
        let mut present = 0;
        let mut defaulted = 0;
        let mut dacl = ptr::null_mut();
        // SAFETY: `self.0` is a live descriptor; all outputs are locals. The
        // returned DACL pointer is range-checked before it is read.
        if unsafe {
            GetSecurityDescriptorDacl(self.0.as_ptr(), &mut present, &mut dacl, &mut defaulted)
        } == 0
        {
            return Err(io::Error::last_os_error().into());
        }
        if present == 0 || dacl.is_null() {
            return Err(AclValidationError::MissingDacl.into());
        }
        ValidatedDacl::from_bytes(self.bytes_from_pointer(dacl.cast())?).map_err(Into::into)
    }

    /// The bytes of the descriptor block from `pointer` to its end, provided
    /// `pointer` lies inside the block.
    fn bytes_from_pointer(&self, pointer: *const u8) -> Result<&[u8], AclValidationError> {
        // SAFETY: `self.0` is a live descriptor owned for this call.
        let length = unsafe { GetSecurityDescriptorLength(self.0.as_ptr()) } as usize;
        if length == 0 {
            return Err(AclValidationError::DescriptorRange);
        }
        let offset = (pointer as usize)
            .checked_sub(self.0.as_ptr() as usize)
            .filter(|offset| *offset < length)
            .ok_or(AclValidationError::DescriptorRange)?;
        // SAFETY: Windows reports `length` for this live LocalAlloc block, and
        // the offset was proven to fall inside it.
        let bytes = unsafe { std::slice::from_raw_parts(self.0.as_ptr().cast::<u8>(), length) };
        Ok(&bytes[offset..])
    }
}

impl Drop for LocalSecurityDescriptor {
    fn drop(&mut self) {
        // SAFETY: this value uniquely owns the LocalAlloc block.
        unsafe { LocalFree(self.0.as_ptr()) };
    }
}

/// Whether the current token is an enabled member of `group`.
pub fn current_token_is_member_of(group: &OwnedSid) -> bool {
    let mut is_member = 0;
    // SAFETY: a null token means the current thread or process token; the
    // SID is a live LocalAlloc block owned by `group`; the output is a local.
    let checked = unsafe { CheckTokenMembership(ptr::null_mut(), group.as_ptr(), &mut is_member) };
    checked != 0 && is_member != 0
}

#[cfg(test)]
mod tests {
    use super::{LocalSecurityDescriptor, OwnedSid, SecurityDescriptor};

    #[test]
    fn a_tree_reset_from_sddl_is_read_back_as_the_same_allowed_aces() {
        let directory = tempfile::tempdir().unwrap();
        let child = directory.path().join("child");
        std::fs::create_dir(&child).unwrap();
        let descriptor =
            SecurityDescriptor::from_sddl("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)").unwrap();
        descriptor.apply_to_tree(directory.path(), true).unwrap();

        let system = OwnedSid::from_string("S-1-5-18").unwrap();
        let queried = LocalSecurityDescriptor::query_named_file(&child).unwrap();
        let dacl = queried.dacl().unwrap();
        assert!(dacl.allowed_aces().any(|ace| ace.trustee_matches(&system)));
        assert!(
            LocalSecurityDescriptor::query_named_file(&directory.path().join("missing")).is_err()
        );
    }
}
