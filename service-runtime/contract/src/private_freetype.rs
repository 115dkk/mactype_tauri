#![forbid(unsafe_code)]

use crate::profile::{profile_structure_bytes, trim_ascii};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PrivateFreeTypePolicy {
    skip_detected: bool,
}

impl PrivateFreeTypePolicy {
    pub fn from_profile_bytes(bytes: &[u8]) -> Self {
        let Ok(structure) = profile_structure_bytes(bytes) else {
            return Self::default();
        };
        let mut policy = Self::default();
        let mut in_general = false;
        for raw_line in structure.split(|byte| *byte == b'\n') {
            let line = trim_ascii(raw_line);
            if line.is_empty() || matches!(line[0], b';' | b'#') {
                continue;
            }
            if line.len() >= 3 && line[0] == b'[' && line[line.len() - 1] == b']' {
                in_general = trim_ascii(&line[1..line.len() - 1]).eq_ignore_ascii_case(b"General");
                continue;
            }
            if !in_general {
                continue;
            }
            let Some(separator) = line.iter().position(|byte| *byte == b'=') else {
                continue;
            };
            if trim_ascii(&line[..separator]).eq_ignore_ascii_case(b"SkipPrivateFreeType") {
                policy.skip_detected = trim_ascii(&line[separator + 1..]) == b"1";
            }
        }
        policy
    }

    pub const fn skip_detected(self) -> bool {
        self.skip_detected
    }
}
