use serde::{Deserialize, Serialize};
#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum ServicePresence {
    Absent,
    Owned,
    CompatibleUnquoted,
    Foreign,
    DeletePending,
    Inaccessible,
}

#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum ServiceRuntimeState {
    Stopped,
    StartPending,
    StopPending,
    Running,
    ContinuePending,
    PausePending,
    Paused,
    Unknown,
}

pub(super) fn require_stable_migration_state(state: ServiceRuntimeState) -> Result<(), String> {
    if matches!(
        state,
        ServiceRuntimeState::Running | ServiceRuntimeState::Stopped
    ) {
        Ok(())
    } else {
        Err("legacy SCM service must be exactly running or stopped for migration".to_owned())
    }
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LegacyServiceStatus {
    pub presence: ServicePresence,
    pub state: ServiceRuntimeState,
    pub binary_path: Option<String>,
    pub win32_error: Option<u32>,
    pub trusted_binary_available: bool,
    pub registry_conflict: bool,
    pub can_remove: bool,
    pub can_stop: bool,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct ServiceConfiguration {
    pub(crate) display_name: String,
    pub(crate) binary_path: String,
    pub(crate) service_type: u32,
    pub(crate) start_type: u32,
    pub(crate) error_control: u32,
    pub(crate) load_order_group: Option<String>,
    pub(crate) tag_id: u32,
    pub(crate) account: String,
    pub(crate) dependencies: Vec<String>,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct FailureAction {
    pub(crate) action_type: i32,
    pub(crate) delay_ms: u32,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct FailureActionsConfiguration {
    pub(crate) reset_period_seconds: u32,
    pub(crate) reboot_message: Option<String>,
    pub(crate) command: Option<String>,
    pub(crate) actions: Vec<FailureAction>,
}

#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub(crate) enum ServiceTriggerConfiguration {
    None,
}

pub(super) fn snapshot_trigger_configuration(
    trigger_count: u32,
    has_trigger_data: bool,
    has_reserved_data: bool,
) -> Result<ServiceTriggerConfiguration, String> {
    if trigger_count == 0 && !has_trigger_data && !has_reserved_data {
        Ok(ServiceTriggerConfiguration::None)
    } else {
        Err("custom legacy service triggers are not migration-safe".to_owned())
    }
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct SecurityDescriptorSnapshot {
    pub(crate) self_relative: Vec<u8>,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct ServiceExtendedConfiguration {
    pub(crate) description: Option<String>,
    pub(crate) failure_actions: FailureActionsConfiguration,
    pub(crate) failure_actions_on_non_crash: bool,
    pub(crate) delayed_auto_start: bool,
    pub(crate) service_sid_type: u32,
    pub(crate) required_privileges: Vec<String>,
    pub(crate) preshutdown_timeout_ms: u32,
    pub(crate) triggers: ServiceTriggerConfiguration,
    pub(crate) security_descriptor: SecurityDescriptorSnapshot,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct LegacyScmSnapshot {
    pub(crate) presence: ServicePresence,
    pub(crate) state: ServiceRuntimeState,
    pub(crate) configuration: ServiceConfiguration,
    pub(crate) extended: ServiceExtendedConfiguration,
}
