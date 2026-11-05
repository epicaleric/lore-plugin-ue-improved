import { useEffect, useMemo, useState } from "react";
import { RefreshCw, UploadCloud } from "lucide-react";
import { Empty, Field, Pill, Spinner } from "../components/ui";
import { api } from "../lib/api";
import { invalidateStatus } from "../lib/actions";
import { endJob, startJob } from "../lib/jobs";
import { mirrorNow } from "../lib/mirror";
import { parseStatus } from "../lib/parse";
import { useStore } from "../lib/store";
import type { Project } from "../lib/types";

interface Change {
  path: string;
  kind: string;
  staged: boolean;
}

/** Reads the sections of `lore status --scan`. Folders are left out, their files are what counts. */
function parseChanges(stdout: string): Change[] {
  const changes: Change[] = [];
  let staged = false;
  for (const raw of stdout.split(/\r?\n/)) {
    if (/^Changes staged for commit/.test(raw)) staged = true;
    else if (/^(Changes not staged|Untracked files)/.test(raw)) staged = false;
    const match = raw.match(/^([AMDR])\s+(.+?)\s*$/);
    if (match && !match[2].endsWith("/")) changes.push({ path: match[2], kind: match[1], staged });
  }
  return changes;
}

const KIND: Record<string, { label: string; tone: "good" | "info" | "bad" }> = {
  A: { label: "New", tone: "good" },
  M: { label: "Changed", tone: "info" },
  R: { label: "Moved", tone: "info" },
  D: { label: "Deleted", tone: "bad" },
};

/** Saves what changed on this PC as a new version and sends it to the server. */
export function Changes({ project }: { project: Project }) {
  const { server, settings, update, reload, toast } = useStore();
  const path = project.local!.path;
  const [changes, setChanges] = useState<Change[] | null>(null);
  const [skipped, setSkipped] = useState<Set<string>>(new Set());
  const [behind, setBehind] = useState(0);
  const [message, setMessage] = useState("");
  const [busy, setBusy] = useState("");

  const load = async () => {
    setChanges(null);
    const result = await api.lore(["status", "--scan"], path, server?.id);
    if (!result.ok) {
      toast((result.stderr || result.stdout).trim().split("\n")[0], "bad");
      setChanges([]);
      return;
    }
    const status = parseStatus(result.stdout);
    setBehind(Math.max(0, status.remoteRevision - status.localRevision));
    setChanges(parseChanges(result.stdout));
    setSkipped(new Set());
  };
  useEffect(() => { load(); }, [path]);

  const chosen = useMemo(() => (changes ?? []).filter((c) => !skipped.has(c.path)), [changes, skipped]);

  const targets = async (paths: string[]) => {
    const file = `${await api.bareDir()}/targets.txt`;
    await api.writeText(file, paths.join("\n") + "\n");
    return file;
  };

  const save = async () => {
    if (!server || !changes) return;
    if (!startJob(path)) return toast("Something is already running in this folder.", "bad");
    const fail = (result: { stderr: string; stdout: string }) => new Error((result.stderr || result.stdout).trim().split("\n").slice(0, 3).join(" "));
    try {
      setBusy("Adding your files");
      const toStage = chosen.filter((c) => !c.staged).map((c) => c.path);
      const toUnstage = changes.filter((c) => c.staged && skipped.has(c.path)).map((c) => c.path);
      if (toUnstage.length) {
        const result = await api.lore(["unstage", "--targets", await targets(toUnstage)], path, server.id);
        if (!result.ok) throw fail(result);
      }
      if (toStage.length) {
        const result = await api.lore(["stage", "--targets", await targets(toStage)], path, server.id);
        if (!result.ok) throw fail(result);
      }
      setBusy("Saving the version");
      let result = await api.loreStream(["commit", message.trim()], path, server.id, (line) => setBusy(line));
      if (!result.ok) throw fail(result);
      setBusy("Sending it to the server");
      result = await api.loreStream(["push"], path, server.id, (line) => setBusy(line));
      if (!result.ok) throw fail(result);

      // A project with a GitHub mirror goes along right away.
      const mirror = project.id ? settings.mirrors[project.id] : undefined;
      if (mirror) {
        setBusy("Sending it to GitHub");
        const state = await api.lore(["status"], path, server.id);
        await mirrorNow(path, mirror, message.trim());
        if (state.ok) update({ mirrors: { ...settings.mirrors, [project.id!]: { ...mirror, revision: parseStatus(state.stdout).localRevision } } });
      }
      toast("The new version is on the server", "good");
      setMessage("");
      invalidateStatus();
      await reload();
      await load();
    } catch (error) {
      toast(String(error).replace(/^Error: /, ""), "bad");
    } finally {
      endJob(path);
      setBusy("");
    }
  };

  if (!changes) return <div className="panel"><Spinner /> <span className="muted">Looking for changes in the folder...</span></div>;
  if (changes.length === 0) return <Empty icon={<UploadCloud size={28} />} title="Nothing to save">Everything on this PC is saved on the server.<div style={{ marginTop: 12 }}><button className="btn" onClick={load}><RefreshCw size={15} /> Look again</button></div></Empty>;

  const toggle = (file: string) => setSkipped((all) => {
    const next = new Set(all);
    if (next.has(file)) next.delete(file);
    else next.add(file);
    return next;
  });
  const allOn = skipped.size === 0;

  return (
    <div className="col">
      {behind > 0 && <div className="banner warn">The server has {behind} newer version{behind > 1 ? "s" : ""}. Sync first, then save your changes.</div>}
      <div className="panel">
        <div className="row between">
          <h3>{changes.length.toLocaleString()} changed file{changes.length === 1 ? "" : "s"}</h3>
          <div className="row gap-s">
            <button className="btn small ghost" onClick={() => setSkipped(allOn ? new Set(changes.map((c) => c.path)) : new Set())}>{allOn ? "Select none" : "Select all"}</button>
            <button className="btn small ghost" onClick={load}><RefreshCw size={14} /> Look again</button>
          </div>
        </div>
        <div className="change-list">
          {changes.map((change) => (
            <label key={change.path} className="change-row">
              <input type="checkbox" className="checkbox" checked={!skipped.has(change.path)} onChange={() => toggle(change.path)} />
              <span className="mono change-path" title={change.path}>{change.path}</span>
              <Pill tone={KIND[change.kind]?.tone ?? "info"}>{KIND[change.kind]?.label ?? change.kind}</Pill>
            </label>
          ))}
        </div>
      </div>
      <div className="panel">
        <Field label="What changed?">
          <textarea className="input" rows={3} value={message} onChange={(e) => setMessage(e.target.value)} placeholder="For example: new lighting in the main level" />
        </Field>
        <div className="row between">
          <span className="faint" style={{ fontSize: 12.5, minWidth: 0, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>{busy}</span>
          <button className="btn primary" disabled={!!busy || !message.trim() || chosen.length === 0 || behind > 0} onClick={save}>
            {busy ? <Spinner /> : <UploadCloud size={16} />} Save {chosen.length.toLocaleString()} file{chosen.length === 1 ? "" : "s"} as a new version
          </button>
        </div>
      </div>
    </div>
  );
}
