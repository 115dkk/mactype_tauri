mod deployment;
mod journal;
mod retention;
mod uninstall;

use std::path::{Path, PathBuf};

use mactype_service_contract::MachinePaths;

use self::journal::{validate_runtime_pointer, RuntimePointer, MAX_POINTER_BYTES};
use crate::profile_bridge::ProfileRuntimeBridge;
use crate::storage::{
    atomic_write, read_bounded_regular_file, reject_reparse_ancestors, SetupError,
};

pub struct FixedPayload {
    root: PathBuf,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InstalledRuntime {
    version: String,
    service_binary: PathBuf,
}

impl InstalledRuntime {
    pub fn version(&self) -> &str {
        &self.version
    }

    pub fn service_binary(&self) -> &Path {
        &self.service_binary
    }
}

pub struct RuntimeInstaller {
    paths: MachinePaths,
}

impl RuntimeInstaller {
    pub const fn new(paths: MachinePaths) -> Self {
        Self { paths }
    }

    pub fn deploy_with_health_check<F>(
        &self,
        payload: &FixedPayload,
        health_check: F,
    ) -> Result<InstalledRuntime, SetupError>
    where
        F: FnOnce(&Path) -> Result<(), SetupError>,
    {
        self.deploy(payload, health_check, false)
    }

    pub fn repair_with_health_check<F>(
        &self,
        payload: &FixedPayload,
        health_check: F,
    ) -> Result<InstalledRuntime, SetupError>
    where
        F: FnOnce(&Path) -> Result<(), SetupError>,
    {
        self.repair_current_with_health_check(payload, health_check)
    }

    pub fn repair_current_with_health_check<F>(
        &self,
        payload: &FixedPayload,
        health_check: F,
    ) -> Result<InstalledRuntime, SetupError>
    where
        F: FnOnce(&Path) -> Result<(), SetupError>,
    {
        let current = self.recover_interrupted_activation()?.ok_or_else(|| {
            SetupError::Runtime("no active protected runtime is installed".to_owned())
        })?;
        let bundled_version = payload.load()?.verified.version().to_owned();
        if current.version() != bundled_version {
            return Err(SetupError::Runtime(
                "repair cannot replace an outdated runtime; use upgrade".to_owned(),
            ));
        }
        self.deploy(payload, health_check, true)
    }

    pub fn restore_pinned_current_with_health_check<F>(
        &self,
        health_check: F,
    ) -> Result<InstalledRuntime, SetupError>
    where
        F: FnOnce(&Path) -> Result<(), SetupError>,
    {
        self.recover_interrupted_activation()?;
        let current = self.current()?.ok_or_else(|| {
            SetupError::Runtime("no active protected runtime is installed".to_owned())
        })?;
        let pinned = self.load_verified_migration_pins()?;
        if !pinned.contains_key(current.version()) {
            return Err(SetupError::Runtime(
                "the active runtime is not protected by a migration pin".to_owned(),
            ));
        }
        health_check(current.service_binary())?;
        Ok(current)
    }

    fn deploy<F>(
        &self,
        payload: &FixedPayload,
        health_check: F,
        replace_invalid: bool,
    ) -> Result<InstalledRuntime, SetupError>
    where
        F: FnOnce(&Path) -> Result<(), SetupError>,
    {
        self.recover_interrupted_activation()?;
        let payload = payload.load()?;
        let version = payload.verified.version().to_owned();
        let destination = self.paths.runtime_versions().join(&version);
        self.stage_payload(&payload, &destination, replace_invalid)?;
        self.write_runtime_receipt(&payload)?;

        let old_pointer = if self.paths.runtime_pointer().exists() {
            let bytes = read_bounded_regular_file(
                self.paths.runtime_pointer(),
                MAX_POINTER_BYTES,
                "active runtime pointer",
            )?;
            Some(validate_runtime_pointer(&bytes)?)
        } else {
            None
        };
        let pointer = serde_json::to_vec(&RuntimePointer {
            schema: 1,
            version: version.clone(),
        })?;
        let activated_pointer = RuntimePointer {
            schema: 1,
            version: version.clone(),
        };
        self.write_activation_journal(old_pointer.clone(), activated_pointer.clone())?;
        atomic_write(self.paths.runtime_pointer(), &pointer)?;

        let service_binary = destination.join("mactype-service.exe");
        let activation = ProfileRuntimeBridge::new(self.paths.clone())
            .materialize_active()
            .and_then(|_| health_check(&service_binary));
        if let Err(error) = activation {
            let mut rollback_failures = Vec::new();
            let pointer_restored = match self
                .restore_runtime_pointer(old_pointer.as_ref(), Some(&activated_pointer))
            {
                Ok(()) => true,
                Err(rollback_error) => {
                    rollback_failures.push(format!("pointer restoration failed: {rollback_error}"));
                    false
                }
            };
            if pointer_restored {
                if let Err(rollback_error) =
                    ProfileRuntimeBridge::new(self.paths.clone()).materialize_active()
                {
                    rollback_failures.push(format!(
                        "profile rematerialization failed: {rollback_error}"
                    ));
                }
            } else {
                rollback_failures.push(
                    "profile rematerialization was skipped because pointer ownership was unknown"
                        .to_owned(),
                );
            }
            if rollback_failures.is_empty() {
                if let Err(rollback_error) = self.remove_activation_journal() {
                    rollback_failures.push(format!(
                        "activation journal cleanup failed: {rollback_error}"
                    ));
                }
            }
            if rollback_failures.is_empty() {
                return Err(error);
            }
            return Err(SetupError::CleanupUnknown(format!(
                "runtime activation failed ({error}); rollback remained incomplete: {}. The activation journal was retained",
                rollback_failures.join("; ")
            )));
        }
        self.remove_activation_journal()?;
        if let Err(error) = self.finalize_retention(old_pointer.as_ref(), &version) {
            eprintln!("runtime retention deferred: {error}");
        }

        Ok(InstalledRuntime {
            version,
            service_binary,
        })
    }

    pub fn current(&self) -> Result<Option<InstalledRuntime>, SetupError> {
        if !self.paths.runtime_pointer().exists() {
            return Ok(None);
        }
        let bytes = read_bounded_regular_file(
            self.paths.runtime_pointer(),
            MAX_POINTER_BYTES,
            "active runtime pointer",
        )?;
        let pointer = validate_runtime_pointer(&bytes)?;
        let service_binary = self
            .paths
            .runtime_versions()
            .join(&pointer.version)
            .join("mactype-service.exe");
        reject_reparse_ancestors(&service_binary)?;
        if !service_binary.is_file() {
            return Err(SetupError::Runtime(
                "active service binary is missing".to_owned(),
            ));
        }
        Ok(Some(InstalledRuntime {
            version: pointer.version,
            service_binary,
        }))
    }

    pub fn inspect_current_stable(&self) -> Result<Option<InstalledRuntime>, SetupError> {
        let repair_journal = self.paths.service_root().join("runtime-repair.json");
        if self.paths.runtime_activation_journal().exists() || repair_journal.exists() {
            return Err(SetupError::Runtime(
                "a runtime transaction is pending".to_owned(),
            ));
        }
        let current = self.current()?;
        if let Some(current) = &current {
            let directory = current.service_binary().parent().ok_or_else(|| {
                SetupError::Runtime("active runtime has no generation directory".to_owned())
            })?;
            self.verify_runtime_generation_receipt(current.version(), directory)?;
        }
        if self.paths.runtime_activation_journal().exists() || repair_journal.exists() {
            return Err(SetupError::Runtime(
                "a runtime transaction is pending".to_owned(),
            ));
        }
        Ok(current)
    }
}
