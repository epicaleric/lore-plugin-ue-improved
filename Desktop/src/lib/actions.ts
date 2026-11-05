import { useEffect, useState, useSyncExternalStore } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { api } from "./api";
import { parseHistory, parseStatus } from "./parse";
import { useStore } from "./store";
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

// ---- actions on projects

export function useProjectActions() {
  const { server, settings, engines, toast, reload, update } = useStore();
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

  const openProject = (project: Project) => {
    const uproject = project.local?.uproject;
    if (!uproject) {
      toast("No .uproject file found in this folder.", "bad");
      return;
    }
    const engine = engineFor(project);
    if (project.id) update({ opened: { ...settings.opened, [project.id]: Date.now() } });
    api.openProject(uproject, engine?.editor ?? null).catch((e) => toast(String(e), "bad"));
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
      const result = await api.lore(["sync"], path, server.id);
      if (!result.ok) {
        const text = (result.stderr || result.stdout).trim();
        if (/file has local changes/i.test(text)) {
          const files = [...text.matchAll(/File has local changes:\s*(\S+)/g)].map((m) => m[1]);
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
