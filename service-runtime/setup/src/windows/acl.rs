use std::io;
use std::path::{Path, PathBuf};
use std::ptr;

use windows_sys::Win32::Foundation::{LocalFree, ERROR_SUCCESS, GENERIC_EXECUTE, GENERIC_READ};
use windows_sys::Win32::Security::Authorization::{
    ConvertStringSecurityDescriptorToSecurityDescriptorW, ConvertStringSidToSidW,
    GetNamedSecurityInfoW, TreeSetNamedSecurityInfoW, SDDL_REVISION_1, SE_FILE_OBJECT,
    TREE_SEC_INFO_RESET,
};
use windows_sys::Win32::Security::{
    AclSizeInformation, EqualSid, GetAce, GetAclInformation, GetSecurityDescriptorControl,
    ACCESS_ALLOWED_ACE, ACL, ACL_SIZE_INFORMATION, DACL_SECURITY_INFORMATION, INHERIT_ONLY_ACE,
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
    let paths = collect_protected_tree(path)
        .map_err(|error| error.at_machine_path("enumerate protected ACL tree", path))?;
    let descriptor = OwnedSecurityDescriptor::from_sddl(MACHINE_TREE_SDDL)
        .map_err(|error| error.at_machine_path("build protected ACL descriptor", path))?;
    let dacl = descriptor
        .dacl()
        .map_err(|error| error.at_machine_path("read protected ACL descriptor", path))?;
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
        return Err(SetupError::Io(io::Error::from_raw_os_error(status as i32))
            .at_machine_path("reset protected ACL tree", path));
    }

    let sids = ExpectedSids::new()
        .map_err(|error| error.at_machine_path("build protected ACL trustees", path))?;
    for entry in paths {
        verify_protected_acl(&entry, &sids, entry == path)
            .map_err(|error| error.at_machine_path("verify protected ACL entry", &entry))?;
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

fn verify_protected_acl(
    path: &Path,
    sids: &ExpectedSids,
    require_protected: bool,
) -> Result<(), SetupError> {
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
    if unsafe { GetSecurityDescriptorControl(descriptor, &mut control, &mut revision) } == 0 {
        return Err(SetupError::Io(io::Error::last_os_error()));
    }
    if require_protected && control & SE_DACL_PROTECTED == 0 {
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
            if ace.Mask != FILE_ALL_ACCESS {
                return Err(invalid_acl(path));
            }
            if ace_applies_to_current_object(ace.Header.AceFlags) {
                if saw_system {
                    return Err(invalid_acl(path));
                }
                saw_system = true;
            }
        } else if unsafe { EqualSid(sid, sids.administrators.0) } != 0 {
            if ace.Mask != FILE_ALL_ACCESS {
                return Err(invalid_acl(path));
            }
            if ace_applies_to_current_object(ace.Header.AceFlags) {
                if saw_administrators {
                    return Err(invalid_acl(path));
                }
                saw_administrators = true;
            }
        } else if unsafe { EqualSid(sid, sids.users.0) } != 0 {
            if !is_users_read_execute_mask(ace.Mask) {
                return Err(invalid_acl_ace(path, "Users", ace));
            }
            if ace_applies_to_current_object(ace.Header.AceFlags) {
                saw_users = true;
            }
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

fn is_users_read_execute_mask(mask: u32) -> bool {
    mask == (GENERIC_READ | GENERIC_EXECUTE) || mask == (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE)
}

fn ace_applies_to_current_object(flags: u8) -> bool {
    flags & INHERIT_ONLY_ACE as u8 == 0
}

fn invalid_acl_ace(path: &Path, trustee: &str, ace: &ACCESS_ALLOWED_ACE) -> SetupError {
    SetupError::Runtime(format!(
        "protected machine ACL has invalid {trustee} rights (mask=0x{:08X}, flags=0x{:02X}, expected=0x{:08X} or 0x{:08X}): {}",
        ace.Mask,
        ace.Header.AceFlags,
        GENERIC_READ | GENERIC_EXECUTE,
        FILE_GENERIC_READ | FILE_GENERIC_EXECUTE,
        path.display()
    ))
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

#[cfg(test)]
mod tests {
    use mactype_service_contract::BrokerCommand;
    #[cfg(feature = "ci-test-adapter")]
    use mactype_service_contract::{sha256_digest, MachinePaths};
    #[cfg(feature = "ci-test-adapter")]
    use std::collections::BTreeMap;
    use std::path::{Path, PathBuf};
    use std::process::Command;
    use windows_sys::Win32::Security::{CONTAINER_INHERIT_ACE, INHERITED_ACE, OBJECT_INHERIT_ACE};

    use crate::windows::broker::prepare_machine_storage_for_command;
    #[cfg(feature = "ci-test-adapter")]
    use crate::{FixedPayload, RuntimeInstaller};

    use super::{
        ace_applies_to_current_object, harden_machine_directory, is_users_read_execute_mask,
        verify_protected_acl, ExpectedSids, OwnedSecurityDescriptor, DACL_SECURITY_INFORMATION,
        ERROR_SUCCESS, FILE_GENERIC_EXECUTE, FILE_GENERIC_READ, GENERIC_EXECUTE, GENERIC_READ,
        INHERIT_ONLY_ACE, PROTECTED_DACL_SECURITY_INFORMATION, SE_FILE_OBJECT, TREE_SEC_INFO_RESET,
    };
    #[cfg(feature = "ci-test-adapter")]
    use super::{OwnedSid, ADMINISTRATORS_SID};

    const BASE_ACL: &str = "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)";

    #[test]
    fn hardening_accepts_the_generic_read_execute_ace_emitted_for_the_root() {
        let directory = tempfile::tempdir().unwrap();

        let result = harden_machine_directory(directory.path());

        assert!(result.is_ok(), "{result:?}");
    }

    #[test]
    fn hardening_errors_identify_the_bounded_operation_and_path() {
        let directory = tempfile::tempdir().unwrap();
        let missing = directory.path().join("missing-machine-root");

        let error = harden_machine_directory(&missing).unwrap_err();
        let message = error.to_string();

        assert!(
            message.contains("enumerate protected ACL tree"),
            "{message}"
        );
        assert!(
            message.contains(&missing.display().to_string()),
            "{message}"
        );
    }

    #[test]
    fn hardening_verifies_nested_directories_and_regular_files() {
        let directory = tempfile::tempdir().unwrap();
        let nested = directory.path().join("payload").join("generation-1");
        std::fs::create_dir_all(&nested).unwrap();
        std::fs::write(nested.join("manifest.json"), b"{}").unwrap();

        let result = harden_machine_directory(directory.path());

        assert!(result.is_ok(), "{result:?}");
        assert_eq!(
            acl_snapshot(&nested),
            vec![
                (
                    "SYSTEM",
                    super::FILE_ALL_ACCESS,
                    (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERITED_ACE) as u8,
                ),
                (
                    "Administrators",
                    super::FILE_ALL_ACCESS,
                    (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERITED_ACE) as u8,
                ),
                (
                    "Users",
                    FILE_GENERIC_READ | FILE_GENERIC_EXECUTE,
                    INHERITED_ACE as u8,
                ),
                (
                    "Users",
                    GENERIC_READ | GENERIC_EXECUTE,
                    (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE | INHERITED_ACE)
                        as u8,
                ),
            ]
        );
        assert_eq!(
            acl_snapshot(&nested.join("manifest.json")),
            vec![
                ("SYSTEM", super::FILE_ALL_ACCESS, INHERITED_ACE as u8),
                (
                    "Administrators",
                    super::FILE_ALL_ACCESS,
                    INHERITED_ACE as u8,
                ),
                (
                    "Users",
                    FILE_GENERIC_READ | FILE_GENERIC_EXECUTE,
                    INHERITED_ACE as u8,
                ),
            ]
        );
    }

    #[test]
    fn repair_preflight_removes_users_modify_from_a_runtime_file_before_recovery() {
        let directory = tempfile::tempdir().unwrap();
        let runtime = directory.path().join("bin").join("0.2.0");
        std::fs::create_dir_all(&runtime).unwrap();
        let service = runtime.join("mactype-service.exe");
        std::fs::write(&service, b"service").unwrap();
        harden_machine_directory(directory.path()).unwrap();
        apply_acl(&service, "D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x001301BF;;;BU)");
        let sids = ExpectedSids::new().unwrap();
        verify_protected_acl(&service, &sids, false)
            .expect_err("the regression fixture must grant Users Modify");

        prepare_machine_storage_for_command(BrokerCommand::Repair, directory.path()).unwrap();

        verify_protected_acl(directory.path(), &sids, true).unwrap();
        verify_protected_acl(&service, &sids, false).unwrap();
    }

    #[test]
    fn hardening_removes_the_exact_users_modify_ace_emitted_by_icacls() {
        let directory = tempfile::tempdir().unwrap();
        let _cleanup = ResetFixtureAclOnDrop(directory.path().to_owned());
        let runtime = directory.path().join("bin").join("0.2.0");
        std::fs::create_dir_all(&runtime).unwrap();
        let service = runtime.join("mactype-service.exe");
        std::fs::write(&service, b"service").unwrap();
        harden_machine_directory(directory.path()).unwrap();

        grant_users_modify_with_icacls(&service);
        let sids = ExpectedSids::new().unwrap();
        verify_protected_acl(&service, &sids, false)
            .expect_err("the exact hosted-CI fixture must grant Users Modify");

        harden_machine_directory(directory.path()).unwrap();

        verify_protected_acl(directory.path(), &sids, true).unwrap();
        verify_protected_acl(&service, &sids, false).unwrap();
    }

    #[cfg(feature = "ci-test-adapter")]
    #[test]
    fn administrator_required_fixture_never_skips_in_ci() {
        assert_eq!(administrator_fixture_policy(false, true), Ok(true));
        assert_eq!(administrator_fixture_policy(true, true), Ok(true));
        assert_eq!(administrator_fixture_policy(false, false), Ok(false));
        assert!(administrator_fixture_policy(true, false).is_err());
    }

    #[cfg(feature = "ci-test-adapter")]
    #[test]
    fn repair_lifecycle_survives_the_exact_users_modify_ace_emitted_by_icacls() {
        match administrator_fixture_policy(
            running_in_ci(),
            current_token_is_enabled_administrator(),
        ) {
            Ok(true) => {}
            Ok(false) => {
                eprintln!(
                    "skipped locally: protected runtime repair requires an enabled Administrator token"
                );
                return;
            }
            Err(message) => panic!("{message}"),
        }
        let base = tempfile::tempdir_in(std::env::current_dir().unwrap()).unwrap();
        let _cleanup = ResetFixtureAclOnDrop(base.path().to_owned());
        let program_files = base.path().join("Program Files");
        let program_data = base.path().join("ProgramData");
        std::fs::create_dir_all(&program_files).unwrap();
        std::fs::create_dir_all(&program_data).unwrap();
        let paths = MachinePaths::from_trusted_os_roots(&program_files, &program_data).unwrap();
        let payload = test_payload(base.path(), "0.2.0");
        let installer = RuntimeInstaller::new(paths.clone());
        installer
            .deploy_with_health_check(&payload, |_| Ok(()))
            .unwrap();
        harden_machine_directory(paths.service_root()).unwrap();
        let service = paths
            .runtime_versions()
            .join("0.2.0")
            .join("mactype-service.exe");
        grant_users_modify_with_icacls(&service);

        prepare_machine_storage_for_command(BrokerCommand::Repair, paths.service_root()).unwrap();
        installer
            .repair_current_with_health_check(&payload, |_| Ok(()))
            .unwrap();

        verify_protected_acl(&service, &ExpectedSids::new().unwrap(), false).unwrap();
    }

    #[test]
    fn machine_root_with_inheritable_dacl_is_rejected() {
        let directory = tempfile::tempdir().unwrap();
        apply_acl_with_security_information(
            directory.path(),
            "D:(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)",
            DACL_SECURITY_INFORMATION,
        );

        let error = verify_protected_acl(directory.path(), &ExpectedSids::new().unwrap(), true)
            .expect_err("machine root must block parent ACL inheritance");

        assert!(error.to_string().contains("still permits inheritance"));
    }

    #[test]
    fn descendant_users_write_access_is_rejected() {
        let directory = tempfile::tempdir().unwrap();
        let child = directory.path().join("runtime-receipts");
        std::fs::create_dir(&child).unwrap();
        apply_acl(directory.path(), &format!("{BASE_ACL}(A;OICI;GW;;;BU)"));

        let error = verify_protected_acl(&child, &ExpectedSids::new().unwrap(), false)
            .expect_err("descendant Users write access must fail closed");

        assert!(error.to_string().contains("invalid Users rights"));
    }

    #[test]
    fn descendant_unapproved_write_trustee_is_rejected() {
        let directory = tempfile::tempdir().unwrap();
        let child = directory.path().join("runtime-receipts");
        std::fs::create_dir(&child).unwrap();
        apply_acl(directory.path(), &format!("{BASE_ACL}(A;OICI;GW;;;WD)"));

        let error = verify_protected_acl(&child, &ExpectedSids::new().unwrap(), false)
            .expect_err("descendant unapproved write trustee must fail closed");

        assert!(error.to_string().contains("unapproved allow ACE"));
    }

    #[test]
    fn inherit_only_trusted_writer_aces_do_not_replace_current_object_access() {
        for sddl in [
            "D:P(A;OICIIO;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)",
            "D:P(A;OICI;FA;;;SY)(A;OICIIO;FA;;;BA)(A;OICI;GRGX;;;BU)",
        ] {
            let error = verify_acl_fixture(sddl)
                .expect_err("inherit-only trusted writer ACE must not satisfy root access");

            assert!(error
                .to_string()
                .contains("does not match SYSTEM/Admin Full"));
        }
    }

    #[test]
    fn supplemental_inherit_only_trusted_writer_aces_are_allowed() {
        verify_acl_fixture(&format!("{BASE_ACL}(A;OICIIO;FA;;;SY)(A;OICIIO;FA;;;BA)"))
            .expect("inherit-only propagation ACEs may accompany current-object writer ACEs");
    }

    #[test]
    fn hosted_root_and_mapped_child_read_execute_masks_are_safe() {
        assert!(is_users_read_execute_mask(0xA000_0000));
        assert!(is_users_read_execute_mask(0x0012_00A9));
        assert_eq!(GENERIC_READ | GENERIC_EXECUTE, 0xA000_0000);
        assert_eq!(FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, 0x0012_00A9);
        assert!(!ace_applies_to_current_object(
            INHERIT_ONLY_ACE as u8 | 0x03
        ));
        assert!(ace_applies_to_current_object(0x10));
    }

    #[test]
    fn users_write_access_is_rejected() {
        let error = verify_acl_fixture(&format!("{BASE_ACL}(A;OICI;GW;;;BU)"))
            .expect_err("Users write access must fail closed");

        assert!(error.to_string().contains("invalid Users rights"));
    }

    #[test]
    fn everyone_write_access_is_rejected() {
        let error = verify_acl_fixture(&format!("{BASE_ACL}(A;OICI;GW;;;WD)"))
            .expect_err("Everyone write access must fail closed");

        assert!(error.to_string().contains("unapproved allow ACE"));
    }

    #[test]
    fn authenticated_users_write_access_is_rejected() {
        let error = verify_acl_fixture(&format!("{BASE_ACL}(A;OICI;GW;;;AU)"))
            .expect_err("Authenticated Users write access must fail closed");

        assert!(error.to_string().contains("unapproved allow ACE"));
    }

    fn verify_acl_fixture(sddl: &str) -> Result<(), super::SetupError> {
        let directory = tempfile::tempdir().unwrap();
        apply_acl(directory.path(), sddl);
        verify_protected_acl(directory.path(), &ExpectedSids::new().unwrap(), true)
    }

    fn apply_acl(path: &Path, sddl: &str) {
        apply_acl_with_security_information(
            path,
            sddl,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        );
    }

    fn apply_acl_with_security_information(path: &Path, sddl: &str, security_info: u32) {
        let descriptor = OwnedSecurityDescriptor::from_sddl(sddl).unwrap();
        let dacl = descriptor.dacl().unwrap();
        let path_wide = super::wide_path(path);
        let status = unsafe {
            super::TreeSetNamedSecurityInfoW(
                path_wide.as_ptr(),
                SE_FILE_OBJECT,
                security_info,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                dacl,
                std::ptr::null_mut(),
                TREE_SEC_INFO_RESET,
                None,
                0,
                std::ptr::null(),
            )
        };
        assert_eq!(status, ERROR_SUCCESS);
    }

    fn grant_users_modify_with_icacls(path: &Path) {
        let output = Command::new(r"C:\Windows\System32\icacls.exe")
            .arg(path)
            .args(["/grant", "*S-1-5-32-545:(M)"])
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "icacls failed: {}",
            String::from_utf8_lossy(&output.stderr)
        );
    }

    struct ResetFixtureAclOnDrop(PathBuf);

    impl Drop for ResetFixtureAclOnDrop {
        fn drop(&mut self) {
            let _ = Command::new(r"C:\Windows\System32\icacls.exe")
                .arg(&self.0)
                .args(["/reset", "/T", "/C", "/Q"])
                .output();
        }
    }

    #[cfg(feature = "ci-test-adapter")]
    fn administrator_fixture_policy(
        running_in_ci: bool,
        enabled_administrator: bool,
    ) -> Result<bool, &'static str> {
        if enabled_administrator {
            Ok(true)
        } else if running_in_ci {
            Err("CI must provide an enabled Administrator token for the protected repair fixture")
        } else {
            Ok(false)
        }
    }

    #[cfg(feature = "ci-test-adapter")]
    fn running_in_ci() -> bool {
        ["GITHUB_ACTIONS", "CI"]
            .iter()
            .any(|name| std::env::var(name).is_ok_and(|value| value.eq_ignore_ascii_case("true")))
    }

    #[cfg(feature = "ci-test-adapter")]
    fn current_token_is_enabled_administrator() -> bool {
        let administrators = match OwnedSid::from_string(ADMINISTRATORS_SID) {
            Ok(sid) => sid,
            Err(_) => return false,
        };
        let mut is_member = 0;
        unsafe {
            windows_sys::Win32::Security::CheckTokenMembership(
                std::ptr::null_mut(),
                administrators.0,
                &mut is_member,
            ) != 0
                && is_member != 0
        }
    }

    #[cfg(feature = "ci-test-adapter")]
    fn test_payload(base: &Path, version: &str) -> FixedPayload {
        let root = base.join("payload");
        let files_root = root.join("files");
        std::fs::create_dir_all(&files_root).unwrap();
        let payload_files: [(&str, &[u8]); 5] = [
            ("mactype-service.exe", b"service"),
            ("mactype-injector32.exe", b"injector-32"),
            ("mactype-injector64.exe", b"injector-64"),
            ("MacType.dll", b"mactype-32"),
            ("MacType64.dll", b"mactype-64"),
        ];
        let mut files = BTreeMap::new();
        for (name, contents) in payload_files {
            std::fs::write(files_root.join(name), contents).unwrap();
            files.insert(name.to_owned(), sha256_digest(contents));
        }
        std::fs::write(
            root.join("manifest.json"),
            serde_json::to_vec(&serde_json::json!({
                "schema": 1,
                "version": version,
                "files": files,
            }))
            .unwrap(),
        )
        .unwrap();
        FixedPayload::from_test_root(root).unwrap()
    }

    fn acl_snapshot(path: &Path) -> Vec<(&'static str, u32, u8)> {
        let path_wide = super::wide_path(path);
        let mut dacl = std::ptr::null_mut();
        let mut descriptor = std::ptr::null_mut();
        let status = unsafe {
            super::GetNamedSecurityInfoW(
                path_wide.as_ptr(),
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                &mut dacl,
                std::ptr::null_mut(),
                &mut descriptor,
            )
        };
        assert_eq!(status, ERROR_SUCCESS);
        assert!(!descriptor.is_null());
        assert!(!dacl.is_null());
        let _descriptor = super::OwnedLocalMemory(descriptor);

        let mut size = super::ACL_SIZE_INFORMATION::default();
        assert_ne!(
            unsafe {
                super::GetAclInformation(
                    dacl,
                    &mut size as *mut _ as *mut std::ffi::c_void,
                    std::mem::size_of::<super::ACL_SIZE_INFORMATION>() as u32,
                    super::AclSizeInformation,
                )
            },
            0
        );
        let sids = ExpectedSids::new().unwrap();
        (0..size.AceCount)
            .map(|index| {
                let mut raw_ace = std::ptr::null_mut();
                assert_ne!(unsafe { super::GetAce(dacl, index, &mut raw_ace) }, 0);
                let ace = unsafe { &*(raw_ace as *const super::ACCESS_ALLOWED_ACE) };
                let sid = std::ptr::addr_of!(ace.SidStart) as super::PSID;
                let trustee = if unsafe { super::EqualSid(sid, sids.system.0) } != 0 {
                    "SYSTEM"
                } else if unsafe { super::EqualSid(sid, sids.administrators.0) } != 0 {
                    "Administrators"
                } else if unsafe { super::EqualSid(sid, sids.users.0) } != 0 {
                    "Users"
                } else {
                    "Unknown"
                };
                (trustee, ace.Mask, ace.Header.AceFlags)
            })
            .collect()
    }
}
