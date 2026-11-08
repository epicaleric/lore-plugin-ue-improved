import { useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { Copy, FolderOpen, ImagePlus } from "lucide-react";
import { api } from "../lib/api";
import { duplicateProject, validProjectName } from "../lib/duplicate";
import { invalidateStatus } from "../lib/actions";
import { useStore } from "../lib/store";
import type { Project } from "../lib/types";
import { Field, Modal, Spinner, Switch, Thumb } from "./ui";

export function DuplicateModal({ project, onClose }: { project: Project; onClose: () => void }) {
  const { server, session, settings, update, reload, toast, categories, remote } = useStore();
  const source = project.local!;
  const [name, setName] = useState("");
  const [parent, setParent] = useState(source.path.replace(/[\\/][^\\/]+[\\/]?$/, ""));
  const [thumbnail, setThumbnail] = useState<string | null>(null);
  const [category, setCategory] = useState((project.id && settings.assignments[project.id]) || "");
  const [toServer, setToServer] = useState(!!session);
  const [step, setStep] = useState<string | null>(null);

  const taken = remote.some((r) => r.name.toLowerCase() === name.toLowerCase());
  const problem = !name ? "" : !validProjectName(name) ? "Unreal allows letters, digits and _, starting with a letter, at most 20 characters." : toServer && taken ? "There is already a project with this name on the server." : "";

  const run = async () => {
    setStep("Starting");
    try {
      const target = await duplicateProject(source, { name, parent, thumbnail, server: toServer && session ? server ?? null : null }, setStep);
      // The new project is found where it was put, and gets the chosen category once its id is known.
      const searched = [...(await api.defaultRoots()), ...settings.roots];
      if (!searched.some((root) => target.toLowerCase().startsWith(root.toLowerCase()))) update({ roots: [...settings.roots, parent] });
      const id = await api.repoId(target);
      if (id && category) update({ assignments: { ...settings.assignments, [id]: category } });
      invalidateStatus();
      await reload();
      toast(`${name} is ready${toServer ? ` and on ${server?.label}` : ""}`, "good");
      onClose();
    } catch (error) {
      toast(String(error).replace(/^Error: /, ""), "bad");
      setStep(null);
    }
  };

  return (
    <Modal
      title={`Duplicate ${project.name}`}
      sub="A new Unreal project with its own name and a fresh history, made from the files of the copy on this PC."
      onClose={() => !step && onClose()}
      actions={
        <>
          <button className="btn" disabled={!!step} onClick={onClose}>Cancel</button>
          <button className="btn primary" disabled={!!step || !name || !!problem} onClick={run}>{step ? <Spinner /> : <Copy size={16} />} Duplicate</button>
        </>
      }
    >
      <Field label="Name of the new project" hint={problem || "Used for the .uproject, the project settings in Unreal, the folder and the project on the server."}>
        <input className="input" autoFocus value={name} placeholder="MyGame" onChange={(e) => setName(e.target.value.trim())} />
      </Field>
      <Field label="Folder" hint={name ? `The project goes into ${parent}\\${name}` : undefined}>
        <div className="row gap-s">
          <input className="input" readOnly value={parent} />
          <button className="btn" onClick={async () => { const chosen = await open({ directory: true, multiple: false, title: "Where should the new project go?" }); if (chosen && !Array.isArray(chosen)) setParent(chosen); }}><FolderOpen size={16} /> Browse</button>
        </div>
      </Field>
      <Field label="Thumbnail" hint="Saved in the project, so Unreal and everyone on the server see it. Without a choice the original's is kept.">
        <div className="row gap-s">
          {thumbnail && <Thumb name={name || project.name} image={thumbnail} className="list-thumb" style={{ width: 52, height: 52 }} />}
          <button className="btn" onClick={async () => { const file = await open({ multiple: false, filters: [{ name: "Image", extensions: ["png", "jpg", "jpeg"] }] }); if (file && !Array.isArray(file)) setThumbnail(file); }}><ImagePlus size={16} /> Choose image</button>
          {thumbnail && <button className="btn ghost" onClick={() => setThumbnail(null)}>Keep the original</button>}
        </div>
      </Field>
      <Field label="Category">
        <select className="select" value={category} onChange={(e) => setCategory(e.target.value)}>
          <option value="">No category</option>
          {categories.map((c) => <option key={c.id} value={c.id}>{c.parentId ? "   " : ""}{c.name}</option>)}
        </select>
      </Field>
      <div className="row gap-s">
        <Switch on={toServer && !!session} onChange={setToServer} />
        <span>{session ? `Put it on ${server?.label} as a new project` : "Sign in to put it on the server as well"}</span>
      </div>
      {step && <div className="muted row" style={{ marginTop: 14 }}><Spinner size={14} /> {step}</div>}
    </Modal>
  );
}
