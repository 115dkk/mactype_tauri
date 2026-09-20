use mactype_service_contract::{MachinePaths, StructuredServiceError};
use std::sync::Arc;

use crate::{
    initialize_process_orchestration, FixedHelperBroker, HostEventSink, InitializedRuntime,
    ProtectedProfileInitializer, ProtectedRuntimeAssets, RuntimeInitializer, WindowsHelperLauncher,
    WindowsProcessInspector, WindowsStartupSafety, WmiProcessEventSource,
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
        let profile_initializer = ProtectedProfileInitializer::new(self.paths.clone());
        let prepared_profile = profile_initializer.prepare()?;
        let generation = profile_initializer.resolve_generation(&prepared_profile)?;
        let profile =
            profile_initializer.initialize_with_generation(prepared_profile, &generation)?;
        let assets = ProtectedRuntimeAssets::load_from_generation(&generation)?;
        WindowsStartupSafety::verify(&assets.root().join("mactype-service.exe"))?;
        let source = WmiProcessEventSource::connect()?;
        let service_pid = std::process::id();
        let inspector = WindowsProcessInspector::new();
        let launcher = WindowsHelperLauncher::new(crate::scm::stop_requested);
        let broker = FixedHelperBroker::new(&assets, launcher, self.events.clone());
        initialize_process_orchestration(
            profile.active_profile_digest,
            service_pid,
            assets.generation_id(),
            Box::new(source),
            Box::new(inspector),
            Box::new(broker),
            self.events.clone(),
        )
    }
}
