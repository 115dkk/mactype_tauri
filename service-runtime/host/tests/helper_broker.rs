use std::ffi::OsString;
use std::fs;
use std::sync::Mutex;

use mactype_service_contract::{
    GenerationId, GenerationPointer, MachinePaths, ProfileDigest, RendererActivationDisposition,
    RendererActivationEvidenceV1, RendererActivationReason, RendererArchitecture,
    RendererCapability, RendererModuleLoad, RendererProcessIdentity, RendererRuntimeBinding,
    RuntimeGenerationId,
};

fn test_binding() -> RendererRuntimeBinding {
    RendererRuntimeBinding::new(
        RuntimeGenerationId::parse(
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        )
        .unwrap(),
        ProfileDigest::parse(
            "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        )
        .unwrap(),
    )
}
use mactype_service_host::{
    BrokerDisposition, BrokerResult, FixedHelperBroker, HelperInvocation, HelperLaunchError,
    HelperLaunchStage, HelperLauncher, HelperOutput, InjectionBroker, InjectionRequest,
    ProcessArchitecture, ProcessIdentity, ProtectedRendererRuntime,
};

fn runtime() -> (tempfile::TempDir, ProtectedRendererRuntime) {
    let base = tempfile::tempdir_in(std::env::current_dir().unwrap()).unwrap();
    let program_files = base.path().join("Program Files");
    let program_data = base.path().join("ProgramData");
    fs::create_dir_all(&program_files).unwrap();
    fs::create_dir_all(&program_data).unwrap();
    let paths = MachinePaths::from_trusted_os_roots(&program_files, &program_data).unwrap();
    let generation = paths.runtime_versions().join("0.2.0");
    fs::create_dir_all(&generation).unwrap();
    for name in [
        "mactype-service.exe",
        "mactype-injector32.exe",
        "mactype-injector64.exe",
        "MacType.dll",
        "MacType64.dll",
    ] {
        fs::write(generation.join(name), name.as_bytes()).unwrap();
    }
    let profile = b"[General]\r\nHintingMode=0\r\n";
    fs::write(generation.join("MacType.ini"), profile).unwrap();
    fs::create_dir_all(paths.runtime_pointer().parent().unwrap()).unwrap();
    fs::write(
        paths.runtime_pointer(),
        br#"{"schema":1,"version":"0.2.0"}"#,
    )
    .unwrap();
    let profile_generation = GenerationId::from_profile_bytes(profile);
    let profile_root = paths
        .profile_generations()
        .join(profile_generation.directory_name());
    fs::create_dir_all(&profile_root).unwrap();
    fs::write(profile_root.join("profile.ini"), profile).unwrap();
    fs::create_dir_all(paths.active_profile().parent().unwrap()).unwrap();
    fs::write(
        paths.active_profile(),
        serde_json::to_vec(&GenerationPointer::new(profile_generation)).unwrap(),
    )
    .unwrap();
    let runtime = ProtectedRendererRuntime::load(paths).unwrap();
    (base, runtime)
}

#[derive(Default)]
struct RecordingLauncher {
    invocations: Mutex<Vec<HelperInvocation>>,
}

struct StaticLauncher {
    output: Mutex<Option<HelperOutput>>,
}

impl HelperLauncher for StaticLauncher {
    fn launch(&self, _invocation: &HelperInvocation) -> Result<HelperOutput, HelperLaunchError> {
        Ok(self.output.lock().unwrap().take().unwrap())
    }
}

impl HelperLauncher for RecordingLauncher {
    fn launch(&self, invocation: &HelperInvocation) -> Result<HelperOutput, HelperLaunchError> {
        self.invocations.lock().unwrap().push(invocation.clone());
        Ok(HelperOutput {
            exit_code: 0,
            stdout: format!(
                "{{\"schemaVersion\":2,\"status\":\"injected\",\"code\":\"renderer-active\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{}\",\"module\":\"MacType.dll\",\"windowsError\":0,\"cleanupComplete\":true,\"rendererEvidence\":\"{}\"}}",
                invocation.binding.runtime_generation_id(),
                active_evidence_hex(invocation),
            )
            .into_bytes(),
        })
    }
}

#[test]
fn fixed_helper_broker_selects_architecture_and_emits_only_the_strict_cli_contract() {
    let (_base, runtime) = runtime();
    let assets = runtime.assets();
    let launcher = RecordingLauncher::default();
    let broker = FixedHelperBroker::new(&runtime, &launcher);
    let request = InjectionRequest {
        identity: ProcessIdentity {
            pid: 42,
            creation_time: 133_967_890_123_456_789,
            session_id: 2,
            architecture: ProcessArchitecture::X86,
        },
        binding: runtime.binding(),
    };

    let result = broker.inject(&request);

    assert_eq!(result.disposition, BrokerDisposition::Injected);
    let invocations = launcher.invocations.lock().unwrap();
    assert_eq!(invocations.len(), 1);
    assert_eq!(invocations[0].timeout, std::time::Duration::from_secs(20));
    assert_eq!(invocations[0].executable, assets.injector32());
    assert_eq!(invocations[0].target, request.identity);
    assert_eq!(
        invocations[0].arguments_for_process_handle(4096),
        [
            OsString::from("--process-handle"),
            OsString::from("4096"),
            OsString::from("--pid"),
            OsString::from("42"),
            OsString::from("--creation-time"),
            OsString::from("133967890123456789"),
            OsString::from("--session-id"),
            OsString::from("2"),
            OsString::from("--generation-id"),
            OsString::from(assets.generation_id().as_str()),
            OsString::from("--profile-digest"),
            OsString::from(runtime.binding().profile_digest().as_str()),
        ]
    );
}

struct ErrorLauncher {
    kind: std::io::ErrorKind,
    stage: HelperLaunchStage,
}

impl HelperLauncher for ErrorLauncher {
    fn launch(&self, _invocation: &HelperInvocation) -> Result<HelperOutput, HelperLaunchError> {
        Err(HelperLaunchError::new(
            self.stage,
            std::io::Error::new(self.kind, "synthetic launcher failure"),
        ))
    }
}

#[test]
fn interrupted_helper_is_a_service_stop_cancellation() {
    let (_base, runtime) = runtime();
    let broker = FixedHelperBroker::new(
        &runtime,
        ErrorLauncher {
            kind: std::io::ErrorKind::Interrupted,
            stage: HelperLaunchStage::BeforeResume,
        },
    );
    let result = broker.inject(&InjectionRequest {
        identity: ProcessIdentity {
            pid: 42,
            creation_time: 100,
            session_id: 2,
            architecture: ProcessArchitecture::X64,
        },
        binding: runtime.binding(),
    });

    assert_eq!(result.disposition, BrokerDisposition::Cancelled);
    assert_eq!(result.code, "helper-cancelled-service-stop");
}

#[test]
fn before_resume_launch_failure_never_claims_unknown_target_cleanup() {
    let (_base, runtime) = runtime();
    let broker = FixedHelperBroker::new(
        &runtime,
        ErrorLauncher {
            kind: std::io::ErrorKind::InvalidInput,
            stage: HelperLaunchStage::BeforeResume,
        },
    );
    let result = broker.inject(&InjectionRequest {
        identity: ProcessIdentity {
            pid: 42,
            creation_time: 100,
            session_id: 2,
            architecture: ProcessArchitecture::X64,
        },
        binding: runtime.binding(),
    });

    assert_eq!(result.disposition, BrokerDisposition::Rejected);
    assert_eq!(result.code, "helper-launch-failed-before-resume");
    assert!(!result.code.ends_with("-cleanup-unknown"));
}

#[test]
fn post_resume_service_stop_is_terminal_cleanup_unknown() {
    let (_base, runtime) = runtime();
    let broker = FixedHelperBroker::new(
        &runtime,
        ErrorLauncher {
            kind: std::io::ErrorKind::Interrupted,
            stage: HelperLaunchStage::AfterResume,
        },
    );
    let result = broker.inject(&InjectionRequest {
        identity: ProcessIdentity {
            pid: 42,
            creation_time: 100,
            session_id: 2,
            architecture: ProcessArchitecture::X64,
        },
        binding: runtime.binding(),
    });

    assert_eq!(result.disposition, BrokerDisposition::UncertainCleanup);
    assert_eq!(result.code, "helper-service-stop-cleanup-unknown");
}

#[test]
fn absolute_helper_timeout_is_terminal_cleanup_unknown() {
    let (_base, runtime) = runtime();
    let broker = FixedHelperBroker::new(
        &runtime,
        ErrorLauncher {
            kind: std::io::ErrorKind::TimedOut,
            stage: HelperLaunchStage::AfterResume,
        },
    );
    let result = broker.inject(&InjectionRequest {
        identity: ProcessIdentity {
            pid: 42,
            creation_time: 100,
            session_id: 2,
            architecture: ProcessArchitecture::X64,
        },
        binding: runtime.binding(),
    });

    assert_eq!(result.disposition, BrokerDisposition::UncertainCleanup);
    assert_eq!(result.code, "helper-absolute-timeout-cleanup-unknown");
}

#[cfg(windows)]
fn stop_already_requested() -> bool {
    true
}

#[cfg(windows)]
#[test]
fn stop_requested_before_launch_prevents_a_new_helper_process() {
    use std::path::PathBuf;
    use std::time::{Duration, Instant};

    use mactype_service_host::WindowsHelperLauncher;
    use windows_sys::Win32::Foundation::{CloseHandle, FILETIME};
    use windows_sys::Win32::System::Threading::{
        GetProcessTimes, OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION,
    };

    let process = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, std::process::id()) };
    assert!(!process.is_null());
    let mut created = FILETIME::default();
    let mut exited = FILETIME::default();
    let mut kernel = FILETIME::default();
    let mut user = FILETIME::default();
    assert_ne!(
        unsafe { GetProcessTimes(process, &mut created, &mut exited, &mut kernel, &mut user) },
        0
    );
    unsafe { CloseHandle(process) };
    let creation_time =
        (u64::from(created.dwHighDateTime) << 32) | u64::from(created.dwLowDateTime);

    let launcher = WindowsHelperLauncher::new(stop_already_requested);
    let started = Instant::now();
    let error = launcher
        .launch(&HelperInvocation {
            executable: PathBuf::from(r"C:\Windows\System32\notepad.exe"),
            target: ProcessIdentity {
                pid: std::process::id(),
                creation_time,
                session_id: 1,
                architecture: ProcessArchitecture::X64,
            },
            binding: test_binding(),
            timeout: Duration::from_secs(2),
        })
        .expect_err("a helper must not start after service stop was requested");

    assert_eq!(error.kind(), std::io::ErrorKind::Interrupted);
    assert!(started.elapsed() < Duration::from_millis(250));
}

