#![forbid(unsafe_code)]

use std::fs::File;
use std::io::Read;
use std::path::Path;

const MAX_PE_HEADER_BYTES: u64 = 64 * 1024;
const DOS_SIGNATURE: &[u8; 2] = b"MZ";
const DOS_PE_POINTER_OFFSET: usize = 0x3c;
const DOS_HEADER_BYTES: usize = 0x40;
const COFF_HEADER_BYTES: usize = 20;
const COFF_OPTIONAL_HEADER_SIZE_OFFSET: usize = 16;
const OPTIONAL_HEADER_SUBSYSTEM_OFFSET: usize = 68;
const PE_SIGNATURE: &[u8; 4] = b"PE\0\0";
const PE32_MAGIC: u16 = 0x10b;
const PE32_PLUS_MAGIC: u16 = 0x20b;
const WINDOWS_GUI_SUBSYSTEM: u16 = 2;
const WINDOWS_CUI_SUBSYSTEM: u16 = 3;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ImageSubsystem {
    Console,
    Gui,
    Other,
    Unavailable,
}

pub fn read_image_subsystem(path: &Path) -> ImageSubsystem {
    let Ok(file) = File::open(path) else {
        return ImageSubsystem::Unavailable;
    };
    let mut bytes = Vec::with_capacity(MAX_PE_HEADER_BYTES as usize);
    if file
        .take(MAX_PE_HEADER_BYTES)
        .read_to_end(&mut bytes)
        .is_err()
    {
        return ImageSubsystem::Unavailable;
    }
    parse_image_subsystem(&bytes).unwrap_or(ImageSubsystem::Unavailable)
}

fn parse_image_subsystem(bytes: &[u8]) -> Option<ImageSubsystem> {
    if bytes.get(..2)? != DOS_SIGNATURE {
        return None;
    }
    let pe_offset = read_u32(bytes, DOS_PE_POINTER_OFFSET)? as usize;
    if pe_offset < DOS_HEADER_BYTES
        || bytes.get(pe_offset..pe_offset.checked_add(4)?)? != PE_SIGNATURE
    {
        return None;
    }
    let coff_header = pe_offset.checked_add(4)?;
    let optional_header_size = usize::from(read_u16(
        bytes,
        coff_header.checked_add(COFF_OPTIONAL_HEADER_SIZE_OFFSET)?,
    )?);
    if optional_header_size < OPTIONAL_HEADER_SUBSYSTEM_OFFSET + 2 {
        return None;
    }
    let optional_header = coff_header.checked_add(COFF_HEADER_BYTES)?;
    let optional_header_end = optional_header.checked_add(optional_header_size)?;
    bytes.get(optional_header..optional_header_end)?;
    if !matches!(
        read_u16(bytes, optional_header)?,
        PE32_MAGIC | PE32_PLUS_MAGIC
    ) {
        return None;
    }
    let subsystem = read_u16(
        bytes,
        optional_header.checked_add(OPTIONAL_HEADER_SUBSYSTEM_OFFSET)?,
    )?;
    Some(match subsystem {
        WINDOWS_CUI_SUBSYSTEM => ImageSubsystem::Console,
        WINDOWS_GUI_SUBSYSTEM => ImageSubsystem::Gui,
        _ => ImageSubsystem::Other,
    })
}

fn read_u16(bytes: &[u8], offset: usize) -> Option<u16> {
    Some(u16::from_le_bytes(
        bytes.get(offset..offset.checked_add(2)?)?.try_into().ok()?,
    ))
}

