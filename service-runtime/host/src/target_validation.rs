#![forbid(unsafe_code)]

use mactype_service_contract::{
    PrivateFreeTypePolicy, StructuredServiceError, UnityFontHookMode, UnityFontHookPolicy,
};

use crate::observer::ProcessIdentity;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TargetLiveness {
    Alive,
    Vanished,
    Unknown,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum InspectionEvidence<T> {
    Known(T),
    Unavailable,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DynamicCodePolicy {
    pub prohibit_dynamic_code: bool,
    pub allow_thread_opt_out: bool,
}

impl DynamicCodePolicy {
    pub const fn explicitly_blocks_hooks(self) -> bool {
        self.prohibit_dynamic_code && !self.allow_thread_opt_out
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BinarySignaturePolicy {
    pub microsoft_signed_only: bool,
    pub store_signed_only: bool,
    pub mitigation_opt_in: bool,
}

impl BinarySignaturePolicy {
    pub const fn explicitly_blocks_modules(self) -> bool {
        self.microsoft_signed_only || self.store_signed_only || self.mitigation_opt_in
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProcessInspection {
    pub identity: ProcessIdentity,
    pub image_name: InspectionEvidence<String>,
    pub protected: InspectionEvidence<bool>,
    pub critical: InspectionEvidence<bool>,
    pub dynamic_code: InspectionEvidence<DynamicCodePolicy>,
    pub binary_signature: InspectionEvidence<BinarySignaturePolicy>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ProcessInspectionError {
    TargetUnavailable(StructuredServiceError),
    Infrastructure(StructuredServiceError),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnityProcessClassification {
    NotUnity,
    Unity,
    UnityWithAntiCheat,
    Unavailable,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PrivateFreeTypeClassification {
    NotDetected,
    Detected,
    Unavailable,
}

pub trait ProcessInspector {
    fn inspect(&self, pid: u32) -> Result<ProcessInspection, ProcessInspectionError>;

    /// Re-checks whether the exact verified identity still exists after a
    /// terminal result that could not be trusted. The default cannot prove a
    /// vanish, so callers keep their conservative classification.
    fn probe_target_liveness(&self, identity: &ProcessIdentity) -> TargetLiveness {
        let _ = identity;
        TargetLiveness::Unknown
    }

    fn classify_unity_process(&self, identity: &ProcessIdentity) -> UnityProcessClassification {
        let _ = identity;
        UnityProcessClassification::NotUnity
    }

    fn classify_private_freetype_process(
        &self,
        identity: &ProcessIdentity,
    ) -> PrivateFreeTypeClassification {
        let _ = identity;
        PrivateFreeTypeClassification::NotDetected
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProcessSkipReason {
    TargetUnavailable,
    ServiceSelf,
    SessionZero,
    Protected,
    ProtectionUnavailable,
    Critical,
    CriticalityUnavailable,
    ImportantWindowsProcess,
    InstallerControlProcess,
    ImageNameUnavailable,
    DynamicCodeProhibited,
    BinarySignatureRestricted,
    PrivateFreeTypeDetected,
    UnityAntiCheatDetected,
    UnitySafetyEvidenceUnavailable,
}

impl ProcessSkipReason {
    pub const fn code(self) -> &'static str {
        match self {
            Self::TargetUnavailable => "target-unavailable",
            Self::ServiceSelf => "service-self",
            Self::SessionZero => "session-zero",
            Self::Protected => "protected-process",
            Self::ProtectionUnavailable => "protection-query-unavailable",
            Self::Critical => "critical-process",
            Self::CriticalityUnavailable => "criticality-query-unavailable",
            Self::ImportantWindowsProcess => "important-windows-process",
            Self::InstallerControlProcess => "installer-control-process",
            Self::ImageNameUnavailable => "image-name-unavailable",
            Self::DynamicCodeProhibited => "dynamic-code-policy-blocks-hooks",
            Self::BinarySignatureRestricted => "binary-signature-policy-blocks-module",
            Self::PrivateFreeTypeDetected => "private-freetype-detected",
            Self::UnityAntiCheatDetected => "unity-anticheat-detected",
            Self::UnitySafetyEvidenceUnavailable => "unity-safety-evidence-unavailable",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ProcessTargetDecision {
    Eligible(ProcessIdentity),
    Skipped {
        identity: Option<ProcessIdentity>,
        reason: ProcessSkipReason,
    },
}

pub struct ProcessTargetValidator<'a> {
    service_pid: u32,
    inspector: &'a dyn ProcessInspector,
    unity_font_hook: UnityFontHookPolicy,
    private_freetype: PrivateFreeTypePolicy,
}

impl<'a> ProcessTargetValidator<'a> {
    pub fn new(service_pid: u32, inspector: &'a dyn ProcessInspector) -> Self {
        Self {
            service_pid,
            inspector,
            unity_font_hook: UnityFontHookPolicy::default(),
            private_freetype: PrivateFreeTypePolicy::default(),
        }
    }

    pub fn with_unity_font_hook_policy(
        service_pid: u32,
        inspector: &'a dyn ProcessInspector,
        unity_font_hook: UnityFontHookPolicy,
    ) -> Self {
        Self {
            service_pid,
            inspector,
            unity_font_hook,
            private_freetype: PrivateFreeTypePolicy::default(),
        }
    }

    pub fn with_profile_policies(
        service_pid: u32,
        inspector: &'a dyn ProcessInspector,
        unity_font_hook: UnityFontHookPolicy,
        private_freetype: PrivateFreeTypePolicy,
    ) -> Self {
        Self {
            service_pid,
            inspector,
            unity_font_hook,
            private_freetype,
        }
    }

    pub fn validate(&self, pid: u32) -> Result<ProcessTargetDecision, StructuredServiceError> {
        if pid == self.service_pid {
            return Ok(ProcessTargetDecision::Skipped {
                identity: None,
                reason: ProcessSkipReason::ServiceSelf,
            });
        }
        let inspection = match self.inspector.inspect(pid) {
            Ok(inspection) => inspection,
            Err(ProcessInspectionError::TargetUnavailable(_)) => {
                return Ok(ProcessTargetDecision::Skipped {
                    identity: None,
                    reason: ProcessSkipReason::TargetUnavailable,
                });
            }
            Err(ProcessInspectionError::Infrastructure(error)) => return Err(error),
        };
        if inspection.identity.pid != pid {
            return Err(service_error(
                "process-identity-mismatch",
                "the inspected process identity does not match the observed PID",
            ));
        }
        if inspection.identity.session_id == 0 {
            return Ok(skipped(&inspection, ProcessSkipReason::SessionZero));
        }
        match inspection.protected {
            InspectionEvidence::Known(true) => {
                return Ok(skipped(&inspection, ProcessSkipReason::Protected));
            }
            InspectionEvidence::Unavailable => {
                return Ok(skipped(
                    &inspection,
                    ProcessSkipReason::ProtectionUnavailable,
                ));
            }
            InspectionEvidence::Known(false) => {}
        }
        match inspection.critical {
            InspectionEvidence::Known(true) => {
                return Ok(skipped(&inspection, ProcessSkipReason::Critical));
            }
            InspectionEvidence::Unavailable => {
                return Ok(skipped(
                    &inspection,
                    ProcessSkipReason::CriticalityUnavailable,
                ));
            }
            InspectionEvidence::Known(false) => {}
        }
        match inspection.image_name {
            InspectionEvidence::Known(ref name) if is_important_windows_process(name) => {
                return Ok(skipped(
                    &inspection,
                    ProcessSkipReason::ImportantWindowsProcess,
                ));
            }
            InspectionEvidence::Known(ref name) if is_installer_control_process(name) => {
                return Ok(skipped(
                    &inspection,
                    ProcessSkipReason::InstallerControlProcess,
                ));
            }
            InspectionEvidence::Unavailable => {
                return Ok(skipped(
                    &inspection,
                    ProcessSkipReason::ImageNameUnavailable,
                ));
            }
            InspectionEvidence::Known(_) => {}
        }
        if matches!(
            inspection.binary_signature,
            InspectionEvidence::Known(policy) if policy.explicitly_blocks_modules()
        ) {
            return Ok(skipped(
                &inspection,
                ProcessSkipReason::BinarySignatureRestricted,
            ));
        }
        if matches!(
            inspection.dynamic_code,
            InspectionEvidence::Known(policy) if policy.explicitly_blocks_hooks()
        ) {
            return Ok(skipped(
                &inspection,
                ProcessSkipReason::DynamicCodeProhibited,
            ));
        }
        let unity_classification = if self.private_freetype.skip_detected()
            || self.unity_font_hook.mode() == UnityFontHookMode::MostGames
        {
            self.inspector.classify_unity_process(&inspection.identity)
        } else {
            UnityProcessClassification::NotUnity
        };
        if self.unity_font_hook.mode() == UnityFontHookMode::MostGames {
            match unity_classification {
                UnityProcessClassification::UnityWithAntiCheat => {
                    return Ok(skipped(
                        &inspection,
                        ProcessSkipReason::UnityAntiCheatDetected,
                    ));
                }
                UnityProcessClassification::Unavailable => {
                    return Ok(skipped(
                        &inspection,
                        ProcessSkipReason::UnitySafetyEvidenceUnavailable,
                    ));
                }
                UnityProcessClassification::NotUnity | UnityProcessClassification::Unity => {}
            }
        }
        let unity_hook_target = matches!(
            unity_classification,
            UnityProcessClassification::Unity | UnityProcessClassification::UnityWithAntiCheat
        ) && matches!(
            inspection.image_name,
            InspectionEvidence::Known(ref name) if self.unity_font_hook.applies_to(name)
        );
        if self.private_freetype.skip_detected()
            && !unity_hook_target
            && self
                .inspector
                .classify_private_freetype_process(&inspection.identity)
                == PrivateFreeTypeClassification::Detected
        {
            return Ok(skipped(
                &inspection,
                ProcessSkipReason::PrivateFreeTypeDetected,
            ));
        }
        Ok(ProcessTargetDecision::Eligible(inspection.identity))
    }
}

fn skipped(inspection: &ProcessInspection, reason: ProcessSkipReason) -> ProcessTargetDecision {
    ProcessTargetDecision::Skipped {
        identity: Some(inspection.identity.clone()),
        reason,
    }
}

fn is_important_windows_process(name: &str) -> bool {
    matches!(
        name.to_ascii_lowercase().as_str(),
        "smss.exe"
            | "csrss.exe"
            | "wininit.exe"
            | "winlogon.exe"
            | "services.exe"
            | "lsass.exe"
            | "fontdrvhost.exe"
    )
}

fn is_installer_control_process(name: &str) -> bool {
    let name = name.to_ascii_lowercase();
    name == "mactype-service-setup.exe" || is_inno_uninstaller(&name)
}

fn is_inno_uninstaller(name: &str) -> bool {
    let Some((stem, extension)) = name.rsplit_once('.') else {
        return false;
    };
    if !matches!(extension, "exe" | "tmp") {
        return false;
    }
    let stem = stem.strip_prefix('_').unwrap_or(stem);
    stem.strip_prefix("unins")
        .is_some_and(|sequence| sequence.bytes().all(|character| character.is_ascii_digit()))
}

fn service_error(code: &str, message: &str) -> StructuredServiceError {
    StructuredServiceError {
        code: code.to_owned(),
        message: message.to_owned(),
        win32_error: None,
    }
}
