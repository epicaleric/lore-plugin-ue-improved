import { api } from "./api";
import { endJob, startJob } from "./jobs";
import type { Server, UefnBackup, UefnProject } from "./types";

export const BACKUP_SUFFIX = "-UEFN-backup";

/** A project on the server that is the backup of a UEFN project. Also recognised on a PC without UEFN. */
export const isUefnBackup = (name: string) => name.endsWith(BACKUP_SUFFIX);

/** Server names stay simple: letters, digits, dots, dashes and underscores. */
export const serverName = (name: string) => name.trim().replace(/[^A-Za-z0-9._-]+/g, "-").replace(/^-+|-+$/g, "");

const IGNORE = "**/*.tmp\n**/*.temp\n";

/** Whether a folder is tied to Epic's server. Those belong to UEFN, the app must not touch their Lore data. */
export const isEpicProject = async (folder: string) => /\.epicgames\.com/.test((await api.readText(`${folder}/.lore/config.toml`)) ?? "") || api.exists(`${folder}/.urc`);

/**
 * Saves the project as it is on this PC as a new version on the server. This goes through a separate folder
 * with its own Lore link, because the project's own folder is linked to Epic's server.
 * Returns false when nothing had changed since the last backup.
 */
export async function backupUefn(server: Server, project: UefnProject, backup: UefnBackup, onLine: (line: string) => void = () => {}) {
  const { folder } = backup;
  if (!startJob(folder)) throw new Error("This project is already being backed up.");
  const fail = (result: { stderr: string; stdout: string }) => new Error((result.stderr || result.stdout).trim().split("\n").slice(0, 3).join(" "));
  const work = (args: string[]) => api.loreStream(args, folder, server.id, onLine);
  try {
    if (!(await api.exists(`${folder}/.lore`))) {
      await api.writeText(`${folder}/.loreignore`, IGNORE);
      const created = await api.lore(["repository", "create", `${server.loreUrl}/${backup.name}`], folder, server.id);
      if (!created.ok) throw fail(created);
    }
    onLine("Copying the project");
    await api.uefnBackupCopy(project.path, folder);

    let result = await work(["stage", "--scan", "."]);
    if (!result.ok && !/nothing/i.test(result.stderr + result.stdout)) throw fail(result);
    const revision = project.revisions?.[0]?.number;
    result = await work(["commit", `Backup of ${project.name}${revision ? `, UEFN revision ${revision}` : ""}`]);
    if (!result.ok) {
      if (/nothing/i.test(result.stderr + result.stdout)) return false;
      throw fail(result);
    }
    result = await work(["push"]);
    if (!result.ok) throw fail(result);
    return true;
  } finally {
    endJob(folder);
  }
}
