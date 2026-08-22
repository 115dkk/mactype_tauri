use std::ffi::c_void;
use std::io;
use std::mem::{offset_of, size_of};
use std::os::windows::ffi::OsStrExt;
use std::path::Path;
use std::ptr::{self, NonNull};

use windows_sys::Win32::Foundation::{LocalFree, ERROR_SUCCESS, HANDLE};
use windows_sys::Win32::Security::Authorization::{
    ConvertStringSidToSidW, GetNamedSecurityInfoW, GetSecurityInfo, SE_FILE_OBJECT,
    SE_KERNEL_OBJECT,
};
use windows_sys::Win32::Security::{
    GetLengthSid, GetSecurityDescriptorControl, GetSecurityDescriptorDacl,
    GetSecurityDescriptorLength, GetSecurityDescriptorOwner, IsValidSid, ACCESS_ALLOWED_ACE,
    ACE_HEADER, ACL, DACL_SECURITY_INFORMATION, OWNER_SECURITY_INFORMATION, PSECURITY_DESCRIPTOR,
};

use crate::SetupError;

const ACCESS_ALLOWED_ACE_KIND: u8 = 0;
const SID_FIXED_BYTES: usize = 8;
const SID_SUB_AUTHORITY_BYTES: usize = size_of::<u32>();
const ACE_HEADER_BYTES: usize = size_of::<ACE_HEADER>();
const ACL_HEADER_BYTES: usize = size_of::<ACL>();
const ACCESS_ALLOWED_SID_OFFSET: usize = offset_of!(ACCESS_ALLOWED_ACE, SidStart);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(super) enum AclValidationError {
    DescriptorRange,
    MissingDacl,
    MissingOwner,
    AclTooShort,
    InvalidAclSize,
    TruncatedAceHeader,
    InvalidAceSize,
    AceOutOfRange,
    UnsupportedAceType,
    TruncatedSid,
    InvalidSid,
}

#[derive(Debug)]
pub(super) enum SecurityAclError {
    Io(io::Error),
    Invalid(AclValidationError),
}

impl From<io::Error> for SecurityAclError {
    fn from(value: io::Error) -> Self {
        Self::Io(value)
    }
}

impl From<AclValidationError> for SecurityAclError {
    fn from(value: AclValidationError) -> Self {
        Self::Invalid(value)
    }
}

pub(super) struct LocalSecurityDescriptor(NonNull<c_void>);

