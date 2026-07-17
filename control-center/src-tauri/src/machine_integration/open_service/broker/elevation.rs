use super::{
    super::{
        profile_transfer::{
            profile_transfer_nonce_text, OwnedKernelHandle, ProfilePipeServer, PROFILE_PIPE_TIMEOUT,
        },
        SystemServiceAction, BROKER_SWITCH, PROFILE_TRANSFER_SWITCH,
    },
    path_guard::wide,
    process::{combine_broker_cleanup_error, terminate_broker_process},
};

const BROKER_TIMEOUT_MS: u32 = 5 * 60 * 1000;
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
    let transfer = profile_input.map(ProfilePipeServer::create).transpose()?;
    launch_elevated_broker(action, transfer)
}

fn launch_elevated_broker(
    action: SystemServiceAction,
    profile_transfer: Option<ProfilePipeServer>,
) -> Result<(), String> {
    if profile_transfer.is_some() != action.needs_profile_input() {
        return Err("the elevated broker has invalid profile transfer state".to_owned());
    }
    let executable = std::env::current_exe().map_err(|error| error.to_string())?;
    let executable = wide(executable.as_os_str());
    let verb = wide("runas");
    let parameter_text = match profile_transfer.as_ref() {
        Some(server) => format!(
            "{BROKER_SWITCH} {} {PROFILE_TRANSFER_SWITCH} {} {}",
            action.broker_verb(),
            server.token().server_pid,
            profile_transfer_nonce_text(&server.token().nonce)
        ),
        None => format!("{BROKER_SWITCH} {}", action.broker_verb()),
    };
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
    if exit_code == 0 {
        Ok(())
    } else {
        Err(format!(
            "elevated service broker failed with exit code {exit_code}"
        ))
    }
}
