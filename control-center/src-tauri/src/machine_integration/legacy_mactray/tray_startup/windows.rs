use super::*;
use std::{
    ffi::{c_void, OsStr},
    fs::{File, OpenOptions},
    io::{Read, Write},
    os::windows::ffi::OsStrExt,
    time::{SystemTime, UNIX_EPOCH},
};
use windows_sys::{
    core::{GUID, HRESULT},
    Win32::{
        Foundation::{
            CloseHandle, GetLastError, LocalFree, ERROR_FILE_NOT_FOUND, ERROR_MORE_DATA,
            ERROR_NO_MORE_ITEMS, ERROR_PATH_NOT_FOUND, ERROR_SUCCESS, HANDLE, HLOCAL,
        },
        Security::{
            Authorization::ConvertSidToStringSidW, GetTokenInformation, TokenUser, TOKEN_QUERY,
            TOKEN_USER,
        },
        Storage::FileSystem::{
            GetFileAttributesW, FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_REPARSE_POINT,
            INVALID_FILE_ATTRIBUTES,
        },
        System::{
            Com::{
                CoCreateInstance, CoInitializeEx, CoTaskMemFree, CoUninitialize,
                CLSCTX_INPROC_SERVER, COINIT_APARTMENTTHREADED, STGM_READ,
            },
            Environment::ExpandEnvironmentStringsW,
            Registry::{
                RegCloseKey, RegDeleteValueW, RegEnumValueW, RegOpenKeyExW, RegQueryValueExW,
                RegSetValueExW, HKEY, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, KEY_QUERY_VALUE,
                KEY_READ, KEY_SET_VALUE, KEY_WOW64_32KEY, KEY_WOW64_64KEY, REG_EXPAND_SZ, REG_SZ,
            },
            Threading::{GetCurrentProcess, GetCurrentProcessId, OpenProcessToken},
        },
        UI::Shell::{FOLDERID_Startup, SHGetKnownFolderPath, ShellLink, SLGP_RAWPATH},
    },
};

const RUN_SUBKEY: &str = r"Software\Microsoft\Windows\CurrentVersion\Run";
const MAX_REGISTRY_NAME_UNITS: usize = 16_384;
const MAX_REGISTRY_VALUE_BYTES: usize = 65_536;
const MAX_LINK_BYTES: u64 = 1_048_576;
const MAX_WIDE_UNITS: usize = 32_768;
const RPC_E_CHANGED_MODE: HRESULT = 0x8001_0106_u32 as i32;
const IID_ISHELL_LINK_W: GUID = GUID::from_u128(0x000214f9_0000_0000_c000_000000000046);
const IID_IPERSIST_FILE: GUID = GUID::from_u128(0x0000010b_0000_0000_c000_000000000046);

struct RegistrySource {
    root: HKEY,
    hive: &'static str,
    view: u32,
    access_view: u32,
    source: LegacyTrayStartupSource,
}

struct RegistryObservationContext<'a> {
    expected: &'a Path,
    user_sid: &'a str,
    recorded_at: u64,
}

struct RegistryKey(HKEY);

impl Drop for RegistryKey {
    fn drop(&mut self) {
        unsafe { RegCloseKey(self.0) };
    }
}

struct OwnedHandle(HANDLE);

impl Drop for OwnedHandle {
    fn drop(&mut self) {
        unsafe { CloseHandle(self.0) };
    }
}

struct ComApartment {
    uninitialize: bool,
}

impl ComApartment {
    fn initialize() -> Result<Self, StructuredServiceError> {
        let result = unsafe { CoInitializeEx(std::ptr::null(), COINIT_APARTMENTTHREADED as u32) };
        if result >= 0 {
            Ok(Self { uninitialize: true })
        } else if result == RPC_E_CHANGED_MODE {
            Ok(Self {
                uninitialize: false,
            })
        } else {
            Err(error(
                "legacy-tray-startup-com-unavailable",
                &format!("COM initialization failed with HRESULT {result:#x}"),
                None,
            ))
        }
    }
}

impl Drop for ComApartment {
    fn drop(&mut self) {
        if self.uninitialize {
            unsafe { CoUninitialize() };
        }
    }
}

struct ComPointer(*mut c_void);

impl ComPointer {
    fn as_ptr(&self) -> *mut c_void {
        self.0
    }
}

impl Drop for ComPointer {
    fn drop(&mut self) {
        if self.0.is_null() {
            return;
        }
        type Release = unsafe extern "system" fn(*mut c_void) -> u32;
        let release: Release = unsafe { com_method(self.0, 2) };
        unsafe { release(self.0) };
    }
}

pub(super) fn observe() -> LegacyTrayStartupState {
    let Some(expected) = super::super::windows::expected_mactray_path() else {
        return LegacyTrayStartupState::Unknown {
            error: error(
                "legacy-tray-startup-program-files-unavailable",
                "the fixed Program Files MacTray path could not be determined",
                None,
            ),
        };
    };
    let user_sid = match current_user_sid() {
        Ok(sid) => sid,
        Err(error) => return LegacyTrayStartupState::Unknown { error },
    };
    let recorded_at = match recorded_at() {
        Ok(value) => value,
        Err(error) => return LegacyTrayStartupState::Unknown { error },
    };
    let registry_context = RegistryObservationContext {
        expected: &expected,
        user_sid: &user_sid,
        recorded_at,
    };
    let mut observations = Vec::new();
    for source in registry_sources_for_observation() {
        match observe_registry_source(&source, &registry_context) {
            Ok(found) => observations.extend(found),
            Err(error) => observations.push(LegacyTrayStartupObservation::Unknown(error)),
        }
    }
    match observe_startup_folder(
        &FOLDERID_Startup,
        LegacyTrayStartupSource::CurrentUserStartup,
        &expected,
        &user_sid,
        recorded_at,
    ) {
        Ok(found) => observations.extend(found),
        Err(error) => observations.push(LegacyTrayStartupObservation::Unknown(error)),
    }
    classify_startup_inventory(observations)
}

