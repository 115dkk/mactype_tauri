use super::protocol::{read_frame, write_frame, Frame, HELLO, HELLO_ACK, SHUTDOWN, VERSION};
use serde::Deserialize;
use std::{
    collections::VecDeque,
    env,
    io::{BufRead, BufReader},
    path::{Path, PathBuf},
    process::{Child, ChildStdin, Command, Stdio},
    sync::{mpsc, Arc, Mutex},
    thread,
    time::Duration,
};

const DIAGNOSTIC_LIMIT: usize = 100;

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
        let diagnostics = Arc::new(Mutex::new(VecDeque::with_capacity(DIAGNOSTIC_LIMIT)));
        let diagnostic_target = Arc::clone(&diagnostics);
        thread::spawn(move || {
            for line in BufReader::new(stderr).lines().map_while(Result::ok) {
                if let Ok(mut entries) = diagnostic_target.lock() {
                    if entries.len() == DIAGNOSTIC_LIMIT {
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
        if response.kind == super::protocol::ERROR {
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
pub(super) struct PreviewManager {
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

    fn record_diagnostic(&mut self, message: String) {
        if self.diagnostics.len() == DIAGNOSTIC_LIMIT {
            self.diagnostics.pop_front();
        }
        self.diagnostics.push_back(message);
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
        let metadata: HelloMetadata = match serde_json::from_slice(&hello.json) {
            Ok(metadata) => metadata,
            Err(error) => {
                helper.stop(self.next_id());
                return Err(error.to_string());
            }
        };
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
            let entries = helper
                .diagnostics
                .lock()
                .map(|entries| entries.iter().cloned().collect::<Vec<_>>())
                .unwrap_or_default();
            for entry in entries {
                self.record_diagnostic(entry);
            }
            helper.stop(id);
        }
        self.install_root = None;
        self.core_version = None;
    }

    pub(super) fn request_built<F>(
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
            let result = self
                .helper
                .as_mut()
                .ok_or_else(|| "preview helper is unavailable after startup".to_owned())?
                .request(Frame {
                    kind,
                    request_id,
                    json,
                    binary: Vec::new(),
                });
            match result {
                Ok(response) => return Ok(response),
                Err(error) if attempt == 0 => {
                    self.record_diagnostic(format!(
                        "helper restart after request failure: {error}"
                    ));
                    self.stop();
                }
                Err(error) => return Err(error),
            }
        }
        Err("preview helper retry loop ended unexpectedly".to_owned())
    }

    pub(super) fn request(
        &mut self,
        install_root: &Path,
        kind: u16,
        json: Vec<u8>,
    ) -> Result<Frame, String> {
        self.request_built(install_root, kind, |_| Ok(json.clone()))
    }

    pub(super) fn diagnostics(&self) -> Vec<String> {
        let mut result = self.diagnostics.iter().cloned().collect::<Vec<_>>();
        if let Some(helper) = &self.helper {
            if let Ok(entries) = helper.diagnostics.lock() {
                result.extend(entries.iter().cloned());
            }
        }
        result
    }

    pub(super) fn probe_core_version(&mut self, install_root: &Path) -> Result<u32, String> {
        if self.install_root.as_deref() != Some(install_root) || self.helper.is_none() {
            self.stop();
            self.start(install_root)?;
        }
        self.core_version
            .ok_or_else(|| "preview helper did not report a core version".to_owned())
    }

    pub(super) fn reconnect(&mut self, install_root: &Path) -> Result<u32, String> {
        self.stop();
        self.probe_core_version(install_root)
    }

    pub(super) fn force_terminate_for_ci(&mut self) -> Result<(), String> {
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

#[cfg(test)]
mod tests {
    use super::*;

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
