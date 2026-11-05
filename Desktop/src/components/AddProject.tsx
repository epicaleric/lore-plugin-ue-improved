import { useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { open } from "@tauri-apps/plugin-dialog";
import { Check, FolderOpen, FolderSearch, UploadCloud, X } from "lucide-react";
import { api } from "../lib/api";
import { invalidateStatus } from "../lib/actions";
import { useStore } from "../lib/store";
import { Field, Modal, Spinner } from "./ui";

/** Server names stay simple: letters, digits, dots, dashes and underscores. */
function serverName(name: string): string {
  return name.trim().replace(/[^A-Za-z0-9._-]+/g, "-").replace(/^-+|-+$/g, "");
}

const IGNORE_TEMPLATE = `# Generated Unreal and workstation state
/Saved/
/Intermediate/
/DerivedDataCache/
/Binaries/
/Builds/
**/Intermediate/
.vs/
.idea/
.vscode/
*.sln
*.suo
*.user
*.VC.db
*.VC.opendb
`;

interface Step {
  label: string;
  state: "wait" | "run" | "done" | "fail";
  detail?: string;
}

export function AddProjectModal({ onClose }: { onClose: () => void }) {
  const { server, session, settings, update, reload, toast } = useStore();
  const [mode, setMode] = useState<"upload" | "folder">("upload");
  const [folder, setFolder] = useState("");
  const [name, setName] = useState("");
  const [steps, setSteps] = useState<Step[]>([]);
  const [running, setRunning] = useState(false);
  const [done, setDone] = useState(false);

  const pick = async () => {
    const chosen = await open({ directory: true, multiple: false, title: "Choose the project folder" });
    if (!chosen || Array.isArray(chosen)) return;
    setFolder(chosen);
    if (!name) setName(chosen.split(/[\\/]/).filter(Boolean).pop() ?? "");
  };

  const addFolder = async () => {
    const chosen = await open({ directory: true, multiple: false, title: "Folder that holds Lore projects" });
    if (!chosen || Array.isArray(chosen)) return;
    if (!settings.roots.includes(chosen)) update({ roots: [...settings.roots, chosen] });
    toast("Folder added. Looking for projects...", "good");
    await reload();
    onClose();
  };

  const upload = async () => {
    if (!server || !session) return;
    const list: Step[] = [
      { label: "Prepare the folder", state: "wait" },
      { label: "Create the project on the server", state: "wait" },
      { label: "Add your files", state: "wait" },
      { label: "Save the first version", state: "wait" },
      { label: "Send it to the server", state: "wait" },
    ];
    setSteps(list);
    setRunning(true);
    const mark = (index: number, state: Step["state"], detail?: string) => {
      list[index] = { ...list[index], state, detail };
      setSteps([...list]);
    };
    const fail = (index: number, result: { stderr: string; stdout: string }) =>
      mark(index, "fail", (result.stderr || result.stdout).trim().split("\n").slice(0, 3).join(" "));

    try {
      mark(0, "run");
      await invoke("write_text_file", { path: `${folder}\\.loreignore`, contents: IGNORE_TEMPLATE, overwrite: false });
      mark(0, "done");

      // A folder that was prepared by an earlier attempt already is a repository: carry on from there.
      mark(1, "run");
      let result: { ok: boolean; stderr: string; stdout: string } = { ok: true, stderr: "", stdout: "" };
      if (!(await invoke<boolean>("path_exists", { path: `${folder}\\.lore` }))) {
        result = await api.lore(["repository", "create", `${server.loreUrl}/${serverName(name)}`], folder, server.id);
        if (!result.ok) return fail(1, result);
      }
      mark(1, "done");

      // `stage .` only takes files that are already marked as changed, so ask Lore to scan the folder.
      mark(2, "run");
      result = await api.lore(["stage", "--scan", "."], folder, server.id);
      if (!result.ok) return fail(2, result);
      mark(2, "done");

      mark(3, "run");
      result = await api.lore(["commit", "Initial version"], folder, server.id);
      if (!result.ok && !/nothing/i.test(result.stderr + result.stdout)) return fail(3, result);
      mark(3, "done");

      mark(4, "run");
      result = await api.lore(["push"], folder, server.id);
      if (!result.ok) return fail(4, result);
      mark(4, "done");

      invalidateStatus();
      await reload();
      setDone(true);
      toast(`${serverName(name)} is on the server`, "good");
    } finally {
      setRunning(false);
    }
  };

  return (
    <Modal
      wide
      title="Add a project"
      sub="Put a project from this PC on the server, or show a folder that already has Lore projects."
      onClose={onClose}
      actions={
        mode === "upload" && !done ? (
          <>
            <button className="btn" onClick={onClose} disabled={running}>Cancel</button>
            <button className="btn primary" disabled={!folder || !serverName(name) || running || !session} onClick={upload}>
              {running ? <Spinner /> : <UploadCloud size={16} />} Upload to {server?.label ?? "server"}
            </button>
          </>
        ) : (
          <button className="btn primary" onClick={onClose}>{done ? "Done" : "Close"}</button>
        )
      }
    >
      <div className="tabs">
        <button className={`tab ${mode === "upload" ? "active" : ""}`} onClick={() => setMode("upload")}><UploadCloud size={16} /> Upload a local project</button>
        <button className={`tab ${mode === "folder" ? "active" : ""}`} onClick={() => setMode("folder")}><FolderSearch size={16} /> Show an existing folder</button>
      </div>

      {mode === "upload" ? (
        <>
          {!session && <div className="banner warn">Sign in to a server first.</div>}
          <Field label="Project folder" hint="The folder that holds the .uproject file.">
            <div className="row gap-s">
              <input className="input" readOnly value={folder} placeholder="Choose a folder" />
              <button className="btn" onClick={pick}><FolderOpen size={16} /> Browse</button>
            </div>
          </Field>
          <Field label="Name on the server" hint={serverName(name) && serverName(name) !== name.trim() ? `It will be called ${serverName(name)}. Names on the server cannot have spaces.` : "Letters, digits, dots, dashes and underscores."}>
            <input className="input" value={name} onChange={(e) => setName(e.target.value)} />
          </Field>
          <div className="muted" style={{ fontSize: 12.5 }}>
            A <span className="mono">.loreignore</span> file is added so Saved, Intermediate, Binaries and DerivedDataCache stay out of the project.
          </div>
          {steps.length > 0 && (
            <div className="col" style={{ marginTop: 16 }}>
              {steps.map((step) => (
                <div key={step.label} className="row" style={{ gap: 10 }}>
                  <span style={{ width: 20, display: "grid", placeItems: "center", color: step.state === "done" ? "var(--good)" : step.state === "fail" ? "var(--bad)" : "var(--muted)" }}>
                    {step.state === "run" ? <Spinner size={15} /> : step.state === "done" ? <Check size={16} /> : step.state === "fail" ? <X size={16} /> : "•"}
                  </span>
                  <div>
                    <div>{step.label}</div>
                    {step.detail && <div className="faint" style={{ fontSize: 12 }}>{step.detail}</div>}
                  </div>
                </div>
              ))}
            </div>
          )}
        </>
      ) : (
        <div className="col">
          <div className="muted">Pick a folder that contains projects you already cloned with Lore. Projects inside it (up to three levels deep) show up in the list.</div>
          <div><button className="btn primary" onClick={addFolder}><FolderSearch size={16} /> Choose folder</button></div>
        </div>
      )}
    </Modal>
  );
}
