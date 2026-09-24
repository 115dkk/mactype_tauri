//! Pure rules for the `[FontSubstitutesBold]` pair list.
//!
//! The renderer and the frontend apply the same rules: a family name loses a
//! trailing `,<digits>` charset suffix, families compare case-insensitively,
//! the last pair written for a family wins, and a broken pair line counts as
//! no pair at all.

use std::collections::BTreeMap;

/// Trims a family name and drops a trailing `,<digits>` charset suffix.
fn family_name(raw: &str) -> &str {
    let trimmed = raw.trim();
    match trimmed.rsplit_once(',') {
        Some((family, charset))
            if !charset.is_empty() && charset.bytes().all(|byte| byte.is_ascii_digit()) =>
        {
            family.trim()
        }
        _ => trimmed,
    }
}

/// Parses one `Family=BoldFamily` line. Returns `None` for a malformed line:
/// no `=`, an empty side, or a family paired with itself.
pub(super) fn parse_pair(line: &str) -> Option<(String, String)> {
    let (family, bold) = line.split_once('=')?;
    let family = family_name(family);
    let bold = family_name(bold);
    if family.is_empty() || bold.is_empty() || family.to_lowercase() == bold.to_lowercase() {
        return None;
    }
    Some((family.to_owned(), bold.to_owned()))
}

/// Valid pairs in canonical `Family=BoldFamily` form, in the order each
/// family first appears; malformed lines are dropped and the last pair
/// written for a family replaces the earlier ones, as an ini key would.
pub(super) fn canonical_pairs<'a>(lines: impl IntoIterator<Item = &'a String>) -> Vec<String> {
    let mut order = Vec::new();
    let mut latest = BTreeMap::new();
    for (family, bold) in lines.into_iter().filter_map(|line| parse_pair(line)) {
        let key = family.to_lowercase();
        if latest.insert(key.clone(), (family, bold)).is_none() {
            order.push(key);
        }
    }
    order
        .iter()
        .map(|key| {
            let (family, bold) = &latest[key];
            format!("{family}={bold}")
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lines(items: &[&str]) -> Vec<String> {
        items.iter().map(|item| (*item).to_owned()).collect()
    }

    #[test]
    fn parse_pair_normalises_and_rejects_broken_lines() {
        assert_eq!(
            parse_pair(" Pretendard Medium,129 = Pretendard Bold,129 "),
            Some(("Pretendard Medium".to_owned(), "Pretendard Bold".to_owned()))
        );
        assert_eq!(parse_pair("no separator"), None);
        assert_eq!(parse_pair("=Bold"), None);
        assert_eq!(parse_pair("Family= "), None);
        assert_eq!(parse_pair("Inter=inter,0"), None);
    }

    #[test]
    fn parse_pair_keeps_a_comma_that_is_not_a_charset_suffix() {
        assert_eq!(
            parse_pair("Arial=Odd,Name"),
            Some(("Arial".to_owned(), "Odd,Name".to_owned()))
        );
        assert_eq!(
            parse_pair("Tahoma=Odd,"),
            Some(("Tahoma".to_owned(), "Odd,".to_owned()))
        );
    }

    #[test]
    fn canonical_pairs_keep_the_last_pair_per_family_in_first_appearance_order() {
        let pairs = lines(&[
            "Inter=Inter Bold",
            "broken",
            "Noto=Noto Bold,1",
            "inter=Other",
        ]);
        assert_eq!(
            canonical_pairs(&pairs),
            vec!["inter=Other", "Noto=Noto Bold"]
        );
    }
}
