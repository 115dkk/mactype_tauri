#![forbid(unsafe_code)]

use crate::{ini_policy, profile::profile_structure_bytes};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PrivateFreeTypePolicy {
    skip_detected: bool,
}

impl PrivateFreeTypePolicy {
    pub fn from_profile_bytes(bytes: &[u8]) -> Self {
        let Ok(structure) = profile_structure_bytes(bytes) else {
            return Self::default();
        };
        Self {
            skip_detected: ini_policy::lookup(&structure, b"General", b"SkipPrivateFreeType")
                == Some(b"1"),
        }
    }

    pub const fn skip_detected(self) -> bool {
        self.skip_detected
    }
}