pub(super) fn observe_owned(
    scope: LegacyTrayStartupScope,
) -> Result<Vec<LegacyTrayStartupArtifact>, StructuredServiceError> {
    let expected = super::super::windows::expected_mactray_path().ok_or_else(|| {
        error(
            "legacy-tray-startup-program-files-unavailable",
            "the fixed Program Files MacTray path could not be determined",
            None,
        )
    })?;
    let user_sid = current_user_sid()?;
    let recorded_at = recorded_at()?;
    let registry_context = RegistryObservationContext {
        expected: &expected,
        user_sid: &user_sid,
        recorded_at,
    };
    let mut observations = Vec::new();
    for source in registry_sources(scope) {
        observations.extend(observe_registry_source(&source, &registry_context)?);
    }
    if let Some((folder_id, source)) = startup_folder_source(scope) {
        observations.extend(observe_startup_folder(
            folder_id,
            source,
            &expected,
            &user_sid,
            recorded_at,
        )?);
    }

    let mut owned = Vec::new();
    for observation in observations {
        match observation {
            LegacyTrayStartupObservation::Owned(artifact) => owned.push(artifact),
            LegacyTrayStartupObservation::Untrusted(_) => {
                return Err(error(
                    "legacy-tray-startup-untrusted",
                    "an untrusted MacTray startup entry exists in the requested scope",
                    None,
                ));
            }
            LegacyTrayStartupObservation::Unknown(problem) => return Err(problem),
        }
    }
    Ok(owned)
}

// `HKEY_CURRENT_USER\...\CurrentVersion\Run` is not subject to WOW64 registry
// redirection (only `HKCU\Software\Classes` is), so its 32-bit and 64-bit views
// alias to one physical key. Probing both would observe the same value twice and
// then break exact removal — the second delete finds the value already gone.
// `HKEY_LOCAL_MACHINE\...\Run` IS redirected, so its two views are distinct
// physical keys and both must be probed.
fn current_user_run_source() -> RegistrySource {
    RegistrySource {
        root: HKEY_CURRENT_USER,
        hive: "HKCU",
        view: 64,
        access_view: KEY_WOW64_64KEY,
        source: LegacyTrayStartupSource::CurrentUserRun64,
    }
}

fn local_machine_run_sources() -> [RegistrySource; 2] {
    [
        RegistrySource {
            root: HKEY_LOCAL_MACHINE,
            hive: "HKLM",
            view: 32,
            access_view: KEY_WOW64_32KEY,
            source: LegacyTrayStartupSource::LocalMachineRun32,
        },
        RegistrySource {
            root: HKEY_LOCAL_MACHINE,
            hive: "HKLM",
            view: 64,
            access_view: KEY_WOW64_64KEY,
            source: LegacyTrayStartupSource::LocalMachineRun64,
        },
    ]
}

fn registry_sources_for_observation() -> Vec<RegistrySource> {
    let mut sources = vec![current_user_run_source()];
    sources.extend(local_machine_run_sources());
    sources
}

fn registry_sources(scope: LegacyTrayStartupScope) -> Vec<RegistrySource> {
    match scope {
        LegacyTrayStartupScope::CurrentUser => vec![current_user_run_source()],
        LegacyTrayStartupScope::LocalMachine => local_machine_run_sources().into_iter().collect(),
    }
}

fn startup_folder_source(
    scope: LegacyTrayStartupScope,
) -> Option<(&'static GUID, LegacyTrayStartupSource)> {
    match scope {
        LegacyTrayStartupScope::CurrentUser => Some((
            &FOLDERID_Startup,
            LegacyTrayStartupSource::CurrentUserStartup,
        )),
        LegacyTrayStartupScope::LocalMachine => None,
    }
}

pub(super) fn read_artifact_bytes(
    artifact: &LegacyTrayStartupArtifact,
) -> Result<Option<Vec<u8>>, StructuredServiceError> {
    validate_artifact(artifact)?;
    match &artifact.locator {
        LegacyTrayStartupLocator::Registry {
            value_name,
            value_type,
            ..
        } => {
            let source = registry_source_for_artifact(artifact)?;
            let Some(key) = open_registry_key(&source, KEY_QUERY_VALUE)? else {
                return Ok(None);
            };
            match read_registry_value(&key, value_name)? {
                None => Ok(None),
                Some((current_type, bytes)) if current_type == *value_type => Ok(Some(bytes)),
                Some(_) => Err(error(
                    "legacy-tray-startup-registry-type-changed",
                    "the receipt-named Run value type no longer matches the receipt",
                    None,
                )),
            }
        }
        LegacyTrayStartupLocator::File { startup_file_path } => {
            read_link_if_present(artifact, startup_file_path)
        }
    }
}

pub(super) fn remove_artifact_exact(
    artifact: &LegacyTrayStartupArtifact,
) -> Result<(), StructuredServiceError> {
    validate_artifact(artifact)?;
    match &artifact.locator {
        LegacyTrayStartupLocator::Registry {
            value_name,
            value_type,
            ..
        } => {
            let source = registry_source_for_artifact(artifact)?;
            let key =
                open_registry_key(&source, KEY_QUERY_VALUE | KEY_SET_VALUE)?.ok_or_else(|| {
                    error(
                        "legacy-tray-startup-changed",
                        "the fixed Run key disappeared before removal",
                        None,
                    )
                })?;
            let current = read_registry_value(&key, value_name)?;
            if current
                .as_ref()
                .map(|(kind, bytes)| (*kind, bytes.as_slice()))
                != Some((*value_type, artifact.raw_bytes.as_slice()))
            {
                return Err(error(
                    "legacy-tray-startup-changed",
                    "the Run value changed before exact removal",
                    None,
                ));
            }
            let value_name = wide(value_name);
            let status = unsafe { RegDeleteValueW(key.0, value_name.as_ptr()) };
            if status != ERROR_SUCCESS {
                return Err(error(
                    "legacy-tray-startup-registry-delete-failed",
                    "the verified Run value could not be removed",
                    Some(status),
                ));
            }
            Ok(())
        }
        LegacyTrayStartupLocator::File { startup_file_path } => {
            let current = read_link_if_present(artifact, startup_file_path)?;
            if current.as_deref() != Some(artifact.raw_bytes.as_slice()) {
                return Err(error(
                    "legacy-tray-startup-changed",
                    "the Startup shortcut changed before exact removal",
                    None,
                ));
            }
            std::fs::remove_file(startup_file_path).map_err(|io| {
                error(
                    "legacy-tray-startup-link-delete-failed",
                    &format!("the verified Startup shortcut could not be removed: {io}"),
                    io.raw_os_error().map(|value| value as u32),
                )
            })
        }
    }
}

