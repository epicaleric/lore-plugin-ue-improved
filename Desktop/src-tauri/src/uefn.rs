//! Projects of Unreal Editor for Fortnite. They belong to UEFN and Epic's server, so no Lore command ever runs in
//! their folders: even `lore history` writes to the store, and in an old .urc project it starts an upgrade.
//! The history is read from a copy of the store instead.

use serde_json::{json, Value};
use std::path::{Path, PathBuf};
use tauri::{AppHandle, Manager};
use tokio::process::Command;

const EDITOR: &str = "UnrealEditorFortnite-Win64-Shipping.exe";

fn hidden(mut command: Command) -> Command {
    #[cfg(windows)]
    command.creation_flags(0x0800_0000);
    command
}

/// UEFN's own settings, with the projects it knows and the one it opens at start-up.
fn settings_ini() -> Option<PathBuf> {
    let base = std::env::var("LOCALAPPDATA").ok()?;
    Some(Path::new(&base).join(r"UnrealEditorFortnite\Saved\Config\WindowsEditor\EditorPerProjectUserSettings.ini"))
}

fn known_project_files() -> Vec<PathBuf> {
    let Some(bytes) = settings_ini().and_then(|path| std::fs::read(path).ok()) else { return Vec::new() };
    let text = if bytes.starts_with(&[0xFF, 0xFE]) {
        String::from_utf16_lossy(&bytes[2..].chunks_exact(2).map(|b| u16::from_le_bytes([b[0], b[1]])).collect::<Vec<_>>())
    } else {
        String::from_utf8_lossy(&bytes).to_string()
    };
    text.lines()
        .filter_map(|line| line.strip_prefix("AdditionalProjectFiles=").or_else(|| line.strip_prefix("LastProjectFileName=")))
        .map(|path| PathBuf::from(path.trim()))
        .filter(|path| path.exists())
        .collect()
}

fn collect(dir: &Path, depth: u32, out: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    let entries: Vec<_> = entries.flatten().collect();
    if let Some(file) = entries.iter().find(|e| e.path().extension().is_some_and(|x| x == "uefnproject")) {
        out.push(file.path());
        return;
    }
    if depth == 0 {
        return;
    }
    for entry in entries {
        let name = entry.file_name().to_string_lossy().to_string();
        if entry.path().is_dir() && !name.starts_with('.') && !matches!(name.as_str(), "node_modules" | "Content" | "Intermediate" | "Saved" | "Binaries" | "DerivedDataCache") {
            collect(&entry.path(), depth - 1, out);
        }
    }
}

fn read(file: &Path) -> Value {
    let dir = file.parent().unwrap_or(file);
    let stem = file.file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or_default();
    let info: Value = std::fs::read_to_string(file).ok().and_then(|text| serde_json::from_str(text.trim_start_matches('\u{feff}')).ok()).unwrap_or(Value::Null);
    let title = info["title"].as_str().filter(|t| !t.is_empty()).map(str::to_string).unwrap_or(stem);
    let thumbnail = [info["keyArt"].as_str().unwrap_or(""), "Resources/thumbnail.png"]
        .iter()
        .filter(|art| !art.is_empty())
        .map(|art| dir.join(art))
        .find(|path| path.is_file())
        .map(|path| path.to_string_lossy().replace('/', "\\"));
    let store = if dir.join(".lore").exists() { Some("lore") } else if dir.join(".urc").exists() { Some("urc") } else { None };
    // Revisions name Epic account ids, this one is the person on this PC.
    let config = std::fs::read_to_string(dir.join(".lore").join("config.toml")).unwrap_or_default();
    let identity = config.lines().find_map(|line| line.strip_prefix("identity = ")).map(|id| id.trim().trim_matches('"').to_string());
    json!({
        "path": dir.to_string_lossy().replace('/', "\\"),
        "file": file.to_string_lossy().replace('/', "\\"),
        "name": title,
        "description": info["description"].as_str().filter(|d| !d.is_empty()),
        "thumbnail": thumbnail,
        "projectId": info["bindings"]["projectId"].as_str().filter(|d| !d.is_empty()),
        "version": info["compatibilityVersion"].as_str().filter(|d| !d.is_empty()),
        "store": store,
        "identity": identity,
    })
}

/// The projects UEFN knows, plus what is next to them and in the folders the person added.
#[tauri::command]
pub async fn scan_uefn_projects(roots: Vec<String>) -> Result<Vec<Value>, String> {
    tokio::task::spawn_blocking(move || {
        let mut files = known_project_files();
        let mut places: Vec<PathBuf> = roots.iter().map(PathBuf::from).collect();
        if let Ok(profile) = std::env::var("USERPROFILE") {
            places.push(Path::new(&profile).join(r"Documents\Fortnite Projects"));
        }
        places.extend(files.iter().filter_map(|file| file.parent()?.parent().map(Path::to_path_buf)));
        places.sort();
        places.dedup();
        for place in places {
            collect(&place, 2, &mut files);
        }
        let mut seen = std::collections::HashSet::new();
        files.retain(|file| seen.insert(file.to_string_lossy().replace('\\', "/").to_lowercase()));
        files.iter().map(|file| read(file)).collect()
    })
    .await
    .map_err(|e| e.to_string())
}

async fn running() -> bool {
    let mut command = hidden(Command::new("tasklist"));
    // The table format cuts long names off, CSV keeps them whole.
    command.args(["/FI", &format!("IMAGENAME eq {EDITOR}"), "/FO", "CSV", "/NH"]);
    match command.output().await {
        Ok(output) => String::from_utf8_lossy(&output.stdout).contains(EDITOR),
        Err(_) => false,
    }
}

