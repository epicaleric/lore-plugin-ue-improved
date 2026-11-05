import { useEffect, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { ArrowLeft, Download, FolderOpen, Lock, Play, RefreshCw, ShieldCheck, Users } from "lucide-react";
import { Avatar, Empty, Field, Pill, Spinner, Thumb } from "../components/ui";
import { api, adminApi } from "../lib/api";
import { fullDate, timeAgo } from "../lib/format";
import { layoutDescription, parseHistory } from "../lib/parse";
import { useProjectActions } from "../lib/actions";
import { useStore } from "../lib/store";
import { pluginIsOutdated, useUpdates } from "../lib/updates";
import type { AdminUser, Grant, Revision } from "../lib/types";
import { StatusPill, useProjectView } from "./Projects";

type Tab = "overview" | "history" | "access" | "settings";

function History({ path }: { path: string }) {
  const [revisions, setRevisions] = useState<Revision[] | null>(null);
  const [error, setError] = useState("");
  useEffect(() => {
    api.lore(["history", "60"], path).then((result) => {
      if (result.ok) setRevisions(parseHistory(result.stdout));
      else setError((result.stderr || result.stdout).trim());
    });
  }, [path]);
  if (error) return <div className="banner bad">{error}</div>;
  if (!revisions) return <div className="muted row"><Spinner /> Loading history</div>;
  return (
    <div className="timeline">
      {revisions.map((revision, index) => {
        const lines = layoutDescription(revision.description);
        return (
          <div className="rev" key={revision.signature || revision.number}>
            <div className="rev-rail"><span className={`rev-node ${index === 0 ? "head" : ""}`} /></div>
            <div className="rev-card">
              <div className="rev-head">
                <span className="rev-num">{revision.number}</span>
                <Avatar name={revision.author} size="small" />
                <b>{revision.author}</b>
                <span className="spacer" />
                <span className="muted" title={fullDate(revision.date)}>{timeAgo(revision.date)}</span>
              </div>
              <div className="rev-desc">
                {lines.map((line, i) => <div key={i} className={i > 0 ? "sub" : ""}>{line}</div>)}
              </div>
            </div>
          </div>
        );
      })}
    </div>
  );
}

function Access({ repoId, repoName }: { repoId: string; repoName: string }) {
  const { server, isAdmin, toast } = useStore();
  const [users, setUsers] = useState<AdminUser[]>([]);
  const [grants, setGrants] = useState<Grant[]>([]);
  const [busy, setBusy] = useState<string | null>(null);
  const [loaded, setLoaded] = useState(false);

  const load = async () => {
    if (!server || !isAdmin) return;
    const [u, g] = await Promise.all([adminApi.users(server), adminApi.grants(server)]);
    setUsers(u);
    setGrants(g);
    setLoaded(true);
  };
  useEffect(() => {
    load().catch((e) => toast(String(e), "bad"));
  }, [server?.id, isAdmin]);

  if (!isAdmin) {
    return (
      <Empty icon={<Lock size={28} />} title="Only administrators see who has access">
        Ask an administrator of this server if you need access for someone.
      </Empty>
    );
  }
  if (!loaded) return <div className="muted row"><Spinner /> Loading</div>;

  const hasGrant = (user: AdminUser) => grants.some((g) => g.user === user.name && (g.repository === repoId || g.repository === "*"));
  const viaAll = (user: AdminUser) => user.admin || grants.some((g) => g.user === user.name && g.repository === "*");

  const toggle = async (user: AdminUser) => {
    if (!server) return;
    setBusy(user.name);
    try {
      if (grants.some((g) => g.user === user.name && g.repository === repoId)) await adminApi.revoke(server, user.name, repoId);
      else await adminApi.grant(server, user.name, repoId);
      await load();
    } catch (e) {
      toast(String(e), "bad");
    } finally {
      setBusy(null);
    }
  };

  return (
    <div className="panel">
      <h3>Who can use {repoName}</h3>
      <div className="desc">Administrators can use every project. For everyone else, switch access on or off here.</div>
      <table className="table">
        <tbody>
          {users.map((user) => (
            <tr key={user.name}>
              <td style={{ width: 44 }}><Avatar name={user.name} /></td>
              <td>
                <b>{user.displayName || user.name}</b>
                <div className="muted" style={{ fontSize: 12.5 }}>{user.name}{user.disabled ? " · disabled" : ""}</div>
              </td>
              <td style={{ width: 160 }}>{user.admin && <Pill tone="accent"><ShieldCheck size={12} /> Administrator</Pill>}</td>
              <td style={{ width: 110, textAlign: "right" }}>
                {viaAll(user) ? (
                  <span className="muted">All projects</span>
                ) : busy === user.name ? <Spinner /> : (
                  <button className={`switch ${hasGrant(user) ? "on" : ""}`} onClick={() => toggle(user)} title={hasGrant(user) ? "Remove access" : "Give access"} />
                )}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function ProjectSettings({ projectKey }: { projectKey: string }) {
  const { projects, settings, update, engines, toast } = useStore();
  const project = projects.find((p) => p.key === projectKey)!;
  const id = project.id;
  const override = (id && settings.overrides[id]) || {};
  const [name, setName] = useState(override.displayName ?? "");

  const set = (patch: Partial<typeof override>) => {
    if (!id) return;
    update({ overrides: { ...settings.overrides, [id]: { ...override, ...patch } } });
  };
  const assigned = id ? settings.assignments[id] ?? "" : "";

  if (!id) return <div className="banner">Put this project on the server first to change its settings.</div>;

  return (
    <div className="panel">
      <h3>Project settings</h3>
      <div className="desc">These are only stored in this app on this PC.</div>
      <Field label="Display name">
        <input className="input" value={name} placeholder={project.name} onChange={(e) => setName(e.target.value)} onBlur={() => set({ displayName: name.trim() || undefined })} />
      </Field>
      <Field label="Unreal version" hint="By default the version from the project itself is used.">
        <select className="select" value={override.engine ?? ""} onChange={(e) => set({ engine: e.target.value || undefined })}>
          <option value="">Automatic ({project.local?.engine ?? "unknown"})</option>
          {engines.map((e) => <option key={e.path} value={e.version}>{e.version}</option>)}
        </select>
      </Field>
      <Field label="Category">
        <select
          className="select"
          value={assigned}
          onChange={(e) => {
            const next = { ...settings.assignments };
            if (e.target.value) next[id] = e.target.value;
            else delete next[id];
            update({ assignments: next });
          }}
        >
          <option value="">No category</option>
          {settings.categories.map((c) => <option key={c.id} value={c.id}>{c.parentId ? "   " : ""}{c.name}</option>)}
        </select>
      </Field>
      <Field label="Thumbnail">
        <div className="row gap-s">
          <button className="btn" onClick={async () => {
            const file = await open({ multiple: false, filters: [{ name: "Image", extensions: ["png", "jpg", "jpeg"] }] });
            if (file && !Array.isArray(file)) set({ thumbnail: file });
          }}>Choose image</button>
          {override.thumbnail && <button className="btn ghost" onClick={() => { set({ thumbnail: undefined }); toast("Thumbnail reset"); }}>Use the project's own</button>}
        </div>
      </Field>
    </div>
  );
}

export function ProjectDetail() {
  const { projects, projectKey, go, server, reload, settings } = useStore();
  const { busy, openProject, clone, sync } = useProjectActions();
  const updates = useUpdates();
  const project = projects.find((p) => p.key === projectKey);
  const [tab, setTab] = useState<Tab>("overview");
  const view = useProjectView(project ?? { key: "", name: "", onServer: false });
  if (!project) return <Empty icon={<FolderOpen size={28} />} title="Project not found"><button className="btn" onClick={() => go("projects")}>Back to projects</button></Empty>;

  const working = busy[project.key];
  const behind = view.status ? view.status.remoteRevision - view.status.localRevision : 0;
  const category = project.id ? settings.categories.find((c) => c.id === settings.assignments[project.id!]) : undefined;
  const pluginOld = pluginIsOutdated(project.local?.plugin ?? null, updates.plugin.latest);
  const pluginBusy = !!updates.busy[`plugin:${project.local?.path}`];

  return (
    <>
      <div className="topbar" style={{ paddingBottom: 6 }}>
        <button className="btn icon ghost" onClick={() => go("projects")} title="Back"><ArrowLeft size={18} /></button>
        <Thumb name={view.name} image={view.thumbnail} className="list-thumb" style={{ width: 84, height: 52 }} />
        <div>
          <h1>{view.name}</h1>
          <div className="row gap-s" style={{ marginTop: 4 }}>
            <StatusPill project={project} status={view.status} />
            {view.engine && <Pill>Unreal {view.engine}</Pill>}
            {category && <Pill tone="accent">{category.name}</Pill>}
          </div>
        </div>
        <div className="spacer" />
        {project.local ? (
          <>
            {behind > 0 && <button className="btn" disabled={!!working} onClick={() => sync(project)}>{working ? <Spinner /> : <Download size={16} />} Sync {behind}</button>}
            <button className="btn" onClick={() => api.openPath(project.local!.path)}><FolderOpen size={16} /> Folder</button>
            <button className="btn primary" onClick={() => openProject(project)}><Play size={16} /> Open in Unreal</button>
          </>
        ) : project.onServer ? (
          <button className="btn primary" disabled={!!working} onClick={() => clone(project)}>{working ? <Spinner /> : <Download size={16} />} Clone to this PC</button>
        ) : null}
      </div>
      <div className="content">
        <div className="tabs">
          {(["overview", "history", "access", "settings"] as Tab[]).map((t) => (
            <button key={t} className={`tab ${tab === t ? "active" : ""}`} onClick={() => setTab(t)}>
              {t === "access" && <Users size={15} />}{t[0].toUpperCase() + t.slice(1)}
            </button>
          ))}
        </div>

        {tab === "overview" && (
          <div className="col">
            {behind > 0 && <div className="banner warn"><RefreshCw size={18} /><div style={{ flex: 1 }}>Your copy is {behind} version{behind > 1 ? "s" : ""} behind the server.</div><button className="btn primary small" onClick={() => sync(project)}>Sync now</button></div>}
            <div className="panel">
              <h3>Latest change</h3>
              {view.last ? (
                <div className="row" style={{ alignItems: "flex-start", marginTop: 10 }}>
                  <Avatar name={view.last.author} size="large" />
                  <div>
                    <div><b>{view.last.author}</b> <span className="muted">· revision {view.last.number} · {fullDate(view.last.date)}</span></div>
                    <div className="rev-desc" style={{ margin: "6px 0 0" }}>{layoutDescription(view.last.description).map((l, i) => <div key={i} className={i > 0 ? "sub" : ""}>{l}</div>)}</div>
                  </div>
                </div>
              ) : <div className="muted">{project.local ? "No history yet." : "Clone the project to see its history."}</div>}
            </div>
            {project.local && (
              <div className="panel">
                <h3>On this PC</h3>
                <table className="table"><tbody>
                  <tr><td className="muted" style={{ width: 170 }}>Folder</td><td className="mono">{project.local.path}</td></tr>
                  <tr><td className="muted">Revision</td><td>{view.status ? `${view.status.localRevision} of ${view.status.remoteRevision} on the server` : "-"}</td></tr>
                  <tr><td className="muted">Changed files</td><td>{view.status?.changes ?? "-"}</td></tr>
                  <tr><td className="muted">Lore plugin</td><td>
                    <div className="row">
                      <span>{project.local.plugin ? `Installed (${project.local.plugin})` : "Not installed in this project"}{pluginOld && updates.plugin.latest ? ` · ${updates.plugin.latest} is available` : ""}</span>
                      <button className="btn small" disabled={pluginBusy || !project.local.uproject} onClick={() => updates.installPlugin("project", project.local!.path)}>
                        {pluginBusy ? <Spinner size={14} /> : <Download size={14} />} {project.local.plugin ? (pluginOld ? "Update" : "Reinstall") : "Install"}
                      </button>
                    </div>
                  </td></tr>
                </tbody></table>
              </div>
            )}
            {!project.onServer && <div className="banner">This project is only on this PC. Use <b>Add project</b> on the Projects page to put it on {server?.label ?? "the server"}.</div>}
          </div>
        )}

        {tab === "history" && (project.local ? <History path={project.local.path} /> : <Empty icon={<Download size={28} />} title="Clone the project to see its history">The history is read from the copy on your PC.</Empty>)}
        {tab === "access" && (project.id ? <Access repoId={project.id} repoName={view.name} /> : <div className="banner">Access can be set once the project is on the server.</div>)}
        {tab === "settings" && <ProjectSettings projectKey={project.key} />}
      </div>
    </>
  );
}