#[test]
fn incomplete_remote_thread_cleanup_is_a_terminal_broker_result() {
    let (_base, runtime) = runtime();
    let assets = runtime.assets();
    let generation = assets.generation_id().to_owned();
    let launcher = StaticLauncher {
        output: Mutex::new(Some(HelperOutput {
            exit_code: 4,
            stdout: format!(
                "{{\"schemaVersion\":2,\"status\":\"timeout\",\"code\":\"remote-load-timeout\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{generation}\",\"module\":\"MacType64.dll\",\"windowsError\":1460,\"cleanupComplete\":false,\"rendererEvidence\":null}}"
            )
            .into_bytes(),
        })),
    };
    let broker = FixedHelperBroker::new(&runtime, launcher);
    let result = broker.inject(&InjectionRequest {
        identity: ProcessIdentity {
            pid: 42,
            creation_time: 100,
            session_id: 2,
            architecture: ProcessArchitecture::X64,
        },
        binding: runtime.binding(),
    });

    assert_eq!(result.disposition, BrokerDisposition::UncertainCleanup);
    assert_eq!(result.code, "remote-load-timeout");
    assert_eq!(result.win32_error, Some(1460));
}

#[test]
fn explicit_post_injection_unknown_code_is_preserved_for_generation_health() {
    let (_base, runtime) = runtime();
    let assets = runtime.assets();
    let generation = assets.generation_id().to_owned();
    let launcher = StaticLauncher {
        output: Mutex::new(Some(HelperOutput {
            exit_code: 3,
            stdout: format!(
                "{{\"schemaVersion\":2,\"status\":\"failed\",\"code\":\"post-injection-state-cleanup-unknown\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{generation}\",\"module\":\"MacType64.dll\",\"windowsError\":299,\"cleanupComplete\":false,\"rendererEvidence\":null}}"
            )
            .into_bytes(),
        })),
    };
    let broker = FixedHelperBroker::new(&runtime, launcher);
    let result = broker.inject(&InjectionRequest {
        identity: ProcessIdentity {
            pid: 42,
            creation_time: 100,
            session_id: 2,
            architecture: ProcessArchitecture::X64,
        },
        binding: runtime.binding(),
    });

    assert_eq!(result.disposition, BrokerDisposition::UncertainCleanup);
    assert_eq!(result.code, "post-injection-state-cleanup-unknown");
    assert_eq!(result.win32_error, Some(299));
}

