use std::sync::Arc;

use mactype_service_contract::{MachinePaths, StructuredServiceError};

use crate::{
    initialize_process_orchestration_with_profile_policies, EtwProcessEventSource,
    FixedHelperBroker, HostEvent, HostEventSink, InitializedRuntime, LiveObserver,
    ObserverRecoveryPolicy, ProcessEventSource, ProtectedRendererRuntime, RuntimeInitializer,
    WindowsHelperLauncher, WindowsProcessInspector, WindowsStartupSafety, WmiProcessEventSource,
    PROCESS_START_SESSION,
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
        let source = self.live_process_event_source()?;
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
            source,
            Box::new(inspector),
            Box::new(broker),
            self.events.clone(),
        )
    }
}

impl WindowsOpenServiceInitializer {
    /// The fastest process event source this machine will give us.
    ///
    /// A private real-time ETW session hears about a new process in tens of
    /// milliseconds, where WMI's shared session takes hundreds. Opening one
    /// needs an elevated or LocalSystem token, which the service has and a
    /// developer shell may not, so a refusal falls back to WMI rather than
    /// failing the start. The two behave identically from here on; only the
    /// summary detail says which one is live.
    fn live_process_event_source(
        &self,
    ) -> Result<Box<dyn ProcessEventSource>, StructuredServiceError> {
        let (source, observer): (Box<dyn ProcessEventSource>, LiveObserver) =
            match EtwProcessEventSource::start(PROCESS_START_SESSION) {
                Ok(source) => (Box::new(source), LiveObserver::Etw),
                Err(_) => (
                    Box::new(WmiProcessEventSource::connect()?),
                    LiveObserver::Wmi,
                ),
            };
        self.events
            .record(HostEvent::LiveObserverSelected(observer));
        Ok(source)
    }
}
