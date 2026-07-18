use std::ffi::c_void;
use std::fs;
use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};

use windows::core::{IUnknown, Interface, PCWSTR};
use windows::Win32::System::Com::{
    CoCreateInstance, CoInitializeEx, CoUninitialize, IPersistFile, CLSCTX_INPROC_SERVER,
    COINIT_APARTMENTTHREADED, STGM_READ,
};
use windows::Win32::UI::Shell::{IShellLinkW, ShellLink, SLGP_RAWPATH};
use windows_sys::core::GUID;
use windows_sys::Win32::Foundation::{
    ERROR_FILE_NOT_FOUND, ERROR_NO_MORE_ITEMS, ERROR_PATH_NOT_FOUND, ERROR_SUCCESS,
};
use windows_sys::Win32::System::Com::CoTaskMemFree;
use windows_sys::Win32::System::Registry::{
    RegCloseKey, RegEnumValueW, RegOpenKeyExW, HKEY, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE,
    KEY_READ, KEY_WOW64_32KEY, KEY_WOW64_64KEY, REG_EXPAND_SZ, REG_SZ,
};
use windows_sys::Win32::System::RemoteDesktop::{
    ProcessIdToSessionId, WTSEnumerateProcessesW, WTSFreeMemory, WTS_PROCESS_INFOW,
};
use windows_sys::Win32::UI::Shell::{FOLDERID_Startup, SHGetKnownFolderPath, KF_FLAG_DEFAULT};

use crate::ConflictObservation;

const RUN_KEY: &str = r"SOFTWARE\Microsoft\Windows\CurrentVersion\Run";
const MAX_PROCESS_NAME_UNITS: usize = 32_768;
const MAX_REGISTRY_VALUE_NAME_UNITS: usize = 16_384;
const MAX_REGISTRY_VALUE_BYTES: usize = 1_048_576;
const MAX_SHORTCUT_TEXT_UNITS: usize = 32_768;
const MAX_STARTUP_TEXT_BYTES: u64 = 1_048_576;

pub(super) fn observe_conflict() -> ConflictObservation {
    combine([
        observe_interactive_processes(),
        observe_run_entries(),
        observe_startup_folders(),
    ])
}

fn combine(observations: impl IntoIterator<Item = ConflictObservation>) -> ConflictObservation {
    let mut unknown = false;
    for observation in observations {
        match observation {
            ConflictObservation::Detected => return ConflictObservation::Detected,
            ConflictObservation::Unknown => unknown = true,
            ConflictObservation::Clear => {}
        }
    }
    if unknown {
        ConflictObservation::Unknown
    } else {
        ConflictObservation::Clear
    }
}

struct WtsProcessList(*mut WTS_PROCESS_INFOW);

impl Drop for WtsProcessList {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { WTSFreeMemory(self.0.cast()) };
        }
    }
}

fn observe_interactive_processes() -> ConflictObservation {
    let mut processes = std::ptr::null_mut();
    let mut count = 0_u32;
    if unsafe { WTSEnumerateProcessesW(std::ptr::null_mut(), 0, 1, &mut processes, &mut count) }
        == 0
    {
        return ConflictObservation::Unknown;
    }
    let list = WtsProcessList(processes);
    if count == 0 {
        return ConflictObservation::Clear;
    }
    if list.0.is_null() {
        return ConflictObservation::Unknown;
    }
    let entries = unsafe { std::slice::from_raw_parts(list.0, count as usize) };
    for entry in entries {
        let Some(name) = bounded_wide_string(entry.pProcessName, MAX_PROCESS_NAME_UNITS) else {
            return ConflictObservation::Unknown;
        };
        if !name.eq_ignore_ascii_case("MacTray.exe") {
            continue;
        }
        let mut confirmed_session = 0_u32;
        if unsafe { ProcessIdToSessionId(entry.ProcessId, &mut confirmed_session) } == 0
            || confirmed_session != entry.SessionId
        {
            return ConflictObservation::Unknown;
        }
        if confirmed_session != 0 {
            return ConflictObservation::Detected;
        }
    }
    ConflictObservation::Clear
}

