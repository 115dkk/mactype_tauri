use serde::{Deserialize, Serialize};
use std::{
    collections::{BTreeMap, VecDeque},
    env, fs,
    io::{BufRead, BufReader, Read, Write},
    path::{Path, PathBuf},
    process::{Child, ChildStdin, Command, Stdio},
    sync::{mpsc, Arc, Mutex},
    thread,
    time::Duration,
};
use tauri::{AppHandle, Manager};

const MAGIC: u32 = 0x4350_544D;
const VERSION: u16 = 1;
const MAX_JSON: usize = 64 * 1024;
const MAX_BINARY: usize = 8 * 1024 * 1024;
const HELLO: u16 = 1;
const RENDER_PREVIEW: u16 = 3;
const SHUTDOWN: u16 = 4;
const SHOW_NATIVE_PREVIEW: u16 = 6;
const HIDE_NATIVE_PREVIEW: u16 = 7;
const HELLO_ACK: u16 = 101;
const PREVIEW_RENDERED: u16 = 103;
const NATIVE_PREVIEW_STATE: u16 = 105;
const ERROR: u16 = 199;

#[derive(Debug)]
struct Frame {
    kind: u16,
    request_id: u64,
    json: Vec<u8>,
    binary: Vec<u8>,
}

impl Frame {
    fn json_text(&self) -> Result<&str, String> {
        std::str::from_utf8(&self.json).map_err(|error| error.to_string())
    }
}

fn read_frame(reader: &mut impl Read) -> Result<Frame, String> {
    let mut header = [0_u8; 24];
    reader
        .read_exact(&mut header)
        .map_err(|error| error.to_string())?;
    let magic = u32::from_le_bytes(header[0..4].try_into().unwrap());
    let version = u16::from_le_bytes(header[4..6].try_into().unwrap());
    if magic != MAGIC || version != VERSION {
        return Err("preview helper returned an unsupported frame".to_owned());
    }
    let kind = u16::from_le_bytes(header[6..8].try_into().unwrap());
    let request_id = u64::from_le_bytes(header[8..16].try_into().unwrap());
    let json_length = u32::from_le_bytes(header[16..20].try_into().unwrap()) as usize;
    let binary_length = u32::from_le_bytes(header[20..24].try_into().unwrap()) as usize;
    if json_length > MAX_JSON || binary_length > MAX_BINARY {
        return Err("preview helper frame exceeds the size limit".to_owned());
    }
    let mut json = vec![0; json_length];
    let mut binary = vec![0; binary_length];
    reader
        .read_exact(&mut json)
        .map_err(|error| error.to_string())?;
    reader
        .read_exact(&mut binary)
        .map_err(|error| error.to_string())?;
    Ok(Frame {
        kind,
        request_id,
        json,
        binary,
    })
}

fn write_frame(writer: &mut impl Write, frame: &Frame) -> Result<(), String> {
    if frame.json.len() > MAX_JSON || frame.binary.len() > MAX_BINARY {
        return Err("preview request exceeds the size limit".to_owned());
    }
    writer
        .write_all(&MAGIC.to_le_bytes())
        .map_err(|error| error.to_string())?;
    writer
        .write_all(&VERSION.to_le_bytes())
        .map_err(|error| error.to_string())?;
    writer
        .write_all(&frame.kind.to_le_bytes())
        .map_err(|error| error.to_string())?;
    writer
        .write_all(&frame.request_id.to_le_bytes())
        .map_err(|error| error.to_string())?;
    writer
        .write_all(&(frame.json.len() as u32).to_le_bytes())
        .map_err(|error| error.to_string())?;
    writer
        .write_all(&(frame.binary.len() as u32).to_le_bytes())
        .map_err(|error| error.to_string())?;
    writer
        .write_all(&frame.json)
        .map_err(|error| error.to_string())?;
    writer
        .write_all(&frame.binary)
        .map_err(|error| error.to_string())?;
    writer.flush().map_err(|error| error.to_string())
}

fn helper_path() -> Result<PathBuf, String> {
    if let Some(explicit) = env::var_os("MACTYPE_PREVIEW_HELPER") {
        let path = PathBuf::from(explicit);
        if path.is_file() {
            return Ok(path);
        }
    }
    let executable = env::current_exe().map_err(|error| error.to_string())?;
    if let Some(parent) = executable.parent() {
        let installed = parent.join("mactype-preview32.exe");
        if installed.is_file() {
            return Ok(installed);
        }
    }
    for ancestor in executable.ancestors() {
        for relative in [
            Path::new("build/preview-helper/Release/mactype-preview32.exe"),
            Path::new("preview-helper/build/Release/mactype-preview32.exe"),
        ] {
            let candidate = ancestor.join(relative);
            if candidate.is_file() {
                return Ok(candidate);
            }
        }
    }
    Err(
        "mactype-preview32.exe was not found beside the app or in a development build directory"
            .to_owned(),
    )
}

