#[cfg(windows)]
use std::collections::BTreeSet;

#[cfg(windows)]
fn decode_face_name(face: &[u16; 32]) -> Option<String> {
    let length = face
        .iter()
        .position(|value| *value == 0)
        .unwrap_or(face.len());
    let name = String::from_utf16_lossy(&face[..length]).trim().to_owned();
    (!name.is_empty() && !name.starts_with('@')).then_some(name)
}

#[cfg(windows)]
pub fn installed_families() -> Result<Vec<String>, String> {
    use windows_sys::Win32::{
        Foundation::LPARAM,
        Graphics::Gdi::{
            CreateCompatibleDC, DeleteDC, EnumFontFamiliesExW, DEFAULT_CHARSET, LOGFONTW,
            TEXTMETRICW,
        },
    };

    unsafe extern "system" fn collect_family(
        logfont: *const LOGFONTW,
        _metrics: *const TEXTMETRICW,
        _font_type: u32,
        context: LPARAM,
    ) -> i32 {
        if let Some(name) = logfont
            .as_ref()
            .and_then(|font| decode_face_name(&font.lfFaceName))
        {
            let families = &mut *(context as *mut BTreeSet<String>);
            families.insert(name);
        }
        1
    }

    let device_context = unsafe { CreateCompatibleDC(std::ptr::null_mut()) };
    if device_context.is_null() {
        return Err(std::io::Error::last_os_error().to_string());
    }
    let request = LOGFONTW {
        lfCharSet: DEFAULT_CHARSET,
        ..LOGFONTW::default()
    };
    let mut families = BTreeSet::new();
    unsafe {
        EnumFontFamiliesExW(
            device_context,
            &request,
            Some(collect_family),
            (&raw mut families) as LPARAM,
            0,
        );
        DeleteDC(device_context);
    }
    if families.is_empty() {
        Err("Windows did not report any installed font families".to_owned())
    } else {
        Ok(families.into_iter().collect())
    }
}

#[cfg(not(windows))]
pub fn installed_families() -> Result<Vec<String>, String> {
    Err("installed font discovery is supported only on Windows".to_owned())
}

#[tauri::command]
pub(crate) fn installed_font_families() -> Result<Vec<String>, String> {
    installed_families()
}

#[cfg(all(test, windows))]
mod tests {
    use super::*;

    #[test]
    fn vertical_and_empty_faces_are_not_exposed() {
        let mut vertical = [0_u16; 32];
        for (target, value) in vertical.iter_mut().zip("@Vertical".encode_utf16()) {
            *target = value;
        }
        assert_eq!(decode_face_name(&vertical), None);
        assert_eq!(decode_face_name(&[0_u16; 32]), None);
    }

    #[test]
    fn installed_families_are_detected_and_deduplicated() {
        let families = installed_families().unwrap();
        assert!(!families.is_empty());
        assert!(families
            .iter()
            .all(|name| !name.is_empty() && !name.starts_with('@')));
        assert!(families.windows(2).all(|pair| pair[0] < pair[1]));
    }
}