#[test]
fn helper_adapter_classifies_only_verified_pre_injection_races_as_retryable() {
    let (_base, runtime) = runtime();
    let assets = runtime.assets();
    for code in [
        "session-unavailable",
        "identity-unavailable",
        "architecture-unavailable",
        "module-inventory-unavailable",
    ] {
        let generation = assets.generation_id().to_owned();
        let launcher = StaticLauncher {
            output: Mutex::new(Some(HelperOutput {
                exit_code: 3,
                stdout: format!(
                    "{{\"schemaVersion\":2,\"status\":\"failed\",\"code\":\"{code}\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{generation}\",\"module\":\"MacType64.dll\",\"windowsError\":5,\"cleanupComplete\":true,\"rendererEvidence\":null}}"
                )
                .into_bytes(),
            })),
        };
        let broker = FixedHelperBroker::new(&runtime, launcher);
        let result = broker.inject(&InjectionRequest {
            identity: ProcessIdentity {
                pid: 42,
                creation_time: 100,
                session_id: 2,
                architecture: ProcessArchitecture::X64,
            },
            binding: runtime.binding(),
        });

        assert_eq!(result.disposition, BrokerDisposition::Retryable, "{code}");
        assert_eq!(result.code, code);
    }
}

fn active_evidence_hex(invocation: &HelperInvocation) -> String {
    renderer_evidence_hex(
        &invocation.target,
        invocation.binding,
        RendererModuleLoad::LoadedByRequest,
        RendererActivationDisposition::Active,
        RendererActivationReason::None,
    )
}

