#![forbid(unsafe_code)]

use crate::{
    ini_policy,
    profile::{profile_structure_bytes, trim_ascii},
};

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
        ini_policy::scan(
            &structure,
            |_| {},
            |section, key, value| {
                let section = trim_ascii(section);
                if section.eq_ignore_ascii_case(b"General")
                    && key.eq_ignore_ascii_case(b"UnityFontHook")
                {
                    let mode = value
                        .and_then(|value| std::str::from_utf8(value).ok())
                        .and_then(|value| value.parse::<u8>().ok())
                        .unwrap_or_default();
                    policy.mode = UnityFontHookMode::from_profile_value(mode);
                } else if section.eq_ignore_ascii_case(b"UnityInclude") && value.is_none() {
                    push_game(&mut policy.selected_games, key);
                } else if section.eq_ignore_ascii_case(b"UnityExclude") && value.is_none() {
                    push_game(&mut policy.excluded_games, key);
                }
            },
        );
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
