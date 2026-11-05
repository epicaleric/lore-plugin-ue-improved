import { api, adminApi } from "./api";
import type { Category, Server, SharedConfig } from "./types";

/** The project on the server that holds the categories for everybody. */
export const SHARED_NAME = "lorehub-meta";
const FILE = "lorehub.json";

const fail = (result: { stderr: string; stdout: string }) =>
  new Error((result.stderr || result.stdout).trim().split("\n").slice(0, 3).join(" "));

async function folder(server: Server) {
  return api.metaDir(server.id);
}

/** Windows accepts forward slashes, which keeps backslash escapes out of the way. */
const at = (dir: string, name: string) => `${dir}/${name}`;

// Clone, sync and push all use the same folder, so they run one after the other.
let queue: Promise<unknown> = Promise.resolve();
function inOrder<T>(work: () => Promise<T>): Promise<T> {
  const next = queue.then(work, work);
  queue = next.catch(() => {});
  return next;
}

async function clone(server: Server, dir: string) {
  const cloned = await api.lore(["clone", `${server.loreUrl}/${SHARED_NAME}`, dir], null, server.id);
  if (!cloned.ok) throw fail(cloned);
}

/** The local copy is only a cache of what is on the server. When it is broken, it is set aside and fetched again. */
async function bringUpToDate(server: Server, dir: string) {
  if (await api.exists(at(dir, ".lore"))) {
    const synced = await api.lore(["sync"], dir, server.id);
    if (synced.ok) return;
    // Only a copy that is broken is set aside; a network problem is reported as it is.
    if (!/no remote configured|repository not found|corrupt/i.test(synced.stderr + synced.stdout)) throw fail(synced);
    await api.setAsideFolder(dir);
  }
  await clone(server, dir);
}

/** Brings the local copy up to date and returns what is in it, or null when nothing was saved there yet. */
export function pullShared(server: Server): Promise<SharedConfig | null> {
  return inOrder(async () => {
    const dir = await folder(server);
    await bringUpToDate(server, dir);
    const text = await api.readText(at(dir, FILE));
    if (!text) return null;
    try {
      const parsed = JSON.parse(text);
      return { version: 1, categories: parsed.categories ?? [], assignments: parsed.assignments ?? {} } as SharedConfig;
    } catch {
      return null;
    }
  });
}

async function saveAndSend(server: Server, dir: string, config: SharedConfig, message: string) {
  await api.writeText(at(dir, FILE), JSON.stringify(config, null, 2) + "\n");
  // `stage .` only takes files that are marked as changed already, so let Lore scan the folder.
  const staged = await api.lore(["stage", "--scan", "."], dir, server.id);
  if (!staged.ok) throw fail(staged);
  const committed = await api.lore(["commit", message], dir, server.id);
  // Nothing to commit is not a problem.
  if (!committed.ok && !/nothing|no changes/i.test(committed.stderr + committed.stdout)) throw fail(committed);
  const pushed = await api.lore(["push"], dir, server.id);
  if (!pushed.ok) throw fail(pushed);
}

/** Writes the file and sends it to the server. Syncs first so other people's changes are not lost. */
export function pushShared(server: Server, config: SharedConfig): Promise<void> {
  return inOrder(async () => {
    const dir = await folder(server);
    await bringUpToDate(server, dir);
    await saveAndSend(server, dir, config, "Update categories");
  });
}

/** Creates the shared project from this PC's categories and lets every user read it. Admin only. */
export function createShared(server: Server, config: SharedConfig): Promise<void> {
  return inOrder(async () => {
    const dir = await folder(server);
    if (await api.exists(at(dir, ".lore"))) throw new Error(`${dir} already holds a working copy. Remove it first.`);
    // `lore repository create` runs in an existing folder; writing the file creates it.
    await api.writeText(at(dir, FILE), JSON.stringify(config, null, 2) + "\n");
    const created = await api.lore(["repository", "create", `${server.loreUrl}/${SHARED_NAME}`], dir, server.id);
    if (!created.ok) throw fail(created);
    await saveAndSend(server, dir, config, "Shared categories");
    await shareWithEveryone(server);
  });
}

/** Gives every user (also new ones, when this is run again) read access to the shared project. */
export async function shareWithEveryone(server: Server): Promise<number> {
  const repo = (await adminApi.repositories(server)).find((r) => r.name === SHARED_NAME);
  if (!repo) throw new Error("The shared project is not on the server yet.");
  const [users, grants] = await Promise.all([adminApi.users(server), adminApi.grants(server)]);
  let added = 0;
  for (const user of users) {
    if (user.admin || user.disabled) continue;
    if (grants.some((g) => g.user === user.name && (g.repository === repo.id || g.repository === "*"))) continue;
    await adminApi.grant(server, user.name, repo.id);
    added++;
  }
  return added;
}

/** A category is shown to admins, to everyone when it has no list, and otherwise to the people on its list. Children follow their parent. */
export function visibleCategories(all: Category[], user: string | undefined, isAdmin: boolean): Category[] {
  if (isAdmin) return all;
  const byId = new Map(all.map((c) => [c.id, c]));
  const allowed = (c: Category, seen = new Set<string>()): boolean => {
    if (seen.has(c.id)) return false;
    seen.add(c.id);
    if (c.visibleTo?.length && !(user && c.visibleTo.includes(user))) return false;
    const parent = c.parentId ? byId.get(c.parentId) : undefined;
    return parent ? allowed(parent, seen) : true;
  };
  return all.filter((c) => allowed(c));
}
