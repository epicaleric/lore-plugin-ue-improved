mod changes;
mod grpc;
mod lore;
mod system;
mod uefn;

use lore::{CliResult, Server};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::time::Instant;
use tauri::{AppHandle, Emitter, Manager};

// ------------------------------------------------------------------ settings

fn settings_path(app: &AppHandle) -> Result<std::path::PathBuf, String> {
    let dir = app.path().app_data_dir().map_err(|e| e.to_string())?;
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let target = dir.join("settings.json");
    // The app used to be called Lore Hub (nl.ericflix.lorehub): take its settings along once.
    if !target.exists() {
        if let Some(old) = dir.parent().map(|p| p.join("nl.ericflix.lorehub").join("settings.json")) {
            let _ = std::fs::copy(old, &target);
        }
    }
    Ok(target)
}

#[tauri::command]
fn get_settings(app: AppHandle) -> Result<Value, String> {
    let path = settings_path(&app)?;
    match std::fs::read_to_string(path) {
        Ok(text) => Ok(serde_json::from_str(&text).unwrap_or_else(|_| json!({}))),
        Err(_) => Ok(json!({})),
    }
}

#[tauri::command]
fn save_settings(app: AppHandle, settings: Value) -> Result<(), String> {
    let path = settings_path(&app)?;
    let text = serde_json::to_string_pretty(&settings).map_err(|e| e.to_string())?;
    std::fs::write(path, text).map_err(|e| e.to_string())
}

/// Folder for the shared-settings working copy of a server. The parent is created, the folder itself is left to `lore clone`.
#[tauri::command]
fn meta_dir(app: AppHandle, server_id: String) -> Result<String, String> {
    let dir = app.path().app_data_dir().map_err(|e| e.to_string())?.join("meta");
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    Ok(dir.join(server_id).to_string_lossy().to_string())
}

/// Folder where bare clones of the projects on the server are kept. A bare clone has no files, only the tree of the latest revision.
#[tauri::command]
fn bare_dir(app: AppHandle) -> Result<String, String> {
    let dir = app.path().app_data_dir().map_err(|e| e.to_string())?.join("bare");
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    Ok(dir.to_string_lossy().to_string())
}

/// Removes something from the bare clone folder (it is only a cache of what is on the server).
#[tauri::command]
fn remove_cache(app: AppHandle, path: String) -> Result<(), String> {
    let base = app.path().app_data_dir().map_err(|e| e.to_string())?.join("bare");
    let target = std::path::PathBuf::from(&path);
    if !target.starts_with(&base) || target == base {
        return Err("That is not part of the cache.".into());
    }
    if target.is_dir() {
        std::fs::remove_dir_all(&target).map_err(|e| e.to_string())?;
    }
    Ok(())
}

// ------------------------------------------------------------------ sign-in

