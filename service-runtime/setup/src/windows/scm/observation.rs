use std::path::Path;

use windows_sys::Win32::System::Services::{
    SERVICE_QUERY_CONFIG, SERVICE_QUERY_STATUS, SERVICE_RUNNING, SERVICE_STOPPED,
};

use super::configuration::{
    query_config, quoted_image_path, service_configuration_matches_owned_contract,
};
use super::lifecycle::query_status;
use super::ServiceManager;
use crate::{ConflictObservation, OpenServiceObservation, SetupError};

impl ServiceManager {
    pub fn observe_fixed_service(&self) -> OpenServiceObservation {
        let service = match self.open_service(SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG) {
            Ok(Some(service)) => service,
            Ok(None) => return OpenServiceObservation::Absent,
            Err(_) => return OpenServiceObservation::Unknown,
        };
        let config = match query_config(service.0) {
            Ok(config) => config,
            Err(_) => return OpenServiceObservation::Unknown,
        };
        if !service_configuration_matches_owned_contract(&self.protected_root, &config.observed()) {
            return OpenServiceObservation::Foreign;
        }
        match query_status(service.0).map(|status| status.dwCurrentState) {
            Ok(SERVICE_STOPPED) => OpenServiceObservation::OwnedStopped,
            Ok(SERVICE_RUNNING) => OpenServiceObservation::OwnedRunning,
            _ => OpenServiceObservation::Unknown,
        }
    }

    pub fn observe_legacy_service(&self) -> ConflictObservation {
        match self.open_named_service("MacType", SERVICE_QUERY_STATUS) {
            Ok(Some(_)) => ConflictObservation::Detected,
            Ok(None) => ConflictObservation::Clear,
            Err(_) => ConflictObservation::Unknown,
        }
    }

    pub fn owned_service_points_to(&self, expected_binary: &Path) -> Result<bool, SetupError> {
        let Some(service) = self.open_service(SERVICE_QUERY_CONFIG)? else {
            return Ok(false);
        };
        self.ensure_owned(&service)?;
        let config = query_config(service.0)?;
        Ok(config
            .image_path
            .eq_ignore_ascii_case(&quoted_image_path(expected_binary)?))
    }
}