struct RegistryKey(HKEY);

impl Drop for RegistryKey {
    fn drop(&mut self) {
        unsafe { RegCloseKey(self.0) };
    }
}

fn observe_run_entries() -> ConflictObservation {
    combine([
        observe_run_view(HKEY_CURRENT_USER, KEY_WOW64_32KEY),
        observe_run_view(HKEY_CURRENT_USER, KEY_WOW64_64KEY),
        observe_run_view(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY),
        observe_run_view(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY),
    ])
}

fn observe_run_view(root: HKEY, view: u32) -> ConflictObservation {
    let path = wide_null(RUN_KEY);
    let mut raw_key = std::ptr::null_mut();
    let result = unsafe { RegOpenKeyExW(root, path.as_ptr(), 0, KEY_READ | view, &mut raw_key) };
    if matches!(result, ERROR_FILE_NOT_FOUND | ERROR_PATH_NOT_FOUND) {
        return ConflictObservation::Clear;
    }
    if result != ERROR_SUCCESS || raw_key.is_null() {
        return ConflictObservation::Unknown;
    }
    let key = RegistryKey(raw_key);
    let mut index = 0_u32;
    loop {
        let mut name = vec![0_u16; MAX_REGISTRY_VALUE_NAME_UNITS + 1];
        let mut name_length = MAX_REGISTRY_VALUE_NAME_UNITS as u32;
        let mut value_type = 0_u32;
        let mut data = vec![0_u8; MAX_REGISTRY_VALUE_BYTES];
        let mut data_length = data.len() as u32;
        let result = unsafe {
            RegEnumValueW(
                key.0,
                index,
                name.as_mut_ptr(),
                &mut name_length,
                std::ptr::null(),
                &mut value_type,
                data.as_mut_ptr(),
                &mut data_length,
            )
        };
        if result == ERROR_NO_MORE_ITEMS {
            return ConflictObservation::Clear;
        }
        if result != ERROR_SUCCESS
            || name_length as usize > MAX_REGISTRY_VALUE_NAME_UNITS
            || data_length as usize > data.len()
        {
            return ConflictObservation::Unknown;
        }
        let Ok(name) = String::from_utf16(&name[..name_length as usize]) else {
            return ConflictObservation::Unknown;
        };
        let command = matches!(value_type, REG_SZ | REG_EXPAND_SZ)
            .then(|| decode_registry_string(&data[..data_length as usize]))
            .flatten();
        let observation = classify_run_value(value_type, &name, command.as_deref());
        if observation != ConflictObservation::Clear {
            return observation;
        }
        index += 1;
    }
}

fn classify_run_value(
    value_type: u32,
    value_name: &str,
    decoded_command: Option<&str>,
) -> ConflictObservation {
    if matches!(value_type, REG_SZ | REG_EXPAND_SZ) {
        return match decoded_command {
            Some(command) if contains_mactray_target(command) => ConflictObservation::Detected,
            Some(_) => ConflictObservation::Clear,
            None => ConflictObservation::Unknown,
        };
    }

    if value_name.to_ascii_lowercase().contains("mactray") {
        ConflictObservation::Unknown
    } else {
        ConflictObservation::Clear
    }
}

fn decode_registry_string(bytes: &[u8]) -> Option<String> {
    if bytes.len() % 2 != 0 {
        return None;
    }
    let mut units = bytes
        .chunks_exact(2)
        .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
        .collect::<Vec<_>>();
    while units.last() == Some(&0) {
        units.pop();
    }
    String::from_utf16(&units).ok()
}

fn observe_startup_folders() -> ConflictObservation {
    let _apartment = match ComApartment::initialize() {
        Ok(apartment) => apartment,
        Err(()) => return ConflictObservation::Unknown,
    };
    observe_startup_folder(&FOLDERID_Startup)
}

