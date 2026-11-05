import { useMemo, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { ArrowDownToLine, ArrowUpFromLine, Boxes, Check, CloudOff, Download, FilePen, FolderOpen, FolderPlus, Grid2x2, List, Play, RefreshCw, Search, Server as ServerIcon, Star, UploadCloud } from "lucide-react";
import { Avatar, Empty, Pill, Spinner, Thumb, useMenu } from "../components/ui";
import { firstLine } from "../lib/parse";
import { timeAgo } from "../lib/format";
import { peekRevision, peekStatus, useAuthors, useCacheVersion, useLastRevision, useProjectActions, useStatus } from "../lib/actions";
import { useStore } from "../lib/store";
import { api } from "../lib/api";
import type { Project, SortKey } from "../lib/types";
import { SignInModal } from "../components/Servers";
import { AddProjectModal } from "../components/AddProject";

type Filter = "all" | "local" | "remote" | "unlinked";

export const DRAG_TYPE = "application/x-lore-project";

export function useProjectView(project: Project) {
  const { settings, server } = useStore();
  const override = project.id ? settings.overrides[project.id] : undefined;
  const status = useStatus(project.local?.path, server?.id, 0);
  const last = useLastRevision(project.local?.path, 0);
  const name = override?.displayName || project.name;
  const engine = override?.engine || project.local?.engine || null;
  const thumbnail = override?.thumbnail || project.local?.thumbnail || null;
  return { name, engine, thumbnail, status, last, override };
}

export function StatusPill({ project, status }: { project: Project; status: ReturnType<typeof useStatus> }) {
  const { session } = useStore();
  if (!project.onServer && !session) return <Pill tone="muted">On this PC</Pill>;
  if (!project.onServer) return <Pill tone="warn"><UploadCloud size={12} /> Not on server</Pill>;
  if (!project.local) return <Pill tone="muted"><CloudOff size={12} /> Not on this PC</Pill>;
  if (status === undefined) return <Pill tone="muted"><Spinner size={11} /> Checking</Pill>;
  if (!status) return <Pill tone="muted">Unknown</Pill>;
  const behind = status.remoteRevision - status.localRevision;
  if (behind > 0) return <Pill tone="warn" title={`You have revision ${status.localRevision}, the server has ${status.remoteRevision}`}><ArrowDownToLine size={12} /> Revision {status.localRevision} of {status.remoteRevision}</Pill>;
  if (behind < 0) return <Pill tone="info" title="Versions on this PC that are not on the server yet"><ArrowUpFromLine size={12} /> {-behind} to send</Pill>;
  return <Pill tone="good" title="You have the latest revision"><Check size={12} /> Revision {status.remoteRevision} · latest</Pill>;
}

/** Small overlapping avatars of everybody who saved a version. */
export function AuthorStack({ path }: { path: string | undefined }) {
  const authors = useAuthors(path);
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
          { label: favorite ? "Remove from favorites" : "Add to favorites", onClick: () => toggleFavorite(e) },
          ...(project.id && settings.assignments[project.id] ? [{ label: "Take out of its category", onClick: () => { const next = { ...settings.assignments }; delete next[project.id!]; update({ assignments: next }); } }] : []),
        ])
      }
    >
      <Thumb name={view.name} image={view.thumbnail}>
        <div className="overlay" />
        <div className="badges">
          <StatusPill project={project} status={view.status} />
          {view.status && view.status.changes > 0 && <Pill tone="muted" title={`${view.status.changes} files on this PC have changes that are not saved as a version yet`}><FilePen size={12} /> {view.status.changes}</Pill>}
        </div>
        {project.id && (
          <button className="btn icon small ghost fav" onClick={toggleFavorite} title="Favorite" style={{ background: "rgba(10,12,18,0.55)", color: favorite ? "var(--warn)" : "#fff" }}>
            <Star size={15} fill={favorite ? "currentColor" : "none"} />
          </button>
        )}
      </Thumb>
      <div className="card-body">
        <div>
          <div className="title-row">
            <div className="card-title">{view.name}</div>
            <AuthorStack path={project.local?.path} />
            <span className="spacer" />
            <CategoryTag project={project} />
          </div>
          <div className="card-meta small" style={{ marginTop: 2 }}>
            {view.engine ? <span>Unreal {view.engine}</span> : <span className="faint">Engine unknown</span>}
            {view.last && (
              <>
                <span className="dotsep" />
                <span>{view.last.author}, {timeAgo(view.last.date)}</span>
              </>
            )}
          </div>
        </div>
        <div className="card-meta" style={{ minHeight: 36, alignItems: "flex-start" }}>
          {view.last ? <span style={{ color: "#bfc6d8", display: "-webkit-box", WebkitLineClamp: 2, WebkitBoxOrient: "vertical", overflow: "hidden" }}>{firstLine(view.last.description)}</span> : <span className="faint">{project.local ? "No history yet" : "Clone to see the history"}</span>}
        </div>
        <div className="card-actions" onClick={(e) => e.stopPropagation()}>
          {project.local ? (
            <>
              <button className="btn small icon" title="Open the project folder" onClick={() => api.openPath(project.local!.path)}><FolderOpen size={15} /></button>
              <button className="btn primary small" onClick={() => openProject(project)}><Play size={14} /> Open</button>
              {behind > 0 && (
                <button className="btn small" disabled={!!working} onClick={() => sync(project)}>
                  {working ? <Spinner size={14} /> : <Download size={14} />} Sync
                </button>
              )}
            </>
          ) : project.onServer ? (
            <button className="btn primary small" disabled={!!working} onClick={() => clone(project)}>
              {working ? <Spinner size={14} /> : <Download size={14} />} {working ?? "Clone to this PC"}
            </button>
          ) : null}
          <button className="btn small ghost" onClick={() => go("project", project.key)}>Details</button>
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
          <AuthorStack path={project.local?.path} />
          <CategoryTag project={project} />
        </div>
      </div>
      <div className="muted">{view.engine ? `UE ${view.engine}` : "-"}</div>
      <div><StatusPill project={project} status={view.status} /></div>
      <div className="muted" style={{ fontSize: 13, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>
        {view.last ? `${view.last.author}, ${timeAgo(view.last.date)}` : ""}
      </div>
      <div onClick={(e) => e.stopPropagation()}>
        {project.local ? (
          <div className="row" style={{ gap: 10 }}>
            <button className="btn small icon" title="Open the project folder" onClick={() => api.openPath(project.local!.path)}><FolderOpen size={15} /></button>
            <button className="btn primary small" onClick={() => openProject(project)}><Play size={14} /> Open</button>
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
  useCacheVersion();

  const categoryIds = useMemo(() => {
    if (category === "all") return null;
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

  const shown = projects.filter((p) => {
    if (query && !p.name.toLowerCase().includes(query.toLowerCase())) return false;
    if (filter === "local" && !p.local) return false;
    if (filter === "remote" && p.local) return false;
    if (filter === "unlinked" && p.onServer) return false;
    if (categoryIds) {
      const assigned = p.id ? settings.assignments[p.id] : undefined;
      if (!assigned || !categoryIds.has(assigned)) return false;
    }
    return true;
  });

  const sorted = [...shown].sort((a, b) => {
    const when = (p: Project) => Date.parse(peekRevision(p.local?.path)?.date ?? "") || 0;
    const opened = (p: Project) => (p.id ? settings.opened[p.id] ?? 0 : 0);
    const count = (p: Project) => peekStatus(p.local?.path)?.remoteRevision ?? 0;
    switch (settings.sort) {
      case "opened": return opened(b) - opened(a) || a.name.localeCompare(b.name);
      case "updated": return when(b) - when(a) || a.name.localeCompare(b.name);
      case "revisions": return count(b) - count(a) || a.name.localeCompare(b.name);
      default: return a.name.localeCompare(b.name);
    }
  });

  const title = category === "all" ? "Projects" : categories.find((c) => c.id === category)?.name ?? "Projects";
  const chip = (id: Filter, label: string) => (
    <button key={id} className={`btn small ${filter === id ? "primary" : "ghost"}`} onClick={() => setFilter(id)}>{label}</button>
  );

  return (
    <>
      <div className="topbar">
        <div>
          <h1>{title}</h1>
          <div className="sub">{server ? `${server.label} · ${projects.filter((p) => p.onServer).length} on the server, ${projects.filter((p) => p.local).length} on this PC` : "No server yet"}</div>
        </div>
        <div className="spacer" />
        <label className="search"><Search size={16} /><input placeholder="Search projects" value={query} onChange={(e) => setQuery(e.target.value)} /></label>
        <select className="select sort" value={settings.sort} onChange={(e) => update({ sort: e.target.value as SortKey })} title="Sort">
          <option value="name">Name</option>
          <option value="opened">Recently opened</option>
          <option value="updated">Recently updated</option>
          <option value="revisions">Most revisions</option>
        </select>
        <button className="btn icon" title="Refresh" onClick={reload}>{loading ? <Spinner /> : <RefreshCw size={16} />}</button>
        <div className="row gap-s" style={{ gap: 2 }}>
          <button className={`btn icon ${settings.view === "grid" ? "" : "ghost"}`} onClick={() => update({ view: "grid" })} title="Grid"><Grid2x2 size={16} /></button>
          <button className={`btn icon ${settings.view === "list" ? "" : "ghost"}`} onClick={() => update({ view: "list" })} title="List"><List size={16} /></button>
        </div>
        <button className="btn primary" onClick={() => setAdding(true)}><FolderPlus size={16} /> Add project</button>
      </div>
      <div className="content">
        <div className="row wrap" style={{ gap: 6, marginBottom: 18 }}>
          {chip("all", "All")}
          {chip("local", "On this PC")}
          {chip("remote", "Not on this PC")}
          {chip("unlinked", "Not on server")}
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
          <Empty icon={<Boxes size={28} />} title={projects.length === 0 ? "No projects yet" : "Nothing matches"}>
            {projects.length === 0 ? "Sign in to a server, or add a project from this PC." : "Try another filter or search."}
          </Empty>
        ) : settings.view === "grid" ? (
          <div className="grid">{sorted.map((p) => <ProjectCard key={p.key} project={p} onOpen={() => go("project", p.key)} />)}</div>
        ) : (
          <div className="list">{sorted.map((p) => <ListRow key={p.key} project={p} onOpen={() => go("project", p.key)} />)}</div>
        )}
      </div>
      {signIn && server && <SignInModal server={server} onClose={() => setSignIn(false)} />}
      {adding && <AddProjectModal onClose={() => setAdding(false)} />}
    </>
  );
}

export { open };

