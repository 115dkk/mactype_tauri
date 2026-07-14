use crate::generated_settings::{SettingDefinition, SettingValueType, SETTINGS};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs::{self, File},
    io::Write,
    path::{Path, PathBuf},
};

mod codec;

use codec::{
    decode, detect_line_ending, encode, encode_preserving_legacy_lines, original_legacy_lines,
    split_lines, OriginalLegacyLines,
};

#[cfg(test)]
use encoding_rs::{Encoding, BIG5, GB18030, SHIFT_JIS};

#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum TextEncoding {
    Utf8,
    Utf16Le,
    Utf16Be,
    EucKr,
    Gb18030,
    Big5,
    ShiftJis,
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
        section: String,
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
            | Self::Unknown { raw, .. } => raw,
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
    original_legacy_lines: Option<OriginalLegacyLines>,
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
    pub individuals: Vec<IndividualSetting>,
    pub lists: ProfileLists,
    pub advanced: AdvancedProfile,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct IndividualSetting {
    pub font_face: String,
    pub values: Vec<Option<i32>>,
}

#[derive(Clone, Debug, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ProfileLists {
    pub exclude_fonts: Vec<String>,
    pub include_fonts: Vec<String>,
    pub exclude_modules: Vec<String>,
    pub include_modules: Vec<String>,
    pub unload_dlls: Vec<String>,
    pub exclude_substitution_modules: Vec<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ShadowSetting {
    pub offset_x: i32,
    pub offset_y: i32,
    pub dark_alpha: i32,
    pub dark_color: u32,
    pub light_alpha: i32,
    pub light_color: u32,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AdvancedProfile {
    pub shadow: Option<ShadowSetting>,
    pub lcd_filter_weight: Option<Vec<i32>>,
    pub pixel_layout: Option<Vec<i32>>,
    pub display_affinity: Vec<i32>,
    pub font_substitutes: Vec<String>,
    pub infinality_gamma_correction: Vec<i32>,
    pub infinality_filter_params: Vec<i32>,
}

fn hash(bytes: &[u8]) -> [u8; 32] {
    Sha256::digest(bytes).into()
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
                    section: section.clone(),
                    raw: line.to_owned(),
                };
            };
            let key = body[..separator].trim();
            if key.is_empty() {
                return IniNode::Unknown {
                    section: section.clone(),
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

fn validate_entry(value: &str, label: &str) -> Result<(), String> {
    if value.trim().is_empty()
        || value
            .chars()
            .any(|character| matches!(character, '\r' | '\n'))
        || value.starts_with(['[', ';', '#'])
    {
        return Err(format!(
            "{label} is empty or contains an unsupported character"
        ));
    }
    Ok(())
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
            original_legacy_lines: original_legacy_lines(&bytes, &text, encoding),
            dirty_keys: BTreeSet::new(),
        })
    }

    pub fn path(&self) -> &Path {
        &self.path
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
            individuals: self.individuals(),
            lists: ProfileLists {
                exclude_fonts: self.list_entries("Exclude"),
                include_fonts: self.list_entries("Include"),
                exclude_modules: self.list_entries("ExcludeModule"),
                include_modules: self.list_entries("IncludeModule"),
                unload_dlls: self.list_entries("UnloadDLL"),
                exclude_substitution_modules: self.list_entries("ExcludeSub"),
            },
            advanced: self.advanced(),
        }
    }

    fn raw_value(&self, section_name: &str, key_name: &str) -> Option<&str> {
        let sections: &[&str] = if section_name.eq_ignore_ascii_case("General") {
            &["FreeType", "General"]
        } else {
            std::slice::from_ref(&section_name)
        };
        sections.iter().find_map(|target| {
            self.nodes.iter().rev().find_map(|node| match node {
                IniNode::KeyValue {
                    section,
                    key,
                    value,
                    ..
                } if section.eq_ignore_ascii_case(target) && key.eq_ignore_ascii_case(key_name) => {
                    Some(value.as_str())
                }
                _ => None,
            })
        })
    }

    fn parse_vector(&self, section: &str, key: &str, length: usize) -> Option<Vec<i32>> {
        let values = self
            .raw_value(section, key)?
            .split(|character: char| character == ',' || character.is_whitespace())
            .filter(|part| !part.is_empty())
            .map(|part| part.trim().parse::<i32>().ok())
            .collect::<Option<Vec<_>>>()?;
        (values.len() == length).then_some(values)
    }

    fn advanced(&self) -> AdvancedProfile {
        let shadow = self
            .parse_vector("General", "Shadow", 6)
            .map(|values| ShadowSetting {
                offset_x: values[0],
                offset_y: values[1],
                dark_alpha: values[2],
                dark_color: u32::from_str_radix(
                    self.raw_value("General", "Shadow")
                        .unwrap()
                        .split(',')
                        .nth(3)
                        .unwrap()
                        .trim()
                        .trim_start_matches("0x"),
                    16,
                )
                .unwrap_or(0),
                light_alpha: values[4],
                light_color: u32::from_str_radix(
                    self.raw_value("General", "Shadow")
                        .unwrap()
                        .split(',')
                        .nth(5)
                        .unwrap()
                        .trim()
                        .trim_start_matches("0x"),
                    16,
                )
                .unwrap_or(0),
            })
            .or_else(|| {
                let raw = self.raw_value("General", "Shadow")?;
                let parts = raw.split(',').map(str::trim).collect::<Vec<_>>();
                if parts.len() < 3 {
                    return None;
                }
                Some(ShadowSetting {
                    offset_x: parts[0].parse().ok()?,
                    offset_y: parts[1].parse().ok()?,
                    dark_alpha: parts[2].parse().ok()?,
                    dark_color: parts
                        .get(3)
                        .and_then(|value| {
                            u32::from_str_radix(value.trim_start_matches("0x"), 16).ok()
                        })
                        .unwrap_or(0),
                    light_alpha: parts
                        .get(4)
                        .and_then(|value| value.parse().ok())
                        .unwrap_or_else(|| parts[2].parse().unwrap_or(0)),
                    light_color: parts
                        .get(5)
                        .and_then(|value| {
                            u32::from_str_radix(value.trim_start_matches("0x"), 16).ok()
                        })
                        .unwrap_or(0),
                })
            });
        let parse_or = |key: &str, length: usize, default: &[i32]| {
            self.parse_vector("Infinality", key, length)
                .unwrap_or_else(|| default.to_vec())
        };
        AdvancedProfile {
            shadow,
            lcd_filter_weight: self.parse_vector("General", "LcdFilterWeight", 5),
            pixel_layout: self.parse_vector("General", "PixelLayout", 6),
            display_affinity: self
                .raw_value("General", "DisplayAffinity")
                .map(|value| {
                    value
                        .split(',')
                        .filter_map(|part| part.trim().parse().ok())
                        .collect()
                })
                .unwrap_or_default(),
            font_substitutes: self.list_entries("FontSubstitutes"),
            infinality_gamma_correction: parse_or("INFINALITY_FT_GAMMA_CORRECTION", 2, &[0, 100]),
            infinality_filter_params: parse_or(
                "INFINALITY_FT_FILTER_PARAMS",
                5,
                &[11, 22, 38, 22, 11],
            ),
        }
    }

    fn individuals(&self) -> Vec<IndividualSetting> {
        self.nodes
            .iter()
            .filter_map(|node| match node {
                IniNode::KeyValue {
                    section,
                    key,
                    value,
                    ..
                } if section.eq_ignore_ascii_case("Individual") => Some(IndividualSetting {
                    font_face: key.to_owned(),
                    values: value
                        .split(',')
                        .take(6)
                        .map(|part| {
                            let trimmed = part.trim();
                            if trimmed.is_empty() {
                                None
                            } else {
                                trimmed.parse::<i32>().ok()
                            }
                        })
                        .chain(std::iter::repeat(None))
                        .take(6)
                        .collect(),
                }),
                _ => None,
            })
            .collect()
    }

    fn list_entries(&self, target: &str) -> Vec<String> {
        self.nodes
            .iter()
            .filter_map(|node| match node {
                IniNode::Unknown { section, raw } if section.eq_ignore_ascii_case(target) => {
                    let value = raw.trim();
                    (!value.is_empty()).then(|| value.to_owned())
                }
                IniNode::KeyValue {
                    section,
                    key,
                    value,
                    ..
                } if section.eq_ignore_ascii_case(target) => Some(format!("{key}={value}")),
                _ => None,
            })
            .collect()
    }

    fn ending(&self) -> &'static str {
        match self.line_ending {
            LineEnding::CrLf => "\r\n",
            LineEnding::Lf => "\n",
            LineEnding::Cr => "\r",
        }
    }

    fn value(&self, setting: &SettingDefinition) -> Option<&str> {
        self.raw_value(setting.section, setting.key)
    }

    fn set_raw_value(
        &mut self,
        section_name: &str,
        key_name: &str,
        value: Option<String>,
        dirty_key: &str,
    ) {
        let sections: &[&str] = if section_name.eq_ignore_ascii_case("General") {
            &["FreeType", "General"]
        } else {
            std::slice::from_ref(&section_name)
        };
        if value.is_none() {
            self.nodes.retain(|node| !matches!(node, IniNode::KeyValue { section, key, .. } if sections.iter().any(|target| section.eq_ignore_ascii_case(target)) && key.eq_ignore_ascii_case(key_name)));
            self.dirty_keys.insert(dirty_key.to_owned());
            return;
        }
        let rendered = value.unwrap();
        let target_section = sections.iter().find(|target| self.nodes.iter().any(|node| matches!(node, IniNode::KeyValue { section, key, .. } if section.eq_ignore_ascii_case(target) && key.eq_ignore_ascii_case(key_name)))).copied().unwrap_or(section_name);
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
                if section.eq_ignore_ascii_case(target_section)
                    && key.eq_ignore_ascii_case(key_name)
                {
                    *value = rendered.clone();
                    *raw = format!("{prefix}{separator}{rendered}{suffix}");
                    self.dirty_keys.insert(dirty_key.to_owned());
                    return;
                }
            }
        }
        let ending = self.ending();
        if !self.nodes.iter().any(|node| matches!(node, IniNode::Section { name, .. } if name.eq_ignore_ascii_case(target_section))) {
            self.nodes.push(IniNode::Section { name: target_section.to_owned(), raw: format!("[{target_section}]{ending}") });
        }
        let insert_at = self
            .nodes
            .iter()
            .rposition(|node| match node {
                IniNode::Section { name, .. } => name.eq_ignore_ascii_case(target_section),
                IniNode::KeyValue { section, .. } | IniNode::Unknown { section, .. } => {
                    section.eq_ignore_ascii_case(target_section)
                }
                _ => false,
            })
            .map_or(self.nodes.len(), |index| index + 1);
        self.nodes.insert(
            insert_at,
            IniNode::KeyValue {
                section: target_section.to_owned(),
                key: key_name.to_owned(),
                value: rendered.clone(),
                prefix: key_name.to_owned(),
                separator: "=".to_owned(),
                suffix: ending.to_owned(),
                raw: format!("{key_name}={rendered}{ending}"),
            },
        );
        self.dirty_keys.insert(dirty_key.to_owned());
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
        self.set_raw_value(setting.section, setting.key, Some(rendered), setting_id);
        Ok(())
    }

    pub fn set_advanced(&mut self, advanced: AdvancedProfile) -> Result<(), String> {
        let vector = |values: &[i32],
                      length: usize,
                      min: i32,
                      max: i32,
                      name: &str|
         -> Result<String, String> {
            if values.len() != length || values.iter().any(|value| *value < min || *value > max) {
                return Err(format!(
                    "{name} requires {length} values between {min} and {max}"
                ));
            }
            Ok(values
                .iter()
                .map(i32::to_string)
                .collect::<Vec<_>>()
                .join(","))
        };
        let AdvancedProfile {
            shadow,
            lcd_filter_weight,
            pixel_layout,
            display_affinity,
            font_substitutes,
            infinality_gamma_correction,
            infinality_filter_params,
        } = advanced;
        let shadow = shadow
            .map(|value| {
                if !(0..=255).contains(&value.dark_alpha)
                    || !(0..=255).contains(&value.light_alpha)
                    || value.dark_color > 0xFFFFFF
                    || value.light_color > 0xFFFFFF
                {
                    return Err("shadow alpha or color is outside its supported range".to_owned());
                }
                Ok(format!(
                    "{},{},{},{:06X},{},{:06X}",
                    value.offset_x,
                    value.offset_y,
                    value.dark_alpha,
                    value.dark_color,
                    value.light_alpha,
                    value.light_color
                ))
            })
            .transpose()?;
        let lcd = lcd_filter_weight
            .as_deref()
            .map(|values| vector(values, 5, 0, 255, "LCD filter weight"))
            .transpose()?;
        let pixel = pixel_layout
            .as_deref()
            .map(|values| vector(values, 6, -128, 127, "pixel layout"))
            .transpose()?;
        if display_affinity
            .iter()
            .any(|value| !(0..=255).contains(value))
        {
            return Err("display affinity IDs must be between 0 and 255".to_owned());
        }
        let affinity = (!display_affinity.is_empty()).then(|| {
            display_affinity
                .iter()
                .map(i32::to_string)
                .collect::<Vec<_>>()
                .join(",")
        });
        for mapping in &font_substitutes {
            let Some((source, replacement)) = mapping.split_once('=') else {
                return Err("font substitutions must use Source font=Replacement font".to_owned());
            };
            if source.trim().is_empty() || replacement.trim().is_empty() {
                return Err(
                    "font substitutions require both source and replacement fonts".to_owned(),
                );
            }
        }
        let gamma = vector(
            &infinality_gamma_correction,
            2,
            -1000,
            1000,
            "Infinality gamma correction",
        )?
        .replace(',', " ");
        let filter = vector(
            &infinality_filter_params,
            5,
            0,
            255,
            "Infinality filter parameters",
        )?
        .replace(',', " ");

        // Apply only after every field has been validated so a rejected edit cannot
        // leave an in-memory profile partially changed.
        self.set_raw_value("General", "Shadow", shadow, "advanced:shadow");
        self.set_raw_value(
            "General",
            "LcdFilterWeight",
            lcd,
            "advanced:lcdFilterWeight",
        );
        self.set_raw_value("General", "PixelLayout", pixel, "advanced:pixelLayout");
        self.set_raw_value(
            "General",
            "DisplayAffinity",
            affinity,
            "advanced:displayAffinity",
        );
        self.set_list("fontSubstitutes", font_substitutes)?;
        self.set_raw_value(
            "Infinality",
            "INFINALITY_FT_GAMMA_CORRECTION",
            Some(gamma),
            "advanced:infinalityGammaCorrection",
        );
        self.set_raw_value(
            "Infinality",
            "INFINALITY_FT_FILTER_PARAMS",
            Some(filter),
            "advanced:infinalityFilterParams",
        );
        Ok(())
    }

    fn section_range(&mut self, section: &str) -> std::ops::Range<usize> {
        let start = self
            .nodes
            .iter()
            .position(|node| matches!(node, IniNode::Section { name, .. } if name.eq_ignore_ascii_case(section)))
            .unwrap_or_else(|| {
                let ending = self.ending();
                self.nodes.push(IniNode::Section {
                    name: section.to_owned(),
                    raw: format!("[{section}]{ending}"),
                });
                self.nodes.len() - 1
            });
        let end = self.nodes[start + 1..]
            .iter()
            .position(|node| matches!(node, IniNode::Section { .. }))
            .map_or(self.nodes.len(), |offset| start + 1 + offset);
        start + 1..end
    }

    pub fn set_individuals(&mut self, entries: Vec<IndividualSetting>) -> Result<(), String> {
        let bounds = [(0, 2), (-1, 6), (-64, 64), (-32, 32), (-32, 32), (0, 1)];
        let mut seen = BTreeSet::new();
        for entry in &entries {
            validate_entry(&entry.font_face, "font face")?;
            if entry.font_face.contains('=') {
                return Err("font face cannot contain '='".to_owned());
            }
            if !seen.insert(entry.font_face.to_lowercase()) {
                return Err(format!("duplicate font face: {}", entry.font_face));
            }
            if entry.values.len() != 6 {
                return Err("individual font settings require exactly six values".to_owned());
            }
            for (index, value) in entry.values.iter().enumerate() {
                if let Some(value) = value {
                    let (minimum, maximum) = bounds[index];
                    if *value < minimum || *value > maximum {
                        return Err(format!(
                            "{} value {} must be between {minimum} and {maximum}",
                            entry.font_face,
                            index + 1
                        ));
                    }
                }
            }
        }
        let range = self.section_range("Individual");
        let insert_at = range.start;
        let mut replacement = self
            .nodes
            .drain(range)
            .filter(|node| {
                !matches!(node, IniNode::KeyValue { section, .. } if section.eq_ignore_ascii_case("Individual"))
            })
            .collect::<Vec<_>>();
        let ending = self.ending();
        replacement.extend(entries.into_iter().map(|entry| {
            let value = entry
                .values
                .iter()
                .map(|value| value.map_or_else(String::new, |value| value.to_string()))
                .collect::<Vec<_>>()
                .join(",");
            IniNode::KeyValue {
                section: "Individual".to_owned(),
                key: entry.font_face.clone(),
                value: value.clone(),
                prefix: entry.font_face.clone(),
                separator: "=".to_owned(),
                suffix: ending.to_owned(),
                raw: format!("{}={value}{ending}", entry.font_face),
            }
        }));
        self.nodes.splice(insert_at..insert_at, replacement);
        self.dirty_keys.insert("section:Individual".to_owned());
        Ok(())
    }

    pub fn set_list(&mut self, kind: &str, entries: Vec<String>) -> Result<(), String> {
        let section = match kind {
            "excludeFonts" => "Exclude",
            "includeFonts" => "Include",
            "excludeModules" => "ExcludeModule",
            "includeModules" => "IncludeModule",
            "unloadDlls" => "UnloadDLL",
            "excludeSubstitutionModules" => "ExcludeSub",
            "fontSubstitutes" => "FontSubstitutes",
            _ => return Err(format!("unknown profile list: {kind}")),
        };
        let mut normalized = Vec::new();
        let mut seen = BTreeSet::new();
        for entry in entries {
            let entry = entry.trim().to_owned();
            validate_entry(&entry, "list entry")?;
            if seen.insert(entry.to_lowercase()) {
                normalized.push(entry);
            }
        }
        let range = self.section_range(section);
        let insert_at = range.start;
        let mut replacement = self
            .nodes
            .drain(range)
            .filter(|node| match node {
                IniNode::Unknown {
                    section: item_section,
                    ..
                }
                | IniNode::KeyValue {
                    section: item_section,
                    ..
                } => !item_section.eq_ignore_ascii_case(section),
                _ => true,
            })
            .collect::<Vec<_>>();
        let ending = self.ending();
        replacement.extend(normalized.into_iter().map(|entry| IniNode::Unknown {
            section: section.to_owned(),
            raw: format!("{entry}{ending}"),
        }));
        self.nodes.splice(insert_at..insert_at, replacement);
        self.dirty_keys.insert(format!("section:{section}"));
        Ok(())
    }

    pub fn duplicate_in(&self, directory: &Path, name: &str) -> Result<Self, String> {
        let stem = name.trim().trim_end_matches(".ini");
        validate_entry(stem, "profile name")?;
        if stem
            .chars()
            .any(|character| "<>:\"/\\|?*".contains(character))
        {
            return Err("profile name contains a Windows-reserved character".to_owned());
        }
        fs::create_dir_all(directory).map_err(|error| error.to_string())?;
        let destination = directory.join(format!("{stem}.ini"));
        if destination.exists() {
            return Err("a profile with that name already exists".to_owned());
        }
        let bytes = self.encoded()?;
        let mut output = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&destination)
            .map_err(|error| error.to_string())?;
        output
            .write_all(&bytes)
            .map_err(|error| error.to_string())?;
        output.sync_all().map_err(|error| error.to_string())?;
        Self::open(destination)
    }

    pub fn encoded(&self) -> Result<Vec<u8>, String> {
        let Some(original_lines) = &self.original_legacy_lines else {
            let text = self.nodes.iter().map(IniNode::raw).collect::<String>();
            return encode(&text, self.encoding, self.bom);
        };
        encode_preserving_legacy_lines(
            self.nodes.iter().map(IniNode::raw),
            original_lines,
            self.encoding,
        )
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
        drop(file);
        replace_file(&self.path, &temporary, &backup)?;
        self.original_hash = hash(&bytes);
        let text = self.nodes.iter().map(IniNode::raw).collect::<String>();
        self.original_legacy_lines = original_legacy_lines(&bytes, &text, self.encoding);
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
        let bytes = b"\xEF\xBB\xBF; keep\r\n[General]\r\nUnknown = 7\r\nNormalWeight = 2  \r\n";
        let path = temp_profile(bytes);
        let document = ProfileDocument::open(&path).unwrap();
        assert_eq!(document.encoded().unwrap(), bytes);
        let _ = fs::remove_file(path);
    }

    #[test]
    fn changing_one_key_preserves_comments_order_and_unknown_lines() {
        let bytes = b"; keep\r\n[General]\r\nUnknown = 7\r\nNormalWeight = 2  \r\n# tail\r\n";
        let path = temp_profile(bytes);
        let mut document = ProfileDocument::open(&path).unwrap();
        document.set_value("normal_weight", 4.0).unwrap();
        let rendered = String::from_utf8(document.encoded().unwrap()).unwrap();
        assert_eq!(
            rendered,
            "; keep\r\n[General]\r\nUnknown = 7\r\nNormalWeight = 4  \r\n# tail\r\n"
        );
        let _ = fs::remove_file(path);
    }

    #[test]
    fn detects_external_change_before_save() {
        let path = temp_profile(b"[General]\nNormalWeight=0\n");
        let mut document = ProfileDocument::open(&path).unwrap();
        document.set_value("normal_weight", 3.0).unwrap();
        fs::write(&path, b"[General]\nNormalWeight=9\n").unwrap();
        assert!(document.save().unwrap_err().contains("changed on disk"));
        let _ = fs::remove_file(path);
    }

    #[test]
    fn preserves_utf16le_bom_and_line_endings() {
        let text = "[General]\r\nGammaValue=1.2\r\n";
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
        assert_eq!(rendered, "; empty profile\n[General]\nNormalWeight=6\n");
        let _ = fs::remove_file(path);
    }

    #[test]
    fn edits_individual_fonts_and_lists_without_dropping_comments() {
        let path = temp_profile(
            b"[Individual]\n; keep\nSegoe UI=1,2,3,4,5,1\n[Exclude]\n; fonts\nTahoma\n",
        );
        let mut document = ProfileDocument::open(&path).unwrap();
        document
            .set_individuals(vec![IndividualSetting {
                font_face: "Malgun Gothic".to_owned(),
                values: vec![Some(1), Some(2), None, Some(4), None, Some(1)],
            }])
            .unwrap();
        document
            .set_list("excludeFonts", vec!["Arial".to_owned(), "Arial".to_owned()])
            .unwrap();
        let rendered = String::from_utf8(document.encoded().unwrap()).unwrap();
        assert!(rendered.contains("; keep\nMalgun Gothic=1,2,,4,,1\n"));
        assert!(rendered.contains("; fonts\nArial\n"));
        assert!(!rendered.contains("Tahoma"));
        let _ = fs::remove_file(path);
    }

    #[test]
    fn duplicate_preserves_encoded_profile_and_refuses_overwrite() {
        let path = temp_profile(b"[General]\nNormalWeight=2\n");
        let document = ProfileDocument::open(&path).unwrap();
        let name = format!(
            "mactype-copy-{}",
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        );
        let parent = path.parent().unwrap();
        let mut copy = document.duplicate_in(parent, &name).unwrap();
        assert_eq!(copy.encoded().unwrap(), document.encoded().unwrap());
        assert!(document
            .duplicate_in(parent, &name)
            .unwrap_err()
            .contains("already exists"));
        copy.set_value("normal_weight", 7.0).unwrap();
        copy.save().unwrap();
        let reopened = ProfileDocument::open(&copy.path).unwrap();
        assert_eq!(reopened.snapshot().values.get("normal_weight"), Some(&7.0));
        let _ = fs::remove_file(copy.path);
        let _ = fs::remove_file(path);
    }

    fn legacy_round_trip(codec: &'static Encoding, expected: TextEncoding, comment: &str) {
        let source = format!("; {comment}\r\n[General]\r\nNormalWeight=2\r\nUnknown=유지\r\n")
            .replace("Unknown=유지\r\n", "Unknown=keep\r\n");
        let (bytes, _, had_errors) = codec.encode(&source);
        assert!(!had_errors);
        let path = temp_profile(bytes.as_ref());
        let mut document = ProfileDocument::open(&path).unwrap();
        assert_eq!(document.encoding, expected, "decoded text: {source}");
        assert_eq!(document.encoded().unwrap(), bytes.as_ref());
        document.set_value("normal_weight", 7.0).unwrap();
        let changed = document.encoded().unwrap();
        let (decoded, _, decode_errors) = codec.decode(&changed);
        assert!(!decode_errors);
        assert!(decoded.contains(comment));
        assert!(decoded.contains("NormalWeight=7"));
        fs::write(&path, &changed).unwrap();
        let reopened = ProfileDocument::open(&path).unwrap();
        assert_eq!(reopened.encoding, expected);
        assert_eq!(reopened.encoded().unwrap(), changed);
        let _ = fs::remove_file(path);
    }

    #[test]
    fn gb18030_profile_round_trips_after_edit() {
        legacy_round_trip(GB18030, TextEncoding::Gb18030, "简体中文配置与字体设置");
    }

    #[test]
    fn big5_profile_round_trips_after_edit() {
        legacy_round_trip(BIG5, TextEncoding::Big5, "繁體中文設定與字型調整");
    }

    #[test]
    fn shift_jis_profile_round_trips_after_edit() {
        legacy_round_trip(SHIFT_JIS, TextEncoding::ShiftJis, "日本語プロファイル設定");
    }

    #[test]
    fn advanced_profile_and_freetype_precedence_round_trip() {
        let source = b"[FreeType]\r\nNormalWeight=3\r\nLcdFilterWeight=8,77,86,77,8\r\nPixelLayout=-21,0,0,0,21,0\r\n[General]\r\nNormalWeight=2\r\nShadow=1,2,4,112233,5,AABBCC\r\nDisplayAffinity=0,2\r\n[FontSubstitutes]\r\nArial=Segoe UI\r\n[Infinality]\r\nINFINALITY_FT_GAMMA_CORRECTION=0 100\r\nINFINALITY_FT_FILTER_PARAMS=11 22 38 22 11\r\n";
        let path = temp_profile(source);
        let mut document = ProfileDocument::open(&path).unwrap();
        let snapshot = document.snapshot();
        assert_eq!(snapshot.values.get("normal_weight"), Some(&3.0));
        assert_eq!(
            snapshot.advanced.lcd_filter_weight,
            Some(vec![8, 77, 86, 77, 8])
        );
        assert_eq!(
            snapshot.advanced.pixel_layout,
            Some(vec![-21, 0, 0, 0, 21, 0])
        );
        assert_eq!(snapshot.advanced.display_affinity, vec![0, 2]);
        assert_eq!(snapshot.advanced.font_substitutes, vec!["Arial=Segoe UI"]);
        assert_eq!(snapshot.advanced.infinality_gamma_correction, vec![0, 100]);
        assert_eq!(
            snapshot.advanced.infinality_filter_params,
            vec![11, 22, 38, 22, 11]
        );
        document.set_value("normal_weight", 7.0).unwrap();
        document
            .set_advanced(AdvancedProfile {
                shadow: Some(ShadowSetting {
                    offset_x: -2,
                    offset_y: 3,
                    dark_alpha: 6,
                    dark_color: 0x010203,
                    light_alpha: 7,
                    light_color: 0xA0B0C0,
                }),
                lcd_filter_weight: Some(vec![1, 2, 3, 4, 5]),
                pixel_layout: Some(vec![-20, 0, 0, 0, 20, 0]),
                display_affinity: vec![1, 3],
                font_substitutes: vec!["Tahoma=Segoe UI".to_owned()],
                infinality_gamma_correction: vec![5, 95],
                infinality_filter_params: vec![10, 20, 40, 20, 10],
            })
            .unwrap();
        document
            .set_list("unloadDlls", vec!["example.dll".to_owned()])
            .unwrap();
        document
            .set_list("excludeSubstitutionModules", vec!["legacy.exe".to_owned()])
            .unwrap();
        let rendered = String::from_utf8(document.encoded().unwrap()).unwrap();
        assert!(rendered.contains("[FreeType]\r\nNormalWeight=7"));
        assert!(rendered.contains("NormalWeight=2"));
        assert!(rendered.contains("Shadow=-2,3,6,010203,7,A0B0C0"));
        assert!(rendered.contains("LcdFilterWeight=1,2,3,4,5"));
        assert!(rendered.contains("PixelLayout=-20,0,0,0,20,0"));
        assert!(rendered.contains("DisplayAffinity=1,3"));
        assert!(rendered.contains("Tahoma=Segoe UI"));
        assert!(rendered.contains("INFINALITY_FT_GAMMA_CORRECTION=5 95"));
        assert!(rendered.contains("INFINALITY_FT_FILTER_PARAMS=10 20 40 20 10"));
        assert!(rendered.contains("[UnloadDLL]\r\nexample.dll"));
        assert!(rendered.contains("[ExcludeSub]\r\nlegacy.exe"));
        let _ = fs::remove_file(path);
    }

    #[test]
    fn rejected_advanced_edit_is_transactional() {
        let path = temp_profile(b"[General]\r\nShadow=1,2,3,010203,4,A0B0C0\r\n");
        let mut document = ProfileDocument::open(&path).unwrap();
        let before = document.encoded().unwrap();
        let mut advanced = document.snapshot().advanced;
        advanced.shadow = None;
        advanced.font_substitutes = vec!["missing separator".to_owned()];
        assert!(document.set_advanced(advanced).is_err());
        assert_eq!(document.encoded().unwrap(), before);
        let _ = fs::remove_file(path);
    }
}
