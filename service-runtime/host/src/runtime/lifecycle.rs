use mactype_service_contract::{
    HealthReport, HealthState, InjectionTelemetry, ReadinessReport, StructuredServiceError,
    HEALTH_PROTOCOL_VERSION,
};

use super::{
    HealthPublisher, HostError, RuntimeHealthReporter, RuntimeInitializer, ServiceRuntime,
    StopSignal,
};
use crate::{ServiceStatus, StatusReporter};

const ERROR_SERVICE_SPECIFIC_ERROR: u32 = 1066;

impl ServiceRuntime<'_> {
    pub fn run(
        &self,
        status: &dyn StatusReporter,
        health: &dyn HealthPublisher,
        initializer: &dyn RuntimeInitializer,
        stop: &dyn StopSignal,
    ) -> Result<(), HostError> {
        if let Err(error) = status.report(ServiceStatus::start_pending(1, 10_000)) {
            return Err(self.report_io_failure(
                status,
                health,
                "start-pending-report-failed",
                error,
            ));
        }
        if let Err(error) = health.publish(&HealthReport {
            protocol_version: HEALTH_PROTOCOL_VERSION,
            service_version: self.service_version.to_owned(),
            health: HealthState::Initializing,
            active_profile_digest: None,
            readiness: ReadinessReport::initializing(),
            injection: InjectionTelemetry::default(),
            last_error: None,
        }) {
            return Err(self.report_io_failure(
                status,
                health,
                "initializing-health-publish-failed",
                error,
            ));
        }

        let mut initialized = match initializer.initialize() {
            Ok(initialized) => initialized,
            Err(error) => {
                self.report_failure(status, health, &error);
                return Err(HostError::Runtime(error));
            }
        };

        if let Err(error) = status.report(ServiceStatus::running()) {
            return Err(self.report_io_failure(
                status,
                health,
                "running-status-report-failed",
                error,
            ));
        }
        let ready = HealthReport {
            protocol_version: HEALTH_PROTOCOL_VERSION,
            service_version: self.service_version.to_owned(),
            health: HealthState::Ready,
            active_profile_digest: initialized.active_profile_digest.clone(),
            readiness: initialized.readiness.clone(),
            injection: InjectionTelemetry::default(),
            last_error: None,
        };
        if ready.validate().is_err() {
            let error = StructuredServiceError {
                code: "readiness-incomplete".to_owned(),
                message: "required runtime components are not ready".to_owned(),
                win32_error: None,
            };
            self.report_failure(status, health, &error);
            return Err(HostError::Runtime(error));
        }
        if let Err(publish_error) = health.publish(&ready) {
            let error = StructuredServiceError {
                code: "health-publish-failed".to_owned(),
                message: publish_error.to_string(),
                win32_error: publish_error.raw_os_error().map(|code| code as u32),
            };
            self.report_failure(status, health, &error);
            return Err(HostError::Runtime(error));
        }

        let runtime_health = RuntimeHealthAdapter {
            publisher: health,
            service_version: self.service_version,
            active_profile_digest: initialized.active_profile_digest.clone(),
        };
        let wait_result = match initialized.driver.as_mut() {
            Some(driver) => driver.run(stop, &runtime_health),
            None => stop.wait(),
        };
        if let Err(error) = wait_result {
            self.report_failure(status, health, &error);
            return Err(HostError::Runtime(error));
        }

        if let Err(error) = status.report(ServiceStatus::stopped()) {
            return Err(self.report_io_failure(
                status,
                health,
                "stopped-status-report-failed",
                error,
            ));
        }
        Ok(())
    }

    fn report_io_failure(
        &self,
        status: &dyn StatusReporter,
        health: &dyn HealthPublisher,
        code: &str,
        error: std::io::Error,
    ) -> HostError {
        let structured = StructuredServiceError {
            code: code.to_owned(),
            message: error.to_string(),
            win32_error: error.raw_os_error().map(|value| value as u32),
        };
        self.report_failure(status, health, &structured);
        HostError::Io(error)
    }

    fn report_failure(
        &self,
        status: &dyn StatusReporter,
        health: &dyn HealthPublisher,
        error: &StructuredServiceError,
    ) {
        let _ = health.publish(&HealthReport {
            protocol_version: HEALTH_PROTOCOL_VERSION,
            service_version: self.service_version.to_owned(),
            health: HealthState::Failed,
            active_profile_digest: None,
            readiness: ReadinessReport::initializing(),
            injection: InjectionTelemetry::default(),
            last_error: Some(error.clone()),
        });
        let _ = status.report(ServiceStatus::stopped_with_error(
            ERROR_SERVICE_SPECIFIC_ERROR,
            1,
        ));
    }
}

struct RuntimeHealthAdapter<'a> {
    publisher: &'a dyn HealthPublisher,
    service_version: &'a str,
    active_profile_digest: Option<String>,
}

impl RuntimeHealthReporter for RuntimeHealthAdapter<'_> {
    fn report(
        &self,
        health: HealthState,
        readiness: ReadinessReport,
        injection: InjectionTelemetry,
        last_error: Option<StructuredServiceError>,
    ) -> Result<(), StructuredServiceError> {
        let report = HealthReport {
            protocol_version: HEALTH_PROTOCOL_VERSION,
            service_version: self.service_version.to_owned(),
            health,
            active_profile_digest: self.active_profile_digest.clone(),
            readiness,
            injection,
            last_error,
        };
        report.validate().map_err(|_| StructuredServiceError {
            code: "runtime-health-invalid".to_owned(),
            message: "the runtime attempted to publish an invalid health report".to_owned(),
            win32_error: None,
        })?;
        self.publisher
            .publish(&report)
            .map_err(|error| StructuredServiceError {
                code: "health-publish-failed".to_owned(),
                message: error.to_string(),
                win32_error: error.raw_os_error().map(|code| code as u32),
            })
    }
}
