export interface Server {
  id: string;
  label: string;
  loreUrl: string;
  authUrl: string;
  webUrl: string;
}

export interface SessionInfo {
  userName: string;
  userId: string;
  expiresAt: number;
}

export interface CliResult {
  ok: boolean;
  code: number;
  stdout: string;
  stderr: string;
}

export interface Category {
  id: string;
  name: string;
  color: string;
  parentId?: string;
  /** user names that may see this category; empty or missing means everyone */
  visibleTo?: string[];
}

export interface ProjectOverride {
  displayName?: string;
  engine?: string;
  thumbnail?: string;
}

export type SortKey = "name" | "opened" | "updated" | "revisions";

export interface Settings {
  servers: Server[];
  activeServer?: string;
  roots: string[];
  categories: Category[];
  /** repository id -> category id */
  assignments: Record<string, string>;
  overrides: Record<string, ProjectOverride>;
  favorites: string[];
  /** working copies the user chose not to see */
  hidden: string[];
  accent: string;
  view: "grid" | "list";
  sort: SortKey;
  /** how big the cards (grid) and the thumbnails (list) are */
  size: { grid: number; list: number };
  /** repository id -> when the project was last opened here */
  opened: Record<string, number>;
  autoUpdate: { lore: boolean; plugin: boolean; app: boolean };
}

/** What is kept on the server, in the shared-settings project, for everyone. */
export interface SharedConfig {
  version: 1;
  categories: Category[];
  /** repository id -> category id */
  assignments: Record<string, string>;
}

export interface RemoteRepo {
  id: string;
  name: string;
}

export interface LocalProject {
  path: string;
  name: string;
  uproject: string | null;
  engine: string | null;
  description: string | null;
  thumbnail: string | null;
  plugin: string | null;
  repoId?: string;
  repoName?: string;
  remoteUrl?: string;
  branch?: string;
}

export interface Project {
  /** repository id (or the local path when the project is not on the server) */
  key: string;
  id?: string;
  name: string;
  onServer: boolean;
  local?: LocalProject;
  /** further working copies of the same project on this PC */
  copies: LocalProject[];
}

export interface RepoStatus {
  branch: string;
  localRevision: number;
  remoteRevision: number;
  changes: number;
  inSync: boolean;
}

export interface Revision {
  number: number;
  signature: string;
  date: string;
  author: string;
  description: string;
}

export interface Engine {
  version: string;
  path: string;
  editor: string;
  plugin: string | null;
}

export interface AdminUser {
  name: string;
  displayName: string;
  admin: boolean;
  disabled: boolean;
}

export interface Grant {
  user: string;
  repository: string;
  repositoryName: string | null;
}

export interface FolderEntry {
  name: string;
  dir: boolean;
  files: number;
  bytes: number;
  skipped: boolean;
}

export interface FolderListing {
  entries: FolderEntry[];
  files: number;
  bytes: number;
}