pub(super) fn restore_artifact_if_absent(
    artifact: &LegacyTrayStartupArtifact,
) -> Result<(), StructuredServiceError> {
    validate_artifact(artifact)?;
    match &artifact.locator {
        LegacyTrayStartupLocator::Registry {
            value_name,
            value_type,
            ..
        } => {
            let source = registry_source_for_artifact(artifact)?;
            let key =
                open_registry_key(&source, KEY_QUERY_VALUE | KEY_SET_VALUE)?.ok_or_else(|| {
                    error(
                        "legacy-tray-startup-registry-key-missing",
                        "the fixed Run key is absent and will not be created during restore",
                        None,
                    )
                })?;
            if read_registry_value(&key, value_name)?.is_some() {
                return Err(error(
                    "legacy-tray-startup-changed",
                    "the Run value is no longer absent before restore",
                    None,
                ));
            }
            let value_name = wide(value_name);
            let status = unsafe {
                RegSetValueExW(
                    key.0,
                    value_name.as_ptr(),
                    0,
                    *value_type,
                    artifact.raw_bytes.as_ptr(),
                    artifact.raw_bytes.len() as u32,
                )
            };
            if status != ERROR_SUCCESS {
                return Err(error(
                    "legacy-tray-startup-registry-restore-failed",
                    "the original Run value bytes could not be restored",
                    Some(status),
                ));
            }
            let restored = read_registry_value(&key, artifact.entry.display_name.as_str())?;
            if restored
                .as_ref()
                .map(|(kind, bytes)| (*kind, bytes.as_slice()))
                != Some((*value_type, artifact.raw_bytes.as_slice()))
            {
                return Err(error(
                    "legacy-tray-startup-registry-restore-unverified",
                    "the restored Run value does not match the receipt",
                    None,
                ));
            }
            Ok(())
        }
        LegacyTrayStartupLocator::File { startup_file_path } => {
            if read_link_if_present(artifact, startup_file_path)?.is_some() {
                return Err(error(
                    "legacy-tray-startup-changed",
                    "the Startup shortcut is no longer absent before restore",
                    None,
                ));
            }
            restore_link_atomically(startup_file_path, &artifact.raw_bytes)?;
            let restored = read_link_if_present(artifact, startup_file_path)?;
            if restored.as_deref() != Some(artifact.raw_bytes.as_slice()) {
                return Err(error(
                    "legacy-tray-startup-link-restore-unverified",
                    "the restored Startup shortcut does not match the receipt",
                    None,
                ));
            }
            Ok(())
        }
    }
}

fn validate_artifact(artifact: &LegacyTrayStartupArtifact) -> Result<(), StructuredServiceError> {
    let expected = super::super::windows::expected_mactray_path().ok_or_else(|| {
        error(
            "legacy-tray-startup-program-files-unavailable",
            "the fixed Program Files MacTray path could not be determined",
            None,
        )
    })?;
    if artifact.raw_bytes.is_empty()
        || artifact.raw_bytes.len() > MAX_LINK_BYTES as usize
        || !same_windows_path(&artifact.normalized_target_path, &expected)
        || !same_windows_path(&artifact.entry.target_path, &expected)
    {
        return Err(error(
            "legacy-tray-startup-receipt-untrusted",
            "the startup receipt artifact is not an exact bounded MacTray target",
            None,
        ));
    }
    if startup_source_requires_current_user_sid(artifact.entry.source_kind)
        && artifact.user_sid != current_user_sid()?
    {
        return Err(error(
            "legacy-tray-startup-receipt-user-mismatch",
            "the startup receipt does not belong to the current user",
            None,
        ));
    }
    match &artifact.locator {
        LegacyTrayStartupLocator::Registry {
            subkey,
            value_name,
            value_type,
            ..
        } => {
            if subkey != RUN_SUBKEY
                || value_name != &artifact.entry.display_name
                || (*value_type != REG_SZ && *value_type != REG_EXPAND_SZ)
                || artifact.raw_bytes.len() > MAX_REGISTRY_VALUE_BYTES
            {
                return Err(error(
                    "legacy-tray-startup-receipt-locator-untrusted",
                    "the startup receipt does not name a supported fixed Run value",
                    None,
                ));
            }
            registry_source_for_artifact(artifact)?;
            let decoded = decode_registry_string(&artifact.raw_bytes)?;
            let command = if *value_type == REG_EXPAND_SZ {
                expand_environment(&decoded)?
            } else {
                decoded
            };
            if classify_startup_command(&command, &expected)
                != StartupTargetClassification::Owned(expected.clone())
            {
                return Err(error(
                    "legacy-tray-startup-receipt-untrusted",
                    "the receipt Run bytes do not encode the fixed MacTray command",
                    None,
                ));
            }
        }
        LegacyTrayStartupLocator::File { startup_file_path } => {
            validate_link_locator(artifact, startup_file_path)?;
        }
    }
    Ok(())
}

