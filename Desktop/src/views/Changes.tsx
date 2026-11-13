import { useEffect, useMemo, useState } from "react";
import { Cpu, FolderOpen, Lock, RefreshCw, RotateCcw, UploadCloud } from "lucide-react";
import { Empty, Field, Modal, Pill, Spinner } from "../components/ui";
import { api } from "../lib/api";
import { invalidateStatus } from "../lib/actions";
import { endJob, startJob } from "../lib/jobs";
import { mirrorNow } from "../lib/mirror";
import { parseStatus } from "../lib/parse";
import { BACKUP_FOLDER, isBuiltModule, readLocks, sameUser, undoChanges, type FileChange } from "../lib/safety";
import { useStore } from "../lib/store";
import type { Project } from "../lib/types";

type Change = FileChange;

/** Reads the sections of `lore status --scan`. Folders are left out, their files are what counts. */
function parseChanges(stdout: string): Change[] {
  const changes: Change[] = [];
  let staged = false;
  for (const raw of stdout.split(/\r?\n/)) {
    if (/^Changes staged for commit/.test(raw)) staged = true;
    else if (/^(Changes not staged|Untracked files)/.test(raw)) staged = false;
    const match = raw.match(/^([AMDR])\s+(.+?)\s*$/);
    if (match && !match[2].endsWith("/") && !match[2].startsWith(`${BACKUP_FOLDER}/`)) changes.push({ path: match[2], kind: match[1], staged });
  }
  return changes;
}

const KIND: Record<string, { label: string; tone: "good" | "info" | "bad" }> = {
  A: { label: "New", tone: "good" },
  M: { label: "Changed", tone: "info" },
  R: { label: "Moved", tone: "info" },
  D: { label: "Deleted", tone: "bad" },
};

const plural = (n: number, word: string) => `${n.toLocaleString()} ${word}${n === 1 ? "" : "s"}`;

