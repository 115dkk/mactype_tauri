use std::io;
use std::path::Path;
use std::ptr;

use windows_sys::Win32::System::Services::{
    QueryServiceConfigW, QUERY_SERVICE_CONFIGW, SC_HANDLE, SERVICE_AUTO_START,
    SERVICE_ERROR_NORMAL, SERVICE_WIN32_OWN_PROCESS,
};

use super::DISPLAY_NAME;
use crate::SetupError;

mod metadata;

pub(super) use metadata::configure_metadata;

#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct ServiceConfig {
    pub(super) service_type: u32,
    pub(super) start_type: u32,
    pub(super) error_control: u32,
    pub(super) image_path: String,
    pub(super) account: String,
    pub(super) display_name: String,
    pub(super) load_order_group: String,
    pub(super) tag_id: u32,
    pub(super) dependencies: Vec<String>,
}

#[derive(Clone, Copy, Debug)]
pub struct ObservedServiceConfiguration<'a> {
    pub service_type: u32,
    pub start_type: u32,
    pub error_control: u32,
    pub image_path: &'a str,
    pub account: &'a str,
    pub display_name: &'a str,
    pub load_order_group: &'a str,
    pub tag_id: u32,
    pub dependencies: &'a [String],
}

impl ServiceConfig {
    pub(super) fn observed(&self) -> ObservedServiceConfiguration<'_> {
        ObservedServiceConfiguration {
            service_type: self.service_type,
            start_type: self.start_type,
            error_control: self.error_control,
            image_path: &self.image_path,
            account: &self.account,
            display_name: &self.display_name,
            load_order_group: &self.load_order_group,
            tag_id: self.tag_id,
            dependencies: &self.dependencies,
        }
    }
}

pub(super) fn query_config(service: SC_HANDLE) -> Result<ServiceConfig, SetupError> {
    let mut needed = 0;
    unsafe {
        QueryServiceConfigW(service, ptr::null_mut(), 0, &mut needed);
    }
    if needed < std::mem::size_of::<QUERY_SERVICE_CONFIGW>() as u32 {
        return Err(SetupError::Io(io::Error::last_os_error()));
    }
    let word_count = (needed as usize).div_ceil(std::mem::size_of::<usize>());
    let mut storage = vec![0usize; word_count];
    let config = storage.as_mut_ptr().cast::<QUERY_SERVICE_CONFIGW>();
    if unsafe { QueryServiceConfigW(service, config, needed, &mut needed) } == 0 {
        return Err(SetupError::Io(io::Error::last_os_error()));
    }
    let config = unsafe { &*config };
    Ok(ServiceConfig {
        service_type: config.dwServiceType,
        start_type: config.dwStartType,
        error_control: config.dwErrorControl,
        image_path: unsafe { wide_pointer_to_string(config.lpBinaryPathName) },
        account: unsafe { wide_pointer_to_string(config.lpServiceStartName) },
        display_name: unsafe { wide_pointer_to_string(config.lpDisplayName) },
        load_order_group: unsafe { wide_pointer_to_string(config.lpLoadOrderGroup) },
        tag_id: config.dwTagId,
        dependencies: unsafe { wide_multi_pointer_to_strings(config.lpDependencies) },
    })
}

unsafe fn wide_multi_pointer_to_strings(pointer: *const u16) -> Vec<String> {
    if pointer.is_null() {
        return Vec::new();
    }
    let mut result = Vec::new();
    let mut offset = 0usize;
    loop {
        let start = unsafe { pointer.add(offset) };
        if unsafe { *start } == 0 {
            break;
        }
        let mut length = 0usize;
        while unsafe { *start.add(length) } != 0 {
            length += 1;
        }
        result.push(String::from_utf16_lossy(unsafe {
            std::slice::from_raw_parts(start, length)
        }));
        offset += length + 1;
    }
    result
}

unsafe fn wide_pointer_to_string(pointer: *const u16) -> String {
    if pointer.is_null() {
        return String::new();
    }
    let mut length = 0usize;
    while unsafe { *pointer.add(length) } != 0 {
        length += 1;
    }
    String::from_utf16_lossy(unsafe { std::slice::from_raw_parts(pointer, length) })
}