fn registry_source_for_artifact(
    artifact: &LegacyTrayStartupArtifact,
) -> Result<RegistrySource, StructuredServiceError> {
    let source = match artifact.entry.source_kind {
        LegacyTrayStartupSource::CurrentUserRun32 => RegistrySource {
            root: HKEY_CURRENT_USER,
            hive: "HKCU",
            view: 32,
            access_view: KEY_WOW64_32KEY,
            source: LegacyTrayStartupSource::CurrentUserRun32,
        },
        LegacyTrayStartupSource::CurrentUserRun64 => RegistrySource {
            root: HKEY_CURRENT_USER,
            hive: "HKCU",
            view: 64,
            access_view: KEY_WOW64_64KEY,
            source: LegacyTrayStartupSource::CurrentUserRun64,
        },
        LegacyTrayStartupSource::LocalMachineRun32 => RegistrySource {
            root: HKEY_LOCAL_MACHINE,
            hive: "HKLM",
            view: 32,
            access_view: KEY_WOW64_32KEY,
            source: LegacyTrayStartupSource::LocalMachineRun32,
        },
        LegacyTrayStartupSource::LocalMachineRun64 => RegistrySource {
            root: HKEY_LOCAL_MACHINE,
            hive: "HKLM",
            view: 64,
            access_view: KEY_WOW64_64KEY,
            source: LegacyTrayStartupSource::LocalMachineRun64,
        },
        LegacyTrayStartupSource::CurrentUserStartup => {
            return Err(error(
                "legacy-tray-startup-receipt-locator-untrusted",
                "a Startup folder source cannot name a registry value",
                None,
            ));
        }
    };
    let LegacyTrayStartupLocator::Registry {
        hive, view, subkey, ..
    } = &artifact.locator
    else {
        return Err(error(
            "legacy-tray-startup-receipt-locator-untrusted",
            "a Run source must use a registry locator",
            None,
        ));
    };
    if hive != source.hive || *view != source.view || subkey != RUN_SUBKEY {
        return Err(error(
            "legacy-tray-startup-receipt-locator-untrusted",
            "the Run locator does not match its fixed hive, view, and subkey",
            None,
        ));
    }
    Ok(source)
}

fn open_registry_key(
    source: &RegistrySource,
    access: u32,
) -> Result<Option<RegistryKey>, StructuredServiceError> {
    let subkey = wide(RUN_SUBKEY);
    let mut raw_key = std::ptr::null_mut();
    let status = unsafe {
        RegOpenKeyExW(
            source.root,
            subkey.as_ptr(),
            0,
            access | source.access_view,
            &mut raw_key,
        )
    };
    if status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND {
        return Ok(None);
    }
    if status != ERROR_SUCCESS || raw_key.is_null() {
        return Err(error(
            "legacy-tray-startup-registry-inaccessible",
            &format!(
                "{} {}-bit Run key cannot be opened with the required fixed access",
                source.hive, source.view
            ),
            Some(status),
        ));
    }
    Ok(Some(RegistryKey(raw_key)))
}

fn read_registry_value(
    key: &RegistryKey,
    value_name: &str,
) -> Result<Option<(u32, Vec<u8>)>, StructuredServiceError> {
    let value_name = wide(value_name);
    let mut value_type = 0_u32;
    let mut raw = vec![0_u8; MAX_REGISTRY_VALUE_BYTES];
    let mut raw_length = raw.len() as u32;
    let status = unsafe {
        RegQueryValueExW(
            key.0,
            value_name.as_ptr(),
            std::ptr::null_mut(),
            &mut value_type,
            raw.as_mut_ptr(),
            &mut raw_length,
        )
    };
    if status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND {
        return Ok(None);
    }
    if status == ERROR_MORE_DATA {
        return Err(error(
            "legacy-tray-startup-registry-value-oversized",
            "the Run value exceeds the bounded startup receipt",
            Some(status),
        ));
    }
    if status != ERROR_SUCCESS || raw_length as usize > raw.len() {
        return Err(error(
            "legacy-tray-startup-registry-read-failed",
            "the receipt-named Run value cannot be read exactly",
            Some(status),
        ));
    }
    raw.truncate(raw_length as usize);
    Ok(Some((value_type, raw)))
}

fn validate_link_locator(
    artifact: &LegacyTrayStartupArtifact,
    path: &Path,
) -> Result<PathBuf, StructuredServiceError> {
    let folder_id = match artifact.entry.source_kind {
        LegacyTrayStartupSource::CurrentUserStartup => &FOLDERID_Startup,
        LegacyTrayStartupSource::CurrentUserRun32
        | LegacyTrayStartupSource::CurrentUserRun64
        | LegacyTrayStartupSource::LocalMachineRun32
        | LegacyTrayStartupSource::LocalMachineRun64 => {
            return Err(error(
                "legacy-tray-startup-receipt-locator-untrusted",
                "a Run source cannot name a Startup shortcut",
                None,
            ));
        }
    };
    let folder = known_folder(folder_id)?;
    require_regular_directory(&folder)?;
    if !matches!(path.parent(), Some(parent) if same_windows_path(parent, &folder))
        || !matches!(
            path.extension().and_then(OsStr::to_str),
            Some(extension) if extension.eq_ignore_ascii_case("lnk")
        )
        || path
            .file_stem()
            .map(|stem| stem.to_string_lossy())
            .as_deref()
            != Some(artifact.entry.display_name.as_str())
    {
        return Err(error(
            "legacy-tray-startup-receipt-locator-untrusted",
            "the Startup shortcut locator is not a direct .lnk child of its fixed folder",
            None,
        ));
    }
    Ok(folder)
}

fn read_link_if_present(
    artifact: &LegacyTrayStartupArtifact,
    path: &Path,
) -> Result<Option<Vec<u8>>, StructuredServiceError> {
    let folder = validate_link_locator(artifact, path)?;
    match std::fs::symlink_metadata(path) {
        Ok(_) => {
            require_regular_link_under(&folder, path)?;
            read_bounded_link(path).map(Some)
        }
        Err(io) if io.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(io) => Err(error(
            "legacy-tray-startup-link-inaccessible",
            &format!("the receipt-named Startup shortcut cannot be inspected: {io}"),
            io.raw_os_error().map(|value| value as u32),
        )),
    }
}

