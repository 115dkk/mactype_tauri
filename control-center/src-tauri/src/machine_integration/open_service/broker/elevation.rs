use super::{
    super::{
        profile_transfer::{
            profile_transfer_nonce_text, BrokerResultPipeServer, OwnedKernelHandle,
            ProfilePipeServer, PROFILE_PIPE_TIMEOUT,
        },
        BrokerResultDisposition, SystemServiceAction, BROKER_SWITCH, BROKER_TRANSFER_SWITCH,
    },
    path_guard::wide,
    process::{combine_broker_cleanup_error, terminate_broker_process},
};

const BROKER_TIMEOUT_MS: u32 = 5 * 60 * 1000;
const BROKER_TIMEOUT: std::time::Duration =
    std::time::Duration::from_millis(BROKER_TIMEOUT_MS as u64);
use windows_sys::Win32::{
    Foundation::{GetLastError, ERROR_CANCELLED, WAIT_OBJECT_0, WAIT_TIMEOUT},
    System::Threading::{GetExitCodeProcess, GetProcessId, WaitForSingleObject},
    UI::Shell::{ShellExecuteExW, SEE_MASK_NOCLOSEPROCESS, SHELLEXECUTEINFOW},
};

pub(in crate::machine_integration::open_service) fn run_elevated(
    action: SystemServiceAction,
    profile_input: Option<&[u8]>,
) -> Result<(), String> {
    if profile_input.is_some() != action.needs_profile_input() {
        return Err("the elevated service action has an invalid profile payload".to_owned());
    }
    let result_transfer = BrokerResultPipeServer::create()?;
    let profile_transfer = profile_input
        .map(|profile| ProfilePipeServer::create_with_nonce(profile, result_transfer.token().nonce))
        .transpose()?;
    launch_elevated_broker(action, result_transfer, profile_transfer)
}

fn launch_elevated_broker(
    action: SystemServiceAction,
    result_transfer: BrokerResultPipeServer,
    profile_transfer: Option<ProfilePipeServer>,
) -> Result<(), String> {
    if profile_transfer.is_some() != action.needs_profile_input() {
        return Err("the elevated broker has invalid profile transfer state".to_owned());
    }
    let executable = std::env::current_exe().map_err(|error| error.to_string())?;
    let executable = wide(executable.as_os_str());
    let verb = wide("runas");
    let parameter_text = format!(
        "{BROKER_SWITCH} {} {BROKER_TRANSFER_SWITCH} {} {}",
        action.broker_verb(),
        result_transfer.token().server_pid,
        profile_transfer_nonce_text(&result_transfer.token().nonce)
    );
    let parameters = wide(parameter_text);
    let mut info = SHELLEXECUTEINFOW {
        cbSize: std::mem::size_of::<SHELLEXECUTEINFOW>() as u32,
        fMask: SEE_MASK_NOCLOSEPROCESS,
        lpVerb: verb.as_ptr(),
        lpFile: executable.as_ptr(),
        lpParameters: parameters.as_ptr(),
        nShow: 0,
        ..Default::default()
    };
    if unsafe { ShellExecuteExW(&mut info) } == 0 {
        let error = unsafe { GetLastError() };
        return if error == ERROR_CANCELLED {
            Err("administrator approval was cancelled".to_owned())
        } else {
            Err(format!("ShellExecuteExW failed with {error}"))
        };
    }
    if info.hProcess.is_null() {
        return Err("elevated service broker did not return a process handle".to_owned());
    }
    let process = OwnedKernelHandle(info.hProcess);
    let broker_pid = unsafe { GetProcessId(process.raw()) };
    if broker_pid == 0 {
        return Err(combine_broker_cleanup_error(
            "could not identify the elevated service broker process",
            terminate_broker_process(process.raw()),
        ));
    }
    if let Some(server) = profile_transfer {
        if let Err(error) = server.send_to(broker_pid, Some(process.raw()), PROFILE_PIPE_TIMEOUT) {
            return Err(combine_broker_cleanup_error(
                &error,
                terminate_broker_process(process.raw()),
            ));
        }
    }
    let broker_result =
        match result_transfer.receive_from(broker_pid, Some(process.raw()), BROKER_TIMEOUT) {
            Ok(result) => Some(result),
            Err(error) => {
                if let Some(exit_code) = finished_exit_code(process.raw()) {
                    return Err(broker_channel_failure(Some(exit_code), &error));
                }
                let cleanup = terminate_broker_process(process.raw());
                return Err(combine_broker_cleanup_error(
                    &broker_channel_failure(None, &error),
                    cleanup,
                ));
            }
        };
    let wait = unsafe { WaitForSingleObject(process.raw(), BROKER_TIMEOUT_MS) };
    if wait == WAIT_TIMEOUT {
        return Err(combine_broker_cleanup_error(
            "elevated service broker timed out",
            terminate_broker_process(process.raw()),
        ));
    }
    if wait != WAIT_OBJECT_0 {
        let error = format!("waiting for the elevated service broker failed with {wait}");
        return Err(combine_broker_cleanup_error(
            &error,
            terminate_broker_process(process.raw()),
        ));
    }
    let mut exit_code = 0;
    let ok = unsafe { GetExitCodeProcess(process.raw(), &mut exit_code) };
    if ok == 0 {
        return Err("could not read the elevated service broker exit code".to_owned());
    }
    match (exit_code, broker_result) {
        (0, Some(result)) if result.disposition == BrokerResultDisposition::Success => Ok(()),
        (0, Some(result)) => Err(format!(
            "elevated service broker reported {} after a successful exit: {}",
            result.stage, result.error_chain
        )),
        (code, Some(result)) if result.disposition != BrokerResultDisposition::Success => {
            Err(format!(
                "{}; elevated service broker exit code {code}",
                result.error_chain
            ))
        }
        (code, Some(_)) => Err(format!(
            "elevated service broker failed with exit code {code} after reporting success"
        )),
        (code, None) => Err(format!(
            "elevated service broker failed with exit code {code}"
        )),
    }
}

fn finished_exit_code(process: windows_sys::Win32::Foundation::HANDLE) -> Option<u32> {
    if unsafe { WaitForSingleObject(process, 0) } != WAIT_OBJECT_0 {
        return None;
    }
    let mut exit_code = 0;
    (unsafe { GetExitCodeProcess(process, &mut exit_code) } != 0).then_some(exit_code)
}

fn broker_channel_failure(exit_code: Option<u32>, channel_error: &str) -> String {
    exit_code.map_or_else(
        || format!("broker result channel failed: {channel_error}"),
        |code| {
            format!(
                "elevated service broker failed with exit code {code}; broker result channel failed: {channel_error}"
            )
        },
    )
}

#[cfg(test)]
mod tests {
    use super::broker_channel_failure;

    #[test]
    fn missing_child_detail_preserves_exit_code_and_channel_failure() {
        let error = broker_channel_failure(
            Some(21),
            "the broker result pipe closed before sending a complete frame",
        );

        assert!(error.contains("exit code 21"));
        assert!(error.contains("broker result channel failed"));
        assert!(error.contains("closed before sending a complete frame"));
    }
}
