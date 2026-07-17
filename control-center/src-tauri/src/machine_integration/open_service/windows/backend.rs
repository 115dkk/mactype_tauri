use super::*;
use crate::machine_integration::legacy_migration;

#[derive(Default)]
pub(in crate::machine_integration::open_service) struct SystemMigrationBackend {
    removal_digest: Option<String>,
    published_profile: Option<Vec<u8>>,
}

impl MigrationBackend for SystemMigrationBackend {
    type OpenServiceSnapshot = SystemOpenServiceSnapshot;

    fn prepare_legacy_backup(&mut self) -> Result<(), String> {
        legacy_migration::prepare_backup().map(|_| ())
    }

    fn legacy_backup_is_valid(&mut self) -> bool {
        legacy_migration::backup_is_valid()
    }

    fn capture_open_service(&mut self) -> Result<Self::OpenServiceSnapshot, String> {
        capture_open_service_snapshot()
    }

    fn stop_legacy(&mut self) -> Result<(), String> {
        legacy_migration::stop_legacy().map(|_| ())
    }

    fn publish_profile(
        &mut self,
        snapshot: &Self::OpenServiceSnapshot,
        profile: &[u8],
    ) -> Result<(), String> {
        ensure_open_service_unchanged(snapshot)?;
        self.published_profile = Some(profile.to_vec());
        let operation = (|| {
            let state = query();
            if state.runtime == RuntimeState::Running {
                run_setup(SystemServiceAction::Stop, None)?;
            }
            run_setup(SystemServiceAction::PublishProfile, Some(profile))
        })();
        combine_mutation_recording(operation, record_protected_mutations(snapshot, profile))
    }

    fn activate_open_service(
        &mut self,
        snapshot: &Self::OpenServiceSnapshot,
    ) -> Result<(), String> {
        ensure_open_service_unchanged(snapshot)?;
        let profile = self
            .published_profile
            .as_deref()
            .ok_or_else(|| "migration activation has no published profile receipt".to_owned())?;
        let operation = (|| {
            for action in migration_activation_actions(snapshot.status.installation)? {
                run_setup(*action, None)?;
            }
            Ok(())
        })();
        combine_mutation_recording(operation, record_protected_mutations(snapshot, profile))
    }

    fn strict_ready(&mut self, expected_digest: &str) -> Result<bool, String> {
        Ok(query().system_injection_active(Some(expected_digest)))
    }

    fn verify_injection_smoke(&mut self, expected_digest: &str) -> Result<bool, String> {
        system_injection_smoke(expected_digest)
    }

    fn complete_migration(&mut self, snapshot: &Self::OpenServiceSnapshot) -> Result<(), String> {
        release_migration_runtime_pin(snapshot)
    }

    fn rollback_open_service(
        &mut self,
        snapshot: &Self::OpenServiceSnapshot,
    ) -> Result<(), String> {
        rollback_open_service_snapshot(snapshot)
    }

    fn restore_legacy(&mut self) -> Result<(), String> {
        legacy_migration::rollback().map(|_| ())
    }

    fn removal_verification(
        &mut self,
        expected_digest: &str,
    ) -> Result<MigrationVerification, String> {
        let verification = system_removal_verification(expected_digest)?;
        self.removal_digest = verification
            .permits_removal()
            .then(|| expected_digest.to_owned());
        Ok(verification)
    }

    fn remove_legacy(&mut self) -> Result<(), String> {
        let expected = self
            .removal_digest
            .as_deref()
            .ok_or_else(|| "legacy removal has no verified profile digest".to_owned())?;
        let verification = system_removal_verification(expected)?;
        if !verification.permits_removal() {
            return Err("legacy removal verification changed before SCM deletion".to_owned());
        }
        legacy_migration::remove_after_verified(legacy_migration::RemovalVerification {
            new_service_ready: verification.scm_running_ready,
            active_digest_match: verification.active_digest_match,
            backup_valid: legacy_migration::backup_is_valid(),
        })
        .map(|_| ())
    }
}
