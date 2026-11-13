//! File work around undoing changes and sync conflicts: a backup copy always comes first.

use std::path::{Component, Path, PathBuf};

/// A path from `lore status` inside the project folder. Anything that could leave the folder is refused.
fn inside(root: &Path, relative: &str) -> Result<PathBuf, String> {
    let relative = Path::new(relative);
    if relative.components().any(|c| !matches!(c, Component::Normal(_))) {
        return Err(format!("{} is not a path inside the project", relative.display()));
    }
    Ok(root.join(relative))
}

/// Copies a file, or a folder with everything in it, creating the parents of the target.
fn copy_any(source: &Path, target: &Path) -> std::io::Result<()> {
    if source.is_dir() {
        std::fs::create_dir_all(target)?;
        for entry in std::fs::read_dir(source)? {
            let entry = entry?;
            copy_any(&entry.path(), &target.join(entry.file_name()))?;
        }
        return Ok(());
    }
    if let Some(parent) = target.parent() {
        std::fs::create_dir_all(parent)?;
    }
    std::fs::copy(source, target).map(|_| ())
}

/// Copies the files as they are on this PC to `<project>/Saved/LoreBackups/<stamp>/<path>` and returns that folder.
/// Folders are copied whole. Files that are not there (deleted on this PC) are skipped. Any other failure stops everything, so nothing is
/// discarded without a copy.
#[tauri::command]
pub fn backup_files(root: String, stamp: String, paths: Vec<String>) -> Result<String, String> {
    let root = PathBuf::from(root);
    if stamp.is_empty() || stamp.contains(['/', '\\', '.']) {
        return Err("Invalid backup name".into());
    }
    let folder = root.join("Saved").join("LoreBackups").join(&stamp);
    for path in &paths {
        let source = inside(&root, path)?;
        if !source.exists() {
            continue;
        }
        copy_any(&source, &inside(&folder, path)?).map_err(|e| format!("Could not back up {path}: {e}"))?;
    }
    std::fs::create_dir_all(&folder).map_err(|e| e.to_string())?;
    Ok(folder.to_string_lossy().to_string())
}

/// Puts files from a backup folder back into the project, over what is there now.
#[tauri::command]
pub fn restore_files(backup: String, root: String, paths: Vec<String>) -> Result<(), String> {
    let backup = PathBuf::from(backup);
    let root = PathBuf::from(root);
    for path in &paths {
        let source = inside(&backup, path)?;
        if !source.exists() {
            continue;
        }
        copy_any(&source, &inside(&root, path)?).map_err(|e| format!("Could not put back {path}: {e}"))?;
    }
    Ok(())
}

/// Removes new files that Lore does not know yet (`lore reset` leaves those alone). Only call after `backup_files`.
#[tauri::command]
pub fn remove_files(root: String, paths: Vec<String>) -> Result<(), String> {
    let root = PathBuf::from(root);
    for path in &paths {
        let target = inside(&root, path)?;
        if target.is_file() {
            std::fs::remove_file(&target).map_err(|e| format!("Could not remove {path}: {e}"))?;
        }
    }
    Ok(())
}

/// Opens Explorer with the file selected, or the nearest folder that exists.
#[tauri::command]
pub fn reveal_path(path: String) -> Result<(), String> {
    let target = PathBuf::from(path.replace('/', "\\"));
    let mut command = std::process::Command::new("explorer");
    if target.is_file() {
        command.arg(format!("/select,{}", target.display()));
    } else {
        let mut folder = target.as_path();
        while !folder.is_dir() {
            match folder.parent() {
                Some(parent) => folder = parent,
                None => break,
            }
        }
        command.arg(folder);
    }
    command.spawn().map_err(|e| e.to_string())?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn backup_restore_and_remove() {
        let root = std::env::temp_dir().join(format!("loreplus-backup-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(root.join("Content")).unwrap();
        std::fs::write(root.join("Content/A.uasset"), b"mine").unwrap();
        let folder = backup_files(root.to_string_lossy().into(), "2026-11-13_12-00-00".into(), vec!["Content/A.uasset".into(), "Gone.txt".into()]).unwrap();
        assert_eq!(std::fs::read(Path::new(&folder).join("Content/A.uasset")).unwrap(), b"mine");
        std::fs::write(root.join("Content/A.uasset"), b"theirs").unwrap();
        restore_files(folder.clone(), root.to_string_lossy().into(), vec!["Content/A.uasset".into()]).unwrap();
        assert_eq!(std::fs::read(root.join("Content/A.uasset")).unwrap(), b"mine");
        remove_files(root.to_string_lossy().into(), vec!["Content/A.uasset".into()]).unwrap();
        assert!(!root.join("Content/A.uasset").exists());
        assert!(backup_files(root.to_string_lossy().into(), "x".into(), vec!["../escape".into()]).is_err());
        let _ = std::fs::remove_dir_all(&root);
    }
}
