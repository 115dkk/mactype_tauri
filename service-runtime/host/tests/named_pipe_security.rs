#![cfg(windows)]

use std::ptr;
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

use mactype_service_contract::HealthReport;
use mactype_service_host::{HealthPublisher, NamedPipeHealthPublisher, HEALTH_PIPE_SECURITY_SDDL};
use windows_sys::Win32::Foundation::{CloseHandle, INVALID_HANDLE_VALUE};
use windows_sys::Win32::Storage::FileSystem::{CreateFileW, FILE_GENERIC_READ, OPEN_EXISTING};

#[test]
fn health_pipe_acl_is_explicit_read_only_for_authenticated_users() {
    assert_eq!(
        HEALTH_PIPE_SECURITY_SDDL,
        "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)"
    );
    assert!(HEALTH_PIPE_SECURITY_SDDL.contains("(A;;GA;;;SY)"));
    assert!(HEALTH_PIPE_SECURITY_SDDL.contains("(A;;GA;;;BA)"));
    assert!(HEALTH_PIPE_SECURITY_SDDL.contains("(A;;GR;;;AU)"));
    assert!(!HEALTH_PIPE_SECURITY_SDDL.contains("(A;;GW;;;AU)"));
    assert!(!HEALTH_PIPE_SECURITY_SDDL.contains("(A;;GA;;;AU)"));
}

#[test]
fn stalled_health_client_cannot_block_service_shutdown() {
    let pipe_name = format!(
        r"\\.\pipe\MacTypeControlCenter.health.shutdown-test.{}",
        std::process::id()
    );
    let publisher = NamedPipeHealthPublisher::start(&pipe_name).unwrap();
    publisher
        .publish(&HealthReport::ready(
            "test",
            Some(
                "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                    .to_owned(),
            ),
        ))
        .unwrap();

    let wide_name = pipe_name.encode_utf16().chain(Some(0)).collect::<Vec<_>>();
    let client = unsafe {
        CreateFileW(
            wide_name.as_ptr(),
            FILE_GENERIC_READ,
            0,
            ptr::null(),
            OPEN_EXISTING,
            0,
            ptr::null_mut(),
        )
    };
    assert_ne!(client, INVALID_HANDLE_VALUE);
    thread::sleep(Duration::from_millis(250));

    let (finished_tx, finished_rx) = mpsc::channel();
    let started = Instant::now();
    let drop_worker = thread::spawn(move || {
        drop(publisher);
        let _ = finished_tx.send(());
    });
    let bounded = finished_rx.recv_timeout(Duration::from_secs(1)).is_ok();
    unsafe {
        CloseHandle(client);
    }
    drop_worker.join().unwrap();

    assert!(bounded, "stalled health client blocked publisher shutdown");
    assert!(started.elapsed() < Duration::from_secs(2));
}
