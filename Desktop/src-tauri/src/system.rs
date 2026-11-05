//! Things on this computer: Unreal installations, projects, downloads and installs.

use base64::Engine as _;
use serde_json::{json, Value};
use std::path::{Path, PathBuf};
use std::process::Stdio;
use tauri::{AppHandle, Emitter};
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::process::Command;

const PLUGIN_ZIP: &str = "https://github.com/epicaleric/lore-plugin-ue-improved/archive/refs/heads/main.zip";

fn user_agent() -> &'static str {
    "LoreHub"
}

fn log(app: &AppHandle, task: &str, line: impl Into<String>) {
    let _ = app.emit("task-log", json!({ "task": task, "line": line.into() }));
}

#[tauri::command]
pub fn default_roots() -> Vec<String> {
    let mut roots = Vec::new();
    if let Ok(profile) = std::env::var("USERPROFILE") {
        roots.push(format!(r"{profile}\Documents\Unreal Projects"));
    }
    roots
}

// ------------------------------------------------------------------ engines

#[tauri::command]
pub fn list_engines() -> Vec<Value> {
    let mut paths: Vec<PathBuf> = Vec::new();

    if let Ok(text) = std::fs::read_to_string(r"C:\ProgramData\Epic\UnrealEngineLauncher\LauncherInstalled.dat") {
        if let Ok(json) = serde_json::from_str::<Value>(&text) {
            if let Some(list) = json["InstallationList"].as_array() {
                for item in list {
                    let name = item["AppName"].as_str().unwrap_or("");
                    if name.starts_with("UE_") {
                        if let Some(location) = item["InstallLocation"].as_str() {
                            paths.push(PathBuf::from(location));
                        }
                    }
                }
            }
        }
    }
    for base in [r"C:\Program Files\Epic Games", r"C:\Games\Epic Games", r"D:\Epic Games", r"D:\Games\Epic Games"] {
        if let Ok(entries) = std::fs::read_dir(base) {
            for entry in entries.flatten() {
                if entry.file_name().to_string_lossy().starts_with("UE_") {
                    paths.push(entry.path());
                }
            }
        }
    }
    paths.sort();
    paths.dedup();

    let mut engines = Vec::new();
    for path in paths {
        let editor = path.join(r"Engine\Binaries\Win64\UnrealEditor.exe");
        if !editor.exists() {
            continue;
        }
        let mut version = path.file_name().map(|n| n.to_string_lossy().trim_start_matches("UE_").to_string()).unwrap_or_default();
        if let Ok(text) = std::fs::read_to_string(path.join(r"Engine\Build\Build.version")) {
            if let Ok(json) = serde_json::from_str::<Value>(&text) {
                if let (Some(major), Some(minor)) = (json["MajorVersion"].as_i64(), json["MinorVersion"].as_i64()) {
                    version = format!("{major}.{minor}");
                }
            }
        }
        let plugin = path.join(r"Engine\Plugins\Marketplace\LoreSourceControl\LoreSourceControl.uplugin");
        engines.push(json!({
            "version": version,
            "path": path.to_string_lossy(),
            "editor": editor.to_string_lossy(),
            "plugin": plugin_version_of(&plugin),
        }));
    }
    engines
}

fn plugin_version_of(uplugin: &Path) -> Value {
    std::fs::read_to_string(uplugin)
        .ok()
        .and_then(|text| serde_json::from_str::<Value>(&text).ok())
        .and_then(|json| json["VersionName"].as_str().map(|s| Value::String(s.to_string())))
        .unwrap_or(Value::Null)
}

#[tauri::command]
pub fn plugin_version(project_or_engine: String, kind: String) -> Value {
    let base = PathBuf::from(project_or_engine);
    let uplugin = if kind == "engine" {
        base.join(r"Engine\Plugins\Marketplace\LoreSourceControl\LoreSourceControl.uplugin")
    } else {
        base.join(r"Plugins\LoreSourceControl\LoreSourceControl.uplugin")
    };
    plugin_version_of(&uplugin)
}

// ------------------------------------------------------------------ projects