fn read_u32(bytes: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        bytes.get(offset..offset.checked_add(4)?)?.try_into().ok()?,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn pe_image(magic: u16, subsystem: u16) -> Vec<u8> {
        let pe_offset = 0x80_usize;
        let optional_header = pe_offset + 4 + COFF_HEADER_BYTES;
        let mut image = vec![0_u8; optional_header + OPTIONAL_HEADER_SUBSYSTEM_OFFSET + 2];
        image[..2].copy_from_slice(DOS_SIGNATURE);
        image[DOS_PE_POINTER_OFFSET..DOS_PE_POINTER_OFFSET + 4]
            .copy_from_slice(&(pe_offset as u32).to_le_bytes());
        image[pe_offset..pe_offset + 4].copy_from_slice(PE_SIGNATURE);
        let size_offset = pe_offset + 4 + COFF_OPTIONAL_HEADER_SIZE_OFFSET;
        image[size_offset..size_offset + 2]
            .copy_from_slice(&(OPTIONAL_HEADER_SUBSYSTEM_OFFSET as u16 + 2).to_le_bytes());
        image[optional_header..optional_header + 2].copy_from_slice(&magic.to_le_bytes());
        let subsystem_offset = optional_header + OPTIONAL_HEADER_SUBSYSTEM_OFFSET;
        image[subsystem_offset..subsystem_offset + 2].copy_from_slice(&subsystem.to_le_bytes());
        image
    }

    fn classify(bytes: &[u8]) -> ImageSubsystem {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("image.exe");
        fs::write(&path, bytes).unwrap();
        read_image_subsystem(&path)
    }

    #[test]
    #[cfg_attr(
        all(miri, windows),
        ignore = "Windows Miri does not implement CreateDirectoryW"
    )]
    fn reads_console_gui_and_other_subsystems_from_pe32_and_pe32_plus() {
        for magic in [PE32_MAGIC, PE32_PLUS_MAGIC] {
            assert_eq!(classify(&pe_image(magic, 3)), ImageSubsystem::Console);
            assert_eq!(classify(&pe_image(magic, 2)), ImageSubsystem::Gui);
            assert_eq!(classify(&pe_image(magic, 1)), ImageSubsystem::Other);
        }
    }

    #[test]
    #[cfg_attr(
        all(miri, windows),
        ignore = "Windows Miri does not implement CreateDirectoryW"
    )]
    fn invalid_missing_and_out_of_bound_images_are_unavailable() {
        assert_eq!(
            classify(b"random temporary bytes"),
            ImageSubsystem::Unavailable
        );
        let directory = tempfile::tempdir().unwrap();
        assert_eq!(
            read_image_subsystem(&directory.path().join("missing.exe")),
            ImageSubsystem::Unavailable
        );

        let mut image = pe_image(PE32_MAGIC, 3);
        image[DOS_PE_POINTER_OFFSET..DOS_PE_POINTER_OFFSET + 4]
            .copy_from_slice(&(MAX_PE_HEADER_BYTES as u32).to_le_bytes());
        image.resize(MAX_PE_HEADER_BYTES as usize + 256, 0);
        let pe_offset = MAX_PE_HEADER_BYTES as usize;
        image[pe_offset..pe_offset + 4].copy_from_slice(PE_SIGNATURE);
        assert_eq!(classify(&image), ImageSubsystem::Unavailable);
    }

    #[test]
    #[cfg_attr(
        all(miri, windows),
        ignore = "Windows Miri does not implement CreateDirectoryW"
    )]
    fn rejects_invalid_pe_structure_and_truncated_optional_headers() {
        let valid = pe_image(PE32_MAGIC, 3);

        let mut image = valid.clone();
        image[..2].copy_from_slice(b"NZ");
        assert_eq!(classify(&image), ImageSubsystem::Unavailable);

        let mut image = valid.clone();
        image[DOS_PE_POINTER_OFFSET..DOS_PE_POINTER_OFFSET + 4]
            .copy_from_slice(&0x20_u32.to_le_bytes());
        assert_eq!(classify(&image), ImageSubsystem::Unavailable);

        let mut image = valid.clone();
        image[0x80..0x84].copy_from_slice(b"PX\0\0");
        assert_eq!(classify(&image), ImageSubsystem::Unavailable);

        let mut image = valid.clone();
        image.truncate(0x80 + 4 + COFF_HEADER_BYTES - 1);
        assert_eq!(classify(&image), ImageSubsystem::Unavailable);

        let mut image = valid.clone();
        let size_offset = 0x80 + 4 + COFF_OPTIONAL_HEADER_SIZE_OFFSET;
        image[size_offset..size_offset + 2]
            .copy_from_slice(&(OPTIONAL_HEADER_SUBSYSTEM_OFFSET as u16 + 1).to_le_bytes());
        assert_eq!(classify(&image), ImageSubsystem::Unavailable);

        let mut image = valid.clone();
        let optional_header = 0x80 + 4 + COFF_HEADER_BYTES;
        image[optional_header..optional_header + 2].copy_from_slice(&0x30b_u16.to_le_bytes());
        assert_eq!(classify(&image), ImageSubsystem::Unavailable);

        let mut image = valid;
        image.truncate(image.len() - 1);
        assert_eq!(classify(&image), ImageSubsystem::Unavailable);
    }

    #[cfg(windows)]
    #[test]
    fn classifies_windows_cmd_and_explorer_images() {
        let windows = std::env::var_os("SystemRoot").unwrap();
        let windows = Path::new(&windows);
        assert_eq!(
            read_image_subsystem(&windows.join("System32").join("cmd.exe")),
            ImageSubsystem::Console
        );
        assert_eq!(
            read_image_subsystem(&windows.join("explorer.exe")),
            ImageSubsystem::Gui
        );
    }
}