#[derive(Serialize, Deserialize, Clone)]
#[serde(rename_all = "camelCase")]
struct Stored {
    token: String,
    expires_at: i64,
    user_id: String,
    user_name: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct SessionInfo {
    user_name: String,
    user_id: String,
    expires_at: i64,
}

fn entry(server_id: &str) -> Result<keyring::Entry, String> {
    keyring::Entry::new("LoreHub", server_id).map_err(|e| e.to_string())
}

fn load_token(server_id: &str) -> Option<Stored> {
    let text = entry(server_id).ok()?.get_password().ok()?;
    serde_json::from_str(&text).ok()
}

fn save_token(server_id: &str, token: &grpc::UserToken) -> Result<SessionInfo, String> {
    let stored = Stored {
        token: token.token.clone(),
        expires_at: token.expires_at,
        user_id: token.user_id.clone(),
        user_name: token.user_name.clone(),
    };
    let text = serde_json::to_string(&stored).map_err(|e| e.to_string())?;
    entry(server_id)?.set_password(&text).map_err(|e| e.to_string())?;
    Ok(SessionInfo { user_name: stored.user_name, user_id: stored.user_id, expires_at: stored.expires_at })
}

fn random_state() -> String {
    use rand::RngCore;
    let mut bytes = [0u8; 16];
    rand::thread_rng().fill_bytes(&mut bytes);
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

#[tauri::command]
async fn sign_in(server: Server, username: String, password: String) -> Result<SessionInfo, String> {
    let state = random_state();
    let session = grpc::start_session(&server.auth_url, &state).await?;
    if session.is_empty() {
        return Err("The authentication service did not start a sign-in session".into());
    }
    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(20))
        .build()
        .map_err(|e| e.to_string())?;
    let response = client
        .post(format!("{}/api/sign-in", server.web_url.trim_end_matches('/')))
        .json(&json!({ "session": session, "username": username, "password": password }))
        .send()
        .await
        .map_err(|e| format!("Could not reach the sign-in page: {e}"))?;
    if !response.status().is_success() {
        let body: Value = response.json().await.unwrap_or_else(|_| json!({}));
        let message = body["error"]["message"].as_str().unwrap_or("Sign-in failed").to_string();
        return Err(message);
    }
    let token = grpc::get_session(&server.auth_url, &session, &state).await?;
    save_token(&server.id, &token)
}

#[tauri::command]
async fn sign_in_api_key(server: Server, key: String) -> Result<SessionInfo, String> {
    let token = grpc::exchange_api_key(&server.auth_url, &key).await?;
    save_token(&server.id, &token)
}

#[tauri::command]
fn session_info(server_id: String) -> Option<SessionInfo> {
    let stored = load_token(&server_id)?;
    Some(SessionInfo { user_name: stored.user_name, user_id: stored.user_id, expires_at: stored.expires_at })
}

#[tauri::command]
fn sign_out(server_id: String) -> Result<(), String> {
    if let Ok(entry) = entry(&server_id) {
        let _ = entry.delete_credential();
    }
    Ok(())
}

// ------------------------------------------------------------------ lore client

#[tauri::command]
async fn lore_cli(args: Vec<String>, repository: Option<String>, server_id: Option<String>) -> CliResult {
    let token = server_id.as_deref().and_then(load_token).map(|s| s.token);
    lore::run(&args, repository.as_deref(), token.as_deref()).await
}

/// Same as `lore_cli`, and every line of output is sent to the window as a `lore-line` event while it runs.
#[tauri::command]
async fn lore_cli_stream(app: AppHandle, args: Vec<String>, repository: Option<String>, server_id: Option<String>, stream_id: String) -> CliResult {
    let token = server_id.as_deref().and_then(load_token).map(|s| s.token);
    let id = stream_id.clone();
    lore::run_stream(&stream_id, &args, repository.as_deref(), token.as_deref(), move |line| {
        let _ = app.emit("lore-line", json!({ "id": id, "line": line }));
    })
    .await
}

#[tauri::command]
async fn lore_client_login(server: Server) -> Result<CliResult, String> {
    let stored = load_token(&server.id).ok_or("Not signed in")?;
    Ok(lore::login(&server, &stored.token).await)
}

/// Stops a command that was started with `lore_cli_stream`.
#[tauri::command]
fn lore_cancel(stream_id: String) {
    lore::cancel(&stream_id);
}

// ------------------------------------------------------------------ admin API

#[tauri::command]
async fn admin_request(server: Server, method: String, path: String, body: Option<Value>) -> Result<Value, String> {
    let stored = load_token(&server.id).ok_or("Not signed in")?;
    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(20))
        .build()
        .map_err(|e| e.to_string())?;
    let method = reqwest::Method::from_bytes(method.as_bytes()).map_err(|e| e.to_string())?;
    let url = format!("{}/api/v1{}", server.web_url.trim_end_matches('/'), path);
    let mut request = client.request(method, url).bearer_auth(stored.token);
    if let Some(body) = body {
        request = request.json(&body);
    }
    let response = request.send().await.map_err(|e| format!("Could not reach the admin API: {e}"))?;
    let status = response.status().as_u16();
    let text = response.text().await.unwrap_or_default();
    let body: Value = if text.is_empty() { Value::Null } else { serde_json::from_str(&text).unwrap_or(Value::String(text)) };
    Ok(json!({ "status": status, "body": body }))
}

#[tauri::command]
async fn server_health(server: Server) -> Value {
    async fn probe(url: String) -> Value {
        let client = match reqwest::Client::builder().timeout(std::time::Duration::from_secs(5)).build() {
            Ok(client) => client,
            Err(_) => return json!({ "ok": false, "ms": 0 }),
        };
        let started = Instant::now();
        let ok = matches!(client.get(url).send().await, Ok(r) if r.status().is_success());
        json!({ "ok": ok, "ms": started.elapsed().as_millis() as u64 })
    }
    let host = server
        .lore_url
        .trim_start_matches("lores://")
        .trim_start_matches("lore://")
        .split([':', '/'])
        .next()
        .unwrap_or("")
        .to_string();
    let lore = probe(format!("http://{host}:41339/health_check")).await;
    let auth = probe(format!("{}/health", server.web_url.trim_end_matches('/'))).await;
    json!({ "lore": lore, "auth": auth })
}

// ------------------------------------------------------------------ app

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .invoke_handler(tauri::generate_handler![
            get_settings,
            save_settings,
            meta_dir,
            bare_dir,
            remove_cache,
            system::read_text_file,
            sign_in,
            sign_in_api_key,
            session_info,
            sign_out,
            lore_cli,
            lore_cli_stream,
            lore_cancel,
            lore_client_login,
            admin_request,
            server_health,
            system::list_engines,
            system::scan_lore_projects,
            system::read_project,
            system::read_image,
            system::open_project,
            system::open_path,
            system::plugin_version,
            system::http_get_json,
            system::http_get_text,
            system::install_lore_client,
            system::install_app_update,
            system::install_plugin,
            system::default_roots,
            system::write_text_file,
            system::path_exists,
            system::delete_folder,
            system::move_folder,
            system::set_aside_lore,
            system::set_aside_folder,
            system::relink_lore,
            system::repo_id,
            system::git_remotes,
            system::git_mirror,
            system::write_binary_file,
            system::folder_stats,
            system::folder_listing,
            system::copy_project,
            system::rename_path,
            changes::backup_files,
            changes::restore_files,
            changes::remove_files,
            changes::reveal_path,
            uefn::scan_uefn_projects,
            uefn::uefn_running,
            uefn::uefn_snapshot,
            uefn::open_uefn,
            uefn::uefn_backup_copy,
        ])
        .build(tauri::generate_context!())
        .expect("error while building Lore+")
        .run(|_app, event| {
            // A lore command must not keep running (and writing) after the app is gone.
            if let tauri::RunEvent::Exit = event {
                lore::cancel_all();
            }
        });
}
