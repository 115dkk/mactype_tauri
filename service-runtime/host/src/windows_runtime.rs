use mactype_service_contract::{MachinePaths, StructuredServiceError};

use crate::{
    initialize_process_orchestration, FixedHelperBroker, InitializedRuntime,
    ProtectedRendererRuntime, RuntimeInitializer, WindowsHelperLauncher, WindowsProcessInspector,
    WindowsStartupSafety, WmiProcessEventSource,
};

pub struct WindowsOpenServiceInitializer {
    paths: MachinePaths,
}

impl WindowsOpenServiceInitializer {
    pub const fn new(paths: MachinePaths) -> Self {
        Self { paths }
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
        let broker = FixedHelperBroker::new(&runtime, launcher);
        initialize_process_orchestration(
            runtime.binding(),
            service_pid,
            Box::new(source),
            Box::new(inspector),
            Box::new(broker),
        )
    }
}
