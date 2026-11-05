import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import { api, adminApi } from "./api";
import { parseHistory, parseInfo, parseRepoList } from "./parse";
import { readableOn } from "./format";
import { cachedRemoteInfo, loadRemoteInfo } from "./remote";
import { SHARED_NAME, createShared, pullShared, pushShared, shareWithEveryone, visibleCategories } from "./shared";
import type { Category, Engine, RemoteInfo, LocalProject, Project, RemoteRepo, Revision, Server, SessionInfo, Settings, UefnProject } from "./types";

export type View = "projects" | "project" | "admin" | "updates" | "settings";

export interface Toast {
  id: number;
  text: string;
  kind: "info" | "good" | "bad";
}

export const defaultSettings: Settings = {
  servers: [],
  roots: [],
  categories: [],
  assignments: {},
  overrides: {},
  favorites: [],
  hidden: [],
  accent: "#1ab6ff",
  view: "grid",
  sort: "name",
  mirrors: {},
  uefnBackups: {},
  groupBy: "none",
  collapsed: [],
  size: { grid: 250, list: 52 },
  opened: {},
  autoUpdate: { lore: true, plugin: true, app: true },
};

interface Store {
  ready: boolean;
  settings: Settings;
  update: (patch: Partial<Settings>) => void;
  server: Server | undefined;
  setServer: (id: string) => void;
  session: SessionInfo | null;
  isAdmin: boolean;
  refreshSession: () => Promise<void>;
  remote: RemoteRepo[];
  /** the server keeps the categories for everybody */
  sharedActive: boolean;
  /** categories this user may see */
  categories: Category[];
  remoteError: string | null;
  locals: LocalProject[];
  uefns: UefnProject[];
  projects: Project[];
  loading: boolean;
  reload: () => Promise<void>;
  engines: Engine[];
  reloadEngines: () => Promise<void>;
  view: View;
  projectKey: string | null;
  category: string;
  go: (view: View, projectKey?: string | null) => void;
  back: () => void;
  forward: () => void;
  setCategory: (id: string) => void;
  toast: (text: string, kind?: Toast["kind"]) => void;
  toasts: Toast[];
}

const Ctx = createContext<Store>(null as unknown as Store);
export const useStore = () => useContext(Ctx);