/** Saves what changed on this PC as a new version and sends it to the server. */
export function Changes({ project }: { project: Project }) {
  const { server, session, settings, update, reload, toast } = useStore();
  const path = project.local!.path;
  const [changes, setChanges] = useState<Change[] | null>(null);
  const [skipped, setSkipped] = useState<Set<string>>(new Set());
  const [locks, setLocks] = useState<Map<string, string>>(new Map());
  const [behind, setBehind] = useState(0);
  const [message, setMessage] = useState("");
  const [busy, setBusy] = useState("");
  const [undo, setUndo] = useState<Change[] | null>(null);

  const me = session?.userName;
  /** Who else holds the lock on a file, if anybody. */
  const otherOwner = (file: string, from = locks) => {
    const owner = from.get(file);
    return owner && !sameUser(owner, me) ? owner : null;
  };

  const load = async () => {
    setChanges(null);
    const [result, found] = await Promise.all([api.lore(["status", "--scan"], path, server?.id), readLocks(path, server?.id)]);
    if (!result.ok) {
      toast((result.stderr || result.stdout).trim().split("\n")[0], "bad");
      setChanges([]);
      return;
    }
    const status = parseStatus(result.stdout);
    const list = parseChanges(result.stdout);
    setBehind(Math.max(0, status.remoteRevision - status.localRevision));
    setLocks(found);
    setChanges(list);
    // Left out unless chosen: files someone else has locked, and an editor module that was not built from changed code.
    const builtOnly = !list.some((c) => c.path.startsWith("Source/"));
    setSkipped(new Set(list.filter((c) => otherOwner(c.path, found) || (builtOnly && isBuiltModule(c.path))).map((c) => c.path)));
  };
  useEffect(() => { load(); }, [path]);

  const chosen = useMemo(() => (changes ?? []).filter((c) => !skipped.has(c.path)), [changes, skipped]);
  const lockedChanges = (changes ?? []).filter((c) => otherOwner(c.path));
  const builtModules = (changes ?? []).some((c) => c.path.startsWith("Source/")) ? [] : (changes ?? []).filter((c) => isBuiltModule(c.path));

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
      // A lock can have been taken since the list was read: asked again right before anything is staged.
      setBusy("Checking locks");
      const fresh = await readLocks(path, server.id);
      const blocked = chosen.filter((c) => otherOwner(c.path, fresh));
      if (blocked.length) {
        setLocks(fresh);
        setSkipped((all) => new Set([...all, ...blocked.map((c) => c.path)]));
        throw new Error(`${blocked.map((c) => `${c.path} is locked by ${fresh.get(c.path)}`).join(", ")}. Nothing was saved; those files are unchecked now.`);
      }
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

  const undoNow = async (files: Change[]) => {
    if (!startJob(path)) return toast("Something is already running in this folder.", "bad");
    try {
      setBusy("Making a backup copy and undoing");
      const folder = await undoChanges(path, server?.id, files);
      toast(`Undone. A copy of your version is in ${folder}`, "good");
      setUndo(null);
      invalidateStatus();
      await reload();
      await load();
    } catch (error) {
      toast(`Not undone: ${String(error).replace(/^Error: /, "")}`, "bad");
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
  const free = changes.filter((c) => !otherOwner(c.path));
  const allOn = free.every((c) => !skipped.has(c.path));

  return (
    <div className="col">
      {behind > 0 && <div className="banner warn">The server has {behind} newer version{behind > 1 ? "s" : ""}. Sync first, then save your changes.</div>}
      {lockedChanges.length > 0 && (
        <div className="banner bad">
          <Lock size={18} />
          <div style={{ flex: 1 }}>
            {lockedChanges.map((c) => <div key={c.path}><span className="mono">{c.path}</span> is locked by <b>{locks.get(c.path)}</b>.</div>)}
            <div className="faint" style={{ fontSize: 12.5, marginTop: 4 }}>Saving {lockedChanges.length === 1 ? "it" : "them"} would overwrite their work, so {lockedChanges.length === 1 ? "it can" : "they can"}not be part of a version until the lock is released. Ask them, or undo your change (a backup copy is kept).</div>
          </div>
        </div>
      )}
      {builtModules.length > 0 && (
        <div className="banner warn">
          <Cpu size={18} />
          <div style={{ flex: 1 }}>
            <b>{builtModules.map((c) => c.path.split("/").pop()).join(", ")}</b> {builtModules.length === 1 ? "was" : "were"} rebuilt by the Unreal Editor on this PC, while no code under Source changed.
            <div className="faint" style={{ fontSize: 12.5, marginTop: 4 }}>That usually happens with a different engine build, and such a file does not work for the others: it is unchecked, so it is not shared. Undo it to get the shared version back.</div>
          </div>
        </div>
      )}
      <div className="panel">
        <div className="row between">
          <h3>{plural(changes.length, "changed file")}</h3>
          <div className="row gap-s">
            <button className="btn small ghost" disabled={!!busy || chosen.length === 0} onClick={() => setUndo(chosen)} title="Puts the checked files back to the version on this PC, after a backup copy"><RotateCcw size={14} /> Undo my changes ({chosen.length.toLocaleString()})</button>
            <button className="btn small ghost" onClick={() => setSkipped(allOn ? new Set(changes.map((c) => c.path)) : new Set(changes.filter((c) => otherOwner(c.path)).map((c) => c.path)))}>{allOn ? "Select none" : "Select all"}</button>
            <button className="btn small ghost" onClick={load}><RefreshCw size={14} /> Look again</button>
          </div>
        </div>
        <div className="change-list">
          {changes.map((change) => {
            const owner = locks.get(change.path);
            const other = otherOwner(change.path);
            return (
              <label key={change.path} className={`change-row ${other ? "locked" : ""}`} title={other ? `Locked by ${other}: cannot be saved as a version` : undefined}>
                <input type="checkbox" className="checkbox" disabled={!!other} checked={!skipped.has(change.path)} onChange={() => toggle(change.path)} />
                <span className="mono change-path" title={change.path}>{change.path}</span>
                {owner && <Pill tone={other ? "bad" : "muted"}><Lock size={11} /> {other ? `Locked by ${owner}` : "Locked by you"}</Pill>}
                {builtModules.includes(change) && <Pill tone="warn">Built on this PC</Pill>}
                <Pill tone={KIND[change.kind]?.tone ?? "info"}>{KIND[change.kind]?.label ?? change.kind}</Pill>
                <button className="btn small ghost undo" title="Undo my changes to this file" disabled={!!busy} onClick={(e) => { e.preventDefault(); setUndo([change]); }}><RotateCcw size={14} /></button>
              </label>
            );
          })}
        </div>
      </div>
      <div className="panel">
        <Field label="What changed?">
          <textarea className="input" rows={3} value={message} onChange={(e) => setMessage(e.target.value)} placeholder="For example: new lighting in the main level" />
        </Field>
        <div className="row between">
          <span className="faint" style={{ fontSize: 12.5, minWidth: 0, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>{busy}</span>
          <button className="btn primary" disabled={!!busy || !message.trim() || chosen.length === 0 || behind > 0} onClick={save}>
            {busy ? <Spinner /> : <UploadCloud size={16} />} Save {plural(chosen.length, "file")} as a new version
          </button>
        </div>
      </div>
      {undo && (
        <Modal
          title={undo.length === 1 ? "Undo your changes to this file?" : `Undo your changes to ${undo.length.toLocaleString()} files?`}
          sub={`They go back to how they were in the version you last synced. A copy of what you have now is kept in ${BACKUP_FOLDER} first.`}
          onClose={() => !busy && setUndo(null)}
          actions={
            <>
              <button className="btn" disabled={!!busy} onClick={() => setUndo(null)}>Cancel</button>
              <button className="btn danger" disabled={!!busy} onClick={() => undoNow(undo)}>{busy ? <Spinner /> : <RotateCcw size={16} />} Undo {plural(undo.length, "file")}</button>
            </>
          }
        >
          <div className="file-list mono">
            {undo.map((c) => <div key={c.path}>{c.path}{c.kind === "A" ? "  (new file: it is removed)" : c.kind === "D" ? "  (comes back)" : ""}</div>)}
          </div>
          <button className="btn small ghost" style={{ marginTop: 8 }} onClick={() => api.revealPath(`${path}/${undo[0].path}`)}><FolderOpen size={14} /> Show in folder</button>
        </Modal>
      )}
    </div>
  );
}
