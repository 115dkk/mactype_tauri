mod acl;
mod appinit;
mod broker;
mod installer;
mod known_folders;
mod machine_lock;
pub(crate) mod scm;

use mactype_service_contract::BrokerCommand;

use crate::SetupError;

pub fn run_installer_bootstrap() -> Result<String, SetupError> {
    installer::run_bootstrap().map(|outcome| outcome.to_json("bootstrap-install"))
}

pub fn run_owned_uninstall() -> Result<String, SetupError> {
    installer::run_uninstall().map(|outcome| outcome.to_json())
}

pub fn run(command: BrokerCommand, profile_input: Option<&[u8]>) -> Result<String, SetupError> {
    broker::run(command, profile_input)
}