fn renderer_evidence_hex(
    identity: &ProcessIdentity,
    binding: RendererRuntimeBinding,
    module_load: RendererModuleLoad,
    disposition: RendererActivationDisposition,
    reason: RendererActivationReason,
) -> String {
    let architecture = match identity.architecture {
        ProcessArchitecture::X86 => RendererArchitecture::X86,
        ProcessArchitecture::X64 => RendererArchitecture::X64,
    };
    let mut evidence = RendererActivationEvidenceV1::request(
        RendererProcessIdentity {
            pid: identity.pid,
            creation_time: identity.creation_time,
            session_id: identity.session_id,
            architecture,
        },
        binding,
        module_load,
    );
    evidence.disposition = disposition as u8;
    evidence.reason = reason as u16;
    evidence.lifecycle_revision = 1;
    match disposition {
        RendererActivationDisposition::Active => {
            evidence
                .effective_profile_digest
                .copy_from_slice(binding.profile_digest().as_wire_bytes());
            evidence.capability_active = RendererCapability::Gdi.bit();
        }
        RendererActivationDisposition::QuietSkip => {
            evidence
                .effective_profile_digest
                .copy_from_slice(binding.profile_digest().as_wire_bytes());
            evidence.capability_unavailable = RendererCapability::Gdi.bit();
        }
        RendererActivationDisposition::Failed => {
            evidence.capability_failed = RendererCapability::Gdi.bit();
        }
    }
    evidence
        .to_wire_bytes()
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect()
}