struct HelperProcess {
    child: Child,
    input: ChildStdin,
    responses: mpsc::Receiver<Result<Frame, String>>,
    diagnostics: Arc<Mutex<VecDeque<String>>>,
}

impl HelperProcess {
    fn spawn(install_root: &Path) -> Result<Self, String> {
        let mut command = Command::new(helper_path()?);
        command
            .arg("--install-root")
            .arg(install_root)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.creation_flags(0x0800_0000);
        }
        let mut child = command.spawn().map_err(|error| error.to_string())?;
        let input = child
            .stdin
            .take()
            .ok_or_else(|| "preview helper stdin is unavailable".to_owned())?;
        let output = child
            .stdout
            .take()
            .ok_or_else(|| "preview helper stdout is unavailable".to_owned())?;
        let stderr = child
            .stderr
            .take()
            .ok_or_else(|| "preview helper stderr is unavailable".to_owned())?;
        let (sender, responses) = mpsc::channel();
        thread::spawn(move || {
            let mut reader = BufReader::new(output);
            loop {
                let response = read_frame(&mut reader);
                let finished = response.is_err();
                if sender.send(response).is_err() || finished {
                    break;
                }
            }
        });
        let diagnostics = Arc::new(Mutex::new(VecDeque::with_capacity(100)));
        let diagnostic_target = Arc::clone(&diagnostics);
        thread::spawn(move || {
            for line in BufReader::new(stderr).lines().map_while(Result::ok) {
                if let Ok(mut entries) = diagnostic_target.lock() {
                    if entries.len() == 100 {
                        entries.pop_front();
                    }
                    entries.push_back(line);
                }
            }
        });
        Ok(Self {
            child,
            input,
            responses,
            diagnostics,
        })
    }

    fn request(&mut self, frame: Frame) -> Result<Frame, String> {
        let expected_id = frame.request_id;
        write_frame(&mut self.input, &frame)?;
        let response = self
            .responses
            .recv_timeout(Duration::from_secs(2))
            .map_err(|error| match error {
                mpsc::RecvTimeoutError::Timeout => {
                    "preview helper did not respond within 2 seconds".to_owned()
                }
                mpsc::RecvTimeoutError::Disconnected => {
                    "preview helper stopped before responding".to_owned()
                }
            })??;
        if response.request_id != expected_id {
            return Err("preview helper returned a mismatched request id".to_owned());
        }
        if response.kind == ERROR {
            return Err(format!("preview helper error: {}", response.json_text()?));
        }
        Ok(response)
    }

    fn stop(&mut self, request_id: u64) {
        let _ = self.request(Frame {
            kind: SHUTDOWN,
            request_id,
            json: Vec::new(),
            binary: Vec::new(),
        });
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

#[derive(Default)]
pub struct PreviewManager {
    helper: Option<HelperProcess>,
    install_root: Option<PathBuf>,
    core_version: Option<u32>,
    next_request_id: u64,
    diagnostics: VecDeque<String>,
}

impl PreviewManager {
    fn next_id(&mut self) -> u64 {
        self.next_request_id = self.next_request_id.wrapping_add(1).max(1);
        self.next_request_id
    }

    fn start(&mut self, install_root: &Path) -> Result<(), String> {
        let mut helper = HelperProcess::spawn(install_root)?;
        let hello_request_id = self.next_id();
        let hello = match helper.request(Frame {
            kind: HELLO,
            request_id: hello_request_id,
            json: br#"{"client":"mactype-control-center","protocolVersion":1}"#.to_vec(),
            binary: Vec::new(),
        }) {
            Ok(response) => response,
            Err(error) => {
                helper.stop(self.next_id());
                return Err(error);
            }
        };
        if hello.kind != HELLO_ACK {
            helper.stop(self.next_id());
            return Err("preview helper did not acknowledge the protocol".to_owned());
        }
        let metadata: HelloMetadata =
            serde_json::from_slice(&hello.json).map_err(|error| error.to_string())?;
        if metadata.protocol_version != VERSION || metadata.core_version == 0 {
            helper.stop(self.next_id());
            return Err("preview helper returned invalid core metadata".to_owned());
        }
        self.install_root = Some(install_root.to_path_buf());
        self.core_version = Some(metadata.core_version);
        self.helper = Some(helper);
        Ok(())
    }

    fn stop(&mut self) {
        let id = self.next_id();
        if let Some(mut helper) = self.helper.take() {
            if let Ok(entries) = helper.diagnostics.lock() {
                self.diagnostics.extend(entries.iter().cloned());
            }
            helper.stop(id);
        }
        self.install_root = None;
        self.core_version = None;
    }

    fn request_built<F>(
        &mut self,
        install_root: &Path,
        kind: u16,
        mut build_json: F,
    ) -> Result<Frame, String>
    where
        F: FnMut(u64) -> Result<Vec<u8>, String>,
    {
        for attempt in 0..2 {
            if self.install_root.as_deref() != Some(install_root) || self.helper.is_none() {
                self.stop();
                self.start(install_root)?;
            }
            let request_id = self.next_id();
            let json = build_json(request_id)?;
            let result = self.helper.as_mut().unwrap().request(Frame {
                kind,
                request_id,
                json,
                binary: Vec::new(),
            });
            match result {
                Ok(response) => return Ok(response),
                Err(error) if attempt == 0 => {
                    self.diagnostics
                        .push_back(format!("helper restart after request failure: {error}"));
                    self.stop();
                }
                Err(error) => return Err(error),
            }
        }
        Err("preview helper retry loop ended unexpectedly".to_owned())
    }

    fn request(&mut self, install_root: &Path, kind: u16, json: Vec<u8>) -> Result<Frame, String> {
        self.request_built(install_root, kind, |_| Ok(json.clone()))
    }

    pub fn diagnostics(&self) -> Vec<String> {
        let mut result = self.diagnostics.iter().cloned().collect::<Vec<_>>();
        if let Some(helper) = &self.helper {
            if let Ok(entries) = helper.diagnostics.lock() {
                result.extend(entries.iter().cloned());
            }
        }
        result
    }

    pub fn probe_core_version(&mut self, install_root: &Path) -> Result<u32, String> {
        if self.install_root.as_deref() != Some(install_root) || self.helper.is_none() {
            self.stop();
            self.start(install_root)?;
        }
        self.core_version
            .ok_or_else(|| "preview helper did not report a core version".to_owned())
    }

    pub fn reconnect(&mut self, install_root: &Path) -> Result<u32, String> {
        self.stop();
        self.probe_core_version(install_root)
    }

    pub fn force_terminate_for_ci(&mut self) -> Result<(), String> {
        let helper = self
            .helper
            .as_mut()
            .ok_or_else(|| "preview helper is not running".to_owned())?;
        helper.child.kill().map_err(|error| error.to_string())?;
        helper.child.wait().map_err(|error| error.to_string())?;
        Ok(())
    }
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct HelloMetadata {
    protocol_version: u16,
    core_version: u32,
}

impl Drop for PreviewManager {
    fn drop(&mut self) {
        self.stop();
    }
}

#[derive(Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PreviewSample {
    pub text: String,
    pub font_face: String,
    pub font_size_pt: f64,
    pub width_px: u32,
    pub height_px: u32,
    pub dpi: u32,
    pub foreground: String,
    pub background: String,
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
pub struct PreviewResult {
    pub request_id: u64,
    pub image_path: String,
    pub width: u32,
    pub height: u32,
    pub dpi: u32,
    pub elapsed_ms: u64,
    pub core_version: u32,
}

pub fn render_preview(
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

pub fn set_native_preview(
    manager: &mut PreviewManager,
    install_root: &Path,
    visible: bool,
) -> Result<bool, String> {
    let kind = if visible {
        SHOW_NATIVE_PREVIEW
    } else {
        HIDE_NATIVE_PREVIEW
    };
    let response = manager.request(install_root, kind, Vec::new())?;
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frame_round_trip_preserves_binary_payload() {
        let original = Frame {
            kind: RENDER_PREVIEW,
            request_id: 41,
            json: br#"{"ok":true}"#.to_vec(),
            binary: vec![1, 2, 3, 4],
        };
        let mut bytes = Vec::new();
        write_frame(&mut bytes, &original).unwrap();
        let decoded = read_frame(&mut bytes.as_slice()).unwrap();
        assert_eq!(decoded.kind, original.kind);
        assert_eq!(decoded.request_id, original.request_id);
        assert_eq!(decoded.json, original.json);
        assert_eq!(decoded.binary, original.binary);
    }

    #[test]
    fn oversized_frame_is_rejected_before_allocation() {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&MAGIC.to_le_bytes());
        bytes.extend_from_slice(&VERSION.to_le_bytes());
        bytes.extend_from_slice(&RENDER_PREVIEW.to_le_bytes());
        bytes.extend_from_slice(&1_u64.to_le_bytes());
        bytes.extend_from_slice(&((MAX_JSON + 1) as u32).to_le_bytes());
        bytes.extend_from_slice(&0_u32.to_le_bytes());
        assert!(read_frame(&mut bytes.as_slice())
            .unwrap_err()
            .contains("size limit"));
    }

    #[test]
    fn hello_metadata_requires_the_real_core_version() {
        let metadata: HelloMetadata = serde_json::from_slice(
            br#"{"protocolVersion":1,"coreVersion":20220712,"dllGetVersion":true}"#,
        )
        .unwrap();
        assert_eq!(metadata.protocol_version, VERSION);
        assert_eq!(metadata.core_version, 20220712);
        assert!(serde_json::from_slice::<HelloMetadata>(br#"{"protocolVersion":1}"#).is_err());
    }
}
