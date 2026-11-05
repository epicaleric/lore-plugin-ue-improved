import { api } from "./api";
import { parseHistory, parseStatus } from "./parse";
import type { RemoteInfo, RemoteRepo, Server } from "./types";

const at = (dir: string, name: string) => `${dir}/${name}`;
const clean = (text: string) => text.replace(/^\uFEFF/, "");

/** Reads "Path / Type / Size" blocks from `lore file info`. */
function parseFileInfo(stdout: string) {
  const out: { path: string; dir: boolean; bytes: number }[] = [];
  let current: { path: string; dir: boolean; bytes: number } | null = null;
  for (const line of stdout.split(/\r?\n/)) {
    const match = line.match(/^(Path|Type|Size):\s*(.*)$/);
    if (!match) continue;
    if (match[1] === "Path") {
      current = { path: match[2].trim(), dir: false, bytes: 0 };
      out.push(current);
    } else if (current && match[1] === "Type") {
      current.dir = match[2].trim() === "dir";
    } else if (current) {
      current.bytes = Number(match[2]) || 0;
    }
  }
  return out;
}

/** The cached answer for a project, if there is one. */
export async function cachedRemoteInfo(repo: RemoteRepo): Promise<RemoteInfo | null> {
  try {
    const base = await api.bareDir();
    const text = await api.readText(at(base, `${repo.id}.json`));
    return text ? (JSON.parse(clean(text)) as RemoteInfo) : null;
  } catch {
    return null;
  }
}

/**
 * Looks at a project on the server without downloading it: a bare clone holds only the file tree, and single files
 * (the .uproject and the thumbnail) are written out on request.
 */
export async function loadRemoteInfo(server: Server, repo: RemoteRepo, known: RemoteInfo | null): Promise<RemoteInfo | null> {
  const base = await api.bareDir();
  const dir = at(base, repo.id);

  // A bare clone is tiny and quick to make, so it is made again every time instead of updated (which could fetch files).
  if (await api.exists(at(dir, ".lore"))) await api.removeCache(dir);
  const cloned = await api.lore(["clone", "--bare", `${server.loreUrl}/${repo.name}`, dir], null, server.id);
  if (!cloned.ok) return known;

  const state = await api.lore(["status"], dir, server.id);
  const revision = state.ok ? parseStatus(state.stdout).remoteRevision : 0;
  if (known && known.revision === revision && (known.thumbnail === null || (await api.exists(known.thumbnail)))) return known;

  // Every file of the project, as a bare clone lists them as not present.
  const scan = await api.lore(["status", "--scan"], dir, server.id);
  const paths = scan.ok ? scan.stdout.split(/\r?\n/).map((l) => l.match(/^[AMD]\s+(.+)$/)?.[1]?.trim()).filter((p): p is string => !!p) : [];
  const files = paths.filter((p) => !p.endsWith("/"));
  const top = [...new Set(paths.map((p) => p.replace(/\/$/, "").split("/")[0]))];

  const sizes = top.length ? await api.lore(["file", "info", ".", ...top], dir, server.id) : null;
  const infos = sizes?.ok ? parseFileInfo(sizes.stdout) : [];
  const total = infos.find((i) => i.path === "" || i.path === ".")?.bytes ?? 0;
  const entries = infos
    .filter((i) => i.path !== "" && i.path !== ".")
    .map((i) => ({ name: i.path, dir: i.dir, bytes: i.bytes }))
    .sort((a, b) => b.bytes - a.bytes);

  // Unreal version from the .uproject, thumbnail from the picture next to it.
  const side = at(base, `${repo.id}.files`);
  await api.writeText(at(side, ".keep"), "");
  const uproject = files.find((p) => !p.includes("/") && p.toLowerCase().endsWith(".uproject"));
  let engine: string | null = null;
  let thumbnail: string | null = null;
  if (uproject) {
    const out = at(side, "project.uproject");
    const wrote = await api.lore(["file", "write", "--path", uproject, "--output", out], dir, server.id);
    if (wrote.ok) {
      try {
        engine = JSON.parse(clean((await api.readText(out)) ?? "{}")).EngineAssociation || null;
      } catch {
        engine = null;
      }
    }
    const png = `${uproject.replace(/\.uproject$/i, "")}.png`;
    if (files.includes(png)) {
      const target = at(side, `thumbnail-${revision}.png`);
      const got = await api.lore(["file", "write", "--path", png, "--output", target], dir, server.id);
      if (got.ok) thumbnail = target;
    }
  }

  const history = await api.lore(["history", "200"], dir, server.id);
  const revisions = history.ok ? parseHistory(history.stdout) : [];
  const authors: string[] = [];
  for (const r of revisions) if (r.author && !authors.includes(r.author)) authors.push(r.author);

  const info: RemoteInfo = { revision, files: files.length, bytes: total, entries, engine, thumbnail, authors, last: revisions[0] ?? null };
  await api.writeText(at(base, `${repo.id}.json`), JSON.stringify(info));
  return info;
}