fn observe_startup_folder(identifier: &GUID) -> ConflictObservation {
    let folder = match known_folder(identifier) {
        Ok(folder) => folder,
        Err(()) => return ConflictObservation::Unknown,
    };
    let entries = match fs::read_dir(folder) {
        Ok(entries) => entries,
        Err(_) => return ConflictObservation::Unknown,
    };
    for entry in entries {
        let entry = match entry {
            Ok(entry) => entry,
            Err(_) => return ConflictObservation::Unknown,
        };
        let path = entry.path();
        let file_type = match entry.file_type() {
            Ok(file_type) => file_type,
            Err(_) => return ConflictObservation::Unknown,
        };
        let name = entry.file_name().to_string_lossy().into_owned();
        if file_type.is_dir() {
            continue;
        }
        if file_type.is_symlink() {
            let target = match fs::canonicalize(&path) {
                Ok(target) => target,
                Err(_) => return ConflictObservation::Unknown,
            };
            if target
                .file_name()
                .is_some_and(|name| name.to_string_lossy().eq_ignore_ascii_case("MacTray.exe"))
            {
                return ConflictObservation::Detected;
            }
        }
        if file_type.is_file() && is_direct_mactray_executable_name(&name) {
            return ConflictObservation::Detected;
        }
        let extension = path
            .extension()
            .and_then(|extension| extension.to_str())
            .unwrap_or_default();
        if extension.eq_ignore_ascii_case("lnk") {
            match inspect_shortcut(&path) {
                Ok(true) => return ConflictObservation::Detected,
                Ok(false) => {}
                Err(()) => return ConflictObservation::Unknown,
            }
        } else {
            match startup_text_might_launch_mactray(&path) {
                ConflictObservation::Detected => return ConflictObservation::Detected,
                ConflictObservation::Unknown => return ConflictObservation::Unknown,
                ConflictObservation::Clear => {}
            }
        }
    }
    ConflictObservation::Clear
}

fn is_direct_mactray_executable_name(value: &str) -> bool {
    value.eq_ignore_ascii_case("MacTray.exe")
}

fn inspect_shortcut(path: &Path) -> Result<bool, ()> {
    let link: IShellLinkW =
        unsafe { CoCreateInstance(&ShellLink, None::<&IUnknown>, CLSCTX_INPROC_SERVER) }
            .map_err(|_| ())?;
    let persist: IPersistFile = link.cast().map_err(|_| ())?;
    let path_wide = wide_null(path.as_os_str());
    unsafe { persist.Load(PCWSTR(path_wide.as_ptr()), STGM_READ) }.map_err(|_| ())?;

    let mut target = vec![0_u16; MAX_SHORTCUT_TEXT_UNITS];
    unsafe { link.GetPath(&mut target, std::ptr::null_mut(), SLGP_RAWPATH.0 as u32) }
        .map_err(|_| ())?;
    let target = nul_terminated_buffer(&target).ok_or(())?;
    if target.trim().is_empty() {
        return Err(());
    }
    if contains_mactray_target(&target) {
        return Ok(true);
    }

    let mut arguments = vec![0_u16; MAX_SHORTCUT_TEXT_UNITS];
    unsafe { link.GetArguments(&mut arguments) }.map_err(|_| ())?;
    let arguments = nul_terminated_buffer(&arguments).ok_or(())?;
    Ok(contains_mactray_target(&arguments))
}

fn startup_text_might_launch_mactray(path: &Path) -> ConflictObservation {
    let extension = path
        .extension()
        .and_then(|extension| extension.to_str())
        .unwrap_or_default();
    if !["bat", "cmd", "js", "ps1", "url", "vbs"]
        .iter()
        .any(|candidate| extension.eq_ignore_ascii_case(candidate))
    {
        return ConflictObservation::Clear;
    }
    let metadata = match fs::metadata(path) {
        Ok(metadata) => metadata,
        Err(_) => return ConflictObservation::Unknown,
    };
    if metadata.len() > MAX_STARTUP_TEXT_BYTES {
        return ConflictObservation::Unknown;
    }
    let bytes = match fs::read(path) {
        Ok(bytes) => bytes,
        Err(_) => return ConflictObservation::Unknown,
    };
    let ascii = String::from_utf8_lossy(&bytes);
    if contains_mactray_target(&ascii) {
        return ConflictObservation::Detected;
    }
    if bytes.len() % 2 == 0 {
        let units = bytes
            .chunks_exact(2)
            .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
            .collect::<Vec<_>>();
        if contains_mactray_target(&String::from_utf16_lossy(&units)) {
            return ConflictObservation::Detected;
        }
    }
    ConflictObservation::Clear
}

