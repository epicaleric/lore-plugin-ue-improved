import { useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { Trash2 } from "lucide-react";
import { api } from "../lib/api";
import { invalidateStatus } from "../lib/actions";
import { useStore } from "../lib/store";
import type { LocalProject, Project } from "../lib/types";
import { Field, Modal, Spinner } from "./ui";

/** Asks the user to type the project name before something that cannot be undone. */
export function ConfirmModal({ title, text, word, button, onConfirm, onClose }: { title: string; text: string; word: string; button: string; onConfirm: () => Promise<void>; onClose: () => void }) {
  const [typed, setTyped] = useState("");
  const [busy, setBusy] = useState(false);
  return (
    <Modal
      title={title}
      sub={text}
      onClose={onClose}
      actions={
        <>
          <button className="btn" onClick={onClose} disabled={busy}>Cancel</button>
          <button
            className="btn danger"
            disabled={busy || typed.trim() !== word}
            onClick={async () => {
              setBusy(true);
              try {
                await onConfirm();
              } finally {
                setBusy(false);
                onClose();
              }
            }}
          >
            {busy ? <Spinner /> : <Trash2 size={16} />} {button}
          </button>
        </>
      }
    >
      <Field label={`Type ${word} to confirm`}>
        <input className="input" autoFocus value={typed} onChange={(e) => setTyped(e.target.value)} />
      </Field>
    </Modal>
  );
}

export function useDeletes() {
  const { server, reload, toast, update, settings } = useStore();

  /** Removes the working copy folder from this PC. The project on the server is not touched. */
  const deleteLocal = async (copy: LocalProject) => {
    try {
      await invoke("delete_folder", { path: copy.path });
      toast("The copy was removed from this PC", "good");
      invalidateStatus();
      await reload();
    } catch (error) {
      toast(String(error), "bad");
    }
  };

  const hide = (copy: LocalProject) => {
    update({ hidden: [...settings.hidden, copy.path] });
    toast("Hidden. You can show it again by removing the folder from the hidden list in Settings.");
  };

  /** Deletes the project, with its whole history, from the Lore server. */
  const deleteFromServer = async (project: Project) => {
    if (!server) return;
    const result = await api.lore(["repository", "delete", `${server.loreUrl}/${encodeURIComponent(project.name)}`], null, server.id);
    if (!result.ok) {
      toast((result.stderr || result.stdout).trim().split("\n").slice(0, 2).join(" "), "bad");
      return;
    }
    toast(`${project.name} was deleted from the server`, "good");
    invalidateStatus();
    await reload();
  };

  return { deleteLocal, hide, deleteFromServer };
}