fn collect_lore_dirs(dir: &Path, depth: u32, out: &mut Vec<String>) {
    if dir.join(".lore").exists() {
        out.push(dir.to_string_lossy().to_string());
        return;
    }
    if depth == 0 {
        return;
    }
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    for entry in entries.flatten() {
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }
        let name = entry.file_name().to_string_lossy().to_string();
        if name.starts_with('.') || matches!(name.as_str(), "node_modules" | "Intermediate" | "Saved" | "Binaries" | "DerivedDataCache") {
            continue;
        }
        collect_lore_dirs(&path, depth - 1, out);
    }
}

#[tauri::command]
pub fn scan_lore_projects(roots: Vec<String>) -> Vec<String> {
    let mut found = Vec::new();
    for root in roots {
        collect_lore_dirs(Path::new(&root), 3, &mut found);
    }
    found.sort();
    found.dedup();
    found
}

#[tauri::command]
pub fn read_project(path: String) -> Value {
    let dir = PathBuf::from(&path);
    let mut uproject: Option<PathBuf> = None;
    if let Ok(entries) = std::fs::read_dir(&dir) {
        for entry in entries.flatten() {
            if entry.path().extension().map(|e| e == "uproject").unwrap_or(false) {
                uproject = Some(entry.path());
                break;
            }
        }
    }
    let mut engine = Value::Null;
    let mut description = Value::Null;
    let mut name = dir.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default();
    if let Some(file) = &uproject {
        if let Some(stem) = file.file_stem() {
            name = stem.to_string_lossy().to_string();
        }
        if let Ok(text) = std::fs::read_to_string(file) {
            if let Ok(json) = serde_json::from_str::<Value>(text.trim_start_matches('\u{feff}')) {
                engine = json["EngineAssociation"].clone();
                description = json["Description"].clone();
            }
        }
    }
    let mut thumbnail = Value::Null;
    for candidate in [r"Saved\AutoScreenshot.png", r"Thumbnail.png", r"Saved\Thumbnail.png"] {
        let path = dir.join(candidate);
        if path.exists() {
            thumbnail = Value::String(path.to_string_lossy().to_string());
            break;
        }
    }
    json!({
        "path": path,
        "name": name,
        "uproject": uproject.map(|p| p.to_string_lossy().to_string()),
        "engine": engine,
        "description": description,
        "thumbnail": thumbnail,
        "plugin": plugin_version_of(&dir.join(r"Plugins\LoreSourceControl\LoreSourceControl.uplugin")),
    })
}

#[tauri::command]
pub fn read_image(path: String) -> Option<String> {
    let bytes = std::fs::read(&path).ok()?;
    if bytes.len() > 6_000_000 {
        return None;
    }
    let mime = if path.to_lowercase().ends_with(".jpg") || path.to_lowercase().ends_with(".jpeg") { "image/jpeg" } else { "image/png" };
    Some(format!("data:{mime};base64,{}", base64::engine::general_purpose::STANDARD.encode(bytes)))
}

#[tauri::command]
pub fn open_project(uproject: String, editor: Option<String>) -> Result<(), String> {
    match editor {
        Some(editor) => {
            let mut command = std::process::Command::new(editor);
            command.arg(&uproject);
            command.spawn().map_err(|e| e.to_string())?;
        }
        None => {
            std::process::Command::new("explorer").arg(&uproject).spawn().map_err(|e| e.to_string())?;
        }
    }
    Ok(())
}

#[tauri::command]
pub fn open_path(path: String) -> Result<(), String> {
    std::process::Command::new("explorer").arg(path).spawn().map_err(|e| e.to_string())?;
    Ok(())
}