fn contains_mactray_target(value: &str) -> bool {
    let lower = value.to_ascii_lowercase();
    let needle = "mactray.exe";
    lower.match_indices(needle).any(|(start, _)| {
        let before = lower[..start].chars().next_back();
        let after = lower[start + needle.len()..].chars().next();
        before.map_or(true, target_boundary) && after.map_or(true, target_boundary)
    })
}

fn target_boundary(character: char) -> bool {
    character.is_whitespace()
        || matches!(
            character,
            '\\' | '/' | '"' | '\'' | '=' | ',' | ';' | '(' | ')'
        )
}

fn nul_terminated_buffer(value: &[u16]) -> Option<String> {
    let length = value.iter().position(|unit| *unit == 0)?;
    String::from_utf16(&value[..length]).ok()
}

fn known_folder(identifier: &GUID) -> Result<PathBuf, ()> {
    let mut raw = std::ptr::null_mut();
    let result = unsafe {
        SHGetKnownFolderPath(
            identifier,
            KF_FLAG_DEFAULT as u32,
            std::ptr::null_mut(),
            &mut raw,
        )
    };
    if result < 0 || raw.is_null() {
        return Err(());
    }
    let path = bounded_wide_string(raw, MAX_SHORTCUT_TEXT_UNITS)
        .map(PathBuf::from)
        .ok_or(());
    unsafe { CoTaskMemFree(raw.cast::<c_void>()) };
    path
}

fn bounded_wide_string(pointer: *const u16, maximum: usize) -> Option<String> {
    if pointer.is_null() {
        return None;
    }
    let mut length = 0_usize;
    while length < maximum && unsafe { *pointer.add(length) } != 0 {
        length += 1;
    }
    if length == maximum {
        return None;
    }
    String::from_utf16(unsafe { std::slice::from_raw_parts(pointer, length) }).ok()
}

fn wide_null(value: impl AsRef<std::ffi::OsStr>) -> Vec<u16> {
    value.as_ref().encode_wide().chain(Some(0)).collect()
}

struct ComApartment;

impl ComApartment {
    fn initialize() -> Result<Self, ()> {
        unsafe { CoInitializeEx(None, COINIT_APARTMENTTHREADED) }
            .ok()
            .map(|()| Self)
            .map_err(|_| ())
    }
}

impl Drop for ComApartment {
    fn drop(&mut self) {
        unsafe { CoUninitialize() };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn run_value_name_alone_is_not_mactray_target_evidence() {
        assert_eq!(
            classify_run_value(
                REG_SZ,
                "MacTray",
                Some(r#""C:\Program Files\Other\Helper.exe""#)
            ),
            ConflictObservation::Clear
        );
        assert_eq!(
            classify_run_value(
                REG_SZ,
                "Unrelated",
                Some(r#""C:\Program Files\MacType\MacTray.exe""#),
            ),
            ConflictObservation::Detected
        );
    }

    #[test]
    fn startup_filename_alone_only_identifies_the_direct_binary() {
        assert!(is_direct_mactray_executable_name("MacTray.exe"));
        assert!(is_direct_mactray_executable_name("mactray.EXE"));
        assert!(!is_direct_mactray_executable_name("MacTray.lnk"));
        assert!(!is_direct_mactray_executable_name(
            "MacTray migration notes.txt"
        ));
        assert!(!is_direct_mactray_executable_name("NotMacTray.exe"));
    }
}
