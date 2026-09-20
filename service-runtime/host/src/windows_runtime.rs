use std::sync::Arc;

use mactype_service_contract::{MachinePaths, StructuredServiceError};

use crate::{
    initialize_process_orchestration_with_profile_policies, FixedHelperBroker, HostEventSink,
    InitializedRuntime, ObserverRecoveryPolicy, ProtectedRendererRuntime, RuntimeInitializer,
    WindowsHelperLauncher, WindowsProcessInspector, WindowsStartupSafety, WmiProcessEventSource,
};

pub struct WindowsOpenServiceInitializer {
    paths: MachinePaths,
    events: Arc<dyn HostEventSink>,
}

impl WindowsOpenServiceInitializer {
    pub fn new(paths: MachinePaths, events: Arc<dyn HostEventSink>) -> Self {
        Self { paths, events }
    }
}

impl RuntimeInitializer for WindowsOpenServiceInitializer {
    fn initialize(&self) -> Result<InitializedRuntime, StructuredServiceError> {
        let runtime = ProtectedRendererRuntime::load(self.paths.clone())?;
        WindowsStartupSafety::verify(&runtime.assets().root().join("mactype-service.exe"))?;
        let source = WmiProcessEventSource::connect()?;
        let service_pid = std::process::id();
        let inspector = WindowsProcessInspector::new();
        let launcher = WindowsHelperLauncher::new(crate::scm::stop_requested);
        let broker = FixedHelperBroker::new(&runtime, launcher, self.events.clone());
        initialize_process_orchestration_with_profile_policies(
            runtime.binding(),
            runtime.unity_font_hook_policy().clone(),
            runtime.private_freetype_policy(),
            runtime.console_process_policy(),
            ObserverRecoveryPolicy::default(),
            service_pid,
            Box::new(source),
            Box::new(inspector),
            Box::new(broker),
            self.events.clone(),
        )
    }
}
