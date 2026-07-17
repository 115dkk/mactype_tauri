mod profile;
mod service;

use mactype_service_contract::{BrokerCommand, MachinePaths};

use super::{known_folders, machine_lock, scm};
use crate::{ProfileStore, RuntimeInstaller, SetupError};

struct BrokerContext {
    paths: MachinePaths,
    manager: scm::ServiceManager,
}

pub(super) fn run(
    command: BrokerCommand,
    profile_input: Option<&[u8]>,
) -> Result<String, SetupError> {
    let _setup_lock = machine_lock::MachineSetupLock::acquire()?;
    let paths = known_folders::machine_paths()?;
    let manager = scm::ServiceManager::connect(paths.service_root().to_owned())?;
    RuntimeInstaller::new(paths.clone()).recover_interrupted_activation()?;
    ProfileStore::new(paths.clone()).recover_interrupted_activation()?;
    let context = BrokerContext { paths, manager };

    match command {
        BrokerCommand::Install => service::install(&context),
        BrokerCommand::Upgrade => service::upgrade(&context),
        BrokerCommand::Repair => service::repair(&context),
        BrokerCommand::Remove => service::remove(&context),
        BrokerCommand::Start => service::start(&context),
        BrokerCommand::Stop => service::stop(&context),
        BrokerCommand::RestoreRuntime => service::restore_runtime(&context),
        BrokerCommand::PublishProfile => profile::publish(&context, profile_input),
        BrokerCommand::Rollback => profile::rollback(&context),
        BrokerCommand::MigrateFromLegacy => Err(SetupError::Runtime(
            "legacy migration requires the separately verified migration workflow".to_owned(),
        )),
    }
}

#[cfg(test)]
mod tests {
    use std::sync::mpsc;
    use std::time::Duration;

    use super::*;

    #[test]
    fn machine_setup_lock_rejects_a_concurrent_writer_with_a_bounded_wait() {
        let (acquired_tx, acquired_rx) = mpsc::channel();
        let (release_tx, release_rx) = mpsc::channel();
        let holder = std::thread::spawn(move || {
            let _guard =
                match machine_lock::MachineSetupLock::acquire_for_test(Duration::from_secs(2)) {
                    Ok(guard) => guard,
                    Err(SetupError::Io(error)) if error.raw_os_error() == Some(1307) => {
                        acquired_tx.send(false).unwrap();
                        return;
                    }
                    Err(error) => panic!("failed to acquire the first setup lock: {error}"),
                };
            acquired_tx.send(true).unwrap();
            release_rx.recv().unwrap();
        });
        if !acquired_rx.recv().unwrap() {
            holder.join().unwrap();
            return;
        }

        let error = machine_lock::MachineSetupLock::acquire_for_test(Duration::from_millis(25))
            .unwrap_err();
        assert!(
            error
                .to_string()
                .contains("another machine setup operation"),
            "unexpected lock error: {error}"
        );

        release_tx.send(()).unwrap();
        holder.join().unwrap();
    }
}
