use super::*;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use std::thread;

use windows_sys::Win32::System::JobObjects::{
    JOB_OBJECT_LIMIT_ACTIVE_PROCESS, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
};

use crate::HelperLaunchStage;

static STOP_DURING_HELPER: AtomicBool = AtomicBool::new(false);
static HELPER_TEST_LOCK: Mutex<()> = Mutex::new(());

fn stop_during_helper() -> bool {
    STOP_DURING_HELPER.load(Ordering::Acquire)
}

fn current_process_invocation(
    executable: std::path::PathBuf,
    timeout: Duration,
) -> HelperInvocation {
    let process = OwnedHandle::new(unsafe {
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, std::process::id())
    })
    .unwrap();
    HelperInvocation {
        executable,
        target: crate::ProcessIdentity {
            pid: std::process::id(),
            creation_time: process_creation_time(process.get()).unwrap(),
            session_id: 1,
            architecture: crate::ProcessArchitecture::X64,
            protected: false,
            critical: false,
        },
        generation_id: "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
            .to_owned(),
        timeout,
    }
}

#[test]
fn helper_job_enforces_single_process_and_kill_on_close() {
    let job = JobObject::new().unwrap();
    let limits = job.query_limits().unwrap();
    assert_eq!(limits.BasicLimitInformation.ActiveProcessLimit, 1);
    assert_ne!(
        limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS,
        0
    );
    assert_ne!(
        limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
        0
    );
}

#[test]
fn in_flight_helper_is_cancelled_without_waiting_for_its_twenty_second_timeout() {
    let _guard = HELPER_TEST_LOCK.lock().unwrap();
    STOP_DURING_HELPER.store(false, Ordering::Release);
    LAST_TEST_CHILD_PID.store(0, Ordering::Release);
    let launcher = WindowsHelperLauncher::new(stop_during_helper);
    let executable =
        std::path::PathBuf::from(r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe");
    let invocation = current_process_invocation(executable, Duration::from_secs(20));
    let stop = thread::spawn(|| {
        thread::sleep(Duration::from_millis(100));
        STOP_DURING_HELPER.store(true, Ordering::Release);
    });
    let started = Instant::now();
    let error = launcher
        .launch_process(&invocation, |_| {
            [
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                "Start-Sleep -Seconds 20; exit 0",
            ]
            .into_iter()
            .map(OsString::from)
            .collect()
        })
        .unwrap_err();
    stop.join().unwrap();
    STOP_DURING_HELPER.store(false, Ordering::Release);
    assert_eq!(error.kind(), io::ErrorKind::Interrupted);
    assert_eq!(error.stage(), HelperLaunchStage::AfterResume);
    assert!(started.elapsed() < Duration::from_secs(3));

    let pid = LAST_TEST_CHILD_PID.load(Ordering::Acquire);
    assert_ne!(pid, 0);
    let child = unsafe { OpenProcess(SYNCHRONIZE, 0, pid) };
    if !child.is_null() {
        let child = OwnedHandle::new(child).unwrap();
        assert_eq!(
            unsafe { WaitForSingleObject(child.get(), 0) },
            WAIT_OBJECT_0
        );
    }
}

#[test]
fn absolute_timeout_terminates_the_helper_job_without_an_orphan() {
    let _guard = HELPER_TEST_LOCK.lock().unwrap();
    STOP_DURING_HELPER.store(false, Ordering::Release);
    LAST_TEST_CHILD_PID.store(0, Ordering::Release);
    let launcher = WindowsHelperLauncher::new(stop_during_helper);
    let executable =
        std::path::PathBuf::from(r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe");
    let invocation = current_process_invocation(executable, Duration::from_millis(700));
    let started = Instant::now();
    let error = launcher
        .launch_process(&invocation, |_| {
            [
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                "Start-Sleep -Seconds 5; exit 0",
            ]
            .into_iter()
            .map(OsString::from)
            .collect()
        })
        .unwrap_err();
    assert_eq!(error.kind(), io::ErrorKind::TimedOut);
    assert_eq!(error.stage(), HelperLaunchStage::AfterResume);
    assert!(started.elapsed() >= Duration::from_millis(400));
    assert!(started.elapsed() < Duration::from_millis(750));

    let pid = LAST_TEST_CHILD_PID.load(Ordering::Acquire);
    assert_ne!(pid, 0);
    let child = unsafe { OpenProcess(SYNCHRONIZE, 0, pid) };
    if !child.is_null() {
        let child = OwnedHandle::new(child).unwrap();
        assert_eq!(
            unsafe { WaitForSingleObject(child.get(), 0) },
            WAIT_OBJECT_0
        );
    }
}

#[test]
fn closing_the_service_owned_job_terminates_a_running_helper() {
    let job = JobObject::new().unwrap();
    let executable =
        std::path::PathBuf::from(r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe");
    let arguments = [
        "-NoProfile",
        "-NonInteractive",
        "-Command",
        "Start-Sleep -Seconds 5; exit 0",
    ]
    .into_iter()
    .map(OsString::from)
    .collect::<Vec<_>>();
    let mut command = command_line(&executable, &arguments);
    let application = executable
        .as_os_str()
        .encode_wide()
        .chain(Some(0))
        .collect::<Vec<_>>();
    let startup = windows_sys::Win32::System::Threading::STARTUPINFOW {
        cb: size_of::<windows_sys::Win32::System::Threading::STARTUPINFOW>() as u32,
        ..Default::default()
    };
    let mut process = PROCESS_INFORMATION::default();
    assert_ne!(
        unsafe {
            CreateProcessW(
                application.as_ptr(),
                command.as_mut_ptr(),
                null(),
                null(),
                0,
                CREATE_NO_WINDOW | CREATE_SUSPENDED,
                null(),
                null(),
                &startup,
                &mut process,
            )
        },
        0
    );
    let child = OwnedHandle::new(process.hProcess).unwrap();
    let thread = OwnedHandle::new(process.hThread).unwrap();
    assert_ne!(
        unsafe { AssignProcessToJobObject(job.handle(), child.get()) },
        0
    );
    assert_ne!(unsafe { ResumeThread(thread.get()) }, u32::MAX);
    assert_eq!(unsafe { WaitForSingleObject(child.get(), 0) }, WAIT_TIMEOUT);

    drop(job);

    assert_eq!(
        unsafe { WaitForSingleObject(child.get(), 2_000) },
        WAIT_OBJECT_0
    );
}
