import { useEffect, useState, useSyncExternalStore } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { api } from "./api";
import { parseHistory, parseStatus } from "./parse";
import { useStore } from "./store";
import { useUpdates } from "./updates";
import type { Project, RepoStatus, Revision } from "./types";

// ---- status and last revision of a local project, cached for the session

const statusCache = new Map<string, RepoStatus | null>();
const revisionCache = new Map<string, Revision | null>();
const authorCache = new Map<string, string[]>();
let generation = 0;

// Lets lists re-sort when more histories have been read.
let cacheVersion = 0;
const listeners = new Set<() => void>();
const bump = () => {
  cacheVersion++;
  listeners.forEach((l) => l());
};
export function useCacheVersion() {
  return useSyncExternalStore((cb) => { listeners.add(cb); return () => { listeners.delete(cb); }; }, () => cacheVersion);
}
export const peekRevision = (path?: string) => (path ? revisionCache.get(path) : undefined);
export const peekStatus = (path?: string) => (path ? statusCache.get(path) : undefined);

export function invalidateStatus() {
  generation++;
  statusCache.clear();
  revisionCache.clear();
  authorCache.clear();
  bump();
}

/** Everybody who saved a version, most recent first. */
export function useAuthors(path: string | undefined) {
  const [authors, setAuthors] = useState<string[] | undefined>(path ? authorCache.get(path) : undefined);
  useEffect(() => {
    if (!path) return;
    if (authorCache.has(path)) {
      setAuthors(authorCache.get(path));
      return;
    }
    let cancelled = false;
    api.lore(["history", "200"], path).then((result) => {
      const names: string[] = [];
      if (result.ok) for (const revision of parseHistory(result.stdout)) if (revision.author && !names.includes(revision.author)) names.push(revision.author);
      authorCache.set(path, names);
      if (!cancelled) setAuthors(names);
    });
    return () => {
      cancelled = true;
    };
  }, [path, generation]);
  return authors;
}

export function useStatus(path: string | undefined, serverId: string | undefined, nonce: number) {
  const [status, setStatus] = useState<RepoStatus | null | undefined>(path ? statusCache.get(path) : null);
  useEffect(() => {
    if (!path) return;
    if (statusCache.has(path)) {
      setStatus(statusCache.get(path));
      return;
    }
    let cancelled = false;
    api.lore(["status"], path, serverId).then((result) => {
      const parsed = result.ok ? parseStatus(result.stdout) : null;
      statusCache.set(path, parsed);
      bump();
      if (!cancelled) setStatus(parsed);
    });
    return () => {
      cancelled = true;
    };
  }, [path, serverId, nonce, generation]);
  return status;
}

export function useLastRevision(path: string | undefined, nonce: number) {
  const [revision, setRevision] = useState<Revision | null | undefined>(path ? revisionCache.get(path) : null);
  useEffect(() => {
    if (!path) return;
    if (revisionCache.has(path)) {
      setRevision(revisionCache.get(path));
      return;
    }
    let cancelled = false;
    api.lore(["history", "1"], path).then((result) => {
      const parsed = result.ok ? parseHistory(result.stdout)[0] ?? null : null;
      revisionCache.set(path, parsed);
      bump();
      if (!cancelled) setRevision(parsed);
    });
    return () => {
      cancelled = true;
    };
  }, [path, nonce, generation]);
  return revision;
}

/** The files in the "Changes staged for commit" part of `lore status`. */
function stagedFiles(stdout: string): string[] {
  const files: string[] = [];
  let inside = false;
  for (const line of stdout.split(/\r?\n/)) {
    if (/^Changes staged/.test(line)) inside = true;
    else if (/^(Changes not staged|Untracked files|Tracked changes)/.test(line)) inside = false;
    const match = line.match(/^[AMDR]\s+(.+?)\s*$/);
    if (inside && match && !match[1].endsWith("/")) files.push(match[1]);
  }
  return files;
}

// ---- actions on projects

