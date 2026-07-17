use windows_sys::Win32::{
    Foundation::{GetLastError, HANDLE, WAIT_OBJECT_0, WAIT_TIMEOUT},
    System::Threading::{TerminateProcess, WaitForSingleObject},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(in crate::machine_integration::open_service) enum BrokerTermination {
    AlreadyExited,
    Terminated,
}

pub(in crate::machine_integration::open_service) trait BrokerProcessControl {
    fn wait(&mut self, process: HANDLE, timeout: u32) -> u32;
    fn terminate(&mut self, process: HANDLE, exit_code: u32) -> Result<(), u32>;
}

struct WindowsBrokerProcessControl;

impl BrokerProcessControl for WindowsBrokerProcessControl {
    fn wait(&mut self, process: HANDLE, timeout: u32) -> u32 {
        unsafe { WaitForSingleObject(process, timeout) }
    }

    fn terminate(&mut self, process: HANDLE, exit_code: u32) -> Result<(), u32> {
        if unsafe { TerminateProcess(process, exit_code) } == 0 {
            Err(unsafe { GetLastError() })
        } else {
            Ok(())
        }
    }
}

pub(in crate::machine_integration::open_service) fn terminate_broker_process_with(
    process: HANDLE,
    control: &mut impl BrokerProcessControl,
) -> Result<BrokerTermination, String> {
    let initial = control.wait(process, 0);
    if initial == WAIT_OBJECT_0 {
        return Ok(BrokerTermination::AlreadyExited);
    }
    if initial != WAIT_TIMEOUT {
        return Err(format!(
            "elevated broker cleanup is unknown: initial process wait failed with {initial}"
        ));
    }

    if let Err(error) = control.terminate(process, 21) {
        let after_failure = control.wait(process, 0);
        if after_failure == WAIT_OBJECT_0 {
            return Ok(BrokerTermination::AlreadyExited);
        }
        return Err(format!(
            "elevated broker cleanup is unknown: TerminateProcess failed with {error} and the process state is {after_failure}"
        ));
    }

    let confirmation = control.wait(process, 5_000);
    if confirmation == WAIT_OBJECT_0 {
        Ok(BrokerTermination::Terminated)
    } else if confirmation == WAIT_TIMEOUT {
        Err(
            "elevated broker cleanup is unknown: termination was not confirmed within 5000 ms"
                .to_owned(),
        )
    } else {
        Err(format!(
            "elevated broker cleanup is unknown: termination confirmation failed with {confirmation}"
        ))
    }
}

pub(super) fn terminate_broker_process(process: HANDLE) -> Result<BrokerTermination, String> {
    terminate_broker_process_with(process, &mut WindowsBrokerProcessControl)
}

pub(in crate::machine_integration::open_service) fn combine_broker_cleanup_error(
    operation_error: &str,
    cleanup: Result<BrokerTermination, String>,
) -> String {
    match cleanup {
        Ok(_) => operation_error.to_owned(),
        Err(cleanup_error) => format!("{operation_error}; {cleanup_error}"),
    }
}
