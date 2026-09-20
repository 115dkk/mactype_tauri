#![allow(dead_code)]

use std::collections::{HashMap, VecDeque};
use std::sync::Mutex;
use std::time::Duration;

use mactype_service_host::{
    ImageSubsystem, PrivateFreeTypeClassification, ProcessIdentity, ProcessInspection,
    ProcessInspectionError, ProcessInspector, TargetLifecycle, TargetLiveness,
    UnityProcessClassification,
};

#[derive(Clone)]
pub(crate) struct InspectorResponse {
    inspection: Result<ProcessInspection, ProcessInspectionError>,
    lifecycle: TargetLifecycle,
    liveness: TargetLiveness,
    unity: UnityProcessClassification,
    private_freetype: PrivateFreeTypeClassification,
    subsystem: ImageSubsystem,
    process_age: Option<Duration>,
}

impl InspectorResponse {
    pub(crate) fn inspected(inspection: ProcessInspection, lifecycle: TargetLifecycle) -> Self {
        Self {
            inspection: Ok(inspection),
            lifecycle,
            liveness: TargetLiveness::Unknown,
            unity: UnityProcessClassification::NotUnity,
            private_freetype: PrivateFreeTypeClassification::NotDetected,
            subsystem: ImageSubsystem::Unavailable,
            process_age: None,
        }
    }

    pub(crate) fn failure(error: ProcessInspectionError) -> Self {
        Self {
            inspection: Err(error),
            lifecycle: TargetLifecycle::Unknown,
            liveness: TargetLiveness::Unknown,
            unity: UnityProcessClassification::Unavailable,
            private_freetype: PrivateFreeTypeClassification::Unavailable,
            subsystem: ImageSubsystem::Unavailable,
            process_age: None,
        }
    }

    pub(crate) const fn with_liveness(mut self, liveness: TargetLiveness) -> Self {
        self.liveness = liveness;
        self
    }

    pub(crate) const fn with_unity(mut self, unity: UnityProcessClassification) -> Self {
        self.unity = unity;
        self
    }

    pub(crate) const fn with_private_freetype(
        mut self,
        private_freetype: PrivateFreeTypeClassification,
    ) -> Self {
        self.private_freetype = private_freetype;
        self
    }

    pub(crate) const fn with_subsystem(mut self, subsystem: ImageSubsystem) -> Self {
        self.subsystem = subsystem;
        self
    }

    pub(crate) const fn with_process_age(mut self, process_age: Option<Duration>) -> Self {
        self.process_age = process_age;
        self
    }
}

pub(crate) struct ScriptedInspector {
    responses: Mutex<HashMap<u32, VecDeque<InspectorResponse>>>,
    active: Mutex<HashMap<(u32, u64), InspectorResponse>>,
    liveness_probes: Mutex<Vec<ProcessIdentity>>,
}

impl ScriptedInspector {
    pub(crate) fn new(responses: impl IntoIterator<Item = (u32, InspectorResponse)>) -> Self {
        let mut scripts = HashMap::<u32, VecDeque<InspectorResponse>>::new();
        for (pid, response) in responses {
            scripts.entry(pid).or_default().push_back(response);
        }
        Self {
            responses: Mutex::new(scripts),
            active: Mutex::new(HashMap::new()),
            liveness_probes: Mutex::new(Vec::new()),
        }
    }

    pub(crate) fn set_lifecycle(&self, identity: &ProcessIdentity, lifecycle: TargetLifecycle) {
        self.active
            .lock()
            .unwrap()
            .get_mut(&(identity.pid, identity.creation_time))
            .expect("the scripted identity must be inspected before its lifecycle changes")
            .lifecycle = lifecycle;
    }

    pub(crate) fn set_liveness(&self, identity: &ProcessIdentity, liveness: TargetLiveness) {
        self.active
            .lock()
            .unwrap()
            .get_mut(&(identity.pid, identity.creation_time))
            .expect("the scripted identity must be inspected before its liveness changes")
            .liveness = liveness;
    }

    pub(crate) fn liveness_probes(&self) -> Vec<ProcessIdentity> {
        self.liveness_probes.lock().unwrap().clone()
    }

    fn active_response(&self, identity: &ProcessIdentity) -> Option<InspectorResponse> {
        self.active
            .lock()
            .unwrap()
            .get(&(identity.pid, identity.creation_time))
            .cloned()
    }
}

impl ProcessInspector for ScriptedInspector {
    fn inspect(&self, pid: u32) -> Result<ProcessInspection, ProcessInspectionError> {
        let response = self
            .responses
            .lock()
            .unwrap()
            .get_mut(&pid)
            .and_then(VecDeque::pop_front)
            .unwrap_or_else(|| panic!("no scripted inspection response remains for PID {pid}"));
        if let Ok(inspection) = &response.inspection {
            self.active.lock().unwrap().insert(
                (inspection.identity.pid, inspection.identity.creation_time),
                response.clone(),
            );
        }
        response.inspection
    }

    fn probe_target_lifecycle(&self, identity: &ProcessIdentity) -> TargetLifecycle {
        self.active_response(identity)
            .map_or(TargetLifecycle::Unknown, |response| response.lifecycle)
    }

    fn probe_target_liveness(&self, identity: &ProcessIdentity) -> TargetLiveness {
        self.liveness_probes.lock().unwrap().push(identity.clone());
        self.active_response(identity)
            .map_or(TargetLiveness::Unknown, |response| response.liveness)
    }

    fn classify_unity_process(&self, identity: &ProcessIdentity) -> UnityProcessClassification {
        self.active_response(identity)
            .map_or(UnityProcessClassification::Unavailable, |response| {
                response.unity
            })
    }

    fn classify_private_freetype_process(
        &self,
        identity: &ProcessIdentity,
    ) -> PrivateFreeTypeClassification {
        self.active_response(identity)
            .map_or(PrivateFreeTypeClassification::Unavailable, |response| {
                response.private_freetype
            })
    }

    fn probe_image_subsystem(&self, identity: &ProcessIdentity) -> ImageSubsystem {
        self.active_response(identity)
            .map_or(ImageSubsystem::Unavailable, |response| response.subsystem)
    }

    fn probe_process_age(&self, identity: &ProcessIdentity) -> Option<Duration> {
        self.active_response(identity)
            .and_then(|response| response.process_age)
    }
}
