import type { RemoteRepo, RepoStatus, Revision } from "./types";

/** "MegaJamTemplate (01a1a69664927291b052d275f6b776fa)" per line. */
export function parseRepoList(stdout: string): RemoteRepo[] {
  const repos: RemoteRepo[] = [];
  for (const line of stdout.split(/\r?\n/)) {
    const match = line.trim().match(/^(.+?)\s+\(([0-9a-f]{32})\)$/i);
    if (match) {
      repos.push({ name: match[1], id: match[2] });
    }
  }
  return repos;
}

export function parseInfo(stdout: string): { name?: string; id?: string; remoteUrl?: string; branch?: string } {
  const out: { name?: string; id?: string; remoteUrl?: string; branch?: string } = {};
  const lines = stdout.split(/\r?\n/);
  const head = lines[0]?.trim().match(/^(.+?)\s+\(([0-9a-f]{32})\)$/i);
  if (head) {
    out.name = head[1];
    out.id = head[2];
  }
  for (const line of lines) {
    const [key, ...rest] = line.split(":");
    const value = rest.join(":").trim();
    if (key.trim() === "Remote URL") out.remoteUrl = value;
    if (key.trim() === "Default branch") out.branch = value.replace(/\s*\(.*\)$/, "");
  }
  return out;
}

export function parseStatus(stdout: string): RepoStatus {
  const status: RepoStatus = { branch: "main", localRevision: 0, remoteRevision: 0, changes: 0, inSync: true };
  for (const line of stdout.split(/\r?\n/)) {
    let match = line.match(/^On branch (\S+) revision (\d+)/);
    if (match) {
      status.branch = match[1];
      status.localRevision = Number(match[2]);
    }
    match = line.match(/^Remote revision (\d+)/);
    if (match) {
      status.remoteRevision = Number(match[1]);
    }
    // Backup copies Lore+ makes before undoing or syncing are not changes to the project.
    if (/^[AMDR]\s+\S/.test(line) && !/\/\s*$/.test(line) && !/^[AMDR]\s+Saved\/LoreBackups\//.test(line)) {
      status.changes += 1;
    }
  }
  status.inSync = status.localRevision === status.remoteRevision;
  return status;
}

export function parseHistory(stdout: string): Revision[] {
  const revisions: Revision[] = [];
  let current: Revision | null = null;
  for (const raw of stdout.split(/\r?\n/)) {
    if (!raw.trim()) continue;
    if (/^\s/.test(raw)) {
      if (current) {
        current.description += (current.description ? "\n" : "") + raw.trim();
      }
      continue;
    }
    const [key, ...rest] = raw.split(":");
    const value = rest.join(":").trim();
    switch (key.trim()) {
      case "Revision":
        current = { number: Number(value), signature: "", date: "", author: "", description: "" };
        revisions.push(current);
        break;
      case "Signature":
        if (current) current.signature = value;
        break;
      case "Date":
        if (current) current.date = value;
        break;
      case "Creator":
        if (current) current.author = value;
        break;
    }
  }
  return revisions;
}

/** First line of a description, for lists. */
export function firstLine(description: string): string {
  const newline = description.indexOf("\n");
  if (newline >= 0) return description.slice(0, newline).trim();
  const dash = description.indexOf(" - ");
  return dash >= 0 ? description.slice(0, dash).trim() : description;
}

/** Summary first, then one bullet per line. Older revisions put everything on one line. */
export function layoutDescription(description: string): string[] {
  let text = description;
  if (!text.includes("\n")) {
    text = text.replace(/ - /g, "\n- ");
  }
  return text
    .split("\n")
    .map((line) => line.trimEnd())
    .filter((line) => line.length > 0)
    .map((line) => (/^[-*]\s/.test(line) ? "• " + line.slice(2).trimStart() : line));
}

export function parseVersion(text: string): number[] {
  const match = text.match(/\d+(\.\d+)*/);
  return match ? match[0].split(".").map(Number) : [];
}

/** -1 when a is older than b, 0 when equal, 1 when newer. */
export function compareVersions(a: string, b: string): number {
  const left = parseVersion(a);
  const right = parseVersion(b);
  for (let i = 0; i < Math.max(left.length, right.length); i++) {
    const x = left[i] ?? 0;
    const y = right[i] ?? 0;
    if (x !== y) return x < y ? -1 : 1;
  }
  return 0;
}