fn restore_link_atomically(
    destination: &Path,
    raw_bytes: &[u8],
) -> Result<(), StructuredServiceError> {
    let parent = destination.parent().ok_or_else(|| {
        error(
            "legacy-tray-startup-receipt-locator-untrusted",
            "the Startup shortcut has no fixed parent folder",
            None,
        )
    })?;
    let process_id = unsafe { GetCurrentProcessId() };
    let mut temporary = None;
    for attempt in 0..32_u32 {
        let candidate = parent.join(format!(
            ".mactype-control-center-restore-{process_id}-{attempt}.tmp"
        ));
        match OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&candidate)
        {
            Ok(mut file) => {
                if let Err(io) = file.write_all(raw_bytes).and_then(|_| file.sync_all()) {
                    drop(file);
                    let _ = std::fs::remove_file(&candidate);
                    return Err(error(
                        "legacy-tray-startup-link-restore-failed",
                        &format!("the Startup shortcut restore staging failed: {io}"),
                        io.raw_os_error().map(|value| value as u32),
                    ));
                }
                drop(file);
                temporary = Some(candidate);
                break;
            }
            Err(io) if io.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(io) => {
                return Err(error(
                    "legacy-tray-startup-link-restore-failed",
                    &format!("the Startup shortcut restore cannot be staged: {io}"),
                    io.raw_os_error().map(|value| value as u32),
                ));
            }
        }
    }
    let temporary = temporary.ok_or_else(|| {
        error(
            "legacy-tray-startup-link-restore-failed",
            "a unique bounded Startup shortcut restore path could not be allocated",
            None,
        )
    })?;
    if let Err(io) = std::fs::rename(&temporary, destination) {
        let _ = std::fs::remove_file(&temporary);
        return Err(error(
            "legacy-tray-startup-link-restore-failed",
            &format!("the original Startup shortcut bytes could not be restored: {io}"),
            io.raw_os_error().map(|value| value as u32),
        ));
    }
    Ok(())
}

fn observe_registry_source(
    source: &RegistrySource,
    context: &RegistryObservationContext<'_>,
) -> Result<Vec<LegacyTrayStartupObservation>, StructuredServiceError> {
    let subkey = wide(RUN_SUBKEY);
    let mut raw_key = std::ptr::null_mut();
    let open = unsafe {
        RegOpenKeyExW(
            source.root,
            subkey.as_ptr(),
            0,
            KEY_READ | source.access_view,
            &mut raw_key,
        )
    };
    if open == ERROR_FILE_NOT_FOUND || open == ERROR_PATH_NOT_FOUND {
        return Ok(Vec::new());
    }
    if open != ERROR_SUCCESS {
        return Err(error(
            "legacy-tray-startup-registry-inaccessible",
            &format!(
                "{} {}-bit Run key is inaccessible",
                source.hive, source.view
            ),
            Some(open),
        ));
    }
    if raw_key.is_null() {
        return Err(error(
            "legacy-tray-startup-registry-invalid",
            "the Run key returned an invalid handle",
            None,
        ));
    }
    let key = RegistryKey(raw_key);
    let mut result = Vec::new();
    let mut index = 0_u32;
    loop {
        let mut name = vec![0_u16; MAX_REGISTRY_NAME_UNITS];
        let mut name_length = name.len() as u32;
        let mut value_type = 0_u32;
        let mut raw = vec![0_u8; MAX_REGISTRY_VALUE_BYTES];
        let mut raw_length = raw.len() as u32;
        let status = unsafe {
            RegEnumValueW(
                key.0,
                index,
                name.as_mut_ptr(),
                &mut name_length,
                std::ptr::null(),
                &mut value_type,
                raw.as_mut_ptr(),
                &mut raw_length,
            )
        };
        if status == ERROR_NO_MORE_ITEMS {
            break;
        }
        if status == ERROR_MORE_DATA {
            return Err(error(
                "legacy-tray-startup-registry-value-oversized",
                "a Run value exceeds the bounded startup inventory",
                Some(status),
            ));
        }
        if status != ERROR_SUCCESS {
            return Err(error(
                "legacy-tray-startup-registry-enumeration-failed",
                "a Run value could not be read",
                Some(status),
            ));
        }
        if name_length as usize >= name.len() || raw_length as usize > raw.len() {
            return Err(error(
                "legacy-tray-startup-registry-value-invalid",
                "a Run value returned invalid bounded lengths",
                None,
            ));
        }
        name.truncate(name_length as usize);
        raw.truncate(raw_length as usize);
        let display_name = String::from_utf16(&name).map_err(|_| {
            error(
                "legacy-tray-startup-registry-name-invalid",
                "a Run value name is not valid UTF-16",
                None,
            )
        })?;
        if let Some(observation) =
            classify_registry_value(source, display_name, value_type, raw, context)?
        {
            result.push(observation);
        }
        index = index.checked_add(1).ok_or_else(|| {
            error(
                "legacy-tray-startup-registry-enumeration-overflow",
                "the Run value inventory exceeded its index range",
                None,
            )
        })?;
    }
    Ok(result)
}

fn classify_registry_value(
    source: &RegistrySource,
    display_name: String,
    value_type: u32,
    raw_bytes: Vec<u8>,
    context: &RegistryObservationContext<'_>,
) -> Result<Option<LegacyTrayStartupObservation>, StructuredServiceError> {
    if value_type != REG_SZ && value_type != REG_EXPAND_SZ {
        return if suspicious_tray_name(&display_name) {
            Err(error(
                "legacy-tray-startup-registry-type-untrusted",
                "a MacTray-named Run value is not a supported string type",
                None,
            ))
        } else {
            Ok(None)
        };
    }
    let decoded = decode_registry_string(&raw_bytes)?;
    let command = if value_type == REG_EXPAND_SZ {
        expand_environment(&decoded)?
    } else {
        decoded
    };
    let candidate = is_legacy_tray_startup_candidate(&display_name, &command);
    if !candidate {
        return Ok(None);
    }
    let target = startup_target_hint(&command).unwrap_or_else(|| PathBuf::from(&command));
    let entry = LegacyTrayStartupEntry {
        source_kind: source.source,
        display_name: display_name.clone(),
        target_path: target,
    };
    match classify_startup_command(&command, context.expected) {
        StartupTargetClassification::Owned(normalized_target_path) => Ok(Some(
            LegacyTrayStartupObservation::Owned(LegacyTrayStartupArtifact {
                entry,
                locator: LegacyTrayStartupLocator::Registry {
                    hive: source.hive.to_owned(),
                    view: source.view,
                    subkey: RUN_SUBKEY.to_owned(),
                    value_name: display_name,
                    value_type,
                },
                raw_bytes,
                normalized_target_path,
                user_sid: context.user_sid.to_owned(),
                recorded_at: context.recorded_at,
            }),
        )),
        StartupTargetClassification::Untrusted => {
            Ok(Some(LegacyTrayStartupObservation::Untrusted(entry)))
        }
    }
}

