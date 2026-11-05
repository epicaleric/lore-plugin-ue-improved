import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import type { AdminUser, CliResult, Engine, FolderListing, Grant, Server, SessionInfo, Settings } from "./types";

const streams = new Set<string>();

export const api = {
  getSettings: () => invoke<Partial<Settings>>("get_settings"),
  saveSettings: (settings: Settings) => invoke<void>("save_settings", { settings }),

  signIn: (server: Server, username: string, password: string) => invoke<SessionInfo>("sign_in", { server, username, password }),
  signInApiKey: (server: Server, key: string) => invoke<SessionInfo>("sign_in_api_key", { server, key }),
  session: (serverId: string) => invoke<SessionInfo | null>("session_info", { serverId }),
  signOut: (serverId: string) => invoke<void>("sign_out", { serverId }),

  lore: (args: string[], repository?: string | null, serverId?: string | null) =>
    invoke<CliResult>("lore_cli", { args, repository: repository ?? null, serverId: serverId ?? null }),

  /** Runs lore and reports every line of output while it works. */
  cancelStreams: () => Promise.all([...streams].map((id) => invoke<void>("lore_cancel", { streamId: id }))),
  loreStream: async (args: string[], repository: string | null, serverId: string | null, onLine: (line: string) => void) => {
    const id = `${Date.now()}-${Math.random().toString(36).slice(2)}`;
    streams.add(id);
    const stop = await listen<{ id: string; line: string }>("lore-line", (event) => {
      if (event.payload.id === id) onLine(event.payload.line);
    });
    try {
      return await invoke<CliResult>("lore_cli_stream", { args, repository, serverId, streamId: id });
    } finally {
      streams.delete(id);
      stop();
    }
  },
  bareDir: () => invoke<string>("bare_dir"),
  removeCache: (path: string) => invoke<void>("remove_cache", { path }),
  moveFolder: (from: string, toParent: string) => invoke<string>("move_folder", { from, toParent }),
  setAsideFolder: (path: string) => invoke<string>("set_aside_folder", { path }),
  repoId: (path: string) => invoke<string | null>("repo_id", { path }),
  writeBinary: (path: string, data: string) => invoke<void>("write_binary_file", { path, data }),
  relinkLore: (folder: string, fresh: string) => invoke<string>("relink_lore", { folder, fresh }),
  setAsideLore: (path: string) => invoke<string>("set_aside_lore", { path }),
  folderListing: (path: string) => invoke<FolderListing>("folder_listing", { path }),
  folderStats: (path: string) => invoke<{ files: number; bytes: number }>("folder_stats", { path }),

  health: (server: Server) => invoke<{ lore: { ok: boolean; ms: number }; auth: { ok: boolean; ms: number } }>("server_health", { server }),

  engines: () => invoke<Engine[]>("list_engines"),
  defaultRoots: () => invoke<string[]>("default_roots"),
  scan: (roots: string[]) => invoke<string[]>("scan_lore_projects", { roots }),
  readProject: (path: string) =>
    invoke<{ path: string; name: string; uproject: string | null; engine: string | null; description: string | null; thumbnail: string | null; plugin: string | null; repoId: string | null }>("read_project", { path }),
  readImage: (path: string) => invoke<string | null>("read_image", { path }),
  openProject: (uproject: string, editor?: string | null) => invoke<void>("open_project", { uproject, editor: editor ?? null }),
  openPath: (path: string) => invoke<void>("open_path", { path }),
  pluginVersion: (target: string, kind: "engine" | "project") => invoke<string | null>("plugin_version", { projectOrEngine: target, kind }),

  metaDir: (serverId: string) => invoke<string>("meta_dir", { serverId }),
  readText: (path: string) => invoke<string | null>("read_text_file", { path }),
  exists: (path: string) => invoke<boolean>("path_exists", { path }),
  writeText: (path: string, contents: string) => invoke<boolean>("write_text_file", { path, contents, overwrite: true }),

  getJson: <T = any>(url: string) => invoke<T>("http_get_json", { url }),
  getText: (url: string) => invoke<string>("http_get_text", { url }),
  installLoreClient: (url: string) => invoke<void>("install_lore_client", { url }),
  installAppUpdate: (url: string) => invoke<void>("install_app_update", { url }),
  installPlugin: (kind: "engine" | "project", target: string) => invoke<void>("install_plugin", { kind, target }),

  /** Admin API of the auth service. Throws with the service's own message on errors. */
  admin: async <T = any>(server: Server, method: string, path: string, body?: unknown): Promise<T> => {
    const reply = await invoke<{ status: number; body: any }>("admin_request", { server, method, path, body: body ?? null });
    if (reply.status >= 400) {
      throw new Error(reply.body?.error?.message ?? `Request failed (HTTP ${reply.status})`);
    }
    return reply.body as T;
  },
};

export const adminApi = {
  me: (s: Server) => api.admin<{ name: string; displayName: string; admin: boolean }>(s, "GET", "/me"),
  users: async (s: Server) => (await api.admin<{ users: AdminUser[] }>(s, "GET", "/users")).users,
  grants: async (s: Server) => (await api.admin<{ grants: Grant[] }>(s, "GET", "/grants")).grants,
  repositories: async (s: Server) => (await api.admin<{ repositories: { id: string; name: string; creator: string | null }[] }>(s, "GET", "/repositories")).repositories,
  createUser: (s: Server, body: { name: string; password: string; displayName?: string; admin?: boolean }) => api.admin(s, "POST", "/users", body),
  updateUser: (s: Server, name: string, body: { displayName?: string; admin?: boolean; disabled?: boolean }) => api.admin(s, "PATCH", `/users/${encodeURIComponent(name)}`, body),
  setPassword: (s: Server, name: string, password: string) => api.admin(s, "PUT", `/users/${encodeURIComponent(name)}/password`, { password }),
  deleteUser: (s: Server, name: string) => api.admin(s, "DELETE", `/users/${encodeURIComponent(name)}`),
  grant: (s: Server, user: string, repository: string) => api.admin(s, "PUT", "/grants", { user, repository }),
  revoke: (s: Server, user: string, repository: string) => api.admin(s, "DELETE", "/grants", { user, repository }),
};
