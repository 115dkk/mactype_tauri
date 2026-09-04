use std::path::Path;

use mactype_service_platform::{ServiceAccess, ServiceState};

use super::configuration::{
    observed_configuration, quoted_image_path, service_configuration_matches_owned_contract,
};
use super::ServiceManager;
use crate::{ConflictObservation, OpenServiceObservation, SetupError};

impl ServiceManager {
    pub fn observe_fixed_service(&self) -> OpenServiceObservation {
        let service = match self.open_service(ServiceAccess::QueryStatusAndConfig) {
            Ok(Some(service)) => service,
            Ok(None) => return OpenServiceObservation::Absent,
            Err(_) => return OpenServiceObservation::Unknown,
        };
        let config = match service.config() {
            Ok(config) => config,
            Err(_) => return OpenServiceObservation::Unknown,
        };
        if !service_configuration_matches_owned_contract(
            &self.protected_root,
            &observed_configuration(&config),
        ) {
            return OpenServiceObservation::Foreign;
        }
        match service.status().map(|status| status.state) {
            Ok(ServiceState::Stopped) => OpenServiceObservation::OwnedStopped,
            Ok(ServiceState::Running) => OpenServiceObservation::OwnedRunning,
            _ => OpenServiceObservation::Unknown,
        }
    }

    pub fn observe_legacy_service(&self) -> ConflictObservation {
        match self.open_named_service("MacType", ServiceAccess::QueryStatus) {
            Ok(Some(_)) => ConflictObservation::Detected,
            Ok(None) => ConflictObservation::Clear,
            Err(_) => ConflictObservation::Unknown,
        }
    }

    pub fn owned_service_points_to(&self, expected_binary: &Path) -> Result<bool, SetupError> {
        let Some(service) = self.open_service(ServiceAccess::QueryConfig)? else {
            return Ok(false);
        };
        self.ensure_owned(&service)?;
        let config = service.config()?;
        Ok(config
            .image_path
            .eq_ignore_ascii_case(&quoted_image_path(expected_binary)?))
    }
}
