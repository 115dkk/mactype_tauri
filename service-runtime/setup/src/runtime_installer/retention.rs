mod pins;
mod receipt;

use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};

use mactype_service_contract::{IMMUTABLE_RUNTIME_FILES, MAX_PINNED_RUNTIMES};

use super::journal::{
    safe_version_component, validate_runtime_pointer, RuntimePointer, MAX_POINTER_BYTES,
};
use super::RuntimeInstaller;
use crate::profile_bridge::GENERATED_PROFILE_NAME;
use crate::storage::{
    atomic_write, read_bounded_directory, read_bounded_regular_file, reject_reparse_ancestors,
    SetupError,
};

impl RuntimeInstaller {
    pub(super) fn finalize_retention(
        &self,
        old_pointer: Option<&RuntimePointer>,
        current_version: &str,
    ) -> Result<(), SetupError> {
        let previous = if let Some(previous) = old_pointer
            .filter(|pointer| pointer.version() != current_version)
            .cloned()
        {
            atomic_write(
                &self.previous_runtime_pointer_path(),
                &previous.to_bytes().map_err(|_| {
                    SetupError::Runtime("previous runtime pointer is invalid".to_owned())
                })?,
            )?;
            Some(previous)
        } else if self.previous_runtime_pointer_path().exists() {
            let bytes = read_bounded_regular_file(
                &self.previous_runtime_pointer_path(),
                MAX_POINTER_BYTES,
                "previous runtime pointer",
            )?;
            Some(validate_runtime_pointer(&bytes)?)
        } else {
            None
        };
        let mut retained = BTreeSet::from([current_version.to_owned()]);
        if let Some(previous) = previous {
            retained.insert(previous.version().to_owned());
        }
        retained.extend(self.load_verified_migration_pins()?.into_keys());
        self.cleanup_stale_generations(&retained)
    }

    fn cleanup_stale_generations(&self, retained: &BTreeSet<String>) -> Result<(), SetupError> {
        if !self.paths.runtime_versions().is_dir() {
            return Ok(());
        }
        reject_reparse_ancestors(self.paths.runtime_versions())?;
        for entry in read_bounded_directory(
            self.paths.runtime_versions(),
            MAX_PINNED_RUNTIMES + 2,
            "runtime retention generation count",
        )? {
            let version = match entry.file_name().into_string() {
                Ok(version) => version,
                Err(_) => continue,
            };
            if retained.contains(&version) || !safe_version_component(&version) {
                continue;
            }
            let path = entry.path();
            if let Err(error) = self.remove_manifest_verified_generation(&version, &path) {
                eprintln!("preserved stale runtime generation {version}: {error}");
            }
        }
        Ok(())
    }

    pub(super) fn remove_manifest_verified_generation(
        &self,
        version: &str,
        directory: &Path,
    ) -> Result<(), SetupError> {
        self.verify_runtime_generation_receipt(version, directory)?;
        let removable = read_bounded_directory(
            directory,
            IMMUTABLE_RUNTIME_FILES.len() + 1,
            "runtime removal entry count",
        )?;
        let mut immutable_names = BTreeSet::new();
        let mut paths = Vec::with_capacity(removable.len());
        for entry in removable {
            let name = entry.file_name().into_string().map_err(|_| {
                SetupError::Runtime("runtime removal filename is not Unicode".to_owned())
            })?;
            if name != GENERATED_PROFILE_NAME && !IMMUTABLE_RUNTIME_FILES.contains(&name.as_str()) {
                return Err(SetupError::Runtime(
                    "runtime removal found an unsigned file after verification".to_owned(),
                ));
            }
            reject_reparse_ancestors(&entry.path())?;
            if !entry.metadata()?.is_file() {
                return Err(SetupError::Runtime(
                    "runtime removal found a non-regular file after verification".to_owned(),
                ));
            }
            if name != GENERATED_PROFILE_NAME {
                immutable_names.insert(name);
            }
            paths.push(entry.path());
        }
        if immutable_names.len() != IMMUTABLE_RUNTIME_FILES.len() {
            return Err(SetupError::Runtime(
                "runtime removal file set changed after verification".to_owned(),
            ));
        }
        for path in paths {
            fs::remove_file(path)?;
        }
        fs::remove_dir(directory)?;
        fs::remove_file(self.runtime_receipt_path(version))?;
        Ok(())
    }

    pub(in crate::runtime_installer) fn previous_runtime_pointer_path(&self) -> PathBuf {
        self.paths.service_root().join("previous-runtime.json")
    }
}