fn observe_startup_folder(
    folder_id: &GUID,
    source: LegacyTrayStartupSource,
    expected: &Path,
    user_sid: &str,
    recorded_at: u64,
) -> Result<Vec<LegacyTrayStartupObservation>, StructuredServiceError> {
    let folder = known_folder(folder_id)?;
    if !folder.exists() {
        return Ok(Vec::new());
    }
    require_regular_directory(&folder)?;
    let entries = std::fs::read_dir(&folder).map_err(|io| {
        error(
            "legacy-tray-startup-folder-inaccessible",
            &format!("the fixed Startup folder cannot be read: {io}"),
            io.raw_os_error().map(|value| value as u32),
        )
    })?;
    let _apartment = ComApartment::initialize()?;
    let mut result = Vec::new();
    for entry in entries {
        let entry = entry.map_err(|io| {
            error(
                "legacy-tray-startup-folder-enumeration-failed",
                &format!("a Startup folder entry cannot be read: {io}"),
                io.raw_os_error().map(|value| value as u32),
            )
        })?;
        let path = entry.path();
        if !matches!(
            path.extension().and_then(OsStr::to_str),
            Some(extension) if extension.eq_ignore_ascii_case("lnk")
        ) {
            continue;
        }
        let display_name = path
            .file_stem()
            .map(|value| value.to_string_lossy().into_owned())
            .unwrap_or_default();
        if let Err(problem) = require_regular_link_under(&folder, &path) {
            if suspicious_tray_name(&display_name) {
                result.push(LegacyTrayStartupObservation::Unknown(problem));
            }
            continue;
        }
        let resolution = resolve_shell_link(&path);
        let (target, arguments) = match resolution {
            Ok(value) => value,
            Err(problem) => {
                if suspicious_tray_name(&display_name) {
                    result.push(LegacyTrayStartupObservation::Unknown(problem));
                }
                continue;
            }
        };
        let is_mactray_target = target
            .file_name()
            .is_some_and(|name| name.to_string_lossy().eq_ignore_ascii_case("MacTray.exe"));
        if !is_mactray_target && !suspicious_tray_name(&display_name) {
            continue;
        }
        let startup_entry = LegacyTrayStartupEntry {
            source_kind: source,
            display_name,
            target_path: target.clone(),
        };
        if same_windows_path(&target, expected) && arguments.is_empty() {
            let raw_bytes = read_bounded_link(&path)?;
            result.push(LegacyTrayStartupObservation::Owned(
                LegacyTrayStartupArtifact {
                    entry: startup_entry,
                    locator: LegacyTrayStartupLocator::File {
                        startup_file_path: path,
                    },
                    raw_bytes,
                    normalized_target_path: expected.to_path_buf(),
                    user_sid: user_sid.to_owned(),
                    recorded_at,
                },
            ));
        } else {
            result.push(LegacyTrayStartupObservation::Untrusted(startup_entry));
        }
    }
    Ok(result)
}

fn decode_registry_string(raw: &[u8]) -> Result<String, StructuredServiceError> {
    if raw.is_empty() || raw.len() % 2 != 0 || raw.len() > MAX_REGISTRY_VALUE_BYTES {
        return Err(error(
            "legacy-tray-startup-registry-string-invalid",
            "a Run string has an invalid bounded UTF-16 byte sequence",
            None,
        ));
    }
    let mut units = raw
        .chunks_exact(2)
        .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
        .collect::<Vec<_>>();
    while units.last() == Some(&0) {
        units.pop();
    }
    if units.is_empty() || units.contains(&0) {
        return Err(error(
            "legacy-tray-startup-registry-string-invalid",
            "a Run string is empty or contains an embedded terminator",
            None,
        ));
    }
    String::from_utf16(&units).map_err(|_| {
        error(
            "legacy-tray-startup-registry-string-invalid",
            "a Run string is not valid UTF-16",
            None,
        )
    })
}

fn expand_environment(value: &str) -> Result<String, StructuredServiceError> {
    let source = wide(value);
    let required = unsafe { ExpandEnvironmentStringsW(source.as_ptr(), std::ptr::null_mut(), 0) };
    if required == 0 || required as usize > MAX_WIDE_UNITS {
        return Err(last_error(
            "legacy-tray-startup-environment-expansion-failed",
            "a Run environment string cannot be expanded within the bound",
        ));
    }
    let mut result = vec![0_u16; required as usize];
    let written =
        unsafe { ExpandEnvironmentStringsW(source.as_ptr(), result.as_mut_ptr(), required) };
    if written == 0 || written > required {
        return Err(last_error(
            "legacy-tray-startup-environment-expansion-failed",
            "a Run environment string changed during bounded expansion",
        ));
    }
    decode_nul_terminated(&result)
}