#[tauri::command]
pub fn write_text_file(path: String, contents: String, overwrite: bool) -> Result<bool, String> {
    let target = Path::new(&path);
    if target.exists() && !overwrite {
        return Ok(false);
    }
    if let Some(parent) = target.parent() {
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    std::fs::write(target, contents).map_err(|e| e.to_string())?;
    Ok(true)
}


/// Removes a working copy. Only folders that hold a Lore working copy are accepted.
#[tauri::command]
pub fn delete_folder(path: String) -> Result<(), String> {
    let dir = Path::new(&path);
    if !dir.join(".lore").exists() {
        return Err("This folder is not a Lore working copy, so it was not touched.".into());
    }
    if dir.components().count() < 3 {
        return Err("That folder is too close to the top of the disk.".into());
    }
    std::fs::remove_dir_all(dir).map_err(|e| format!("Could not remove the folder: {e}"))
}
#[tauri::command]
pub fn path_exists(path: String) -> bool {
    Path::new(&path).exists()
}

#[tauri::command]
pub fn read_text_file(path: String) -> Option<String> {
    std::fs::read_to_string(path).ok()
}
// ------------------------------------------------------------------ web

#[tauri::command]
pub async fn http_get_json(url: String) -> Result<Value, String> {
    let client = reqwest::Client::builder().user_agent(user_agent()).timeout(std::time::Duration::from_secs(15)).build().map_err(|e| e.to_string())?;
    let response = client.get(url).header("Accept", "application/vnd.github+json").send().await.map_err(|e| e.to_string())?;
    if !response.status().is_success() {
        return Err(format!("HTTP {}", response.status().as_u16()));
    }
    response.json::<Value>().await.map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn http_get_text(url: String) -> Result<String, String> {
    let client = reqwest::Client::builder().user_agent(user_agent()).timeout(std::time::Duration::from_secs(15)).build().map_err(|e| e.to_string())?;
    let response = client.get(url).send().await.map_err(|e| e.to_string())?;
    if !response.status().is_success() {
        return Err(format!("HTTP {}", response.status().as_u16()));
    }
    response.text().await.map_err(|e| e.to_string())
}

async fn download(app: &AppHandle, task: &str, url: &str, target: &Path) -> Result<(), String> {
    use futures_util::StreamExt;
    log(app, task, format!("Downloading {url}"));
    let client = reqwest::Client::builder().user_agent(user_agent()).build().map_err(|e| e.to_string())?;
    let response = client.get(url).send().await.map_err(|e| e.to_string())?;
    if !response.status().is_success() {
        return Err(format!("Download failed: HTTP {}", response.status().as_u16()));
    }
    let total = response.content_length().unwrap_or(0);
    let mut file = tokio::fs::File::create(target).await.map_err(|e| e.to_string())?;
    let mut stream = response.bytes_stream();
    let mut done = 0u64;
    let mut last_report = 0u64;
    use tokio::io::AsyncWriteExt;
    while let Some(chunk) = stream.next().await {
        let chunk = chunk.map_err(|e| e.to_string())?;
        file.write_all(&chunk).await.map_err(|e| e.to_string())?;
        done += chunk.len() as u64;
        if total > 0 && done - last_report > 2_000_000 {
            last_report = done;
            log(app, task, format!("  {} of {} MB", done / 1_000_000, total / 1_000_000));
        }
    }
    Ok(())
}

// ------------------------------------------------------------------ installs

#[tauri::command]
pub async fn install_lore_client(app: AppHandle, url: String) -> Result<(), String> {
    let task = "lore-client";
    let target = std::env::temp_dir().join("lorehub-lore-client.msi");
    download(&app, task, &url, &target).await?;
    log(&app, task, "Installing (Windows asks for permission)...");
    let script = format!(
        "Start-Process msiexec.exe -ArgumentList '/i','\"{}\"','/qn' -Verb RunAs -Wait",
        target.to_string_lossy()
    );
    let status = Command::new("powershell")
        .args(["-NoProfile", "-Command", &script])
        .creation_flags_hidden()
        .status()
        .await
        .map_err(|e| e.to_string())?;
    if !status.success() {
        return Err("The installer did not finish. Was the permission prompt declined?".into());
    }
    log(&app, task, "Lore client installed.");
    Ok(())
}

#[tauri::command]
pub async fn install_app_update(app: AppHandle, url: String) -> Result<(), String> {
    let task = "app";
    let target = std::env::temp_dir().join("lorehub-update-setup.exe");
    download(&app, task, &url, &target).await?;
    log(&app, task, "Starting the installer, the app will restart.");
    std::process::Command::new(&target).args(["/S", "/R"]).spawn().map_err(|e| e.to_string())?;
    tokio::time::sleep(std::time::Duration::from_millis(600)).await;
    app.exit(0);
    Ok(())
}

fn copy_dir(from: &Path, to: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(to)?;
    for entry in std::fs::read_dir(from)? {
        let entry = entry?;
        let target = to.join(entry.file_name());
        if entry.path().is_dir() {
            copy_dir(&entry.path(), &target)?;
        } else {
            std::fs::copy(entry.path(), target)?;
        }
    }
    Ok(())
}

fn find_plugin_root(dir: &Path, depth: u32) -> Option<PathBuf> {
    if dir.join("LoreSourceControl.uplugin").exists() {
        return Some(dir.to_path_buf());
    }
    if depth == 0 {
        return None;
    }
    for entry in std::fs::read_dir(dir).ok()?.flatten() {
        if entry.path().is_dir() {
            if let Some(found) = find_plugin_root(&entry.path(), depth - 1) {
                return Some(found);
            }
        }
    }
    None
}

/// kind = "project" (copy the source into the project) or "engine" (build with RunUAT, install into the engine).
#[tauri::command]
pub async fn install_plugin(app: AppHandle, kind: String, target: String) -> Result<(), String> {
    let task = format!("plugin:{target}");
    let work = std::env::temp_dir().join("lorehub-plugin");
    let _ = std::fs::remove_dir_all(&work);
    std::fs::create_dir_all(&work).map_err(|e| e.to_string())?;

    let zip_path = work.join("plugin.zip");
    download(&app, &task, PLUGIN_ZIP, &zip_path).await?;

    log(&app, &task, "Unpacking...");
    let extract_to = work.join("src");
    {
        let file = std::fs::File::open(&zip_path).map_err(|e| e.to_string())?;
        let mut archive = zip::ZipArchive::new(file).map_err(|e| e.to_string())?;
        archive.extract(&extract_to).map_err(|e| e.to_string())?;
    }
    let plugin_root = find_plugin_root(&extract_to, 4).ok_or("The plugin was not found in the download")?;

    if kind == "project" {
        let destination = Path::new(&target).join("Plugins").join("LoreSourceControl");
        log(&app, &task, format!("Copying into {}", destination.display()));
        if destination.exists() {
            // Keep any built binaries out of the way of a clean source copy.
            std::fs::remove_dir_all(&destination).map_err(|e| e.to_string())?;
        }
        copy_dir(&plugin_root, &destination).map_err(|e| e.to_string())?;
        log(&app, &task, "Done. The project compiles the plugin the next time it is opened.");
        return Ok(());
    }

    // Engine install: build with the engine's own tools.
    let engine = PathBuf::from(&target);
    let run_uat = engine.join(r"Engine\Build\BatchFiles\RunUAT.bat");
    if !run_uat.exists() {
        return Err("RunUAT.bat was not found in this engine".into());
    }
    let package = work.join("package");
    let uplugin = plugin_root.join("LoreSourceControl.uplugin");
    log(&app, &task, "Building the plugin for this engine. This takes a few minutes...");
    let mut child = Command::new("cmd")
        .arg("/C")
        .arg(&run_uat)
        .arg("BuildPlugin")
        .arg(format!("-Plugin={}", uplugin.display()))
        .arg(format!("-Package={}", package.display()))
        .arg("-TargetPlatforms=Win64")
        .arg("-Rocket")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .creation_flags_hidden()
        .spawn()
        .map_err(|e| e.to_string())?;
    if let Some(stdout) = child.stdout.take() {
        let app2 = app.clone();
        let task2 = task.clone();
        tokio::spawn(async move {
            let mut lines = BufReader::new(stdout).lines();
            while let Ok(Some(line)) = lines.next_line().await {
                if line.contains("Error") || line.contains("BUILD") || line.contains("Compiling") {
                    log(&app2, &task2, line);
                }
            }
        });
    }
    let status = child.wait().await.map_err(|e| e.to_string())?;
    if !status.success() {
        return Err("The build failed. Check that Visual Studio's C++ tools are installed for this engine.".into());
    }
    let destination = engine.join(r"Engine\Plugins\Marketplace\LoreSourceControl");
    if destination.exists() {
        std::fs::remove_dir_all(&destination).map_err(|e| e.to_string())?;
    }
    copy_dir(&package, &destination).map_err(|e| format!("Could not copy into the engine folder: {e}"))?;
    log(&app, &task, "Installed into the engine.");
    Ok(())
}

trait HiddenWindow {
    fn creation_flags_hidden(&mut self) -> &mut Self;
}

impl HiddenWindow for Command {
    #[cfg(windows)]
    fn creation_flags_hidden(&mut self) -> &mut Self {
        self.creation_flags(0x0800_0000)
    }
    #[cfg(not(windows))]
    fn creation_flags_hidden(&mut self) -> &mut Self {
        self
    }
}


/// Moves a working copy to another folder on the same disk. Nothing is copied or deleted.
#[tauri::command]
pub fn move_folder(from: String, to_parent: String) -> Result<String, String> {
    let source = Path::new(&from);
    if !source.join(".lore").exists() {
        return Err("This folder is not a Lore working copy, so it was not touched.".into());
    }
    let name = source.file_name().ok_or("The folder has no name.")?;
    let target = Path::new(&to_parent).join(name);
    if target.exists() {
        return Err(format!("{} already exists.", target.display()));
    }
    std::fs::rename(source, &target).map_err(|e| format!("Could not move the folder ({e}). Moving works within one disk; for another disk, move it in Explorer and use Find a copy."))?;
    Ok(target.to_string_lossy().to_string())
}

/// Puts the `.lore` folder of a working copy aside, under a new name, so nothing is lost.
#[tauri::command]
pub fn set_aside_lore(path: String) -> Result<String, String> {
    let lore = Path::new(&path).join(".lore");
    if !lore.exists() {
        return Ok(String::new());
    }
    let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let target = Path::new(&path).join(format!(".lore.old-{stamp}"));
    std::fs::rename(&lore, &target).map_err(|e| e.to_string())?;
    Ok(target.to_string_lossy().to_string())
}

/// Number of files and bytes that would go into a project, leaving out generated folders.
#[tauri::command]
pub fn folder_stats(path: String) -> Value {
    fn walk(dir: &Path, files: &mut u64, bytes: &mut u64) {
        let Ok(entries) = std::fs::read_dir(dir) else { return };
        for entry in entries.flatten() {
            let path = entry.path();
            let name = entry.file_name().to_string_lossy().to_string();
            if path.is_dir() {
                if ["Saved", "Intermediate", "DerivedDataCache", "Binaries", "Builds", ".lore", ".git", ".vs", ".idea", ".vscode"].contains(&name.as_str()) || name.starts_with(".lore.old") {
                    continue;
                }
                walk(&path, files, bytes);
            } else if let Ok(meta) = entry.metadata() {
                *files += 1;
                *bytes += meta.len();
            }
        }
    }
    let (mut files, mut bytes) = (0u64, 0u64);
    walk(Path::new(&path), &mut files, &mut bytes);
    json!({ "files": files, "bytes": bytes })
}

/// Renames a whole folder out of the way (nothing is deleted). Used for the cached copy of the shared settings when it is broken.
#[tauri::command]
pub fn set_aside_folder(path: String) -> Result<String, String> {
    let dir = Path::new(&path);
    if !dir.exists() {
        return Ok(String::new());
    }
    let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let mut name = dir.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default();
    name.push_str(&format!(".old-{stamp}"));
    let target = dir.with_file_name(name);
    std::fs::rename(dir, &target).map_err(|e| e.to_string())?;
    Ok(target.to_string_lossy().to_string())
}

/// Gives a working copy a clean `.lore` taken from a fresh clone of its (still empty) project. The old one is renamed, not deleted.
#[tauri::command]
pub fn relink_lore(folder: String, fresh: String) -> Result<String, String> {
    let from = Path::new(&fresh).join(".lore");
    if !from.exists() {
        return Err("The fresh clone has no .lore folder.".into());
    }
    let to = Path::new(&folder).join(".lore");
    let mut aside = String::new();
    if to.exists() {
        let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
        let target = Path::new(&folder).join(format!(".lore.old-{stamp}"));
        std::fs::rename(&to, &target).map_err(|e| e.to_string())?;
        aside = target.to_string_lossy().to_string();
    }
    copy_dir(&from, &to).map_err(|e| e.to_string())?;
    let _ = std::fs::remove_dir_all(&fresh);
    Ok(aside)
}
