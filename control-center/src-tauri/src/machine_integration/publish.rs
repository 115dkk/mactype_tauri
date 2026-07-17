use super::{MachineAction, MachineBackend};
use crate::service_contract::SystemServiceStatus;

pub(crate) fn publish_profile_transaction_with(
    backend: &mut impl MachineBackend,
    profile: &[u8],
) -> Result<(), String> {
    if profile.is_empty() || profile.len() > mactype_service_contract::MAX_PROFILE_BYTES {
        return Err("the published profile payload is outside the allowed range".to_owned());
    }
    let before = backend.new_service_status();
    if before.backend == crate::service_contract::ServiceBackend::Foreign
        || !matches!(
            before.installation,
            crate::service_contract::InstallationState::Absent
                | crate::service_contract::InstallationState::Current
                | crate::service_contract::InstallationState::Outdated
        )
        || !matches!(
            before.runtime,
            crate::service_contract::RuntimeState::Running
                | crate::service_contract::RuntimeState::Stopped
        )
    {
        return Err("the new service is foreign, transitioning, or unsafe".to_owned());
    }

    if before.runtime == crate::service_contract::RuntimeState::Running {
        backend.execute(MachineAction::Stop, None)?;
    }
    if let Err(error) = backend.execute(MachineAction::PublishProfile, Some(profile)) {
        if before.runtime == crate::service_contract::RuntimeState::Running {
            if let Err(restart) = backend.execute(MachineAction::Start, None) {
                return Err(format!(
                    "{error}; machine integration cleanup is unknown because the prior service could not be restarted: {restart}"
                ));
            }
        }
        return Err(error);
    }

    let activation = match before.installation {
        crate::service_contract::InstallationState::Absent => backend
            .execute(MachineAction::Install, None)
            .and_then(|()| backend.execute(MachineAction::Start, None)),
        crate::service_contract::InstallationState::Outdated => backend
            .execute(MachineAction::Upgrade, None)
            .and_then(|()| backend.execute(MachineAction::Start, None)),
        crate::service_contract::InstallationState::Current => {
            backend.execute(MachineAction::Start, None)
        }
        _ => unreachable!("unsafe installation states were rejected"),
    };
    if let Err(error) = activation {
        return Err(combine_rollback_error(
            error,
            rollback_published_profile(backend, &before),
        ));
    }

    let expected = mactype_service_contract::GenerationId::from_profile_bytes(profile);
    let after = backend.new_service_status();
    if !after.system_injection_active(Some(expected.as_str())) {
        return Err(combine_rollback_error(
            "the new service did not become Ready with the published profile".to_owned(),
            rollback_published_profile(backend, &before),
        ));
    }
    Ok(())
}

fn combine_rollback_error(primary: String, rollback: Result<(), String>) -> String {
    match rollback {
        Ok(()) => primary,
        Err(cleanup) => {
            format!("{primary}; machine integration cleanup is unknown: {cleanup}")
        }
    }
}

fn rollback_published_profile(
    backend: &mut impl MachineBackend,
    before: &SystemServiceStatus,
) -> Result<(), String> {
    let mut failures = Vec::new();
    for (label, action) in [
        ("stop", MachineAction::Stop),
        ("profile rollback", MachineAction::Rollback),
    ] {
        if let Err(error) = backend.execute(action, None) {
            failures.push(format!("{label}: {error}"));
        }
    }
    match before.installation {
        crate::service_contract::InstallationState::Absent => {
            if let Err(error) = backend.execute(MachineAction::Remove, None) {
                failures.push(format!("remove newly installed service: {error}"));
            }
        }
        _ if before.runtime == crate::service_contract::RuntimeState::Running => {
            if let Err(error) = backend.execute(MachineAction::Start, None) {
                failures.push(format!("restart prior service: {error}"));
            }
        }
        _ => {}
    }
    if failures.is_empty() {
        Ok(())
    } else {
        Err(failures.join("; "))
    }
}
