import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import { api, adminApi } from "./api";
import { parseInfo, parseRepoList } from "./parse";
import { readableOn } from "./format";
import type { Engine, LocalProject, Project, RemoteRepo, Server, SessionInfo, Settings } from "./types";

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
  accent: "#14a0a5",
  view: "grid",
  autoUpdate: { lore: false, plugin: false, app: false },
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

  const settingsRef = useRef(settings);
  settingsRef.current = settings;

  const update = useCallback((patch: Partial<Settings>) => {
    const next = { ...settingsRef.current, ...patch };
    settingsRef.current = next;
    setSettings(next);
    api.saveSettings(next).catch(() => {});
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
      adminApi
        .me(server)
        .then((me) => setIsAdmin(me.admin))
        .catch(() => setIsAdmin(false));
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
          setRemote(parseRepoList(result.stdout));
          setRemoteError(null);
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
      const merged = { ...defaultSettings, ...saved, autoUpdate: { ...defaultSettings.autoUpdate, ...(saved.autoUpdate ?? {}) } } as Settings;
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

  const projects = useMemo<Project[]>(() => {
    const list: Project[] = [];
    const used = new Set<string>();
    for (const repo of remote) {
      const local = locals.find((l) => l.repoId === repo.id);
      if (local) used.add(local.path);
      list.push({ key: repo.id, id: repo.id, name: repo.name, onServer: true, local });
    }
    for (const local of locals) {
      if (!used.has(local.path)) {
        list.push({ key: local.repoId ?? local.path, id: local.repoId, name: local.repoName ?? local.name, onServer: false, local });
      }
    }
    return list.sort((a, b) => a.name.localeCompare(b.name));
  }, [remote, locals]);

  const go = useCallback((next: View, key?: string | null) => {
    setView(next);
    setProjectKey(key ?? null);
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
    setCategory,
    toast,
    toasts,
  };
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}


