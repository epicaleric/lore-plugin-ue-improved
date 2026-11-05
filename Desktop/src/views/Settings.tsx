import { useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { Edit3, FolderPlus, LogIn, LogOut, Plus, Trash2, X } from "lucide-react";
import { ServerModal, SignInModal } from "../components/Servers";
import { Pill } from "../components/ui";
import { api } from "../lib/api";
import { readableOn } from "../lib/format";
import { useStore } from "../lib/store";
import type { Server } from "../lib/types";

const ACCENTS = ["#14a0a5", "#2f7de1", "#9664dc", "#28aa5f", "#e19620", "#e14b73", "#73829b"];

export function Settings() {
  const { settings, update, session, server, refreshSession, reload, toast } = useStore();
  const [editing, setEditing] = useState<Server | "new" | null>(null);
  const [signingIn, setSigningIn] = useState<Server | null>(null);

  const addRoot = async () => {
    const chosen = await open({ directory: true, multiple: false, title: "Folder with Lore projects" });
    if (!chosen || Array.isArray(chosen)) return;
    if (!settings.roots.includes(chosen)) update({ roots: [...settings.roots, chosen] });
    reload();
  };

  const removeServer = async (target: Server) => {
    if (!window.confirm(`Remove ${target.label} from this app? Your projects on the server are not touched.`)) return;
    await api.signOut(target.id);
    const servers = settings.servers.filter((s) => s.id !== target.id);
    update({ servers, activeServer: servers[0]?.id });
    toast("Server removed");
  };

  return (
    <>
      <div className="topbar"><div><h1>Settings</h1><div className="sub">Servers, folders and looks</div></div></div>
      <div className="content">
        <div className="panel">
          <div className="row between"><div><h3>Servers</h3><div className="desc">The Lore servers this app can work with.</div></div><button className="btn primary" onClick={() => setEditing("new")}><Plus size={16} /> Add server</button></div>
          <table className="table"><tbody>
            {settings.servers.map((s) => (
              <tr key={s.id}>
                <td><b>{s.label}</b><div className="muted mono">{s.loreUrl}</div></td>
                <td style={{ width: 150 }}>{s.id === server?.id ? (session ? <Pill tone="good">Signed in as {session.userName}</Pill> : <Pill tone="muted">Not signed in</Pill>) : null}</td>
                <td style={{ width: 280, textAlign: "right" }}>
                  <div className="row gap-s" style={{ justifyContent: "flex-end" }}>
                    {s.id === server?.id && session ? (
                      <button className="btn small" onClick={async () => { await api.signOut(s.id); await refreshSession(); reload(); }}><LogOut size={14} /> Sign out</button>
                    ) : (
                      <button className="btn small" onClick={() => { update({ activeServer: s.id }); setSigningIn(s); }}><LogIn size={14} /> Sign in</button>
                    )}
                    <button className="btn small icon ghost" title="Edit" onClick={() => setEditing(s)}><Edit3 size={14} /></button>
                    <button className="btn small icon ghost danger" title="Remove" onClick={() => removeServer(s)}><Trash2 size={14} /></button>
                  </div>
                </td>
              </tr>
            ))}
          </tbody></table>
          {settings.servers.length === 0 && <div className="muted">No servers yet.</div>}
        </div>

        <div className="panel">
          <div className="row between"><div><h3>Project folders</h3><div className="desc">The app looks in these folders (and the folders inside them) for projects that use Lore.</div></div><button className="btn" onClick={addRoot}><FolderPlus size={16} /> Add folder</button></div>
          <div className="col">
            {(settings.roots.length ? settings.roots : ["Documents\\Unreal Projects (default)"]).map((root) => (
              <div key={root} className="row between" style={{ background: "var(--bg)", border: "1px solid var(--border-soft)", borderRadius: 10, padding: "9px 14px" }}>
                <span className="mono">{root}</span>
                {settings.roots.includes(root) && <button className="btn icon small ghost" onClick={() => { update({ roots: settings.roots.filter((r) => r !== root) }); reload(); }}><X size={15} /></button>}
              </div>
            ))}
          </div>
        </div>

        <div className="panel">
          <h3>Appearance</h3>
          <div className="desc">The accent colour is used for buttons, highlights and the timeline.</div>
          <div className="swatches">
            {ACCENTS.map((color) => (
              <button key={color} className={`swatch ${settings.accent.toLowerCase() === color ? "on" : ""}`} style={{ background: color }} onClick={() => update({ accent: color })} />
            ))}
            <label className="btn small" style={{ cursor: "pointer" }}>
              Choose colour
              <input type="color" value={settings.accent} onChange={(e) => update({ accent: e.target.value })} style={{ width: 22, height: 22, border: 0, background: "none", padding: 0 }} />
            </label>
          </div>
          <span style={{ display: "none" }}>{readableOn(settings.accent)}</span>
        </div>
      </div>
      {editing && <ServerModal initial={editing === "new" ? undefined : editing} onClose={() => setEditing(null)} onSaved={(s) => { if (editing === "new") setSigningIn(s); }} />}
      {signingIn && <SignInModal server={signingIn} onClose={() => setSigningIn(null)} />}
    </>
  );
}

