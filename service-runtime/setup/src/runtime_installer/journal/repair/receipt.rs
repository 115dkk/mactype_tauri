use std::collections::BTreeMap;
use std::fs;
use std::path::Path;

use mactype_service_contract::{
    sha256_digest, IMMUTABLE_RUNTIME_FILES, MAX_PROFILE_BYTES, MAX_RUNTIME_FILE_BYTES,
};

use super::RuntimeDirectoryReceipt;
use crate::profile_bridge::GENERATED_PROFILE_NAME;
use crate::storage::{
    read_bounded_directory, read_bounded_regular_file, reject_reparse_ancestors, SetupError,
};

pub(super) fn valid_runtime_directory_receipt(receipt: &RuntimeDirectoryReceipt) -> bool {
    !receipt.files.is_empty()
        && receipt.files.len() <= IMMUTABLE_RUNTIME_FILES.len() + 1
        && receipt.files.iter().all(|(name, hash)| {
            (IMMUTABLE_RUNTIME_FILES.contains(&name.as_str()) || name == GENERATED_PROFILE_NAME)
                && hash.len() == 71
                && hash.starts_with("sha256:")
                && hash[7..]
                    .bytes()
                    .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
        })
}

pub(super) fn read_runtime_directory_receipt(
    directory: &Path,
) -> Result<RuntimeDirectoryReceipt, SetupError> {
    reject_reparse_ancestors(directory)?;
    let mut files = BTreeMap::new();
    for entry in read_bounded_directory(
        directory,
        IMMUTABLE_RUNTIME_FILES.len() + 1,
        "runtime repair receipt entry count",
    )? {
        let name = entry.file_name().into_string().map_err(|_| {
            SetupError::Runtime("runtime receipt filename is not Unicode".to_owned())
        })?;
        if !IMMUTABLE_RUNTIME_FILES.contains(&name.as_str()) && name != GENERATED_PROFILE_NAME {
            return Err(SetupError::CleanupUnknown(
                "runtime receipt found an unexpected entry".to_owned(),
            ));
        }
        let path = entry.path();
        reject_reparse_ancestors(&path)?;
        let maximum = if name == GENERATED_PROFILE_NAME {
            MAX_PROFILE_BYTES as u64
        } else {
            MAX_RUNTIME_FILE_BYTES as u64
        };
        let bytes = read_bounded_regular_file(&path, maximum, "runtime receipt entry")?;
        files.insert(name, sha256_digest(&bytes));
    }
    let receipt = RuntimeDirectoryReceipt { files };
    if !valid_runtime_directory_receipt(&receipt) {
        return Err(SetupError::CleanupUnknown(
            "runtime directory does not have a valid bounded receipt".to_owned(),
        ));
    }
    Ok(receipt)
}

pub(super) fn verify_runtime_directory_receipt(
    directory: &Path,
    expected: &RuntimeDirectoryReceipt,
) -> Result<(), SetupError> {
    let actual = read_runtime_directory_receipt(directory).map_err(|error| {
        SetupError::CleanupUnknown(format!(
            "runtime directory receipt could not be verified: {error}"
        ))
    })?;
    if &actual != expected {
        return Err(SetupError::CleanupUnknown(
            "runtime directory no longer matches its transaction receipt".to_owned(),
        ));
    }
    Ok(())
}

pub(super) fn runtime_directory_matches(
    directory: &Path,
    expected: &RuntimeDirectoryReceipt,
) -> Result<bool, SetupError> {
    Ok(read_runtime_directory_receipt(directory)? == *expected)
}

pub(super) fn remove_receipted_runtime_directory(
    directory: &Path,
    expected: &RuntimeDirectoryReceipt,
) -> Result<(), SetupError> {
    verify_runtime_directory_receipt(directory, expected)?;
    remove_repair_directory(directory)
}

pub(super) fn remove_repair_directory(directory: &Path) -> Result<(), SetupError> {
    reject_reparse_ancestors(directory)?;
    let mut files = Vec::new();
    for entry in read_bounded_directory(
        directory,
        IMMUTABLE_RUNTIME_FILES.len() + 1,
        "runtime repair removal entry count",
    )? {
        let name = entry
            .file_name()
            .into_string()
            .map_err(|_| SetupError::Runtime("runtime filename is not Unicode".to_owned()))?;
        if name != GENERATED_PROFILE_NAME && !IMMUTABLE_RUNTIME_FILES.contains(&name.as_str()) {
            return Err(SetupError::Runtime(
                "runtime repair found an unexpected entry".to_owned(),
            ));
        }
        let path = entry.path();
        reject_reparse_ancestors(&path)?;
        let metadata = entry.metadata()?;
        let maximum = if name == GENERATED_PROFILE_NAME {
            MAX_PROFILE_BYTES as u64
        } else {
            MAX_RUNTIME_FILE_BYTES as u64
        };
        if !metadata.is_file() || metadata.len() > maximum {
            return Err(SetupError::Runtime(
                "runtime repair found a non-regular or oversized entry".to_owned(),
            ));
        }
        files.push(path);
    }
    for path in files {
        fs::remove_file(path)?;
    }
    fs::remove_dir(directory)?;
    Ok(())
}