#[test]
fn helper_adapter_uses_typed_cleanup_evidence_instead_of_code_suffixes() {
    let (_base, runtime) = runtime();
    let assets = runtime.assets();
    let generation = assets.generation_id().to_owned();
    let launcher = StaticLauncher {
        output: Mutex::new(Some(HelperOutput {
            exit_code: 3,
            stdout: format!(
                "{{\"schemaVersion\":2,\"status\":\"failed\",\"code\":\"post-injection-state-cleanup-unknown\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{generation}\",\"module\":\"MacType64.dll\",\"windowsError\":299,\"cleanupComplete\":true,\"rendererEvidence\":null}}"
            )
            .into_bytes(),
        })),
    };
    let broker = FixedHelperBroker::new(&runtime, launcher);
    let result = broker.inject(&InjectionRequest {
        identity: ProcessIdentity {
            pid: 42,
            creation_time: 100,
            session_id: 2,
            architecture: ProcessArchitecture::X64,
        },
        binding: runtime.binding(),
    });

    assert_eq!(result.disposition, BrokerDisposition::Rejected);
    assert_eq!(result.code, "post-injection-state-cleanup-unknown");
}

#[test]
fn malformed_helper_frame_is_typed_as_integrity_uncertainty() {
    let (_base, runtime) = runtime();
    let broker = FixedHelperBroker::new(
        &runtime,
        StaticLauncher {
            output: Mutex::new(Some(HelperOutput {
                exit_code: 0,
                stdout: b"not-json".to_vec(),
            })),
        },
    );
    let result = broker.inject(&InjectionRequest {
        identity: ProcessIdentity {
            pid: 42,
            creation_time: 100,
            session_id: 2,
            architecture: ProcessArchitecture::X64,
        },
        binding: runtime.binding(),
    });

    assert_eq!(result.disposition, BrokerDisposition::UncertainIntegrity);
    assert_eq!(result.code, "helper-response-invalid");
}

fn x64_request(runtime: &ProtectedRendererRuntime) -> InjectionRequest {
    InjectionRequest {
        identity: ProcessIdentity {
            pid: 42,
            creation_time: 100,
            session_id: 2,
            architecture: ProcessArchitecture::X64,
        },
        binding: runtime.binding(),
    }
}

fn inject_static_response(
    runtime: &ProtectedRendererRuntime,
    request: &InjectionRequest,
    exit_code: i32,
    response: String,
) -> BrokerResult {
    FixedHelperBroker::new(
        runtime,
        StaticLauncher {
            output: Mutex::new(Some(HelperOutput {
                exit_code,
                stdout: response.into_bytes(),
            })),
        },
    )
    .inject(request)
}

#[test]
fn injected_status_requires_matching_active_renderer_evidence() {
    let (_base, runtime) = runtime();
    let request = x64_request(&runtime);
    let result = inject_static_response(
        &runtime,
        &request,
        0,
        format!(
            "{{\"schemaVersion\":2,\"status\":\"injected\",\"code\":\"renderer-active\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{}\",\"module\":\"MacType64.dll\",\"windowsError\":0,\"cleanupComplete\":true,\"rendererEvidence\":null}}",
            request.binding.runtime_generation_id(),
        ),
    );

    assert_eq!(result.disposition, BrokerDisposition::UncertainIntegrity);
    assert_eq!(result.code, "renderer-evidence-inconsistent");
}

