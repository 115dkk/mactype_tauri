#![forbid(unsafe_code)]

use crate::{ini_policy, profile::profile_structure_bytes};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ConsoleProcessPolicy {
    skip_console: bool,
}

impl ConsoleProcessPolicy {
    pub fn from_profile_bytes(bytes: &[u8]) -> Self {
        let Ok(structure) = profile_structure_bytes(bytes) else {
            return Self::default();
        };
        Self {
            skip_console: ini_policy::lookup(&structure, b"General", b"SkipConsoleProcesses")
                == Some(b"1"),
        }
    }

    pub const fn skip_console(self) -> bool {
        self.skip_console
    }
}