export function useProjectActions() {
  const { server, settings, engines, toast, reload, update } = useStore();
  const updates = useUpdates();
  const [busy, setBusy] = useState<Record<string, string>>({});

  const run = async (key: string, label: string, work: () => Promise<void>) => {
    setBusy((b) => ({ ...b, [key]: label }));
    try {
      await work();
    } catch (error) {
      toast(String(error), "bad");
    } finally {
      setBusy((b) => {
        const next = { ...b };
        delete next[key];
        return next;
      });
    }
  };

  const engineFor = (project: Project) => {
    const wanted = (project.id && settings.overrides[project.id]?.engine) || project.local?.engine || null;
    return engines.find((e) => wanted && (e.version === wanted || e.version.startsWith(`${wanted}.`)));
  };

  const openProject = async (project: Project) => {
    const local = project.local;
    if (!local) return;
    // Not an Unreal project: its folder is what there is to open.
    if (!local.uproject) {
      api.openPath(local.path).catch((e) => toast(String(e), "bad"));
      return;
    }
    const engine = engineFor(project);
    const version = engine?.version ?? local.engine;
    // Unreal refuses a project whose plugin has no build, so that is put right first.
    if (local.pluginBroken) await updates.installPlugin("project", local.path, version);
    else if (local.needsPlugin && !local.plugin && engine && !engine.plugin) await updates.installPlugin("engine", engine.path, engine.version);
    if (project.id) update({ opened: { ...settings.opened, [project.id]: Date.now() } });
    api.openProject(local.uproject, engine?.editor ?? null).catch((e) => toast(String(e), "bad"));
    if (!engine) toast("Opened with Windows' default. The matching engine version was not found.", "info");
  };

  const clone = async (project: Project) => {
    if (!server || !project.id) return;
    const folder = await open({ directory: true, multiple: false, title: `Where should ${project.name} go?` });
    if (!folder || Array.isArray(folder)) return;
    const parent = folder.replace(/[\\/]$/, "");
    const target = `${parent}\\${project.name}`;
    await run(project.key, "Cloning", async () => {
      // The new copy has to be found afterwards, so the folder it went into is searched from now on.
      const searched = [...(await api.defaultRoots()), ...settings.roots];
      if (!searched.some((root) => parent.toLowerCase().startsWith(root.toLowerCase().replace(/[\\/]$/, "")))) {
        update({ roots: [...settings.roots, parent] });
      }
      const result = await api.lore(["clone", `${server.loreUrl}/${encodeURIComponent(project.name)}`, target], null, server.id);
      if (!result.ok) throw new Error((result.stderr || result.stdout).trim().split("\n").slice(0, 3).join(" "));
      toast(`${project.name} is on your PC now`, "good");
      invalidateStatus();
      await reload();
    });
  };

  const sync = async (project: Project) => {
    const path = project.local?.path;
    if (!path || !server) return;
    await run(project.key, "Syncing", async () => {
      let result = await api.lore(["sync"], path, server.id);

      // Lore does not sync with files staged for the next version. They are set back to unstaged for the
      // sync and staged again afterwards, nothing in them changes.
      let staged: string[] = [];
      if (!result.ok && /staged state/i.test(result.stderr + result.stdout)) {
        const state = await api.lore(["status"], path, server.id);
        staged = stagedFiles(state.stdout);
        await api.lore(["unstage", "."], path, server.id);
        result = await api.lore(["sync"], path, server.id);
      }
      const restage = async () => {
        if (!staged.length) return;
        const file = `${await api.bareDir()}/targets.txt`;
        await api.writeText(file, staged.join("\n") + "\n");
        await api.lore(["stage", "--targets", file], path, server.id);
        staged = [];
      };

      const conflicts = (text: string) => [...text.matchAll(/File has local changes:\s*(.+?)\s*\(incoming/g)].map((m) => m[1]);
      // The plugin is not project work: its files can always be put back to what the server has.
      if (!result.ok && conflicts(result.stderr + result.stdout).some((f) => f.startsWith("Plugins/LoreSourceControl/"))) {
        await api.lore(["reset", "Plugins/LoreSourceControl"], path, server.id);
        result = await api.lore(["sync"], path, server.id);
      }
      await restage();
      if (!result.ok) {
        const text = (result.stderr || result.stdout).trim();
        const files = conflicts(text);
        if (files.length) {
          throw new Error(`Syncing stopped: ${files.length} file${files.length === 1 ? " has" : "s have"} changes on this PC that the new version changes as well (${files.slice(0, 2).join(", ")}${files.length > 2 ? ", ..." : ""}). Save your changes as a version first, then sync again.`);
        }
        throw new Error(text.split("\n").slice(0, 3).join(" "));
      }
      toast(`${project.name} is up to date`, "good");
      invalidateStatus();
      await reload();
    });
  };

  return { busy, openProject, clone, sync, engineFor };
}
