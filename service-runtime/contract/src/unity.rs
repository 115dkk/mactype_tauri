#![forbid(unsafe_code)]

use crate::profile::{profile_structure_bytes, trim_ascii};

#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum UnityFontHookMode {
    #[default]
    Off = 0,
    SelectedGames = 1,
    MostGames = 2,
    AllGames = 3,
}

impl UnityFontHookMode {
    pub const fn from_profile_value(value: u8) -> Self {
        match value {
            1 => Self::SelectedGames,
            2 => Self::MostGames,
            3 => Self::AllGames,
            _ => Self::Off,
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct UnityFontHookPolicy {
    mode: UnityFontHookMode,
    selected_games: Vec<String>,
    excluded_games: Vec<String>,
}

impl UnityFontHookPolicy {
    pub fn from_profile_bytes(bytes: &[u8]) -> Self {
        let Ok(structure) = profile_structure_bytes(bytes) else {
            return Self::default();
        };
        let mut policy = Self::default();
        let mut section = Section::Other;
        for raw_line in structure.split(|byte| *byte == b'\n') {
            let line = trim_ascii(raw_line);
            if line.is_empty() || matches!(line[0], b';' | b'#') {
                continue;
            }
            if line.len() >= 3 && line[0] == b'[' && line[line.len() - 1] == b']' {
                let name = trim_ascii(&line[1..line.len() - 1]);
                section = if name.eq_ignore_ascii_case(b"General") {
                    Section::General
                } else if name.eq_ignore_ascii_case(b"UnityInclude") {
                    Section::UnityInclude
                } else if name.eq_ignore_ascii_case(b"UnityExclude") {
                    Section::UnityExclude
                } else {
                    Section::Other
                };
                continue;
            }
            match section {
                Section::General => {
                    let Some(separator) = line.iter().position(|byte| *byte == b'=') else {
                        continue;
                    };
                    if trim_ascii(&line[..separator]).eq_ignore_ascii_case(b"UnityFontHook") {
                        let value = std::str::from_utf8(trim_ascii(&line[separator + 1..]))
                            .ok()
                            .and_then(|value| value.parse::<u8>().ok())
                            .unwrap_or_default();
                        policy.mode = UnityFontHookMode::from_profile_value(value);
                    }
                }
                Section::UnityInclude => push_game(&mut policy.selected_games, line),
                Section::UnityExclude => push_game(&mut policy.excluded_games, line),
                Section::Other => {}
            }
        }
        policy
    }

    pub const fn mode(&self) -> UnityFontHookMode {
        self.mode
    }

    pub fn applies_to(&self, executable: &str) -> bool {
        let executable = canonical_executable(executable);
        match self.mode {
            UnityFontHookMode::Off => false,
            UnityFontHookMode::SelectedGames => {
                self.selected_games.iter().any(|item| item == &executable)
            }
            UnityFontHookMode::MostGames => true,
            UnityFontHookMode::AllGames => {
                !self.excluded_games.iter().any(|item| item == &executable)
            }
        }
    }

    pub fn selected_games(&self) -> &[String] {
        &self.selected_games
    }

    pub fn excluded_games(&self) -> &[String] {
        &self.excluded_games
    }
}

#[derive(Clone, Copy)]
enum Section {
    General,
    UnityInclude,
    UnityExclude,
    Other,
}

fn push_game(destination: &mut Vec<String>, raw: &[u8]) {
    let Ok(value) = std::str::from_utf8(trim_ascii(raw)) else {
        return;
    };
    let value = canonical_executable(value);
    if !value.is_empty() && !destination.iter().any(|existing| existing == &value) {
        destination.push(value);
    }
}

fn canonical_executable(value: &str) -> String {
    value
        .trim()
        .rsplit(['\\', '/'])
        .next()
        .unwrap_or_default()
        .to_lowercase()
}
