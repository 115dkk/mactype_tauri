use std::ffi::c_void;
use std::io;
use std::ptr;

use windows_sys::Win32::System::Services::{
    ChangeServiceConfig2W, SC_ACTION, SC_ACTION_RESTART, SC_HANDLE, SERVICE_CONFIG_DESCRIPTION,
    SERVICE_CONFIG_FAILURE_ACTIONS, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, SERVICE_DESCRIPTIONW,
    SERVICE_FAILURE_ACTIONSW, SERVICE_FAILURE_ACTIONS_FLAG,
};

use super::super::{wide, DESCRIPTION};
use crate::SetupError;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ServiceMetadataOperation {
    Description(&'static str),
    RestartOnFailure {
        reset_period_seconds: u32,
        delays_milliseconds: [u32; 2],
    },
    IncludeNonCrashFailures(bool),
}

const fn service_metadata_plan() -> [ServiceMetadataOperation; 3] {
    [
        ServiceMetadataOperation::Description(DESCRIPTION),
        ServiceMetadataOperation::RestartOnFailure {
            reset_period_seconds: 86_400,
            delays_milliseconds: [5_000, 30_000],
        },
        ServiceMetadataOperation::IncludeNonCrashFailures(true),
    ]
}

trait MetadataConfigurationAdapter {
    fn apply(&mut self, operation: ServiceMetadataOperation) -> Result<(), SetupError>;
}

fn apply_metadata_configuration(
    adapter: &mut impl MetadataConfigurationAdapter,
) -> Result<(), SetupError> {
    for operation in service_metadata_plan() {
        adapter.apply(operation)?;
    }
    Ok(())
}

struct WindowsMetadataConfigurationAdapter {
    service: SC_HANDLE,
}

impl MetadataConfigurationAdapter for WindowsMetadataConfigurationAdapter {
    fn apply(&mut self, operation: ServiceMetadataOperation) -> Result<(), SetupError> {
        match operation {
            ServiceMetadataOperation::Description(text) => {
                let mut description_text = wide(text);
                let description = SERVICE_DESCRIPTIONW {
                    lpDescription: description_text.as_mut_ptr(),
                };
                self.change(
                    SERVICE_CONFIG_DESCRIPTION,
                    &raw const description as *const c_void,
                )
            }
            ServiceMetadataOperation::RestartOnFailure {
                reset_period_seconds,
                delays_milliseconds,
            } => {
                let mut actions = delays_milliseconds.map(|delay| SC_ACTION {
                    Type: SC_ACTION_RESTART,
                    Delay: delay,
                });
                let failure = SERVICE_FAILURE_ACTIONSW {
                    dwResetPeriod: reset_period_seconds,
                    lpRebootMsg: ptr::null_mut(),
                    lpCommand: ptr::null_mut(),
                    cActions: actions.len() as u32,
                    lpsaActions: actions.as_mut_ptr(),
                };
                self.change(
                    SERVICE_CONFIG_FAILURE_ACTIONS,
                    &raw const failure as *const c_void,
                )
            }
            ServiceMetadataOperation::IncludeNonCrashFailures(enabled) => {
                let failure_flag = SERVICE_FAILURE_ACTIONS_FLAG {
                    fFailureActionsOnNonCrashFailures: i32::from(enabled),
                };
                self.change(
                    SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,
                    &raw const failure_flag as *const c_void,
                )
            }
        }
    }
}

impl WindowsMetadataConfigurationAdapter {
    fn change(&self, level: u32, data: *const c_void) -> Result<(), SetupError> {
        if unsafe { ChangeServiceConfig2W(self.service, level, data) } == 0 {
            return Err(SetupError::Io(io::Error::last_os_error()));
        }
        Ok(())
    }
}

pub(in crate::windows::scm) fn configure_metadata(service: SC_HANDLE) -> Result<(), SetupError> {
    apply_metadata_configuration(&mut WindowsMetadataConfigurationAdapter { service })
}

#[cfg(test)]
mod tests {
    use super::{
        apply_metadata_configuration, service_metadata_plan, MetadataConfigurationAdapter,
        ServiceMetadataOperation,
    };
    use crate::SetupError;

    #[derive(Default)]
    struct RecordingMetadataAdapter {
        attempted: Vec<ServiceMetadataOperation>,
        fail_at: Option<usize>,
    }

    impl MetadataConfigurationAdapter for RecordingMetadataAdapter {
        fn apply(&mut self, operation: ServiceMetadataOperation) -> Result<(), SetupError> {
            let index = self.attempted.len();
            self.attempted.push(operation);
            if self.fail_at == Some(index) {
                return Err(SetupError::Runtime(format!(
                    "metadata operation {index} failed"
                )));
            }
            Ok(())
        }
    }

    #[test]
    fn service_recovery_contract_restarts_after_five_and_thirty_seconds_for_non_crash_failures() {
        assert_eq!(
            service_metadata_plan(),
            [
                ServiceMetadataOperation::Description(
                    "Runs the open MacType machine integration runtime."
                ),
                ServiceMetadataOperation::RestartOnFailure {
                    reset_period_seconds: 86_400,
                    delays_milliseconds: [5_000, 30_000],
                },
                ServiceMetadataOperation::IncludeNonCrashFailures(true),
            ]
        );
    }

    #[test]
    fn metadata_configuration_applies_the_complete_recovery_contract_in_order() {
        let mut adapter = RecordingMetadataAdapter::default();

        apply_metadata_configuration(&mut adapter).unwrap();

        assert_eq!(adapter.attempted, service_metadata_plan());
    }

    #[test]
    fn every_metadata_configuration_failure_is_returned_immediately() {
        for fail_at in 0..service_metadata_plan().len() {
            let mut adapter = RecordingMetadataAdapter {
                attempted: Vec::new(),
                fail_at: Some(fail_at),
            };

            let error = apply_metadata_configuration(&mut adapter).unwrap_err();

            assert!(error
                .to_string()
                .contains(&format!("metadata operation {fail_at} failed")));
            assert_eq!(adapter.attempted, service_metadata_plan()[..=fail_at]);
        }
    }
}
