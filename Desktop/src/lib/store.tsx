import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import { api, adminApi } from "./api";
import { parseInfo, parseRepoList } from "./parse";
import { readableOn } from "./format";
import { SHARED_NAME, createShared, pullShared, pushShared, shareWithEveryone, visibleCategories } from "./shared";
import type { Category, Engine, LocalProject, Project, RemoteRepo, Server, SessionInfo, Settings } from "./types";

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
  const [sharedActive, setSharedActive] = useState(false);
  const sharedRef = useRef(false);
  const adminRef = useRef(false);
  const settingUp = useRef(false);
  const sharedWith = useRef<string | null>(null);
  const pushTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const [remoteError, setRemoteError] = useState<string | null>(null);
  const [locals, setLocals] = useState<LocalProject[]>([]);
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
      const roots = current.roots.length ? current.roots : await api.defaultRoots();
      const paths = await api.scan(roots);
      const found = await Promise.all(
        paths.map(async (path): Promise<LocalProject> => {
          const [project, info] = await Promise.all([api.readProject(path), api.lore(["repository", "info"], path)]);
          const parsed = info.ok ? parseInfo(info.stdout) : {};
          return { ...project, repoId: parsed.id, repoName: parsed.name, remoteUrl: parsed.remoteUrl, branch: parsed.branch };
        }),
      );
      setLocals(found);
    } finally {
      setLoading(false);
    }
  }, []);

  // Start-up: settings, then data.
  useEffect(() => {
    (async () => {
      const saved: Partial<Settings> = await api.getSettings().catch(() => ({}));
      const merged = { ...defaultSettings, ...saved, accent: !saved.accent || saved.accent === "#14a0a5" ? defaultSettings.accent : saved.accent, autoUpdate: { ...defaultSettings.autoUpdate, ...(saved.autoUpdate ?? {}) } } as Settings;
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
    for (const repo of remote) {
      const copies = byKey.get(repo.id) ?? [];
      used.add(repo.id);
      list.push({ key: repo.id, id: repo.id, name: repo.name, onServer: true, local: copies[0], copies: copies.slice(1) });
    }
    for (const [key, copies] of byKey) {
      if (!used.has(key)) {
        list.push({ key, id: copies[0].repoId, name: copies[0].repoName ?? copies[0].name, onServer: false, local: copies[0], copies: copies.slice(1) });
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
  }, [remote, locals, settings.hidden, settings.assignments, settings.categories, categories]);

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


