import { api } from "./api";
import { parseHistory, parseStatus } from "./parse";
import type { LocalProject, MirrorConfig, Server, Settings } from "./types";

/** Lore's own folders stay out of Git, whatever the repository's .gitignore says. */
async function keepLoreOutOfGit(path: string) {
  const file = `${path}/.git/info/exclude`;
  const text = (await api.readText(file)) ?? "";
  if (/^\.lore\/$/m.test(text)) return;
  await api.writeText(file, `${text.replace(/\s*$/, "\n")}.lore/\n.lore.old-*/\n`);
}

/** Sends what is in the working copy to GitHub with the description of the Lore version. */
export async function mirrorNow(path: string, config: MirrorConfig, message: string) {
  await keepLoreOutOfGit(path);
  return api.gitMirror(path, config.remote, config.branch, message);
}

/**
 * Projects with a GitHub mirror: gets new versions from the server, and when there is a revision GitHub does not have yet,
 * sends it. Returns the revisions that were sent, by repository id.
 */
export async function runMirrors(server: Server, settings: Settings, locals: LocalProject[]) {
  const sent: Record<string, number> = {};
  for (const [id, config] of Object.entries(settings.mirrors)) {
    const local = locals.find((l) => l.repoId === id && l.isGit);
    if (!local) continue;
    let state = await api.lore(["status"], local.path, server.id);
    if (!state.ok) continue;
    let status = parseStatus(state.stdout);
    if (status.remoteRevision > status.localRevision) {
      await api.lore(["sync"], local.path, server.id);
      state = await api.lore(["status"], local.path, server.id);
      status = parseStatus(state.stdout);
    }
    if (status.localRevision <= config.revision) continue;
    const history = await api.lore(["history", "1"], local.path, server.id);
    const message = (history.ok ? parseHistory(history.stdout)[0]?.description : "") || `Lore revision ${status.localRevision}`;
    await mirrorNow(local.path, config, message);
    sent[id] = status.localRevision;
  }
  return sent;
}
