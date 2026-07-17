use std::time::Duration;

use crate::ProcessIdentity;

pub const MAX_TRACKED_PROCESS_RESULTS: usize = 4_096;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RetryPolicy {
    pub max_attempts: u8,
    pub initial_delay: Duration,
    pub max_delay: Duration,
}

impl Default for RetryPolicy {
    fn default() -> Self {
        Self {
            max_attempts: 3,
            initial_delay: Duration::from_millis(25),
            max_delay: Duration::from_millis(250),
        }
    }
}

pub trait RetryScheduler {
    fn wait(&self, delay: Duration) -> bool;
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProcessOutcome {
    Injected,
    Skipped,
    Duplicate,
    Rejected,
    RetryExhausted,
    Cancelled,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProcessAttemptRecord {
    pub identity: ProcessIdentity,
    pub runtime_generation_id: String,
    pub outcome: ProcessOutcome,
    pub attempts: u8,
    pub code: String,
    pub win32_error: Option<u32>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SessionChange {
    pub event_type: u32,
    pub session_id: u32,
}

impl SessionChange {
    const OVERFLOW_EVENT_TYPE: u32 = u32::MAX;

    pub const fn overflow() -> Self {
        Self {
            event_type: Self::OVERFLOW_EVENT_TYPE,
            session_id: 0,
        }
    }

    pub const fn is_overflow(self) -> bool {
        self.event_type == Self::OVERFLOW_EVENT_TYPE
    }
}