#[test]
fn renderer_evidence_must_match_the_exact_request_identity_and_binding() {
    let (_base, runtime) = runtime();
    let request = x64_request(&runtime);
    let mut wrong_identity = request.identity.clone();
    wrong_identity.pid = 43;
    let evidence = renderer_evidence_hex(
        &wrong_identity,
        request.binding,
        RendererModuleLoad::LoadedByRequest,
        RendererActivationDisposition::Active,
        RendererActivationReason::None,
    );
    let result = inject_static_response(
        &runtime,
        &request,
        0,
        format!(
            "{{\"schemaVersion\":2,\"status\":\"injected\",\"code\":\"renderer-active\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{}\",\"module\":\"MacType64.dll\",\"windowsError\":0,\"cleanupComplete\":true,\"rendererEvidence\":\"{evidence}\"}}",
            request.binding.runtime_generation_id(),
        ),
    );

    assert_eq!(result.disposition, BrokerDisposition::UncertainIntegrity);
    assert_eq!(result.code, "renderer-evidence-mismatch");
}

#[test]
fn verified_renderer_quiet_skip_stays_process_local() {
    let (_base, runtime) = runtime();
    let request = x64_request(&runtime);
    let evidence = renderer_evidence_hex(
        &request.identity,
        request.binding,
        RendererModuleLoad::LoadedByRequest,
        RendererActivationDisposition::QuietSkip,
        RendererActivationReason::ProcessExcluded,
    );
    let result = inject_static_response(
        &runtime,
        &request,
        0,
        format!(
            "{{\"schemaVersion\":2,\"status\":\"skipped\",\"code\":\"process-excluded\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{}\",\"module\":\"MacType64.dll\",\"windowsError\":0,\"cleanupComplete\":true,\"rendererEvidence\":\"{evidence}\"}}",
            request.binding.runtime_generation_id(),
        ),
    );

    assert_eq!(result.disposition, BrokerDisposition::Skipped);
    assert_eq!(result.code, "process-excluded");
}

#[test]
fn verified_already_loaded_renderer_with_a_helper_lease_is_injected() {
    let (_base, runtime) = runtime();
    let request = x64_request(&runtime);
    let evidence = renderer_evidence_hex(
        &request.identity,
        request.binding,
        RendererModuleLoad::AlreadyLoaded,
        RendererActivationDisposition::Active,
        RendererActivationReason::None,
    );
    let result = inject_static_response(
        &runtime,
        &request,
        0,
        format!(
            "{{\"schemaVersion\":2,\"status\":\"injected\",\"code\":\"renderer-active\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{}\",\"module\":\"MacType64.dll\",\"windowsError\":0,\"cleanupComplete\":true,\"rendererEvidence\":\"{evidence}\"}}",
            request.binding.runtime_generation_id(),
        ),
    );

    assert_eq!(result.disposition, BrokerDisposition::Injected);
    assert_eq!(result.code, "renderer-active");
}

#[test]
fn verified_already_loaded_quiet_skip_stays_process_local() {
    let (_base, runtime) = runtime();
    let request = x64_request(&runtime);
    let evidence = renderer_evidence_hex(
        &request.identity,
        request.binding,
        RendererModuleLoad::AlreadyLoaded,
        RendererActivationDisposition::QuietSkip,
        RendererActivationReason::ProcessExcluded,
    );
    let result = inject_static_response(
        &runtime,
        &request,
        0,
        format!(
            "{{\"schemaVersion\":2,\"status\":\"skipped\",\"code\":\"process-excluded\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{}\",\"module\":\"MacType64.dll\",\"windowsError\":0,\"cleanupComplete\":true,\"rendererEvidence\":\"{evidence}\"}}",
            request.binding.runtime_generation_id(),
        ),
    );

    assert_eq!(result.disposition, BrokerDisposition::Skipped);
    assert_eq!(result.code, "process-excluded");
}

