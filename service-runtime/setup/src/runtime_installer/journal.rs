mod repair;

use std::fs::{self, OpenOptions};
use std::io::Read;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use super::{InstalledRuntime, RuntimeInstaller};
use crate::profile_bridge::ProfileRuntimeBridge;
use crate::storage::{
    atomic_write, read_bounded_regular_file, reject_reparse_ancestors, SetupError,
};

const LEGACY_RUNTIME_ACTIVATION_SCHEMA: u32 = 1;
const RUNTIME_ACTIVATION_SCHEMA: u32 = 2;
const MAX_ACTIVATION_JOURNAL_BYTES: u64 = 16 * 1024;
pub(super) const MAX_POINTER_BYTES: u64 = 64 * 1024;

#[derive(Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub(super) struct RuntimePointer {
    pub(super) schema: u32,
    pub(super) version: String,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct RuntimeActivationJournal {
    schema: u32,
    previous: Option<RuntimePointer>,
    activated: RuntimePointer,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct LegacyRuntimeActivationJournal {
    schema: u32,
    previous: Option<RuntimePointer>,
}

struct ParsedRuntimeActivationJournal {
    previous: Option<RuntimePointer>,
    activated: Option<RuntimePointer>,
}

impl RuntimeInstaller {
    pub fn recover_interrupted_activation(&self) -> Result<Option<InstalledRuntime>, SetupError> {
        self.recover_interrupted_repair()?;
        let journal_path = self.activation_journal_path();
        if !journal_path.exists() {
            return self.current();
        }
        let bytes = read_bounded_regular_file(
            &journal_path,
            MAX_ACTIVATION_JOURNAL_BYTES,
            "runtime activation journal",
        )?;
        let journal = parse_runtime_activation_journal(&bytes)?;
        self.restore_runtime_pointer(journal.previous.as_ref(), journal.activated.as_ref())?;
        if journal.previous.is_some() {
            ProfileRuntimeBridge::new(self.paths.clone()).materialize_active()?;
        }
        let recovered = self.current()?;
        self.remove_activation_journal()?;
        Ok(recovered)
    }

    pub(super) fn write_activation_journal(
        &self,
        previous: Option<RuntimePointer>,
        activated: RuntimePointer,
    ) -> Result<(), SetupError> {
        let bytes = serde_json::to_vec(&RuntimeActivationJournal {
            schema: RUNTIME_ACTIVATION_SCHEMA,
            previous,
            activated,
        })?;
        atomic_write(&self.activation_journal_path(), &bytes)
    }

    pub(super) fn remove_activation_journal(&self) -> Result<(), SetupError> {
        let path = self.activation_journal_path();
        if path.exists() {
            reject_reparse_ancestors(&path)?;
            fs::remove_file(path)?;
        }
        Ok(())
    }

    pub(super) fn restore_runtime_pointer(
        &self,
        previous: Option<&RuntimePointer>,
        activated: Option<&RuntimePointer>,
    ) -> Result<(), SetupError> {
        let path = self.paths.runtime_pointer();
        let actual = match fs::symlink_metadata(path) {
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => None,
            Err(error) => {
                return Err(SetupError::CleanupUnknown(format!(
                    "active runtime pointer could not be inspected during rollback: {error}"
                )));
            }
            Ok(metadata) if !metadata.file_type().is_file() => {
                return Err(SetupError::CleanupUnknown(
                    "active runtime pointer became a non-regular path during rollback".to_owned(),
                ));
            }
            Ok(_) => Some(
                read_bounded_regular_file(path, MAX_POINTER_BYTES, "active runtime pointer")
                    .map_err(|error| {
                        SetupError::CleanupUnknown(format!(
                            "active runtime pointer could not be verified during rollback: {error}"
                        ))
                    })?,
            ),
        };
        let previous_bytes = previous.map(serde_json::to_vec).transpose()?;
        let activated_bytes = activated.map(serde_json::to_vec).transpose()?;

        match (actual, previous_bytes, activated_bytes) {
            (None, Some(previous), _) => atomic_write(path, &previous),
            (None, None, _) => Ok(()),
            (Some(actual), Some(previous), _) if actual == previous => Ok(()),
            (Some(actual), Some(previous), Some(activated)) if actual == activated => {
                atomic_write(path, &previous)
            }
            (Some(actual), None, Some(activated)) if actual == activated => {
                remove_exact_runtime_pointer(path, &activated)
            }
            _ => Err(SetupError::CleanupUnknown(
                "active runtime pointer no longer matches the activation transaction receipt"
                    .to_owned(),
            )),
        }
    }

    fn activation_journal_path(&self) -> PathBuf {
        self.paths.runtime_activation_journal().to_owned()
    }
}

fn parse_runtime_activation_journal(
    bytes: &[u8],
) -> Result<ParsedRuntimeActivationJournal, SetupError> {
    let value: serde_json::Value = serde_json::from_slice(bytes)
        .map_err(|_| SetupError::Runtime("runtime activation journal is invalid".to_owned()))?;
    let schema = value
        .get("schema")
        .and_then(serde_json::Value::as_u64)
        .ok_or_else(|| SetupError::Runtime("runtime activation journal is invalid".to_owned()))?;
    let parsed = if schema == u64::from(RUNTIME_ACTIVATION_SCHEMA) {
        let journal: RuntimeActivationJournal = serde_json::from_value(value)
            .map_err(|_| SetupError::Runtime("runtime activation journal is invalid".to_owned()))?;
        ParsedRuntimeActivationJournal {
            previous: journal.previous,
            activated: Some(journal.activated),
        }
    } else if schema == u64::from(LEGACY_RUNTIME_ACTIVATION_SCHEMA) {
        let journal: LegacyRuntimeActivationJournal = serde_json::from_value(value)
            .map_err(|_| SetupError::Runtime("runtime activation journal is invalid".to_owned()))?;
        if journal.schema != LEGACY_RUNTIME_ACTIVATION_SCHEMA {
            return Err(SetupError::Runtime(
                "runtime activation journal has an unsupported value".to_owned(),
            ));
        }
        ParsedRuntimeActivationJournal {
            previous: journal.previous,
            activated: None,
        }
    } else {
        return Err(SetupError::Runtime(
            "runtime activation journal has an unsupported value".to_owned(),
        ));
    };
    if parsed
        .previous
        .iter()
        .chain(parsed.activated.iter())
        .any(|pointer| pointer.schema != 1 || !safe_version_component(&pointer.version))
    {
        return Err(SetupError::Runtime(
            "runtime activation journal has an unsupported value".to_owned(),
        ));
    }
    Ok(parsed)
}

#[cfg(windows)]
fn remove_exact_runtime_pointer(path: &Path, expected: &[u8]) -> Result<(), SetupError> {
    use std::os::windows::fs::{MetadataExt, OpenOptionsExt};
    use std::os::windows::io::AsRawHandle;

    use windows_sys::Win32::Storage::FileSystem::{
        FileDispositionInfo, SetFileInformationByHandle, DELETE, FILE_ATTRIBUTE_REPARSE_POINT,
        FILE_DISPOSITION_INFO, FILE_FLAG_OPEN_REPARSE_POINT, FILE_GENERIC_READ, FILE_SHARE_READ,
    };

    let mut file = OpenOptions::new()
        .access_mode(FILE_GENERIC_READ | DELETE)
        .share_mode(FILE_SHARE_READ)
        .custom_flags(FILE_FLAG_OPEN_REPARSE_POINT)
        .open(path)
        .map_err(|error| {
            SetupError::CleanupUnknown(format!(
                "owned runtime pointer could not be opened for exact deletion: {error}"
            ))
        })?;
    let metadata = file.metadata().map_err(|error| {
        SetupError::CleanupUnknown(format!(
            "owned runtime pointer metadata could not be verified: {error}"
        ))
    })?;
    if metadata.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0
        || !metadata.is_file()
        || metadata.len() == 0
        || metadata.len() > MAX_POINTER_BYTES
    {
        return Err(SetupError::CleanupUnknown(
            "owned runtime pointer became non-regular, reparse, empty, or oversized".to_owned(),
        ));
    }
    let mut actual = Vec::with_capacity(metadata.len() as usize);
    (&mut file)
        .take(MAX_POINTER_BYTES + 1)
        .read_to_end(&mut actual)
        .map_err(|error| {
            SetupError::CleanupUnknown(format!(
                "owned runtime pointer bytes could not be verified: {error}"
            ))
        })?;
    if actual != expected {
        return Err(SetupError::CleanupUnknown(
            "owned runtime pointer changed before exact deletion".to_owned(),
        ));
    }
    let disposition = FILE_DISPOSITION_INFO { DeleteFile: true };
    if unsafe {
        SetFileInformationByHandle(
            file.as_raw_handle(),
            FileDispositionInfo,
            (&raw const disposition).cast(),
            std::mem::size_of::<FILE_DISPOSITION_INFO>() as u32,
        )
    } == 0
    {
        return Err(SetupError::CleanupUnknown(format!(
            "owned runtime pointer exact deletion result is unknown: {}",
            std::io::Error::last_os_error()
        )));
    }
    drop(file);
    Ok(())
}

#[cfg(not(windows))]
fn remove_exact_runtime_pointer(path: &Path, expected: &[u8]) -> Result<(), SetupError> {
    let actual = read_bounded_regular_file(path, MAX_POINTER_BYTES, "owned runtime pointer")
        .map_err(|error| {
            SetupError::CleanupUnknown(format!(
                "owned runtime pointer could not be verified before deletion: {error}"
            ))
        })?;
    if actual != expected {
        return Err(SetupError::CleanupUnknown(
            "owned runtime pointer changed before exact deletion".to_owned(),
        ));
    }
    fs::remove_file(path).map_err(|error| {
        SetupError::CleanupUnknown(format!(
            "owned runtime pointer exact deletion result is unknown: {error}"
        ))
    })
}

pub(super) fn validate_runtime_pointer(bytes: &[u8]) -> Result<RuntimePointer, SetupError> {
    let pointer: RuntimePointer = serde_json::from_slice(bytes)
        .map_err(|_| SetupError::Runtime("active runtime pointer is invalid".to_owned()))?;
    if pointer.schema != 1 || !safe_version_component(&pointer.version) {
        return Err(SetupError::Runtime(
            "active runtime pointer has an unsupported value".to_owned(),
        ));
    }
    Ok(pointer)
}

pub(super) fn safe_version_component(version: &str) -> bool {
    !version.is_empty()
        && version.len() <= 64
        && version
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'+'))
        && !matches!(version, "." | "..")
}