pub(super) fn quoted_image_path(service_binary: &Path) -> Result<String, SetupError> {
    let value = service_binary.to_string_lossy();
    if value.contains('"') {
        return Err(SetupError::Runtime(
            "service binary path contains a quote".to_owned(),
        ));
    }
    Ok(format!("\"{value}\" --service"))
}

pub(super) fn validate_service_binary(
    protected_root: &Path,
    path: &Path,
) -> Result<(), SetupError> {
    if !service_binary_matches_protected_contract(protected_root, path) {
        return Err(SetupError::Runtime(
            "service binary does not match the protected fixed layout".to_owned(),
        ));
    }
    Ok(())
}

pub fn service_image_matches_protected_contract(protected_root: &Path, image_path: &str) -> bool {
    let Some(rest) = image_path.strip_prefix('"') else {
        return false;
    };
    let Some(end_quote) = rest.find('"') else {
        return false;
    };
    let binary_text = &rest[..end_quote];
    if &rest[end_quote + 1..] != " --service" {
        return false;
    }
    service_binary_matches_protected_contract(protected_root, Path::new(binary_text))
}

fn service_binary_matches_protected_contract(protected_root: &Path, binary: &Path) -> bool {
    if !binary.is_absolute()
        || binary
            .file_name()
            .and_then(|name| name.to_str())
            .map_or(true, |name| {
                !name.eq_ignore_ascii_case("mactype-service.exe")
            })
        || !binary.is_file()
    {
        return false;
    }
    let Ok(root) = protected_root.canonicalize() else {
        return false;
    };
    let Ok(binary) = binary.canonicalize() else {
        return false;
    };
    let Ok(relative) = binary.strip_prefix(&root) else {
        return false;
    };
    let components = relative.components().collect::<Vec<_>>();
    components.len() == 3
        && components[0]
            .as_os_str()
            .to_string_lossy()
            .eq_ignore_ascii_case("bin")
        && safe_version_component(components[1].as_os_str().to_string_lossy().as_ref())
        && components[2]
            .as_os_str()
            .to_string_lossy()
            .eq_ignore_ascii_case("mactype-service.exe")
}

pub fn service_configuration_matches_owned_contract(
    protected_root: &Path,
    observed: &ObservedServiceConfiguration<'_>,
) -> bool {
    observed.service_type == SERVICE_WIN32_OWN_PROCESS
        && observed.start_type == SERVICE_AUTO_START
        && observed.error_control == SERVICE_ERROR_NORMAL
        && observed.account.eq_ignore_ascii_case("LocalSystem")
        && observed.display_name == DISPLAY_NAME
        && observed.load_order_group.is_empty()
        && observed.tag_id == 0
        && observed.dependencies.is_empty()
        && service_image_matches_protected_contract(protected_root, observed.image_path)
}

fn safe_version_component(version: &str) -> bool {
    !version.is_empty()
        && version.len() <= 64
        && version
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'+'))
        && !matches!(version, "." | "..")
}

#[cfg(test)]
mod tests {
    use super::validate_service_binary;

    #[test]
    fn service_binary_must_belong_to_the_exact_protected_generation_layout() {
        let base = tempfile::tempdir_in(std::env::current_dir().unwrap()).unwrap();
        let protected_root = base.path().join("Service");
        let protected_binary = protected_root
            .join("bin")
            .join("0.2.0")
            .join("mactype-service.exe");
        let foreign_binary = base.path().join("outside").join("mactype-service.exe");
        std::fs::create_dir_all(protected_binary.parent().unwrap()).unwrap();
        std::fs::create_dir_all(foreign_binary.parent().unwrap()).unwrap();
        std::fs::write(&protected_binary, b"service").unwrap();
        std::fs::write(&foreign_binary, b"foreign").unwrap();

        assert!(validate_service_binary(&protected_root, &protected_binary).is_ok());
        assert!(validate_service_binary(&protected_root, &foreign_binary).is_err());
    }
}