#[test]
fn verified_already_loaded_failure_with_a_released_lease_is_rejected() {
    let (_base, runtime) = runtime();
    let request = x64_request(&runtime);
    let evidence = renderer_evidence_hex(
        &request.identity,
        request.binding,
        RendererModuleLoad::AlreadyLoaded,
        RendererActivationDisposition::Failed,
        RendererActivationReason::InitializationFailed,
    );
    let result = inject_static_response(
        &runtime,
        &request,
        3,
        format!(
            "{{\"schemaVersion\":2,\"status\":\"failed\",\"code\":\"initialization-failed\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{}\",\"module\":\"MacType64.dll\",\"windowsError\":0,\"cleanupComplete\":true,\"rendererEvidence\":\"{evidence}\"}}",
            request.binding.runtime_generation_id(),
        ),
    );

    assert_eq!(result.disposition, BrokerDisposition::Rejected);
    assert_eq!(result.code, "initialization-failed");
}

#[test]
fn legacy_unleased_existing_renderer_response_remains_integrity_uncertainty() {
    let (_base, runtime) = runtime();
    let request = x64_request(&runtime);
    let result = inject_static_response(
        &runtime,
        &request,
        3,
        format!(
            "{{\"schemaVersion\":2,\"status\":\"integrity\",\"code\":\"existing-renderer-unverified\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{}\",\"module\":\"MacType64.dll\",\"windowsError\":0,\"cleanupComplete\":true,\"rendererEvidence\":null}}",
            request.binding.runtime_generation_id(),
        ),
    );

    assert_eq!(result.disposition, BrokerDisposition::UncertainIntegrity);
    assert_eq!(result.code, "existing-renderer-unverified");
}

#[test]
fn already_loaded_reference_release_uncertainty_is_typed_cleanup_uncertainty() {
    let (_base, runtime) = runtime();
    let request = x64_request(&runtime);
    let evidence = renderer_evidence_hex(
        &request.identity,
        request.binding,
        RendererModuleLoad::AlreadyLoaded,
        RendererActivationDisposition::Active,
        RendererActivationReason::None,
    );
    let result = inject_static_response(
        &runtime,
        &request,
        4,
        format!(
            "{{\"schemaVersion\":2,\"status\":\"timeout\",\"code\":\"renderer-reference-release-cleanup-unknown\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{}\",\"module\":\"MacType64.dll\",\"windowsError\":1460,\"cleanupComplete\":false,\"rendererEvidence\":\"{evidence}\"}}",
            request.binding.runtime_generation_id(),
        ),
    );

    assert_eq!(result.disposition, BrokerDisposition::UncertainCleanup);
    assert_eq!(result.code, "renderer-reference-release-cleanup-unknown");
}

#[test]
fn producer_integrity_status_is_typed_without_parsing_its_diagnostic_code() {
    let (_base, runtime) = runtime();
    let request = x64_request(&runtime);
    for (cleanup_complete, expected) in [
        (true, BrokerDisposition::UncertainIntegrity),
        (false, BrokerDisposition::UncertainCleanup),
    ] {
        let result = inject_static_response(
            &runtime,
            &request,
            3,
            format!(
                "{{\"schemaVersion\":2,\"status\":\"integrity\",\"code\":\"arbitrary-bounded-evidence\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{}\",\"module\":\"MacType64.dll\",\"windowsError\":13,\"cleanupComplete\":{cleanup_complete},\"rendererEvidence\":null}}",
                request.binding.runtime_generation_id(),
            ),
        );

        assert_eq!(result.disposition, expected);
        assert_eq!(result.code, "arbitrary-bounded-evidence");
    }
}

#[test]
fn quiet_skip_unload_cleanup_unknown_remains_cleanup_uncertainty() {
    let (_base, runtime) = runtime();
    let request = x64_request(&runtime);
    let evidence = renderer_evidence_hex(
        &request.identity,
        request.binding,
        RendererModuleLoad::LoadedByRequest,
        RendererActivationDisposition::QuietSkip,
        RendererActivationReason::ProcessExcluded,
    );
    let result = inject_static_response(
        &runtime,
        &request,
        4,
        format!(
            "{{\"schemaVersion\":2,\"status\":\"timeout\",\"code\":\"renderer-quiet-skip-unload-cleanup-unknown\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{}\",\"module\":\"MacType64.dll\",\"windowsError\":1460,\"cleanupComplete\":false,\"rendererEvidence\":\"{evidence}\"}}",
            request.binding.runtime_generation_id(),
        ),
    );

    assert_eq!(result.disposition, BrokerDisposition::UncertainCleanup);
    assert_eq!(result.code, "renderer-quiet-skip-unload-cleanup-unknown");
}

