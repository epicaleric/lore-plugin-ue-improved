import { api } from "./api";

/** Where backups go, inside the project. */
export const BACKUP_FOLDER = "Saved/LoreBackups";

/** Text files could in principle be merged; everything else (assets, maps, DLLs, pictures) cannot. */
const TEXT = /\.(ini|txt|md|json|csv|xml|ya?ml|toml|cpp|cc|c|h|hpp|inl|cs|py|ps1|bat|cmd|sh|js|ts|tsx|verse|uproject|uplugin|usf|ush|loreignore|gitignore)$/i;
export const isText = (path: string) => TEXT.test(path);

/** The compiled editor module of an Unreal project, which the editor rebuilds on its own. */
export const isBuiltModule = (path: string) => /^Binaries\/Win64\/[^/]+\.(dll|modules)$/i.test(path);

/** 2026-11-13_14-05-09 in local time, for backup folder names. */
export function stamp(date = new Date()) {
  const two = (n: number) => String(n).padStart(2, "0");
  return `${date.getFullYear()}-${two(date.getMonth() + 1)}-${two(date.getDate())}_${two(date.getHours())}-${two(date.getMinutes())}-${two(date.getSeconds())}`;
}

/** Makes sure Lore never picks up the backups as changes. Unreal projects already ignore all of Saved. */
async function ignoreBackups(root: string) {
  const file = `${root}/.loreignore`;
  const text = (await api.readText(file)) ?? "";
  if (/^\/?Saved(\/(\*\*)?)?\s*$/m.test(text) || /^\/?Saved\/LoreBackups/m.test(text)) return;
  await api.writeText(file, `${text.replace(/\s*$/, text ? "\n" : "")}/${BACKUP_FOLDER}/\n`);
}

/** Copies the files as they are on this PC to a new backup folder and returns that folder. Throws when that fails. */
export async function backup(root: string, paths: string[]) {
  await ignoreBackups(root);
  return api.backupFiles(root, stamp(), paths);
}

async function targets(paths: string[]) {
  const file = `${await api.bareDir()}/targets.txt`;
  await api.writeText(file, paths.join("\n") + "\n");
  return file;
}

const failure = (result: { stderr: string; stdout: string }) => new Error((result.stderr || result.stdout).trim().split("\n").filter((l) => !/^\s+at /.test(l)).slice(0, 3).join(" "));

export interface FileChange {
  path: string;
  /** A new, M changed, D deleted, R moved */
  kind: string;
  staged: boolean;
}

/**
 * Puts files back to the version this PC is on. A backup copy is made first; nothing is touched when that fails.
 * Returns the backup folder.
 */
export async function undoChanges(root: string, serverId: string | undefined, changes: FileChange[]) {
  if (!changes.length) return null;
  const folder = await backup(root, changes.map((c) => c.path));
  // Lore does not reset staged files.
  const staged = changes.filter((c) => c.staged).map((c) => c.path);
  if (staged.length) {
    const result = await api.lore(["unstage", "--targets", await targets(staged)], root, serverId);
    if (!result.ok) throw failure(result);
  }
  const known = changes.filter((c) => c.kind !== "A").map((c) => c.path);
  if (known.length) {
    const result = await api.lore(["reset", "--targets", await targets(known)], root, serverId);
    if (!result.ok) throw failure(result);
  }
  // New files are not in any version: undoing them means removing them (the backup keeps them).
  const added = changes.filter((c) => c.kind === "A").map((c) => c.path);
  if (added.length) await api.removeFiles(root, added);
  return folder;
}

/** Resets files that are in the way of a sync, after a backup. Returns the backup folder. */
export async function setAside(root: string, serverId: string | undefined, paths: string[]) {
  const folder = await backup(root, paths);
  await api.lore(["unstage", "--targets", await targets(paths)], root, serverId);
  const result = await api.lore(["reset", "--targets", await targets(paths)], root, serverId);
  if (!result.ok) throw failure(result);
  return folder;
}

/** Who holds a lock on which file: `path -> owner`. Empty when the server cannot be asked. */
export async function readLocks(root: string, serverId: string | undefined) {
  const locks = new Map<string, string>();
  const result = await api.lore(["lock", "query"], root, serverId);
  if (!result.ok) return locks;
  for (const line of result.stdout.split(/\r?\n/)) {
    const match = line.trim().match(/^(.+?) by (.+?) on branch \S+$/);
    if (match) locks.set(match[1], match[2]);
  }
  return locks;
}

export const sameUser = (a: string | undefined, b: string | undefined) => !!a && !!b && a.trim().toLowerCase() === b.trim().toLowerCase();
