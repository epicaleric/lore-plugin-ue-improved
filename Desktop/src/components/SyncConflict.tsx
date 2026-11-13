import { useEffect, useState } from "react";
import { Download, FolderOpen, Lock } from "lucide-react";
import { api } from "../lib/api";
import { invalidateStatus, peekSyncConflict, setSyncConflict, useProjectActions, useSyncConflict } from "../lib/actions";
import { endJob, startJob } from "../lib/jobs";
import { BACKUP_FOLDER, isText, readLocks, sameUser, setAside } from "../lib/safety";
import { useStore } from "../lib/store";
import { Modal, Pill, Spinner } from "./ui";

type Choice = "take" | "mine";

/**
 * Shown when a sync stops because files changed both on this PC and in the new version.
 * Every choice starts with a backup copy of this PC's file, so nothing is ever lost.
 */
export function SyncConflictDialog() {
  const conflict = useSyncConflict();
  const { server, session, toast, reload } = useStore();
  const { sync } = useProjectActions();
  const [choices, setChoices] = useState<Record<string, Choice>>({});
  const [locks, setLocks] = useState<Map<string, string> | null>(null);
  const [busy, setBusy] = useState("");
  const [done, setDone] = useState<{ folder: string } | null>(null);

  const root = conflict?.project.local?.path;
  useEffect(() => {
    setChoices({});
    setDone(null);
    setLocks(null);
    if (root) readLocks(root, server?.id).then(setLocks);
  }, [conflict]);

  if (!conflict || !root) return null;
  const files = conflict.files;
  const lockedByOther = (file: string) => {
    const owner = locks?.get(file);
    return owner && !sameUser(owner, session?.userName) ? owner : null;
  };
  const choice = (file: string): Choice => (choices[file] === "mine" && !lockedByOther(file) ? "mine" : "take");
  const close = () => !busy && setSyncConflict(null);

  const resolve = async () => {
    if (!startJob(root)) return toast("Something is already running in this folder.", "bad");
    try {
      setBusy("Making a backup copy of your files");
      const folder = await setAside(root, server?.id, files);
      setBusy("Syncing");
      endJob(root);
      const synced = await sync(conflict.project);
      // Files the user keeps go back over the new version, also when the sync failed: then nothing changed at all.
      const mine = files.filter((f) => choice(f) === "mine");
      if (mine.length) await api.restoreFiles(folder, root, mine);
      invalidateStatus();
      await reload();
      if (synced) setDone({ folder });
      // A sync that stopped on other files has opened this dialog again for those; otherwise the message says why.
      else if (peekSyncConflict() === conflict) setSyncConflict(null);
    } catch (error) {
      toast(`Nothing was synced: ${String(error).replace(/^Error: /, "")}`, "bad");
    } finally {
      endJob(root);
      setBusy("");
    }
  };

  if (done) {
    return (
      <Modal
        title="Synced"
        sub={`${conflict.project.name} is up to date.`}
        onClose={close}
        actions={
          <>
            <button className="btn" onClick={() => api.revealPath(done.folder)}><FolderOpen size={16} /> Show backup</button>
            <button className="btn primary" onClick={close}>Close</button>
          </>
        }
      >
        <div className="muted" style={{ fontSize: 13 }}>
          A copy of your version of {files.length === 1 ? "the file" : `the ${files.length} files`} is in <span className="mono">{done.folder}</span>.
          {files.some((f) => choice(f) === "mine") && " The files you kept are still changes on this PC: save them as a version when you are ready."}
        </div>
      </Modal>
    );
  }

  return (
    <Modal
      wide
      title="Changed on both sides"
      sub={`The new version on the server changes ${files.length === 1 ? "a file" : `${files.length} files`} that you also changed on this PC. Choose what happens to ${files.length === 1 ? "it" : "each"}. Your version is always copied to ${BACKUP_FOLDER} first.`}
      onClose={close}
      actions={
        <>
          <span className="faint" style={{ flex: 1, fontSize: 12.5 }}>{busy}</span>
          <button className="btn" onClick={close} disabled={!!busy}>Cancel</button>
          <button className="btn primary" onClick={resolve} disabled={!!busy || !locks}>{busy ? <Spinner /> : <Download size={16} />} Continue and sync</button>
        </>
      }
    >
      <div className="conflict-list">
        {files.map((file) => {
          const owner = locks?.get(file);
          const other = lockedByOther(file);
          const text = isText(file);
          return (
            <div key={file} className="conflict-file">
              <div className="mono conflict-path">{file}</div>
              <div className="row between gap-s" style={{ marginTop: 6 }}>
                <div className="row gap-s">
                  {owner && <Pill tone={other ? "bad" : "muted"}><Lock size={11} /> {other ? `Locked by ${owner}` : "Locked by you"}</Pill>}
                  <Pill tone="flat">{text ? "Text file" : "Cannot be merged"}</Pill>
                </div>
                <button className="btn small ghost" onClick={() => api.revealPath(`${root}/${file}`)}><FolderOpen size={14} /> Show in folder</button>
              </div>
              <label className="choice">
                <input type="radio" name={file} checked={choice(file) === "take"} onChange={() => setChoices((c) => ({ ...c, [file]: "take" }))} />
                <div>
                  <b>Keep a copy of mine and take the new version</b>
                  <div className="faint">Your file is copied to {BACKUP_FOLDER}, then replaced by the version from the server.</div>
                </div>
              </label>
              <label className={`choice ${other ? "disabled" : ""}`}>
                <input type="radio" name={file} disabled={!!other} checked={choice(file) === "mine"} onChange={() => setChoices((c) => ({ ...c, [file]: "mine" }))} />
                <div>
                  <b>Keep mine</b>
                  <div className="faint">
                    {other
                      ? `Not possible: ${other} has this file locked. Ask ${other} first, or take the new version.`
                      : "Your version stays on this PC as a change. When you save it as a version, it replaces theirs on the server and their change is gone."}
                  </div>
                </div>
              </label>
              <div className="faint" style={{ fontSize: 12, marginTop: 4 }}>
                {text
                  ? "Lore cannot merge a file that changed on both sides of the same branch. To combine both: take the new version, then copy your lines over from the backup copy."
                  : "Unreal assets and other binary files cannot be merged: one of the two versions wins."}
              </div>
            </div>
          );
        })}
      </div>
    </Modal>
  );
}