#[tauri::command]
pub async fn uefn_running() -> bool {
    running().await
}

/// Makes one folder the same as another. Robocopy reports success with exit codes below 8.
async fn mirror(from: &Path, to: &Path, extra: &[&str]) -> Result<(), String> {
    let mut command = hidden(Command::new("robocopy"));
    command.arg(from).arg(to).args(["/MIR", "/R:1", "/W:1", "/NFL", "/NDL", "/NJH", "/NJS", "/NP"]).args(extra);
    let status = command.status().await.map_err(|e| format!("Could not start robocopy: {e}"))?;
    if status.code().unwrap_or(16) >= 8 {
        return Err(format!("Copying {} failed.", from.display()));
    }
    Ok(())
}

/// Brings the app's copy of a project's Lore store up to date and returns the folder Lore can read it in.
/// While UEFN is open the store can change halfway, so the copy of the last time is used then.
#[tauri::command]
pub async fn uefn_snapshot(app: AppHandle, path: String, id: String) -> Result<Option<String>, String> {
    let source = Path::new(&path).join(".lore");
    if !source.exists() {
        return Ok(None);
    }
    let name: String = id.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '-').collect();
    let folder = app.path().app_data_dir().map_err(|e| e.to_string())?.join("bare").join("uefn").join(name);
    let copy = folder.join(".lore");
    if !running().await {
        mirror(&source, &copy, &[]).await?;
        // The copy must never talk to Epic's server.
        let config = copy.join("config.toml");
        if let Ok(text) = std::fs::read_to_string(&config) {
            let kept: Vec<&str> = text.lines().filter(|line| !line.trim_start().starts_with("remote_url")).collect();
            std::fs::write(&config, kept.join("\n")).map_err(|e| e.to_string())?;
        }
    }
    Ok(copy.exists().then(|| folder.to_string_lossy().to_string()))
}

fn editor_path() -> Option<PathBuf> {
    let base = std::env::var("ProgramData").unwrap_or_else(|_| r"C:\ProgramData".into());
    let manifests = Path::new(&base).join(r"Epic\EpicGamesLauncher\Data\Manifests");
    for entry in std::fs::read_dir(manifests).ok()?.flatten() {
        let Some(item) = std::fs::read_to_string(entry.path()).ok().and_then(|text| serde_json::from_str::<Value>(&text).ok()) else { continue };
        if item["AppName"] == "Fortnite_Studio" {
            let editor = Path::new(item["InstallLocation"].as_str()?).join(item["LaunchExecutable"].as_str()?);
            return editor.exists().then_some(editor);
        }
    }
    None
}

fn encoded(text: &str, wide: bool) -> Vec<u8> {
    if wide { text.encode_utf16().flat_map(u16::to_le_bytes).collect() } else { text.as_bytes().to_vec() }
}

fn find(haystack: &[u8], needle: &[u8], from: usize) -> Option<usize> {
    haystack[from..].windows(needle.len()).position(|w| w == needle).map(|i| i + from)
}

/// UEFN takes no project on its command line: it opens the last one, so that is set first.
/// Only that one line changes, whatever else is in the file stays byte for byte the same.
fn set_last_project(file: &str) -> Result<(), String> {
    let path = settings_ini().ok_or("UEFN's settings were not found.")?;
    let Ok(bytes) = std::fs::read(&path) else { return Ok(()) };
    let wide = bytes.starts_with(&[0xFF, 0xFE]);
    let key = encoded("\nLastProjectFileName=", wide);
    // Without the line UEFN just shows its project browser.
    let Some(at) = find(&bytes, &key, 0) else { return Ok(()) };
    let start = at + key.len();
    let end = [find(&bytes, &encoded("\r", wide), start), find(&bytes, &encoded("\n", wide), start)].into_iter().flatten().min().unwrap_or(bytes.len());
    let mut next = bytes[..start].to_vec();
    next.extend(encoded(&file.replace('\\', "/"), wide));
    next.extend(&bytes[end..]);
    let temporary = path.with_extension("ini.loreplus");
    std::fs::write(&temporary, next).map_err(|e| e.to_string())?;
    std::fs::rename(&temporary, &path).map_err(|e| e.to_string())
}

/// Opens the project in its own folder on this PC, the one that works with Epic's server.
#[tauri::command]
pub async fn open_uefn(file: String) -> Result<(), String> {
    if running().await {
        return Err("UEFN is already open. Close it first, or open the project from UEFN's project browser.".into());
    }
    let editor = editor_path().ok_or("Unreal Editor for Fortnite was not found. Install it in the Epic Games Launcher.")?;
    set_last_project(&file)?;
    std::process::Command::new(editor).arg(&file).spawn().map_err(|e| e.to_string())?;
    Ok(())
}

/// Copies the project into its backup folder. /MIR removes what is gone from the project, so only a folder
/// that is a Lore project already is accepted; Lore's own data and ignore file there are left alone.
#[tauri::command]
pub async fn uefn_backup_copy(from: String, to: String) -> Result<(), String> {
    let (source, target) = (PathBuf::from(&from), PathBuf::from(&to));
    if target.starts_with(&source) {
        return Err("The backup cannot go inside the project itself.".into());
    }
    if !target.join(".lore").exists() {
        return Err("That folder is not a backup folder.".into());
    }
    mirror(&source, &target, &["/XD", ".lore", ".lore.old-*", ".urc", "Saved", "Intermediate", "Binaries", "DerivedDataCache", ".vs", ".vscode", ".git", "/XF", "session.resume", ".loreignore"]).await
}
