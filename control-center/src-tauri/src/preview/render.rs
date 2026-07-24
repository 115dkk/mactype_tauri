use super::{
    helper::PreviewManager,
    protocol::{
        HIDE_NATIVE_PREVIEW, NATIVE_PREVIEW_STATE, PREVIEW_RENDERED, RENDER_PREVIEW,
        SHOW_NATIVE_PREVIEW,
    },
};
use serde::{Deserialize, Serialize};
use std::{collections::BTreeMap, fs, path::Path};
use tauri::{AppHandle, Manager};

#[derive(Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct PreviewSample {
    text: String,
    font_face: String,
    font_size_pt: f64,
    width_px: u32,
    height_px: u32,
    dpi: u32,
    foreground: String,
    background: String,
    /// Optional GDI style flags; older callers omit them and render regular text.
    #[serde(default)]
    bold: bool,
    #[serde(default)]
    italic: bool,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct RenderRequest<'a> {
    request_id: u64,
    profile_path: &'a str,
    overrides: &'a BTreeMap<String, f64>,
    sample: &'a PreviewSample,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct RenderMetadata {
    width: u32,
    height: u32,
    dpi: u32,
    elapsed_ms: u64,
    core_version: u32,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct PreviewResult {
    request_id: u64,
    image_path: String,
    width: u32,
    height: u32,
    dpi: u32,
    elapsed_ms: u64,
    core_version: u32,
}

pub(super) fn render_preview(
    app: &AppHandle,
    manager: &mut PreviewManager,
    install_root: &Path,
    profile_path: &str,
    overrides: &BTreeMap<String, f64>,
    sample: &PreviewSample,
) -> Result<PreviewResult, String> {
    let response = manager.request_built(install_root, RENDER_PREVIEW, |request_id| {
        serde_json::to_vec(&RenderRequest {
            request_id,
            profile_path,
            overrides,
            sample,
        })
        .map_err(|error| error.to_string())
    })?;
    if response.kind != PREVIEW_RENDERED || !response.binary.starts_with(b"\x89PNG\r\n\x1a\n") {
        return Err("preview helper returned an invalid PNG response".to_owned());
    }
    let metadata: RenderMetadata =
        serde_json::from_slice(&response.json).map_err(|error| error.to_string())?;
    let directory = app
        .path()
        .app_local_data_dir()
        .map_err(|error| error.to_string())?
        .join("preview");
    fs::create_dir_all(&directory).map_err(|error| error.to_string())?;
    let image = directory.join(format!("preview-{}.png", response.request_id));
    fs::write(&image, response.binary).map_err(|error| error.to_string())?;
    Ok(PreviewResult {
        request_id: response.request_id,
        image_path: image.to_string_lossy().into_owned(),
        width: metadata.width,
        height: metadata.height,
        dpi: metadata.dpi,
        elapsed_ms: metadata.elapsed_ms,
        core_version: metadata.core_version,
    })
}

/// Show-request body; older helpers ignore unknown fields, so this stays
/// backward compatible with helpers that predate the display mode.
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct NativePreviewRequest<'a> {
    display_mode: &'a str,
    #[serde(skip_serializing_if = "Option::is_none")]
    listing_text: Option<&'a str>,
}

pub(super) fn set_native_preview(
    manager: &mut PreviewManager,
    install_root: &Path,
    visible: bool,
    display_mode: Option<&str>,
    listing_text: Option<&str>,
) -> Result<bool, String> {
    let kind = if visible {
        SHOW_NATIVE_PREVIEW
    } else {
        HIDE_NATIVE_PREVIEW
    };
    let body = if visible {
        serde_json::to_vec(&NativePreviewRequest {
            display_mode: display_mode.unwrap_or("default"),
            listing_text,
        })
        .map_err(|error| error.to_string())?
    } else {
        Vec::new()
    };
    let response = manager.request(install_root, kind, body)?;
    if response.kind != NATIVE_PREVIEW_STATE {
        return Err("preview helper returned an invalid native-window response".to_owned());
    }
    let value: serde_json::Value =
        serde_json::from_slice(&response.json).map_err(|error| error.to_string())?;
    Ok(value
        .get("visible")
        .and_then(serde_json::Value::as_bool)
        .unwrap_or(false))
}
