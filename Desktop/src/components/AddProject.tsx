import { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { open } from "@tauri-apps/plugin-dialog";
import { Check, FolderOpen, FolderSearch, UploadCloud, X } from "lucide-react";
import { api } from "../lib/api";
import { parseInfo } from "../lib/parse";
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
  /** 0 to 1 when Lore reports how far it is, otherwise the bar just moves */
  progress?: number;
  /** the latest thing Lore said */
  line?: string;
}

const UNITS: Record<string, number> = { B: 1, KiB: 1024, MiB: 1024 ** 2, GiB: 1024 ** 3 };

/** Reads "12/40 files" or "3.1 MiB/17 MiB" out of a line of Lore output. */
function progressOf(line: string): number | undefined {
  const bytes = line.match(/([\d.]+)\s*(B|KiB|MiB|GiB)\s*\/\s*([\d.]+)\s*(B|KiB|MiB|GiB)/);
  if (bytes) {
    const total = parseFloat(bytes[3]) * UNITS[bytes[4]];
    if (total > 0) return Math.min(1, (parseFloat(bytes[1]) * UNITS[bytes[2]]) / total);
  }
  const files = line.match(/(\d+)\s*\/\s*(\d+)\s*files/);
  if (files && Number(files[2]) > 0) return Math.min(1, Number(files[1]) / Number(files[2]));
  return undefined;
}

const clock = (seconds: number) => `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, "0")}`;
const size = (bytes: number) => (bytes >= 1024 ** 3 ? `${(bytes / 1024 ** 3).toFixed(1)} GB` : `${Math.max(1, Math.round(bytes / 1024 ** 2))} MB`);

export function AddProjectModal({ onClose }: { onClose: () => void }) {
  const { server, session, settings, update, reload, toast, remote } = useStore();
  const [mode, setMode] = useState<"upload" | "folder">("upload");
  const [folder, setFolder] = useState("");
  const [name, setName] = useState("");
  const [steps, setSteps] = useState<Step[]>([]);
  const [running, setRunning] = useState(false);
  const [done, setDone] = useState(false);
  const [seconds, setSeconds] = useState(0);
  const started = useRef(0);
  useEffect(() => {
    if (!running) return;
    started.current = Date.now();
    setSeconds(0);
    const timer = setInterval(() => setSeconds(Math.round((Date.now() - started.current) / 1000)), 1000);
    return () => clearInterval(timer);
  }, [running]);

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
    const mark = (index: number, state: Step["state"], extra?: Partial<Step>) => {
      list[index] = { ...list[index], state, ...extra };
      setSteps([...list]);
    };
    const fail = (index: number, result: { stderr: string; stdout: string }) =>
      mark(index, "fail", { detail: (result.stderr || result.stdout).trim().split("\n").slice(0, 3).join(" ") });
    /** Runs a Lore command and shows what it says while it works. */
    const work = (index: number, args: string[]) =>
      api.loreStream(args, folder, server.id, (line) => mark(index, "run", { line, progress: progressOf(line) ?? list[index].progress }));

    try {
      mark(0, "run");
      await invoke("write_text_file", { path: `${folder}\\.loreignore`, contents: IGNORE_TEMPLATE, overwrite: false });
      const stats = await api.folderStats(folder).catch(() => null);
      mark(0, "done", { detail: stats ? `${stats.files.toLocaleString()} files, ${size(stats.bytes)} (without Saved, Intermediate, Binaries and DerivedDataCache)` : undefined });

      mark(1, "run");
      // A folder that has a .lore is either a project that is on the server already (carry on from there),
      // or one whose project was deleted from the server. The old data is then set aside, not deleted.
      let result: { ok: boolean; stderr: string; stdout: string } = { ok: true, stderr: "", stdout: "" };
      let linked = false;
      if (await invoke<boolean>("path_exists", { path: `${folder}\\.lore` })) {
        const info = await api.lore(["repository", "info"], folder);
        const id = info.ok ? parseInfo(info.stdout).id : undefined;
        linked = !!id && remote.some((r) => r.id === id);
        if (!linked) {
          const aside = await api.setAsideLore(folder);
          mark(1, "run", { detail: `This folder belonged to a project that is no longer on the server. Its old Lore data is kept in ${aside.split(/[\\/]/).pop()}.` });
        }
      }
      if (!linked) {
        result = await api.lore(["repository", "create", `${server.loreUrl}/${serverName(name)}`], folder, server.id);
        if (!result.ok) return fail(1, result);
      }
      mark(1, "done", { detail: list[1].detail });

      mark(2, "run");
      // `stage .` only takes files that are already marked as changed, so ask Lore to scan the folder.
      result = await work(2, ["stage", "--scan", "."]);
      if (!result.ok) return fail(2, result);
      mark(2, "done", { progress: 1 });

      mark(3, "run");
      result = await work(3, ["commit", "Initial version"]);
      if (!result.ok && !/nothing/i.test(result.stderr + result.stdout)) return fail(3, result);
      mark(3, "done", { progress: 1 });

      mark(4, "run");
      result = await work(4, ["push"]);
      if (!result.ok) return fail(4, result);
      mark(4, "done", { progress: 1 });

      invalidateStatus();
      // The folder is not under a scanned root yet when it was picked from anywhere on the disk.
      const parent = folder.replace(/[\\/][^\\/]+$/, "");
      if (!settings.roots.some((r) => folder.toLowerCase().startsWith(r.toLowerCase()))) update({ roots: [...settings.roots, parent] });
      await reload();
      setDone(true);
      toast(`${serverName(name)} is on the server`, "good");
      setTimeout(onClose, 1200);
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
              {steps.map((step, i) => (
                <div key={step.label} className="row" style={{ gap: 10, alignItems: "flex-start" }}>
                  <span style={{ width: 20, display: "grid", placeItems: "center", color: step.state === "done" ? "var(--good)" : step.state === "fail" ? "var(--bad)" : "var(--muted)" }}>
                    {step.state === "run" ? <Spinner size={15} /> : step.state === "done" ? <Check size={16} /> : step.state === "fail" ? <X size={16} /> : "•"}
                  </span>
                  <div style={{ flex: 1, minWidth: 0 }}>
                    <div className="row between">
                      <span>{step.label}</span>
                      {step.state === "run" && <span className="faint" style={{ fontSize: 12 }}>{clock(seconds)}{step.progress !== undefined ? `  ·  ${Math.round(step.progress * 100)}%` : ""}</span>}
                    </div>
                    {step.state === "run" && i >= 2 && (
                      <>
                        <div className={`progress ${step.progress === undefined ? "busy" : ""}`}><span style={{ width: `${Math.round((step.progress ?? 0) * 100)}%` }} /></div>
                        {step.line && <div className="faint" style={{ fontSize: 12, marginTop: 4, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{step.line}</div>}
                        {step.progress === undefined && !step.line && <div className="faint" style={{ fontSize: 12, marginTop: 4 }}>Lore is working. For a big project this can take several minutes.</div>}
                      </>
                    )}
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
