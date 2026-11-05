//! Runs the `lore` command-line client as a child process.

use serde::{Deserialize, Serialize};
use std::path::Path;
use std::process::Stdio;
use tokio::process::Command;

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