fn current_user_sid() -> Result<String, StructuredServiceError> {
    let mut token = std::ptr::null_mut();
    if unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) } == 0 {
        return Err(last_error(
            "legacy-tray-startup-user-token-unavailable",
            "the current user token cannot be opened",
        ));
    }
    let token = OwnedHandle(token);
    let mut needed = 0_u32;
    unsafe { GetTokenInformation(token.0, TokenUser, std::ptr::null_mut(), 0, &mut needed) };
    if needed < std::mem::size_of::<TOKEN_USER>() as u32
        || needed as usize > MAX_REGISTRY_VALUE_BYTES
    {
        return Err(last_error(
            "legacy-tray-startup-user-sid-unavailable",
            "the current user SID has an invalid bounded length",
        ));
    }
    let word = std::mem::size_of::<usize>();
    let mut buffer = vec![0_usize; (needed as usize).div_ceil(word)];
    if unsafe {
        GetTokenInformation(
            token.0,
            TokenUser,
            buffer.as_mut_ptr().cast(),
            needed,
            &mut needed,
        )
    } == 0
    {
        return Err(last_error(
            "legacy-tray-startup-user-sid-unavailable",
            "the current user SID cannot be read",
        ));
    }
    let token_user = unsafe { &*buffer.as_ptr().cast::<TOKEN_USER>() };
    let mut sid_string = std::ptr::null_mut();
    if unsafe { ConvertSidToStringSidW(token_user.User.Sid, &mut sid_string) } == 0
        || sid_string.is_null()
    {
        return Err(last_error(
            "legacy-tray-startup-user-sid-unavailable",
            "the current user SID cannot be formatted",
        ));
    }
    let value = decode_wide_pointer(sid_string);
    unsafe { LocalFree(sid_string.cast::<c_void>() as HLOCAL) };
    value
}

fn known_folder(id: &GUID) -> Result<PathBuf, StructuredServiceError> {
    let mut pointer = std::ptr::null_mut();
    let result = unsafe { SHGetKnownFolderPath(id, 0, std::ptr::null_mut(), &mut pointer) };
    if result < 0 || pointer.is_null() {
        return Err(error(
            "legacy-tray-startup-known-folder-unavailable",
            &format!("a fixed Startup folder is unavailable (HRESULT {result:#x})"),
            None,
        ));
    }
    let decoded = decode_wide_pointer(pointer);
    unsafe { CoTaskMemFree(pointer.cast()) };
    decoded.map(PathBuf::from)
}

fn require_regular_directory(path: &Path) -> Result<(), StructuredServiceError> {
    let attributes = file_attributes(path)?;
    if attributes & FILE_ATTRIBUTE_DIRECTORY == 0 || attributes & FILE_ATTRIBUTE_REPARSE_POINT != 0
    {
        Err(error(
            "legacy-tray-startup-folder-untrusted",
            "the fixed Startup folder is not a regular non-reparse directory",
            None,
        ))
    } else {
        Ok(())
    }
}

fn require_regular_link_under(folder: &Path, path: &Path) -> Result<(), StructuredServiceError> {
    if !matches!(path.parent(), Some(parent) if same_windows_path(parent, folder)) {
        return Err(error(
            "legacy-tray-startup-link-untrusted",
            "a Startup shortcut is outside the fixed folder",
            None,
        ));
    }
    let attributes = file_attributes(path)?;
    if attributes & FILE_ATTRIBUTE_DIRECTORY != 0 || attributes & FILE_ATTRIBUTE_REPARSE_POINT != 0
    {
        return Err(error(
            "legacy-tray-startup-link-untrusted",
            "a Startup shortcut is not a regular non-reparse file",
            None,
        ));
    }
    Ok(())
}

fn file_attributes(path: &Path) -> Result<u32, StructuredServiceError> {
    let path = wide(path.as_os_str());
    let attributes = unsafe { GetFileAttributesW(path.as_ptr()) };
    if attributes == INVALID_FILE_ATTRIBUTES {
        Err(last_error(
            "legacy-tray-startup-path-inaccessible",
            "a fixed Startup path cannot be inspected",
        ))
    } else {
        Ok(attributes)
    }
}

fn read_bounded_link(path: &Path) -> Result<Vec<u8>, StructuredServiceError> {
    let mut file = File::open(path).map_err(|io| {
        error(
            "legacy-tray-startup-link-inaccessible",
            &format!("the Startup shortcut cannot be opened: {io}"),
            io.raw_os_error().map(|value| value as u32),
        )
    })?;
    let before = file.metadata().map_err(|io| {
        error(
            "legacy-tray-startup-link-inaccessible",
            &format!("the Startup shortcut metadata cannot be read: {io}"),
            io.raw_os_error().map(|value| value as u32),
        )
    })?;
    if !before.is_file() || before.len() > MAX_LINK_BYTES {
        return Err(error(
            "legacy-tray-startup-link-oversized",
            "the Startup shortcut is not a bounded regular file",
            None,
        ));
    }
    let mut raw = Vec::with_capacity(before.len() as usize);
    Read::by_ref(&mut file)
        .take(MAX_LINK_BYTES + 1)
        .read_to_end(&mut raw)
        .map_err(|io| {
            error(
                "legacy-tray-startup-link-read-failed",
                &format!("the Startup shortcut bytes cannot be read: {io}"),
                io.raw_os_error().map(|value| value as u32),
            )
        })?;
    if raw.len() as u64 != before.len() || raw.len() as u64 > MAX_LINK_BYTES {
        return Err(error(
            "legacy-tray-startup-link-changed",
            "the Startup shortcut changed during bounded capture",
            None,
        ));
    }
    Ok(raw)
}

