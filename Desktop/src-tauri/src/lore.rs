//! Runs the `lore` command-line client as a child process.

use serde::{Deserialize, Serialize};
use std::path::Path;
use std::process::Stdio;
use std::sync::Mutex;
use tokio::process::Command;

/// Running `lore` processes started with a stream id, so Cancel can really stop them.
static RUNNING: Mutex<Vec<(String, u32)>> = Mutex::new(Vec::new());

fn kill_tree(pid: u32) {
    let mut kill = std::process::Command::new("taskkill");
    kill.args(["/PID", &pid.to_string(), "/T", "/F"]);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        kill.creation_flags(0x0800_0000);
    }
    let _ = kill.output();
}

/// Stops the process that belongs to a stream id.
pub fn cancel(id: &str) {
    let pids: Vec<u32> = RUNNING.lock().map(|r| r.iter().filter(|(i, _)| i == id).map(|(_, p)| *p).collect()).unwrap_or_default();
    for pid in pids {
        kill_tree(pid);
    }
}

/// Stops everything that is still running, for when the app closes.
pub fn cancel_all() {
    let pids: Vec<u32> = RUNNING.lock().map(|r| r.iter().map(|(_, p)| *p).collect()).unwrap_or_default();
    for pid in pids {
        kill_tree(pid);
    }
}

#[derive(Serialize)]
pub struct CliResult {
    pub ok: bool,
    pub code: i32,
    pub stdout: String,
    pub stderr: String,
}

#[derive(Clone, Serialize, Deserialize, Debug)]
#[serde(rename_all = "camelCase")]
pub struct Server {
    pub id: String,
    pub label: String,
    /// lores://host:port
    pub lore_url: String,
    /// https://host:port (gRPC authentication service)
    pub auth_url: String,
    /// https://host:port (login page and admin API)
    pub web_url: String,
}

pub fn lore_exe() -> String {
    if let Ok(path) = std::env::var("LORE_EXE") {
        return path;
    }
    let default = r"C:\Program Files\lore\lore.exe";
    if Path::new(default).exists() {
        default.to_string()
    } else {
        "lore".to_string()
    }
}

pub fn strip_ansi(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut chars = text.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '\u{1b}' {
            if chars.peek() == Some(&'[') {
                chars.next();
                for next in chars.by_ref() {
                    if next.is_ascii_alphabetic() {
                        break;
                    }
                }
            }
        } else {
            out.push(c);
        }
    }
    out
}

pub async fn run(args: &[String], repository: Option<&str>, token: Option<&str>) -> CliResult {
    let mut command = Command::new(lore_exe());
    command.arg("--no-pager").arg("--non-interactive");
    if let Some(path) = repository {
        command.arg("--repository").arg(path);
        // Paths in commands such as `file info` are relative to the working directory.
        if Path::new(path).is_dir() {
            command.current_dir(path);
        }
    }
    // More parallel connections make uploads and downloads much quicker.
    if matches!(args.first().map(String::as_str), Some("push" | "clone" | "sync")) {
        command.args(["--max-connections", "8"]);
    }
    command.args(args);
    command.env("NO_COLOR", "1");
    if let Some(token) = token {
        command.env("LORE_IDENTITY_TOKEN", token);
    }
    command.stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    #[cfg(windows)]
    command.creation_flags(0x0800_0000); // CREATE_NO_WINDOW

    match command.output().await {
        Ok(output) => CliResult {
            ok: output.status.success(),
            code: output.status.code().unwrap_or(-1),
            stdout: strip_ansi(&String::from_utf8_lossy(&output.stdout)),
            stderr: strip_ansi(&String::from_utf8_lossy(&output.stderr)),
        },
        Err(error) => CliResult {
            ok: false,
            code: -1,
            stdout: String::new(),
            stderr: format!("Could not start lore: {error}"),
        },
    }
}

