#![forbid(unsafe_code)]

use crate::profile::trim_ascii;

enum Event<'a> {
    Section(&'a [u8]),
    Entry {
        section: &'a [u8],
        key: &'a [u8],
        value: Option<&'a [u8]>,
    },
}

struct Events<'a> {
    bytes: &'a [u8],
    cursor: usize,
    section: &'a [u8],
}

impl<'a> Events<'a> {
    const fn new(bytes: &'a [u8]) -> Self {
        Self {
            bytes,
            cursor: 0,
            section: &[],
        }
    }
}

impl<'a> Iterator for Events<'a> {
    type Item = Event<'a>;

    fn next(&mut self) -> Option<Self::Item> {
        while self.cursor < self.bytes.len() {
            let remaining = &self.bytes[self.cursor..];
            let line_end = remaining
                .iter()
                .position(|byte| *byte == b'\n')
                .unwrap_or(remaining.len());
            let line = trim_ascii(&remaining[..line_end]);
            self.cursor += line_end + usize::from(line_end < remaining.len());

            if line.is_empty() || matches!(line[0], b';' | b'#') {
                continue;
            }
            if line.len() >= 3 && line[0] == b'[' && line[line.len() - 1] == b']' {
                self.section = &line[1..line.len() - 1];
                return Some(Event::Section(self.section));
            }
            if self.section.is_empty() {
                continue;
            }
            return Some(match line.iter().position(|byte| *byte == b'=') {
                Some(separator) => Event::Entry {
                    section: self.section,
                    key: trim_ascii(&line[..separator]),
                    value: Some(trim_ascii(&line[separator + 1..])),
                },
                None => Event::Entry {
                    section: self.section,
                    key: line,
                    value: None,
                },
            });
        }
        None
    }
}

pub(crate) fn scan(
    bytes: &[u8],
    mut visit_section: impl FnMut(&[u8]),
    mut visit_entry: impl FnMut(&[u8], &[u8], Option<&[u8]>),
) {
    for event in Events::new(bytes) {
        match event {
            Event::Section(section) => visit_section(section),
            Event::Entry {
                section,
                key,
                value,
            } => visit_entry(section, key, value),
        }
    }
}

pub(crate) fn lookup<'a>(bytes: &'a [u8], section: &[u8], key: &[u8]) -> Option<&'a [u8]> {
    let mut result = None;
    for event in Events::new(bytes) {
        let Event::Entry {
            section: candidate_section,
            key: candidate_key,
            value,
        } = event
        else {
            continue;
        };
        if trim_ascii(candidate_section).eq_ignore_ascii_case(section)
            && candidate_key.eq_ignore_ascii_case(key)
        {
            result = value;
        }
    }
    result
}

#[cfg(test)]
mod tests {
    use super::{lookup, scan};

    #[test]
    fn scan_handles_comments_whitespace_entries_and_duplicates() {
        let bytes = b" ; ignored\r\n# ignored too\r\n[ General ]\r\n First = one \r\nLoose.exe\r\nfirst=two\r\n";
        let mut entries = Vec::new();
        scan(
            bytes,
            |_| {},
            |section, key, value| {
                entries.push((section.to_vec(), key.to_vec(), value.map(<[u8]>::to_vec)));
            },
        );
        assert_eq!(
            entries,
            vec![
                (
                    b" General ".to_vec(),
                    b"First".to_vec(),
                    Some(b"one".to_vec())
                ),
                (b" General ".to_vec(), b"Loose.exe".to_vec(), None),
                (
                    b" General ".to_vec(),
                    b"first".to_vec(),
                    Some(b"two".to_vec())
                ),
            ]
        );
        assert_eq!(lookup(bytes, b"General", b"First"), Some(b"two".as_slice()));
    }

    #[test]
    fn lookup_rejects_missing_sections_and_stays_within_input_bounds() {
        let bytes = b"Key=before\n[Other]\nKey=value\n[General\nKey=unterminated\n";
        assert_eq!(lookup(bytes, b"General", b"Key"), None);
        assert_eq!(
            lookup(bytes, b"Other", b"Key"),
            Some(b"unterminated".as_slice())
        );
    }
}