impl LocalSecurityDescriptor {
    pub(super) fn query_named_file(path: &Path) -> Result<Self, u32> {
        let path = path
            .as_os_str()
            .encode_wide()
            .chain(Some(0))
            .collect::<Vec<_>>();
        let mut descriptor = ptr::null_mut();
        // SAFETY: the path is NUL-terminated, every optional output is null, and
        // `descriptor` is a live out pointer whose ownership is transferred below.
        let status = unsafe {
            GetNamedSecurityInfoW(
                path.as_ptr(),
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

    pub(super) fn query_kernel_object(handle: HANDLE) -> Result<Self, u32> {
        let mut descriptor = ptr::null_mut();
        // SAFETY: `GetSecurityInfo` validates the kernel handle; all optional
        // outputs are null and `descriptor` is a live out pointer.
        let status = unsafe {
            GetSecurityInfo(
                handle,
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
                // SAFETY: a non-null descriptor returned through this API is
                // allocated with LocalAlloc, including on a partial failure.
                unsafe { LocalFree(descriptor) };
            }
            return Err(status);
        }
        Ok(Self(
            NonNull::new(descriptor).expect("non-null security descriptor"),
        ))
    }

    pub(super) fn control(&self) -> io::Result<u16> {
        let mut control = 0_u16;
        let mut revision = 0_u32;
        // SAFETY: the private constructors establish that `self.0` is a live
        // Windows security descriptor; both outputs are valid stack storage.
        if unsafe { GetSecurityDescriptorControl(self.0.as_ptr(), &mut control, &mut revision) }
            == 0
        {
            return Err(io::Error::last_os_error());
        }
        Ok(control)
    }

    pub(super) fn owner(&self) -> Result<ValidatedSid, SecurityAclError> {
        let mut owner = ptr::null_mut();
        let mut defaulted = 0;
        // SAFETY: `self.0` is a live security descriptor and both outputs are
        // valid stack storage. The returned owner is range-checked below.
        if unsafe { GetSecurityDescriptorOwner(self.0.as_ptr(), &mut owner, &mut defaulted) } == 0 {
            return Err(io::Error::last_os_error().into());
        }
        if owner.is_null() {
            return Err(AclValidationError::MissingOwner.into());
        }
        let bytes = self.bytes_from_pointer(owner.cast())?;
        ValidatedSid::from_bounded_bytes(bytes).map_err(Into::into)
    }

    pub(super) fn dacl(&self) -> Result<ValidatedDacl, SecurityAclError> {
        let mut present = 0;
        let mut defaulted = 0;
        let mut dacl = ptr::null_mut();
        // SAFETY: `self.0` is a live security descriptor and all output pointers
        // refer to valid stack storage. The returned DACL is range-checked below.
        if unsafe {
            GetSecurityDescriptorDacl(self.0.as_ptr(), &mut present, &mut dacl, &mut defaulted)
        } == 0
        {
            return Err(io::Error::last_os_error().into());
        }
        if present == 0 || dacl.is_null() {
            return Err(AclValidationError::MissingDacl.into());
        }
        let bytes = self.bytes_from_pointer(dacl.cast())?;
        ValidatedDacl::from_bytes(bytes).map_err(Into::into)
    }

    fn bytes_from_pointer(&self, pointer: *const u8) -> Result<&[u8], AclValidationError> {
        // SAFETY: `self.0` can only be built from the successful Windows query
        // functions above and remains owned for this method call.
        let descriptor_length = unsafe { GetSecurityDescriptorLength(self.0.as_ptr()) } as usize;
        if descriptor_length == 0 {
            return Err(AclValidationError::DescriptorRange);
        }
        let base = self.0.as_ptr() as usize;
        let pointer = pointer as usize;
        let offset = pointer
            .checked_sub(base)
            .filter(|offset| *offset < descriptor_length)
            .ok_or(AclValidationError::DescriptorRange)?;
        // SAFETY: Windows reports `descriptor_length` for this live LocalAlloc
        // allocation. The child pointer was converted to an in-allocation offset.
        let descriptor =
            unsafe { std::slice::from_raw_parts(self.0.as_ptr().cast::<u8>(), descriptor_length) };
        Ok(&descriptor[offset..])
    }
}

impl Drop for LocalSecurityDescriptor {
    fn drop(&mut self) {
        // SAFETY: this Module uniquely owns the LocalAlloc result.
        unsafe { LocalFree(self.0.as_ptr()) };
    }
}

pub(super) struct OwnedSid {
    raw: NonNull<c_void>,
    bytes: Vec<u8>,
}

impl OwnedSid {
    pub(super) fn from_string(value: &str) -> Result<Self, SetupError> {
        let value = value.encode_utf16().chain(Some(0)).collect::<Vec<_>>();
        let mut sid = ptr::null_mut();
        // SAFETY: the SDDL SID string is NUL-terminated and `sid` is a live out
        // pointer. Successful ownership is transferred to `OwnedSid` below.
        if unsafe { ConvertStringSidToSidW(value.as_ptr(), &mut sid) } == 0 || sid.is_null() {
            return Err(SetupError::Io(io::Error::last_os_error()));
        }
        let raw = NonNull::new(sid).expect("non-null converted SID");
        // SAFETY: `raw` was just returned by ConvertStringSidToSidW.
        if unsafe { IsValidSid(raw.as_ptr()) } == 0 {
            // SAFETY: the failed validation path still uniquely owns `raw`.
            unsafe { LocalFree(raw.as_ptr()) };
            return Err(SetupError::Io(io::Error::new(
                io::ErrorKind::InvalidData,
                "Windows returned an invalid converted SID",
            )));
        }
        // SAFETY: IsValidSid accepted this LocalAlloc-backed SID.
        let length = unsafe { GetLengthSid(raw.as_ptr()) } as usize;
        if length < SID_FIXED_BYTES {
            // SAFETY: this error path still uniquely owns `raw`.
            unsafe { LocalFree(raw.as_ptr()) };
            return Err(SetupError::Io(io::Error::new(
                io::ErrorKind::InvalidData,
                "Windows returned a truncated converted SID",
            )));
        }
        // SAFETY: GetLengthSid returned the initialized byte length of this SID.
        let bytes =
            unsafe { std::slice::from_raw_parts(raw.as_ptr().cast::<u8>(), length) }.to_vec();
        Ok(Self { raw, bytes })
    }

    #[cfg(all(test, feature = "ci-test-adapter"))]
    pub(super) fn as_ptr(&self) -> windows_sys::Win32::Security::PSID {
        self.raw.as_ptr()
    }
}

impl Drop for OwnedSid {
    fn drop(&mut self) {
        // SAFETY: this Module uniquely owns the ConvertStringSidToSidW result.
        unsafe { LocalFree(self.raw.as_ptr()) };
    }
}

#[derive(Debug)]
pub(super) struct ValidatedSid {
    bytes: Vec<u8>,
}

impl ValidatedSid {
    fn from_bounded_bytes(bytes: &[u8]) -> Result<Self, AclValidationError> {
        if bytes.len() < SID_FIXED_BYTES {
            return Err(AclValidationError::TruncatedSid);
        }
        let sub_authorities = usize::from(bytes[1]);
        let length = SID_FIXED_BYTES
            .checked_add(
                sub_authorities
                    .checked_mul(SID_SUB_AUTHORITY_BYTES)
                    .ok_or(AclValidationError::TruncatedSid)?,
            )
            .ok_or(AclValidationError::TruncatedSid)?;
        if length > bytes.len() {
            return Err(AclValidationError::TruncatedSid);
        }

        // Windows SID routines expect an aligned SID pointer. Copying the already
        // bounded bytes into u32 storage keeps that invariant local to this Adapter.
        let mut aligned = vec![0_u32; length.div_ceil(size_of::<u32>())];
        // SAFETY: the byte view covers exactly the initialized u32 allocation;
        // it does not outlive `aligned` and preserves its four-byte alignment.
        let aligned_bytes = unsafe {
            std::slice::from_raw_parts_mut(
                aligned.as_mut_ptr().cast::<u8>(),
                aligned.len() * size_of::<u32>(),
            )
        };
        aligned_bytes[..length].copy_from_slice(&bytes[..length]);
        let sid = aligned.as_mut_ptr().cast::<c_void>();
        // SAFETY: the structural length check above proved every sub-authority
        // byte is present in this aligned allocation.
        if unsafe { IsValidSid(sid) } == 0 {
            return Err(AclValidationError::InvalidSid);
        }
        // SAFETY: IsValidSid accepted the bounded, aligned SID.
        let windows_length = unsafe { GetLengthSid(sid) } as usize;
        if windows_length != length || windows_length > bytes.len() {
            return Err(AclValidationError::InvalidSid);
        }
        Ok(Self {
            bytes: bytes[..length].to_vec(),
        })
    }

    pub(super) fn matches(&self, expected: &OwnedSid) -> bool {
        self.bytes == expected.bytes
    }
}

#[derive(Debug)]
pub(super) struct AllowedAce {
    flags: u8,
    mask: u32,
    trustee: ValidatedSid,
}

impl AllowedAce {
    pub(super) const fn flags(&self) -> u8 {
        self.flags
    }

    pub(super) const fn mask(&self) -> u32 {
        self.mask
    }

    pub(super) fn trustee_matches(&self, expected: &OwnedSid) -> bool {
        self.trustee.matches(expected)
    }
}

#[derive(Debug)]
pub(super) struct ValidatedDacl {
    allowed_aces: Vec<AllowedAce>,
}

impl ValidatedDacl {
    fn from_bytes(bytes: &[u8]) -> Result<Self, AclValidationError> {
        if bytes.len() < ACL_HEADER_BYTES {
            return Err(AclValidationError::AclTooShort);
        }
        let acl_size = usize::from(u16::from_le_bytes([bytes[2], bytes[3]]));
        if acl_size < ACL_HEADER_BYTES || acl_size > bytes.len() {
            return Err(AclValidationError::InvalidAclSize);
        }
        let ace_count = usize::from(u16::from_le_bytes([bytes[4], bytes[5]]));
        let bytes = &bytes[..acl_size];
        if ace_count > (bytes.len() - ACL_HEADER_BYTES) / ACE_HEADER_BYTES {
            return Err(AclValidationError::TruncatedAceHeader);
        }
        let mut offset = ACL_HEADER_BYTES;
        let mut allowed_aces = Vec::with_capacity(ace_count);
        for _ in 0..ace_count {
            let header_end = offset
                .checked_add(ACE_HEADER_BYTES)
                .ok_or(AclValidationError::TruncatedAceHeader)?;
            if header_end > bytes.len() {
                return Err(AclValidationError::TruncatedAceHeader);
            }
            let ace_type = bytes[offset];
            let flags = bytes[offset + 1];
            let ace_size = usize::from(u16::from_le_bytes([bytes[offset + 2], bytes[offset + 3]]));
            if ace_size < ACE_HEADER_BYTES || ace_size % size_of::<u32>() != 0 {
                return Err(AclValidationError::InvalidAceSize);
            }
            let ace_end = offset
                .checked_add(ace_size)
                .ok_or(AclValidationError::AceOutOfRange)?;
            if ace_end > bytes.len() {
                return Err(AclValidationError::AceOutOfRange);
            }
            if ace_type != ACCESS_ALLOWED_ACE_KIND {
                return Err(AclValidationError::UnsupportedAceType);
            }
            if ace_size < ACCESS_ALLOWED_SID_OFFSET + SID_FIXED_BYTES {
                return Err(AclValidationError::InvalidAceSize);
            }
            let mask_offset = offset + offset_of!(ACCESS_ALLOWED_ACE, Mask);
            let mask = u32::from_le_bytes(
                bytes[mask_offset..mask_offset + size_of::<u32>()]
                    .try_into()
                    .expect("validated access mask range"),
            );
            let sid_offset = offset + ACCESS_ALLOWED_SID_OFFSET;
            let trustee = ValidatedSid::from_bounded_bytes(&bytes[sid_offset..ace_end])?;
            allowed_aces.push(AllowedAce {
                flags,
                mask,
                trustee,
            });
            offset = ace_end;
        }
        Ok(Self { allowed_aces })
    }

    pub(super) fn len(&self) -> usize {
        self.allowed_aces.len()
    }

    pub(super) fn allowed_aces(&self) -> impl Iterator<Item = &AllowedAce> {
        self.allowed_aces.iter()
    }
}

#[cfg(test)]
mod tests {
    use super::{AclValidationError, OwnedSid, ValidatedDacl};

    const SYSTEM_SID_BYTES: [u8; 12] = [1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0];

    fn valid_acl() -> Vec<u8> {
        let ace_size = 8 + SYSTEM_SID_BYTES.len();
        let acl_size = 8 + ace_size;
        let mut acl = vec![0_u8; acl_size];
        acl[0] = 2;
        acl[2..4].copy_from_slice(&(acl_size as u16).to_le_bytes());
        acl[4..6].copy_from_slice(&1_u16.to_le_bytes());
        acl[8] = 0;
        acl[9] = 0x10;
        acl[10..12].copy_from_slice(&(ace_size as u16).to_le_bytes());
        acl[12..16].copy_from_slice(&0x001f_0001_u32.to_le_bytes());
        acl[16..].copy_from_slice(&SYSTEM_SID_BYTES);
        acl
    }

    #[test]
    fn valid_access_allowed_ace_exposes_only_validated_policy_fields() {
        let dacl = ValidatedDacl::from_bytes(&valid_acl()).unwrap();
        let system = OwnedSid::from_string("S-1-5-18").unwrap();
        let ace = dacl.allowed_aces().next().unwrap();

        assert_eq!(dacl.len(), 1);
        assert_eq!(ace.flags(), 0x10);
        assert_eq!(ace.mask(), 0x001f_0001);
        assert!(ace.trustee_matches(&system));
    }

    #[test]
    fn short_acl_header_and_invalid_acl_sizes_are_rejected() {
        assert_eq!(
            ValidatedDacl::from_bytes(&[0; 7]).unwrap_err(),
            AclValidationError::AclTooShort
        );

        let mut undersized = valid_acl();
        undersized[2..4].copy_from_slice(&7_u16.to_le_bytes());
        assert_eq!(
            ValidatedDacl::from_bytes(&undersized).unwrap_err(),
            AclValidationError::InvalidAclSize
        );

        let mut oversized = valid_acl();
        let declared = oversized.len() as u16 + 4;
        oversized[2..4].copy_from_slice(&declared.to_le_bytes());
        assert_eq!(
            ValidatedDacl::from_bytes(&oversized).unwrap_err(),
            AclValidationError::InvalidAclSize
        );
    }

    #[test]
    fn truncated_header_and_invalid_ace_sizes_are_rejected() {
        let mut truncated_header = vec![0_u8; 10];
        truncated_header[0] = 2;
        truncated_header[2..4].copy_from_slice(&10_u16.to_le_bytes());
        truncated_header[4..6].copy_from_slice(&1_u16.to_le_bytes());
        assert_eq!(
            ValidatedDacl::from_bytes(&truncated_header).unwrap_err(),
            AclValidationError::TruncatedAceHeader
        );

        let mut undersized = valid_acl();
        undersized[10..12].copy_from_slice(&12_u16.to_le_bytes());
        assert_eq!(
            ValidatedDacl::from_bytes(&undersized).unwrap_err(),
            AclValidationError::InvalidAceSize
        );

        let mut oversized = valid_acl();
        let ace_size = oversized.len() as u16;
        oversized[10..12].copy_from_slice(&ace_size.to_le_bytes());
        assert_eq!(
            ValidatedDacl::from_bytes(&oversized).unwrap_err(),
            AclValidationError::AceOutOfRange
        );
    }

    #[test]
    fn truncated_invalid_and_unsupported_sid_evidence_is_rejected() {
        let mut truncated_sid = valid_acl();
        truncated_sid[10..12].copy_from_slice(&16_u16.to_le_bytes());
        assert_eq!(
            ValidatedDacl::from_bytes(&truncated_sid).unwrap_err(),
            AclValidationError::TruncatedSid
        );

        let mut invalid_sid = valid_acl();
        invalid_sid[16] = 0;
        assert_eq!(
            ValidatedDacl::from_bytes(&invalid_sid).unwrap_err(),
            AclValidationError::InvalidSid
        );

        let mut unsupported = valid_acl();
        unsupported[8] = 1;
        assert_eq!(
            ValidatedDacl::from_bytes(&unsupported).unwrap_err(),
            AclValidationError::UnsupportedAceType
        );
    }
}