/// Signs the `lore` client itself in, so everything that runs it without this app (the Unreal plugin) is signed in too.
/// The token goes through the environment, not the command line, where other programs could read it.
pub async fn login(server: &Server, token: &str) -> CliResult {
    let mut command = Command::new(lore_exe());
    command.args(["--no-pager", "--non-interactive", "auth", "login"]).arg(&server.lore_url);
    command.args(["--token-type", "lore", "--no-browser", "--auth-url"]).arg(&server.auth_url);
    command.env("LORE_TOKEN", token).env("NO_COLOR", "1");
    command.stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    #[cfg(windows)]
    command.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    match command.output().await {
        Ok(output) => CliResult {
            ok: output.status.success(),
            code: output.status.code().unwrap_or(-1),
            stdout: strip_ansi(&String::from_utf8_lossy(&output.stdout)),
            stderr: strip_ansi(&String::from_utf8_lossy(&output.stderr)),
        },
        Err(error) => CliResult { ok: false, code: -1, stdout: String::new(), stderr: format!("Could not start lore: {error}") },
    }
}

/// Like `run`, but hands every line of output to `emit` while the command is still working, so the app can show progress.
pub async fn run_stream<F>(id: &str, args: &[String], repository: Option<&str>, token: Option<&str>, emit: F) -> CliResult
where
    F: Fn(String) + Send + Sync + 'static,
{
    use std::sync::Arc;
    use tokio::io::{AsyncBufReadExt, AsyncRead, BufReader};

    let mut command = Command::new(lore_exe());
    command.arg("--no-pager").arg("--non-interactive").args(["--event-interval", "500"]);
    if let Some(path) = repository {
        command.arg("--repository").arg(path);
        // Paths in commands such as `file info` are relative to the working directory.
        if Path::new(path).is_dir() {
            command.current_dir(path);
        }
    }
    if matches!(args.first().map(String::as_str), Some("push" | "clone" | "sync")) {
        command.args(["--max-connections", "8"]);
    }
    command.args(args);
    command.env("NO_COLOR", "1");
    if let Some(token) = token {
        command.env("LORE_IDENTITY_TOKEN", token);
    }
    command.stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    #[cfg(windows)]
    command.creation_flags(0x0800_0000);

    let mut child = match command.spawn() {
        Ok(child) => child,
        Err(error) => {
            return CliResult { ok: false, code: -1, stdout: String::new(), stderr: format!("Could not start lore: {error}") };
        }
    };

    if let (Some(pid), Ok(mut running)) = (child.id(), RUNNING.lock()) {
        running.push((id.to_string(), pid));
    }

    async fn pump<R: AsyncRead + Unpin>(reader: R, emit: Arc<dyn Fn(String) + Send + Sync>) -> String {
        let mut lines = BufReader::new(reader);
        let mut all = String::new();
        let mut buffer = Vec::new();
        // Progress output often rewrites the same line with a carriage return, so cut on both.
        while let Ok(read) = lines.read_until(b'\n', &mut buffer).await {
            if read == 0 {
                break;
            }
            let text = strip_ansi(&String::from_utf8_lossy(&buffer));
            buffer.clear();
            for part in text.split(['\r', '\n']).filter(|p| !p.trim().is_empty()) {
                emit(part.trim().to_string());
            }
            all.push_str(&text);
        }
        all
    }

    let emit: Arc<dyn Fn(String) + Send + Sync> = Arc::new(emit);
    let out = tokio::spawn(pump(child.stdout.take().unwrap(), emit.clone()));
    let err = tokio::spawn(pump(child.stderr.take().unwrap(), emit));
    let status = child.wait().await;
    if let Ok(mut running) = RUNNING.lock() {
        running.retain(|(i, _)| i != id);
    }
    let stdout = out.await.unwrap_or_default();
    let stderr = err.await.unwrap_or_default();
    match status {
        Ok(status) => CliResult { ok: status.success(), code: status.code().unwrap_or(-1), stdout, stderr },
        Err(error) => CliResult { ok: false, code: -1, stdout, stderr: format!("{stderr}{error}") },
    }
}