fn resolve_shell_link(path: &Path) -> Result<(PathBuf, String), StructuredServiceError> {
    let mut shell_link = std::ptr::null_mut();
    let created = unsafe {
        CoCreateInstance(
            &ShellLink,
            std::ptr::null_mut(),
            CLSCTX_INPROC_SERVER,
            &IID_ISHELL_LINK_W,
            &mut shell_link,
        )
    };
    if created < 0 || shell_link.is_null() {
        return Err(error(
            "legacy-tray-startup-link-resolution-failed",
            &format!("the ShellLink object cannot be created (HRESULT {created:#x})"),
            None,
        ));
    }
    let shell_link = ComPointer(shell_link);
    type QueryInterface =
        unsafe extern "system" fn(*mut c_void, *const GUID, *mut *mut c_void) -> HRESULT;
    let query_interface: QueryInterface = unsafe { com_method(shell_link.as_ptr(), 0) };
    let mut persist_file = std::ptr::null_mut();
    let queried =
        unsafe { query_interface(shell_link.as_ptr(), &IID_IPERSIST_FILE, &mut persist_file) };
    if queried < 0 || persist_file.is_null() {
        return Err(error(
            "legacy-tray-startup-link-resolution-failed",
            &format!("IPersistFile is unavailable (HRESULT {queried:#x})"),
            None,
        ));
    }
    let persist_file = ComPointer(persist_file);
    type Load = unsafe extern "system" fn(*mut c_void, *const u16, u32) -> HRESULT;
    let load: Load = unsafe { com_method(persist_file.as_ptr(), 5) };
    let wide_path = wide(path.as_os_str());
    let loaded = unsafe { load(persist_file.as_ptr(), wide_path.as_ptr(), STGM_READ) };
    if loaded < 0 {
        return Err(error(
            "legacy-tray-startup-link-resolution-failed",
            &format!("the Startup shortcut cannot be loaded (HRESULT {loaded:#x})"),
            None,
        ));
    }
    type GetPath =
        unsafe extern "system" fn(*mut c_void, *mut u16, i32, *mut c_void, u32) -> HRESULT;
    let get_path: GetPath = unsafe { com_method(shell_link.as_ptr(), 3) };
    let mut target = vec![0_u16; MAX_WIDE_UNITS];
    let target_result = unsafe {
        get_path(
            shell_link.as_ptr(),
            target.as_mut_ptr(),
            target.len() as i32,
            std::ptr::null_mut(),
            SLGP_RAWPATH as u32,
        )
    };
    if target_result < 0 {
        return Err(error(
            "legacy-tray-startup-link-resolution-failed",
            &format!("the shortcut target cannot be read (HRESULT {target_result:#x})"),
            None,
        ));
    }
    type GetArguments = unsafe extern "system" fn(*mut c_void, *mut u16, i32) -> HRESULT;
    let get_arguments: GetArguments = unsafe { com_method(shell_link.as_ptr(), 10) };
    let mut arguments = vec![0_u16; MAX_WIDE_UNITS];
    let arguments_result = unsafe {
        get_arguments(
            shell_link.as_ptr(),
            arguments.as_mut_ptr(),
            arguments.len() as i32,
        )
    };
    if arguments_result < 0 {
        return Err(error(
            "legacy-tray-startup-link-resolution-failed",
            &format!("the shortcut arguments cannot be read (HRESULT {arguments_result:#x})"),
            None,
        ));
    }
    let target = decode_nul_terminated(&target)?;
    if target.is_empty() {
        return Err(error(
            "legacy-tray-startup-link-resolution-failed",
            "the Startup shortcut has no target",
            None,
        ));
    }
    Ok((PathBuf::from(target), decode_nul_terminated(&arguments)?))
}

unsafe fn com_method<T>(object: *mut c_void, index: usize) -> T {
    let vtable = unsafe { *(object.cast::<*const *const c_void>()) };
    let method = unsafe { *vtable.add(index) };
    unsafe { std::mem::transmute_copy(&method) }
}

fn startup_target_hint(command: &str) -> Option<PathBuf> {
    if let Some(target) = command.strip_prefix('"') {
        let end = target.find('"')?;
        return Some(PathBuf::from(&target[..end]));
    }
    let lower = command.to_ascii_lowercase();
    let end = lower.find("mactray.exe")? + "mactray.exe".len();
    Some(PathBuf::from(command[..end].trim()))
}

fn suspicious_tray_name(value: &str) -> bool {
    is_legacy_tray_startup_candidate(value, "")
}

fn recorded_at() -> Result<u64, StructuredServiceError> {
    let duration = SystemTime::now().duration_since(UNIX_EPOCH).map_err(|_| {
        error(
            "legacy-tray-startup-clock-invalid",
            "the system clock predates the Unix epoch",
            None,
        )
    })?;
    u64::try_from(duration.as_millis()).map_err(|_| {
        error(
            "legacy-tray-startup-clock-invalid",
            "the startup observation timestamp exceeds its receipt range",
            None,
        )
    })
}

fn decode_wide_pointer(pointer: *const u16) -> Result<String, StructuredServiceError> {
    if pointer.is_null() {
        return Err(error(
            "legacy-tray-startup-wide-string-invalid",
            "a Windows string pointer is null",
            None,
        ));
    }
    let mut length = 0_usize;
    while length < MAX_WIDE_UNITS && unsafe { *pointer.add(length) } != 0 {
        length += 1;
    }
    if length == MAX_WIDE_UNITS {
        return Err(error(
            "legacy-tray-startup-wide-string-invalid",
            "a Windows string is not bounded",
            None,
        ));
    }
    String::from_utf16(unsafe { std::slice::from_raw_parts(pointer, length) }).map_err(|_| {
        error(
            "legacy-tray-startup-wide-string-invalid",
            "a Windows string is not valid UTF-16",
            None,
        )
    })
}

fn decode_nul_terminated(buffer: &[u16]) -> Result<String, StructuredServiceError> {
    let Some(end) = buffer.iter().position(|unit| *unit == 0) else {
        return Err(error(
            "legacy-tray-startup-wide-string-invalid",
            "a Windows string did not terminate within the bound",
            None,
        ));
    };
    String::from_utf16(&buffer[..end]).map_err(|_| {
        error(
            "legacy-tray-startup-wide-string-invalid",
            "a Windows string is not valid UTF-16",
            None,
        )
    })
}

fn wide(value: impl AsRef<OsStr>) -> Vec<u16> {
    value.as_ref().encode_wide().chain(Some(0)).collect()
}

fn last_error(code: &str, message: &str) -> StructuredServiceError {
    error(code, message, Some(unsafe { GetLastError() }))
}

fn error(code: &str, message: &str, win32_error: Option<u32>) -> StructuredServiceError {
    StructuredServiceError {
        code: code.to_owned(),
        message: message.to_owned(),
        win32_error,
    }
}
