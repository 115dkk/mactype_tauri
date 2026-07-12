use crate::generated_settings::{SettingDefinition, SettingValueType, SETTINGS};
use encoding_rs::{EUC_KR, WINDOWS_1252};
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs::{self, File},
    io::Write,
    path::{Path, PathBuf},
};

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum TextEncoding {
    Utf8,
    Utf16Le,
    Utf16Be,
    EucKr,
    Windows1252,
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum BomKind {
    None,
    Utf8,
    Utf16Le,
    Utf16Be,
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum LineEnding {
    CrLf,
    Lf,
    Cr,
}

#[derive(Clone, Debug)]
enum IniNode {
    Blank {
        raw: String,
    },
    Comment {
        raw: String,
    },
    Section {
        name: String,
        raw: String,
    },
    KeyValue {
        section: String,
        key: String,
        value: String,
        prefix: String,
        separator: String,
        suffix: String,
        raw: String,
    },
    Unknown {
        raw: String,
    },
}

impl IniNode {
    fn raw(&self) -> &str {
        match self {
            Self::Blank { raw }
            | Self::Comment { raw }
            | Self::Section { raw, .. }
            | Self::KeyValue { raw, .. }
            | Self::Unknown { raw } => raw,
        }
    }
}

#[derive(Debug)]
pub struct ProfileDocument {
    path: PathBuf,
    encoding: TextEncoding,
    bom: BomKind,
    line_ending: LineEnding,
    nodes: Vec<IniNode>,
    original_hash: [u8; 32],
    dirty_keys: BTreeSet<String>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ProfileSnapshot {
    pub path: String,
    pub encoding: TextEncoding,
    pub bom: BomKind,
    pub line_ending: LineEnding,
    pub original_hash: String,
    pub values: BTreeMap<String, f64>,
    pub dirty_keys: Vec<String>,
}

fn hash(bytes: &[u8]) -> [u8; 32] {
    Sha256::digest(bytes).into()
}

fn decode(bytes: &[u8]) -> Result<(String, TextEncoding, BomKind), String> {
    if let Some(body) = bytes.strip_prefix(&[0xEF, 0xBB, 0xBF]) {
        return String::from_utf8(body.to_vec())
            .map(|text| (text, TextEncoding::Utf8, BomKind::Utf8))
            .map_err(|error| error.to_string());
    }
    if let Some(body) = bytes.strip_prefix(&[0xFF, 0xFE]) {
        if body.len() % 2 != 0 {
            return Err("UTF-16LE profile has an odd byte length".to_owned());
        }
        let units = body
            .chunks_exact(2)
            .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
            .collect::<Vec<_>>();
        return String::from_utf16(&units)
            .map(|text| (text, TextEncoding::Utf16Le, BomKind::Utf16Le))
            .map_err(|error| error.to_string());
    }
    if let Some(body) = bytes.strip_prefix(&[0xFE, 0xFF]) {
        if body.len() % 2 != 0 {
            return Err("UTF-16BE profile has an odd byte length".to_owned());
        }
        let units = body
            .chunks_exact(2)
            .map(|pair| u16::from_be_bytes([pair[0], pair[1]]))
            .collect::<Vec<_>>();
        return String::from_utf16(&units)
            .map(|text| (text, TextEncoding::Utf16Be, BomKind::Utf16Be))
            .map_err(|error| error.to_string());
    }
    if let Ok(text) = String::from_utf8(bytes.to_vec()) {
        return Ok((text, TextEncoding::Utf8, BomKind::None));
    }
    let (korean, _, korean_errors) = EUC_KR.decode(bytes);
    if !korean_errors {
        return Ok((korean.into_owned(), TextEncoding::EucKr, BomKind::None));
    }
    let (western, _, _) = WINDOWS_1252.decode(bytes);
    Ok((
        western.into_owned(),
        TextEncoding::Windows1252,
        BomKind::None,
    ))
}

fn encode(text: &str, encoding: TextEncoding, bom: BomKind) -> Result<Vec<u8>, String> {
    let mut output = Vec::new();
    match encoding {
        TextEncoding::Utf8 => {
            if matches!(bom, BomKind::Utf8) {
                output.extend_from_slice(&[0xEF, 0xBB, 0xBF]);
            }
            output.extend_from_slice(text.as_bytes());
        }
        TextEncoding::Utf16Le | TextEncoding::Utf16Be => {
            if matches!(bom, BomKind::Utf16Le) {
                output.extend_from_slice(&[0xFF, 0xFE]);
            } else if matches!(bom, BomKind::Utf16Be) {
                output.extend_from_slice(&[0xFE, 0xFF]);
            }
            for unit in text.encode_utf16() {
                let bytes = if matches!(encoding, TextEncoding::Utf16Le) {
                    unit.to_le_bytes()
                } else {
                    unit.to_be_bytes()
                };
                output.extend_from_slice(&bytes);
            }
        }
        TextEncoding::EucKr | TextEncoding::Windows1252 => {
            let codec = if matches!(encoding, TextEncoding::EucKr) {
                EUC_KR
            } else {
                WINDOWS_1252
            };
            let (encoded, _, had_errors) = codec.encode(text);
            if had_errors {
                return Err(
                    "profile contains text that cannot be represented in its original encoding"
                        .to_owned(),
                );
            }
            output.extend_from_slice(&encoded);
        }
    }
    Ok(output)
}

fn detect_line_ending(text: &str) -> LineEnding {
    if text.contains("\r\n") {
        LineEnding::CrLf
    } else if text.contains('\n') {
        LineEnding::Lf
    } else {
        LineEnding::Cr
    }
}

fn split_lines(text: &str) -> Vec<&str> {
    let mut lines = Vec::new();
    let mut start = 0;
    for (index, character) in text.char_indices() {
        if character == '\n' || (character == '\r' && !text[index..].starts_with("\r\n")) {
            lines.push(&text[start..=index]);
            start = index + 1;
        }
    }
    if start < text.len() {
        lines.push(&text[start..]);
    }
    lines
}

fn parse_nodes(text: &str) -> Vec<IniNode> {
    let mut section = String::new();
    split_lines(text)
        .into_iter()
        .map(|line| {
            let body = line.trim_end_matches(['\r', '\n']);
            let trimmed = body.trim();
            if trimmed.is_empty() {
                return IniNode::Blank {
                    raw: line.to_owned(),
                };
            }
            if trimmed.starts_with(';') || trimmed.starts_with('#') {
                return IniNode::Comment {
                    raw: line.to_owned(),
                };
            }
            if trimmed.starts_with('[') && trimmed.ends_with(']') {
                section = trimmed[1..trimmed.len() - 1].trim().to_owned();
                return IniNode::Section {
                    name: section.clone(),
                    raw: line.to_owned(),
                };
            }
            let Some(separator) = body.find('=') else {
                return IniNode::Unknown {
                    raw: line.to_owned(),
                };
            };
            let key = body[..separator].trim();
            if key.is_empty() {
                return IniNode::Unknown {
                    raw: line.to_owned(),
                };
            }
            let after_separator = &body[separator + 1..];
            let leading = after_separator.len() - after_separator.trim_start().len();
            let value_with_suffix = &after_separator[leading..];
            let trailing = value_with_suffix.len() - value_with_suffix.trim_end().len();
            let value_end = value_with_suffix.len() - trailing;
            let newline = &line[body.len()..];
            IniNode::KeyValue {
                section: section.clone(),
                key: key.to_owned(),
                value: value_with_suffix[..value_end].to_owned(),
                prefix: body[..separator].to_owned(),
                separator: format!("={}", &after_separator[..leading]),
                suffix: format!("{}{}", &value_with_suffix[value_end..], newline),
                raw: line.to_owned(),
            }
        })
        .collect()
}

fn schema(setting_id: &str) -> Result<&'static SettingDefinition, String> {
    SETTINGS
        .iter()
        .find(|item| item.id == setting_id)
        .ok_or_else(|| format!("unknown setting id: {setting_id}"))
}

impl ProfileDocument {
    pub fn open(path: impl AsRef<Path>) -> Result<Self, String> {
        let path = path.as_ref().to_path_buf();
        let bytes = fs::read(&path).map_err(|error| error.to_string())?;
        let original_hash = hash(&bytes);
        let (text, encoding, bom) = decode(&bytes)?;
        Ok(Self {
            path,
            encoding,
            bom,
            line_ending: detect_line_ending(&text),
            nodes: parse_nodes(&text),
            original_hash,
            dirty_keys: BTreeSet::new(),
        })
    }

    pub fn snapshot(&self) -> ProfileSnapshot {
        let mut values = BTreeMap::new();
        for setting in SETTINGS {
            if let Some(value) = self
                .value(setting)
                .and_then(|value| value.parse::<f64>().ok())
            {
                values.insert(setting.id.to_owned(), value);
            } else {
                values.insert(setting.id.to_owned(), setting.default);
            }
        }
        ProfileSnapshot {
            path: self.path.to_string_lossy().into_owned(),
            encoding: self.encoding,
            bom: self.bom,
            line_ending: self.line_ending,
            original_hash: self
                .original_hash
                .iter()
                .map(|byte| format!("{byte:02x}"))
                .collect(),
            values,
            dirty_keys: self.dirty_keys.iter().cloned().collect(),
        }
    }

    fn value(&self, setting: &SettingDefinition) -> Option<&str> {
        self.nodes.iter().rev().find_map(|node| match node {
            IniNode::KeyValue {
                section,
                key,
                value,
                ..
            } if section.eq_ignore_ascii_case(setting.section)
                && key.eq_ignore_ascii_case(setting.key) =>
            {
                Some(value.as_str())
            }
            _ => None,
        })
    }

    pub fn set_value(&mut self, setting_id: &str, value: f64) -> Result<(), String> {
        let setting = schema(setting_id)?;
        if !value.is_finite() || value < setting.min || value > setting.max {
            return Err(format!(
                "{setting_id} must be between {} and {}",
                setting.min, setting.max
            ));
        }
        let rendered = if matches!(setting.value_type, SettingValueType::Integer) {
            if value.fract() != 0.0 {
                return Err(format!("{setting_id} requires an integer"));
            }
            format!("{}", value as i64)
        } else {
            let mut result = format!("{value:.6}");
            while result.contains('.') && result.ends_with('0') {
                result.pop();
            }
            if result.ends_with('.') {
                result.push('0');
            }
            result
        };
        for node in self.nodes.iter_mut().rev() {
            if let IniNode::KeyValue {
                section,
                key,
                value,
                prefix,
                separator,
                suffix,
                raw,
            } = node
            {
                if section.eq_ignore_ascii_case(setting.section)
                    && key.eq_ignore_ascii_case(setting.key)
                {
                    *value = rendered.clone();
                    *raw = format!("{prefix}{separator}{rendered}{suffix}");
                    self.dirty_keys.insert(setting_id.to_owned());
                    return Ok(());
                }
            }
        }
        let ending = match self.line_ending {
            LineEnding::CrLf => "\r\n",
            LineEnding::Lf => "\n",
            LineEnding::Cr => "\r",
        };
        let has_section = self.nodes.iter().any(|node| matches!(node, IniNode::Section { name, .. } if name.eq_ignore_ascii_case(setting.section)));
        if !has_section {
            self.nodes.push(IniNode::Section {
                name: setting.section.to_owned(),
                raw: format!("[{0}]{ending}", setting.section),
            });
        }
        let insert_at = self
            .nodes
            .iter()
            .rposition(|node| match node {
                IniNode::Section { name, .. } => name.eq_ignore_ascii_case(setting.section),
                IniNode::KeyValue { section, .. } => section.eq_ignore_ascii_case(setting.section),
                _ => false,
            })
            .map_or(self.nodes.len(), |index| index + 1);
        self.nodes.insert(
            insert_at.min(self.nodes.len()),
            IniNode::KeyValue {
                section: setting.section.to_owned(),
                key: setting.key.to_owned(),
                value: rendered.clone(),
                prefix: setting.key.to_owned(),
                separator: "=".to_owned(),
                suffix: ending.to_owned(),
                raw: format!("{}={rendered}{ending}", setting.key),
            },
        );
        self.dirty_keys.insert(setting_id.to_owned());
        Ok(())
    }

    pub fn encoded(&self) -> Result<Vec<u8>, String> {
        let text = self.nodes.iter().map(IniNode::raw).collect::<String>();
        encode(&text, self.encoding, self.bom)
    }

    pub fn save(&mut self) -> Result<(), String> {
        let disk = fs::read(&self.path).map_err(|error| error.to_string())?;
        if hash(&disk) != self.original_hash {
            return Err("profile changed on disk; reload before saving".to_owned());
        }
        let bytes = self.encoded()?;
        let file_name = self
            .path
            .file_name()
            .and_then(|name| name.to_str())
            .unwrap_or("profile.ini");
        let temporary = self
            .path
            .with_file_name(format!(".{file_name}.mactype-{}.tmp", std::process::id()));
        let backup = self.path.with_extension("ini.bak");
        let mut file = File::create(&temporary).map_err(|error| error.to_string())?;
        file.write_all(&bytes).map_err(|error| error.to_string())?;
        file.sync_all().map_err(|error| error.to_string())?;
        replace_file(&self.path, &temporary, &backup)?;
        self.original_hash = hash(&bytes);
        self.dirty_keys.clear();
        Ok(())
    }
}

#[cfg(windows)]
fn replace_file(destination: &Path, replacement: &Path, backup: &Path) -> Result<(), String> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Storage::FileSystem::{ReplaceFileW, REPLACEFILE_WRITE_THROUGH};
    let wide = |path: &Path| {
        path.as_os_str()
            .encode_wide()
            .chain(Some(0))
            .collect::<Vec<_>>()
    };
    let destination = wide(destination);
    let replacement = wide(replacement);
    let backup = wide(backup);
    let result = unsafe {
        ReplaceFileW(
            destination.as_ptr(),
            replacement.as_ptr(),
            backup.as_ptr(),
            REPLACEFILE_WRITE_THROUGH,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        )
    };
    if result == 0 {
        return Err(std::io::Error::last_os_error().to_string());
    }
    Ok(())
}

#[cfg(not(windows))]
fn replace_file(destination: &Path, replacement: &Path, backup: &Path) -> Result<(), String> {
    fs::copy(destination, backup).map_err(|error| error.to_string())?;
    fs::rename(replacement, destination).map_err(|error| error.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_profile(bytes: &[u8]) -> PathBuf {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = std::env::temp_dir().join(format!("mactype-profile-{unique}.ini"));
        fs::write(&path, bytes).unwrap();
        path
    }

    #[test]
    fn unchanged_utf8_profile_round_trips_byte_for_byte() {
        let bytes = b"\xEF\xBB\xBF; keep\r\n[FreeType]\r\nUnknown = 7\r\nNormalWeight = 2  \r\n";
        let path = temp_profile(bytes);
        let document = ProfileDocument::open(&path).unwrap();
        assert_eq!(document.encoded().unwrap(), bytes);
        let _ = fs::remove_file(path);
    }

    #[test]
    fn changing_one_key_preserves_comments_order_and_unknown_lines() {
        let bytes = b"; keep\r\n[FreeType]\r\nUnknown = 7\r\nNormalWeight = 2  \r\n# tail\r\n";
        let path = temp_profile(bytes);
        let mut document = ProfileDocument::open(&path).unwrap();
        document.set_value("normal_weight", 4.0).unwrap();
        let rendered = String::from_utf8(document.encoded().unwrap()).unwrap();
        assert_eq!(
            rendered,
            "; keep\r\n[FreeType]\r\nUnknown = 7\r\nNormalWeight = 4  \r\n# tail\r\n"
        );
        let _ = fs::remove_file(path);
    }

    #[test]
    fn detects_external_change_before_save() {
        let path = temp_profile(b"[FreeType]\nNormalWeight=0\n");
        let mut document = ProfileDocument::open(&path).unwrap();
        document.set_value("normal_weight", 3.0).unwrap();
        fs::write(&path, b"[FreeType]\nNormalWeight=9\n").unwrap();
        assert!(document.save().unwrap_err().contains("changed on disk"));
        let _ = fs::remove_file(path);
    }

    #[test]
    fn preserves_utf16le_bom_and_line_endings() {
        let text = "[FreeType]\r\nGammaValue=1.2\r\n";
        let mut bytes = vec![0xFF, 0xFE];
        for unit in text.encode_utf16() {
            bytes.extend_from_slice(&unit.to_le_bytes());
        }
        let path = temp_profile(&bytes);
        let mut document = ProfileDocument::open(&path).unwrap();
        document.set_value("gamma_value", 1.4).unwrap();
        let encoded = document.encoded().unwrap();
        assert!(encoded.starts_with(&[0xFF, 0xFE]));
        let _ = fs::remove_file(path);
    }

    #[test]
    fn new_section_is_inserted_before_its_first_key() {
        let path = temp_profile(b"; empty profile\n");
        let mut document = ProfileDocument::open(&path).unwrap();
        document.set_value("normal_weight", 5.0).unwrap();
        document.set_value("normal_weight", 6.0).unwrap();
        let rendered = String::from_utf8(document.encoded().unwrap()).unwrap();
        assert_eq!(rendered, "; empty profile\n[FreeType]\nNormalWeight=6\n");
        let _ = fs::remove_file(path);
    }
}
