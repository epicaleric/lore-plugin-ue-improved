import { useEffect, useMemo, useRef, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { ArrowDownToLine, ArrowUpFromLine, Boxes, Check, ChevronDown, ChevronRight, CloudOff, Download, FilePen, FolderOpen, FolderPlus, Grid2x2, List, Play, RefreshCw, Search, Server as ServerIcon, Star, UploadCloud } from "lucide-react";
import { Avatar, Empty, Pill, Spinner, Thumb, useMenu } from "../components/ui";
import { firstLine } from "../lib/parse";
import { formatBytes, timeAgo } from "../lib/format";
import { invalidateStatus, peekRevision, peekStatus, useAuthors, useCacheVersion, useLastRevision, useProjectActions, useStatus } from "../lib/actions";
import { useStore } from "../lib/store";
import { BACKUP_SUFFIX, isUefnBackup } from "../lib/uefn";
import { api } from "../lib/api";
import type { GroupKey, Project, SortKey } from "../lib/types";
import { SignInModal } from "../components/Servers";
import { AddProjectModal } from "../components/AddProject";

type Filter = "all" | "local" | "remote" | "unlinked";

export const DRAG_TYPE = "application/x-lore-project";

export function useProjectView(project: Project) {
  const { settings, server } = useStore();
  const override = project.id ? settings.overrides[project.id] : undefined;
  const status = useStatus(project.local?.path, server?.id, 0);
  const localLast = useLastRevision(project.local?.path, 0);
  const last = localLast ?? project.remote?.last ?? project.uefn?.revisions?.[0] ?? null;
  const name = override?.displayName || project.name.replace(BACKUP_SUFFIX, "");
  const engine = override?.engine || project.local?.engine || project.remote?.engine || null;
  const thumbnail = override?.thumbnail || project.local?.thumbnail || project.remote?.thumbnail || project.uefn?.thumbnail || null;
  return { name, engine, thumbnail, status, last, override, bytes: project.remote?.bytes ?? 0 };
}

export function StatusPill({ project, status }: { project: Project; status: ReturnType<typeof useStatus> }) {
  const { session } = useStore();
  if (project.uefn) {
    const revision = project.uefn.revisions?.[0]?.number;
    return <Pill tone="accent" title="A project of Unreal Editor for Fortnite. Its versions are on Epic's server.">UEFN{revision ? ` · v${revision}` : ""}</Pill>;
  }
  if (!project.onServer && !session) return <Pill tone="muted">On this PC</Pill>;
  if (!project.onServer) return <Pill tone="warn"><UploadCloud size={12} /> Not on server</Pill>;
  if (!project.local) return <Pill tone="muted"><CloudOff size={12} /> Not on this PC</Pill>;
  if (status === undefined) return <Pill tone="muted"><Spinner size={11} /> Checking</Pill>;
  if (!status) return <Pill tone="muted">Unknown</Pill>;
  const behind = status.remoteRevision - status.localRevision;
  if (behind > 0) return <Pill tone="warn" title={`You have revision ${status.localRevision}, the server has ${status.remoteRevision}`}><ArrowDownToLine size={12} /> v{status.localRevision} of v{status.remoteRevision}</Pill>;
  if (behind < 0) return <Pill tone="info" title="Versions on this PC that are not on the server yet"><ArrowUpFromLine size={12} /> {-behind} to send</Pill>;
  return <Pill tone="good" title={`Revision ${status.remoteRevision}, you have the latest`}><Check size={12} /> v{status.remoteRevision}</Pill>;
}

/** What the project is made with, for the line under its name. */
export const madeWith = (project: Project, engine: string | null) =>
  project.uefn ? `UEFN${project.uefn.version ? ` ${project.uefn.version}` : ""}` : isUefnBackup(project.name) ? "UEFN" : engine ? `Unreal ${engine}` : project.local && !project.local.uproject ? "Not an Unreal project" : "Engine unknown";

/** Small overlapping avatars of everybody who saved a version. */
export function AuthorStack({ path, fallback }: { path: string | undefined; fallback?: string[] }) {
  const local = useAuthors(path);
  const authors = local && local.length > 0 ? local : fallback;
  if (!authors || authors.length === 0) return null;
  const shown = authors.slice(0, 4);
  return (
    <span className="avatar-stack" title={`${authors.length} ${authors.length === 1 ? "person has" : "people have"} saved versions: ${authors.join(", ")}`}>
      {shown.map((name) => <Avatar key={name} name={name} size="small" />)}
      {authors.length > shown.length && <span className="avatar small more">+{authors.length - shown.length}</span>}
    </span>
  );
}

export function CategoryTag({ project }: { project: Project }) {
  const { settings, categories } = useStore();
  const category = project.id ? categories.find((c) => c.id === settings.assignments[project.id!]) : undefined;
  if (!category) return null;
  return <span className="cat-tag"><span className="cat-dot" style={{ background: category.color }} />{category.name}</span>;
}

function ProjectCard({ project, onOpen }: { project: Project; onOpen: () => void }) {
  const { settings, update, go } = useStore();
  const { busy, openProject, clone, sync } = useProjectActions();
  const view = useProjectView(project);
  const menu = useMenu();
  const favorite = !!project.id && settings.favorites.includes(project.id);
  const working = busy[project.key];
  const behind = view.status ? view.status.remoteRevision - view.status.localRevision : 0;
  const details = [madeWith(project, view.engine), view.bytes > 0 ? formatBytes(view.bytes) : "", view.last ? `${view.last.author}, ${timeAgo(view.last.date)}` : ""].filter(Boolean).join(" · ");

  const toggleFavorite = (event: React.MouseEvent) => {
    event.stopPropagation();
    if (!project.id) return;
    update({ favorites: favorite ? settings.favorites.filter((f) => f !== project.id) : [...settings.favorites, project.id] });
  };

  return (
    <div
      className="project-card"
      draggable={!!project.id}
      onDragStart={(e) => {
        if (!project.id) return;
        e.dataTransfer.setData(DRAG_TYPE, project.id);
        e.dataTransfer.effectAllowed = "move";
      }}
      onClick={onOpen}
      onContextMenu={(e) =>
        menu.open(e, [
          { label: "Open details", onClick: onOpen },
          ...(project.local ? [{ label: "Show in Explorer", onClick: () => api.openPath(project.local!.path) }] : []),
          ...(project.uefn ? [{ label: "Show in Explorer", onClick: () => api.openPath(project.uefn!.path) }, { label: "Remove from Lore+", onClick: () => update({ uefnAdded: (settings.uefnAdded ?? []).filter((p) => p.toLowerCase() !== project.uefn!.path.toLowerCase()) }) }] : []),
          { label: favorite ? "Remove from favorites" : "Add to favorites", onClick: () => toggleFavorite(e) },
          ...(project.id && settings.assignments[project.id] ? [{ label: "Take out of its category", onClick: () => { const next = { ...settings.assignments }; delete next[project.id!]; update({ assignments: next }); } }] : []),
        ])
      }
    >
      <Thumb name={view.name} image={view.thumbnail}>
        <div className="overlay" />
        <div className="people"><AuthorStack path={project.local?.path} fallback={project.remote?.authors} /></div>
        <div className="badges">
          {isUefnBackup(project.name) && <Pill tone="accent" title="A backup of a UEFN project, kept on this server">UEFN backup</Pill>}
          <StatusPill project={project} status={view.status} />
          {view.status && view.status.changes > 0 && <Pill tone="muted" title={`${view.status.changes} files on this PC have changes that are not saved as a version yet`}><FilePen size={12} /> {view.status.changes}</Pill>}
          <CategoryTag project={project} />
        </div>
        {project.id && (
          <button className="btn icon small ghost fav" onClick={toggleFavorite} title="Favorite" style={{ background: "rgba(10,12,18,0.55)", color: favorite ? "var(--warn)" : "#fff" }}>
            <Star size={15} fill={favorite ? "currentColor" : "none"} />
          </button>
        )}
      </Thumb>
      <div className="card-body">
        <div className="card-text">
          <div className="card-title" title={view.name}>{view.name}</div>
          <div className="card-meta small" title={details}>{details}</div>
        </div>
        <div className="card-actions" onClick={(e) => e.stopPropagation()}>
          {project.local || project.uefn ? (
            <>
              {behind > 0 && (
                <button className="btn small" title="Sync" disabled={!!working} onClick={() => sync(project)}>
                  {working ? <Spinner size={14} /> : <Download size={14} />}<span className="label">Sync</span>
                </button>
              )}
              <button className="btn small icon" title="Open the project folder" onClick={() => api.openPath((project.local ?? project.uefn)!.path)}><FolderOpen size={15} /></button>
              <button className="btn primary small" title={project.uefn ? "Open in UEFN" : "Open"} onClick={() => openProject(project)}><Play size={14} /><span className="label">Open</span></button>
            </>
          ) : project.onServer ? (
            <button className="btn primary small" title="Clone to this PC" disabled={!!working} onClick={() => clone(project)}>
              {working ? <Spinner size={14} /> : <Download size={14} />}<span className="label">{working ?? "Clone"}</span>
            </button>
          ) : null}
        </div>
      </div>
      {menu.element}
    </div>
  );
}

function ListRow({ project, onOpen }: { project: Project; onOpen: () => void }) {
  const { busy, openProject, clone } = useProjectActions();
  const view = useProjectView(project);
  const working = busy[project.key];
  return (
    <div className="list-row" draggable={!!project.id} onDragStart={(e) => { if (project.id) { e.dataTransfer.setData(DRAG_TYPE, project.id); e.dataTransfer.effectAllowed = "move"; } }} onClick={onOpen}>
      <Thumb name={view.name} image={view.thumbnail} className="list-thumb" />
      <div style={{ minWidth: 0 }}>
        <div className="title-row">
          <div className="card-title">{view.name}</div>
          <CategoryTag project={project} />
        </div>
        <div className="card-meta small" style={{ marginTop: 2 }}>{madeWith(project, view.engine)}{view.bytes > 0 ? ` · ${formatBytes(view.bytes)}` : ""}</div>
      </div>
      <div className="row" style={{ gap: 6 }}><StatusPill project={project} status={view.status} /></div>
      <div className="muted" style={{ fontSize: 13, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>
        {view.last ? `${view.last.author}, ${timeAgo(view.last.date)}` : ""}
      </div>
      <div><AuthorStack path={project.local?.path} fallback={project.remote?.authors} /></div>
      <div onClick={(e) => e.stopPropagation()}>
        {project.local ? (
          <div className="row" style={{ gap: 10 }}>
            <button className="btn small icon" title="Open the project folder" onClick={() => api.openPath(project.local!.path)}><FolderOpen size={15} /></button>
            <button className="btn primary small" onClick={() => openProject(project)}><Play size={14} /> Open</button>
          </div>
        ) : project.uefn ? (
          <div className="row" style={{ gap: 10 }}>
            <button className="btn small icon" title="Open the project folder" onClick={() => api.openPath(project.uefn!.path)}><FolderOpen size={15} /></button>
            <button className="btn primary small" title="Open in UEFN" onClick={() => openProject(project)}><Play size={14} /> Open</button>
          </div>
        ) : project.onServer ? (
          <button className="btn small" disabled={!!working} onClick={() => clone(project)}>{working ? <Spinner size={14} /> : <Download size={14} />} Clone</button>
        ) : null}
      </div>
    </div>
  );
}

export function Projects() {
  const { projects, settings, update, loading, reload, go, server, session, remoteError, category, setCategory, categories } = useStore();
  const [query, setQuery] = useState("");
  const [filter, setFilter] = useState<Filter>("all");
  const [signIn, setSignIn] = useState(false);
  const [adding, setAdding] = useState(false);
  const [engine, setEngine] = useState("");
  useCacheVersion();
  const grid = settings.view === "grid";
  const size = grid ? settings.size.grid : settings.size.list;
  const [min, max] = grid ? [180, 420] : [36, 110];
  const setSize = (value: number) => {
    const next = Math.max(min, Math.min(max, Math.round(value)));
    update({ size: { ...settings.size, [grid ? "grid" : "list"]: next } });
  };
  const sizeRef = useRef({ size, setSize });
  sizeRef.current = { size, setSize };
  useEffect(() => {
    const onWheel = (event: WheelEvent) => {
      if (!event.ctrlKey) return;
      event.preventDefault();
      sizeRef.current.setSize(sizeRef.current.size + (event.deltaY < 0 ? 1 : -1) * (grid ? 20 : 6));
    };
    window.addEventListener("wheel", onWheel, { passive: false });
    return () => window.removeEventListener("wheel", onWheel);
  }, [grid]);

  const categoryIds = useMemo(() => {
    if (category === "all" || category === "uefn") return null;
    const ids = new Set<string>([category]);
    let grew = true;
    while (grew) {
      grew = false;
      for (const c of categories) {
        if (c.parentId && ids.has(c.parentId) && !ids.has(c.id)) {
          ids.add(c.id);
          grew = true;
        }
      }
    }
    return ids;
  }, [category, categories]);

  const engineOf = (p: Project) => (p.uefn || isUefnBackup(p.name) ? "UEFN" : (p.id && settings.overrides[p.id]?.engine) || p.local?.engine || p.remote?.engine ? `Unreal ${(p.id && settings.overrides[p.id]?.engine) || p.local?.engine || p.remote?.engine}` : "Unknown");
  const engineNames = [...new Set(projects.map(engineOf))].sort();

  const shown = projects.filter((p) => {
    if (query && !p.name.toLowerCase().includes(query.toLowerCase())) return false;
    if (filter === "local" && !p.local && !p.uefn) return false;
    if (filter === "remote" && (p.local || p.uefn)) return false;
    if (filter === "unlinked" && (p.onServer || p.uefn)) return false;
    if (category === "uefn" && !p.uefn && !isUefnBackup(p.name)) return false;
    if (engine && engineOf(p) !== engine) return false;
    if (categoryIds) {
      const assigned = p.id ? settings.assignments[p.id] : undefined;
      if (!assigned || !categoryIds.has(assigned)) return false;
    }
    return true;
  });

  const sorted = [...shown].sort((a, b) => {
    const when = (p: Project) => Date.parse(peekRevision(p.local?.path)?.date ?? p.uefn?.revisions?.[0]?.date ?? "") || 0;
    const opened = (p: Project) => (p.id ? settings.opened[p.id] ?? 0 : 0);
    const count = (p: Project) => peekStatus(p.local?.path)?.remoteRevision ?? p.uefn?.revisions?.[0]?.number ?? 0;
    switch (settings.sort) {
      case "opened": return opened(b) - opened(a) || a.name.localeCompare(b.name);
      case "updated": return when(b) - when(a) || a.name.localeCompare(b.name);
      case "revisions": return count(b) - count(a) || a.name.localeCompare(b.name);
      default: return a.name.localeCompare(b.name);
    }
  });

  // Sections of the list. Folded ones are remembered, per way of grouping.
  const groupBy = category === "all" ? settings.groupBy : "none";
  const groupOf = (p: Project) => {
    if (groupBy === "engine") return engineOf(p);
    return categories.find((c) => p.id && c.id === settings.assignments[p.id])?.name ?? "No category";
  };
  const groups = groupBy === "none" ? [] : [...new Set(sorted.map(groupOf))].sort((a, b) => Number(a === "No category") - Number(b === "No category") || a.localeCompare(b));
  const toggleGroup = (key: string) => update({ collapsed: settings.collapsed.includes(key) ? settings.collapsed.filter((c) => c !== key) : [...settings.collapsed, key] });
  const items = (list: Project[]) => settings.view === "grid"
    ? <div className="grid">{list.map((p) => <ProjectCard key={p.key} project={p} onOpen={() => go("project", p.key)} />)}</div>
    : <div className="list">{list.map((p) => <ListRow key={p.key} project={p} onOpen={() => go("project", p.key)} />)}</div>;

  const title = category === "uefn" ? "UEFN" : category === "all" ? "Projects" : categories.find((c) => c.id === category)?.name ?? "Projects";
  const chip = (id: Filter, label: string) => (
    <button key={id} className={`btn small ${filter === id ? "primary" : "ghost"}`} onClick={() => setFilter(id)}>{label}</button>
  );

  return (
    <>
      <div className="topbar">
        <div>
          <h1>{title}</h1>
          <div className="sub">{server ? `${server.label} · ${projects.filter((p) => p.onServer).length} on the server, ${projects.filter((p) => p.local || p.uefn).length} on this PC` : "No server yet"}</div>
        </div>
        <div className="spacer" />
        <label className="search"><Search size={16} /><input placeholder="Search projects" value={query} onChange={(e) => setQuery(e.target.value)} /></label>
        {category === "all" && (
          <select className="select sort" value={settings.groupBy} onChange={(e) => update({ groupBy: e.target.value as GroupKey })} title="Group">
            <option value="none">No groups</option>
            <option value="category">By category</option>
            <option value="engine">By engine</option>
          </select>
        )}
        <select className="select sort" value={settings.sort} onChange={(e) => update({ sort: e.target.value as SortKey })} title="Sort">
          <option value="name">Name</option>
          <option value="opened">Recently opened</option>
          <option value="updated">Recently updated</option>
          <option value="revisions">Most revisions</option>
        </select>
        <button className="btn icon" title="Refresh" onClick={() => { invalidateStatus(); reload(); }}>{loading ? <Spinner /> : <RefreshCw size={16} />}</button>
        <div className="row gap-s" style={{ gap: 2 }}>
          <button className={`btn icon ${settings.view === "grid" ? "" : "ghost"}`} onClick={() => update({ view: "grid" })} title="Grid"><Grid2x2 size={16} /></button>
          <button className={`btn icon ${settings.view === "list" ? "" : "ghost"}`} onClick={() => update({ view: "list" })} title="List"><List size={16} /></button>
        </div>
        <button className="btn primary" onClick={() => setAdding(true)}><FolderPlus size={16} /> Add project</button>
      </div>
      <div className="content" style={{ ["--card-min" as string]: `${settings.size.grid}px`, ["--list-thumb" as string]: `${settings.size.list}px` }}>
        <div className="row wrap" style={{ gap: 6, marginBottom: 18 }}>
          {chip("all", "All")}
          {chip("local", "On this PC")}
          {chip("remote", "Not on this PC")}
          {chip("unlinked", "Not on server")}
          {engineNames.length > 1 && (
            <select className="select sort" style={{ height: 30 }} value={engine} onChange={(e) => setEngine(e.target.value)} title="Engine">
              <option value="">All engines</option>
              {engineNames.map((name) => <option key={name} value={name}>{name}</option>)}
            </select>
          )}
        </div>

        {server && !session && (
          <div className="banner warn">
            <ServerIcon size={18} />
            <div style={{ flex: 1 }}>Sign in to <b>{server.label}</b> to see its projects and sync them.</div>
            <button className="btn primary small" onClick={() => setSignIn(true)}>Sign in</button>
          </div>
        )}
        {session && remoteError && <div className="banner bad">{remoteError}</div>}

        {loading && projects.length === 0 ? (
          <div className="grid">{[0, 1, 2, 3].map((i) => <div key={i} className="skeleton" style={{ height: 300 }} />)}</div>
        ) : shown.length === 0 ? (
          <Empty icon={<Boxes size={28} />} title={category === "uefn" ? "No UEFN projects added" : projects.length === 0 ? "No projects yet" : "Nothing matches"}>
            {category === "uefn" ? "Use Add project to choose which UEFN projects are shown here." : projects.length === 0 ? "Sign in to a server, or add a project from this PC." : "Try another filter or search."}
          </Empty>
        ) : groupBy === "none" ? items(sorted) : groups.map((name) => {
          const list = sorted.filter((p) => groupOf(p) === name);
          const key = `${groupBy}:${name}`;
          const folded = settings.collapsed.includes(key);
          return (
            <section key={key} className="group">
              <button className="group-head" onClick={() => toggleGroup(key)}>
                {folded ? <ChevronRight size={16} /> : <ChevronDown size={16} />} {name} <span className="count">{list.length}</span>
              </button>
              {!folded && items(list)}
            </section>
          );
        })}
      </div>
      <div className="size-float" title="Size (also Ctrl + scroll)">
        <Grid2x2 size={13} />
        <input className="size-slider" type="range" min={min} max={max} value={size} onChange={(e) => setSize(Number(e.target.value))} />
        <Grid2x2 size={18} />
      </div>
      {signIn && server && <SignInModal server={server} onClose={() => setSignIn(false)} />}
      {adding && <AddProjectModal start={category === "uefn" ? "uefn" : "upload"} onClose={() => setAdding(false)} />}
    </>
  );
}

export { open };