#[test]
fn cleanup_complete_evidence_rejects_forged_or_stale_top_level_codes() {
    let (_base, runtime) = runtime();
    let request = x64_request(&runtime);
    for (status, exit_code, module_load, disposition, reason) in [
        (
            "injected",
            0,
            RendererModuleLoad::LoadedByRequest,
            RendererActivationDisposition::Active,
            RendererActivationReason::None,
        ),
        (
            "skipped",
            0,
            RendererModuleLoad::LoadedByRequest,
            RendererActivationDisposition::QuietSkip,
            RendererActivationReason::ProcessExcluded,
        ),
        (
            "failed",
            3,
            RendererModuleLoad::LoadedByRequest,
            RendererActivationDisposition::Failed,
            RendererActivationReason::InitializationFailed,
        ),
    ] {
        let evidence = renderer_evidence_hex(
            &request.identity,
            request.binding,
            module_load,
            disposition,
            reason,
        );
        let result = inject_static_response(
            &runtime,
            &request,
            exit_code,
            format!(
                "{{\"schemaVersion\":2,\"status\":\"{status}\",\"code\":\"stale-diagnostic-code\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{}\",\"module\":\"MacType64.dll\",\"windowsError\":0,\"cleanupComplete\":true,\"rendererEvidence\":\"{evidence}\"}}",
                request.binding.runtime_generation_id(),
            ),
        );

        assert_eq!(
            result.disposition,
            BrokerDisposition::UncertainIntegrity,
            "{status}"
        );
        assert_eq!(result.code, "renderer-evidence-inconsistent", "{status}");
    }
}

#[test]
fn cleanup_unknown_evidence_accepts_only_the_producer_tuple_allowlist() {
    let (_base, runtime) = runtime();
    let request = x64_request(&runtime);
    let cases = [
        (
            "injected",
            0,
            RendererModuleLoad::LoadedByRequest,
            RendererActivationDisposition::Active,
            RendererActivationReason::None,
            "renderer-active",
            BrokerDisposition::UncertainIntegrity,
        ),
        (
            "timeout",
            4,
            RendererModuleLoad::AlreadyLoaded,
            RendererActivationDisposition::QuietSkip,
            RendererActivationReason::ProcessExcluded,
            "renderer-quiet-skip-unload-cleanup-unknown",
            BrokerDisposition::UncertainIntegrity,
        ),
        (
            "failed",
            3,
            RendererModuleLoad::LoadedByRequest,
            RendererActivationDisposition::Failed,
            RendererActivationReason::InitializationFailed,
            "initialization-failed",
            BrokerDisposition::UncertainCleanup,
        ),
    ];
    for (status, exit_code, module_load, disposition, reason, code, expected) in cases {
        let evidence = renderer_evidence_hex(
            &request.identity,
            request.binding,
            module_load,
            disposition,
            reason,
        );
        let result = inject_static_response(
            &runtime,
            &request,
            exit_code,
            format!(
                "{{\"schemaVersion\":2,\"status\":\"{status}\",\"code\":\"{code}\",\"pid\":42,\"sessionId\":2,\"generationId\":\"{}\",\"module\":\"MacType64.dll\",\"windowsError\":31,\"cleanupComplete\":false,\"rendererEvidence\":\"{evidence}\"}}",
                request.binding.runtime_generation_id(),
            ),
        );

        assert_eq!(result.disposition, expected, "{status}/{module_load:?}");
        if expected == BrokerDisposition::UncertainIntegrity {
            assert_eq!(result.code, "renderer-evidence-inconsistent");
        } else {
            assert_eq!(result.code, code);
        }
    }
}