export function StoreProvider({ children }: { children: ReactNode }) {
  const [ready, setReady] = useState(false);
  const [settings, setSettings] = useState<Settings>(defaultSettings);
  const [session, setSession] = useState<SessionInfo | null>(null);
  const [isAdmin, setIsAdmin] = useState(false);
  const [remote, setRemote] = useState<RemoteRepo[]>([]);
  const [remoteInfo, setRemoteInfo] = useState<Record<string, RemoteInfo>>({});
  const [sharedActive, setSharedActive] = useState(false);
  const sharedRef = useRef(false);
  const adminRef = useRef(false);
  const settingUp = useRef(false);
  const sharedWith = useRef<string | null>(null);
  const pushTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const [remoteError, setRemoteError] = useState<string | null>(null);
  const [locals, setLocals] = useState<LocalProject[]>([]);
  const [uefns, setUefns] = useState<UefnProject[]>([]);
  const [uefnHistory, setUefnHistory] = useState<Record<string, Revision[]>>({});
  const readingUefn = useRef(false);
  const [loading, setLoading] = useState(false);
  const [engines, setEngines] = useState<Engine[]>([]);
  const [view, setView] = useState<View>("projects");
  const [projectKey, setProjectKey] = useState<string | null>(null);
  const [category, setCategory] = useState("all");
  const [toasts, setToasts] = useState<Toast[]>([]);
  const toastId = useRef(0);

  const toast = useCallback((text: string, kind: Toast["kind"] = "info") => {
    const id = ++toastId.current;
    setToasts((all) => [...all, { id, text, kind }]);
    setTimeout(() => setToasts((all) => all.filter((t) => t.id !== id)), kind === "bad" ? 7000 : 3800);
  }, []);

  const toastRef = useRef(toast);
  toastRef.current = toast;
  const settingsRef = useRef(settings);
  settingsRef.current = settings;

  const update = useCallback((patch: Partial<Settings>) => {
    const next = { ...settingsRef.current, ...patch };
    settingsRef.current = next;
    setSettings(next);
    api.saveSettings(next).catch(() => {});

    // Categories are kept on the server for everybody. Only administrators send changes.
    if (("categories" in patch || "assignments" in patch) && sharedRef.current && adminRef.current) {
      if (pushTimer.current) clearTimeout(pushTimer.current);
      pushTimer.current = setTimeout(async () => {
        const current = settingsRef.current;
        const target = current.servers.find((s) => s.id === current.activeServer) ?? current.servers[0];
        if (!target) return;
        try {
          await pushShared(target, { version: 1, categories: current.categories, assignments: current.assignments });
        } catch (error) {
          toastRef.current(`The categories were not saved on the server: ${error}`, "bad");
        }
      }, 800);
    }
  }, []);

  const server = settings.servers.find((s) => s.id === settings.activeServer) ?? settings.servers[0];

  // Accent colour as CSS variables.
  useEffect(() => {
    document.documentElement.style.setProperty("--accent", settings.accent);
    document.documentElement.style.setProperty("--accent-ink", readableOn(settings.accent));
  }, [settings.accent]);

  const refreshSession = useCallback(async () => {
    if (!server) {
      setSession(null);
      setIsAdmin(false);
      return;
    }
    const info = await api.session(server.id).catch(() => null);
    setSession(info);
    if (info) {
      await adminApi
        .me(server)
        .then((me) => { adminRef.current = me.admin; setIsAdmin(me.admin); })
        .catch(() => { adminRef.current = false; setIsAdmin(false); });
    } else {
      setIsAdmin(false);
    }
  }, [server]);

  const reloadEngines = useCallback(async () => {
    setEngines(await api.engines().catch(() => []));
  }, []);

  const reload = useCallback(async () => {
    setLoading(true);
    try {
      const current = settingsRef.current;
      const active = current.servers.find((s) => s.id === current.activeServer) ?? current.servers[0];

      // Projects on the server.
      if (active && (await api.session(active.id).catch(() => null))) {
        const result = await api.lore(["repository", "list", active.loreUrl], null, active.id);
        if (result.ok) {
          let all = parseRepoList(result.stdout);
          let hasShared = all.some((r) => r.name === SHARED_NAME);

          // Categories always live on the server. The first administrator who signs in creates the project for them.
          if (!hasShared && adminRef.current && !settingUp.current) {
            settingUp.current = true;
            try {
              await createShared(active, { version: 1, categories: current.categories, assignments: current.assignments });
              hasShared = true;
              all = [...all, { id: "", name: SHARED_NAME }];
              toastRef.current("Your categories are now kept on the server for everybody", "good");
            } catch (error) {
              toastRef.current(`Could not set up the shared categories: ${error}`, "bad");
            } finally {
              settingUp.current = false;
            }
          }
          // Everybody needs to be able to read them; this also catches users added since the last time.
          if (hasShared && adminRef.current && sharedWith.current !== active.id) {
            sharedWith.current = active.id;
            shareWithEveryone(active).catch(() => { sharedWith.current = null; });
          }
          sharedRef.current = hasShared;
          setSharedActive(hasShared);
          setRemote(all.filter((r) => r.name !== SHARED_NAME));
          setRemoteError(null);
          if (hasShared) {
            try {
              const shared = await pullShared(active);
              if (!shared && adminRef.current) {
                // The project exists but nothing was saved in it yet.
                await pushShared(active, { version: 1, categories: current.categories, assignments: current.assignments });
              } else if (shared) {
                const next = { ...settingsRef.current, categories: shared.categories, assignments: shared.assignments };
                settingsRef.current = next;
                setSettings(next);
                api.saveSettings(next).catch(() => {});
              }
            } catch (error) {
              toastRef.current(`Could not load the shared categories: ${error}`, "bad");
            }
          }
        } else {
          setRemote([]);
          setRemoteError((result.stderr || result.stdout).trim().split("\n")[0] || "Could not list the projects");
        }
      } else {
        setRemote([]);
        setRemoteError(active ? "Sign in to see the projects on this server." : null);
      }

      // Projects on this PC.
      // The usual places for Unreal projects plus every folder the person added or cloned into.
      const roots = [...new Set([...(await api.defaultRoots()), ...current.roots])];
      const paths = await api.scan(roots);
      const found = await Promise.all(
        paths.map(async (path): Promise<LocalProject> => {
          const [project, info] = await Promise.all([api.readProject(path), api.lore(["repository", "info"], path, active?.id)]);
          const parsed = info.ok ? parseInfo(info.stdout) : {};
          return { ...project, repoId: parsed.id ?? project.repoId ?? undefined, repoName: parsed.name, remoteUrl: parsed.remoteUrl, branch: parsed.branch };
        }),
      );
      setLocals(found);

      // Projects of UEFN. Their revisions are read afterwards, from copies of their stores, one at a time.
      const used = new Set<string>();
      const uefn = (await api.scanUefn([...roots, ...(settingsRef.current.uefnAdded ?? [])]).catch(() => [])).map((project): UefnProject => {
        const id = project.projectId && !used.has(project.projectId) ? `uefn-${project.projectId}` : `uefn-${project.path.replace(/[^A-Za-z0-9]+/g, "-")}`;
        if (project.projectId) used.add(project.projectId);
        return { ...project, id };
      });
      setUefns(uefn);
      // Projects are added by hand. What was already put to use before that existed stays.
      if (!settingsRef.current.uefnAdded) {
        const { assignments, favorites, uefnBackups } = settingsRef.current;
        update({ uefnAdded: uefn.filter((p) => assignments[p.id] || favorites.includes(p.id) || uefnBackups[p.id]).map((p) => p.path) });
      }
      const added = (settingsRef.current.uefnAdded ?? []).map((path) => path.toLowerCase());
      if (!readingUefn.current) {
        readingUefn.current = true;
        (async () => {
          for (const project of uefn.filter((p) => p.store === "lore" && added.includes(p.path.toLowerCase()))) {
            const copy = await api.uefnSnapshot(project.path, project.id).catch(() => null);
            const history = copy ? await api.lore(["history", "60"], copy) : null;
            // Revisions from UEFN usually name an Epic account id, not a person.
            const named = (author: string) => (!/^[0-9a-f]{32}$/i.test(author) ? author : author === project.identity ? "You" : "Teammate");
            if (history?.ok) setUefnHistory((all) => ({ ...all, [project.id]: parseHistory(history.stdout).map((r) => ({ ...r, author: named(r.author) })) }));
          }
        })().finally(() => { readingUefn.current = false; });
      }
    } finally {
      setLoading(false);
    }
  }, []);

  // Start-up: settings, then data.
  useEffect(() => {
    (async () => {
      const saved: Partial<Settings> = await api.getSettings().catch(() => ({}));
      const merged = { ...defaultSettings, ...saved, accent: !saved.accent || saved.accent === "#14a0a5" ? defaultSettings.accent : saved.accent, size: { ...defaultSettings.size, ...(saved.size ?? {}) }, autoUpdate: { ...defaultSettings.autoUpdate, ...(saved.autoUpdate ?? {}) } } as Settings;
      settingsRef.current = merged;
      setSettings(merged);
      setReady(true);
    })();
  }, []);

  useEffect(() => {
    if (!ready) return;
    refreshSession().then(() => reload());
    reloadEngines();
  }, [ready, server?.id]);

  const categories = useMemo(() => visibleCategories(settings.categories, session?.userName, isAdmin), [settings.categories, session?.userName, isAdmin]);

  // What the server knows about each project (thumbnail, size, files, people), read without downloading the projects.
  useEffect(() => {
    if (!ready || !server || !session || remote.length === 0) return;
    let cancelled = false;
    (async () => {
      // What was found last time is shown straight away.
      const cached = await Promise.all(remote.map(async (repo) => [repo.id, await cachedRemoteInfo(repo)] as const));
      if (cancelled) return;
      const found: Record<string, RemoteInfo> = {};
      for (const [id, info] of cached) if (info) found[id] = info;
      setRemoteInfo((all) => ({ ...all, ...found }));
      // Then they are brought up to date, two at a time.
      const queue = [...remote];
      const worker = async () => {
        for (let repo = queue.shift(); repo && !cancelled; repo = queue.shift()) {
          try {
            const known = cached.find(([id]) => id === repo!.id)?.[1] ?? null;
            const info = await loadRemoteInfo(server, repo, known);
            if (info && !cancelled) setRemoteInfo((all) => ({ ...all, [repo!.id]: info }));
          } catch {
            // a project that cannot be read just has no extra information
          }
        }
      };
      await Promise.all([worker(), worker()]);
    })();
    return () => {
      cancelled = true;
    };
  }, [ready, server?.id, session?.userName, remote]);

  const projects = useMemo<Project[]>(() => {
    // One project can have several working copies on this PC; the first one is shown, the others are listed in its details.
    const visible = locals.filter((l) => !settings.hidden.includes(l.path));
    const byKey = new Map<string, LocalProject[]>();
    for (const local of visible) {
      const key = local.repoId ?? local.path;
      byKey.set(key, [...(byKey.get(key) ?? []), local]);
    }
    const list: Project[] = [];
    const used = new Set<string>();
    // The server copy of a UEFN backup is shown with the UEFN project itself.
    const backups = new Set(Object.values(settings.uefnBackups).map((b) => b.name));
    for (const repo of remote.filter((r) => !backups.has(r.name))) {
      const copies = byKey.get(repo.id) ?? [];
      used.add(repo.id);
      list.push({ key: repo.id, id: repo.id, name: repo.name, onServer: true, local: copies[0], copies: copies.slice(1), remote: remoteInfo[repo.id] });
    }
    for (const [key, copies] of byKey) {
      if (!used.has(key)) {
        list.push({ key, id: copies[0].repoId, name: copies[0].repoName ?? copies[0].name, onServer: false, local: copies[0], copies: copies.slice(1) });
      }
    }
    for (const project of uefns) {
      if ((settings.uefnAdded ?? []).some((path) => path.toLowerCase() === project.path.toLowerCase())) {
        list.push({ key: project.id, id: project.id, name: project.name, onServer: false, copies: [], uefn: { ...project, revisions: uefnHistory[project.id] } });
      }
    }
    // Projects in a category this user may not see are left out as well.
    const shown = new Set(categories.map((c) => c.id));
    const known = new Set(settings.categories.map((c) => c.id));
    const allowed = list.filter((p) => {
      const assigned = p.id ? settings.assignments[p.id] : undefined;
      return !assigned || !known.has(assigned) || shown.has(assigned);
    });
    return allowed.sort((a, b) => a.name.localeCompare(b.name));
  }, [remote, locals, uefns, uefnHistory, remoteInfo, settings.hidden, settings.assignments, settings.categories, settings.uefnBackups, settings.uefnAdded, categories]);

  // Where the user has been, so the mouse's back button works like in a browser.
  const trail = useRef<{ view: View; projectKey: string | null; category: string }[]>([{ view: "projects", projectKey: null, category: "all" }]);
  const at = useRef(0);
  const categoryRef = useRef("all");

  const show = (entry: { view: View; projectKey: string | null; category: string }) => {
    categoryRef.current = entry.category;
    setView(entry.view);
    setProjectKey(entry.projectKey);
    setCategory(entry.category);
  };

  const go = useCallback((next: View, key?: string | null) => {
    const entry = { view: next, projectKey: key ?? null, category: categoryRef.current };
    const current = trail.current[at.current];
    if (current && current.view === entry.view && current.projectKey === entry.projectKey && current.category === entry.category) return;
    trail.current = [...trail.current.slice(0, at.current + 1), entry];
    at.current = trail.current.length - 1;
    setView(entry.view);
    setProjectKey(entry.projectKey);
  }, []);

  const back = useCallback(() => {
    if (at.current > 0) show(trail.current[--at.current]);
  }, []);
  const forward = useCallback(() => {
    if (at.current < trail.current.length - 1) show(trail.current[++at.current]);
  }, []);

  const chooseCategory = useCallback((id: string) => {
    categoryRef.current = id;
    setCategory(id);
  }, []);

  const value: Store = {
    ready,
    settings,
    update,
    server,
    setServer: (id) => update({ activeServer: id }),
    session,
    isAdmin,
    refreshSession,
    remote,
    sharedActive,
    categories,
    remoteError,
    locals,
    uefns,
    projects,
    loading,
    reload,
    engines,
    reloadEngines,
    view,
    projectKey,
    category,
    go,
    back,
    forward,
    setCategory: chooseCategory,
    toast,
    toasts,
  };
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}


