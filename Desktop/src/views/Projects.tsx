import { useMemo, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { ArrowDownToLine, Boxes, CloudOff, Download, FolderPlus, Grid2x2, Heart, List, Play, RefreshCw, Search, Server as ServerIcon, Star, UploadCloud } from "lucide-react";
import { Empty, Pill, Spinner, Thumb, useMenu } from "../components/ui";
import { firstLine } from "../lib/parse";
import { timeAgo } from "../lib/format";
import { useLastRevision, useProjectActions, useStatus } from "../lib/actions";
import { useStore } from "../lib/store";
import type { Project } from "../lib/types";
import { SignInModal } from "../components/Servers";
import { AddProjectModal } from "../components/AddProject";

type Filter = "all" | "local" | "remote" | "favorites" | "unlinked";

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
  if (!project.onServer) return <Pill tone="warn"><UploadCloud size={12} /> Not on server</Pill>;
  if (!project.local) return <Pill tone="muted"><CloudOff size={12} /> Not on this PC</Pill>;
  if (status === undefined) return <Pill tone="muted"><Spinner size={11} /> Checking</Pill>;
  if (!status) return <Pill tone="muted">Unknown</Pill>;
  const behind = status.remoteRevision - status.localRevision;
  if (behind > 0) return <Pill tone="warn"><ArrowDownToLine size={12} /> {behind} behind</Pill>;
  if (behind < 0) return <Pill tone="info">{-behind} to push</Pill>;
  if (status.changes > 0) return <Pill tone="info">{status.changes} changed</Pill>;
  return <Pill tone="good">Up to date</Pill>;
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
      onClick={onOpen}
      onContextMenu={(e) =>
        menu.open(e, [
          { label: "Open details", onClick: onOpen },
          ...(project.local ? [{ label: "Show in Explorer", onClick: () => openPath(project.local!.path) }] : []),
          { label: favorite ? "Remove from favorites" : "Add to favorites", onClick: () => toggleFavorite(e) },
        ])
      }
    >
      <Thumb name={view.name} image={view.thumbnail}>
        <div className="overlay" />
        <div className="badges">
          <StatusPill project={project} status={view.status} />
        </div>
        {project.id && (
          <button className="btn icon small ghost fav" onClick={toggleFavorite} title="Favorite" style={{ background: "rgba(10,12,18,0.55)", color: favorite ? "var(--warn)" : "#fff" }}>
            <Star size={15} fill={favorite ? "currentColor" : "none"} />
          </button>
        )}
      </Thumb>
      <div className="card-body">
        <div>
          <div className="card-title">{view.name}</div>
          <div className="card-meta" style={{ marginTop: 3 }}>
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

function openPath(path: string) {
  import("../lib/api").then(({ api }) => api.openPath(path));
}

function ListRow({ project, onOpen }: { project: Project; onOpen: () => void }) {
  const { busy, openProject, clone } = useProjectActions();
  const view = useProjectView(project);
  const working = busy[project.key];
  return (
    <div className="list-row" onClick={onOpen}>
      <Thumb name={view.name} image={view.thumbnail} className="list-thumb" />
      <div style={{ minWidth: 0 }}>
        <div className="card-title">{view.name}</div>
        <div className="muted" style={{ fontSize: 12.5 }}>{project.local?.path ?? "Only on the server"}</div>
      </div>
      <div className="muted">{view.engine ? `UE ${view.engine}` : "-"}</div>
      <div><StatusPill project={project} status={view.status} /></div>
      <div className="muted" style={{ fontSize: 13, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>
        {view.last ? `${view.last.author}, ${timeAgo(view.last.date)}` : ""}
      </div>
      <div onClick={(e) => e.stopPropagation()}>
        {project.local ? (
          <button className="btn primary small" onClick={() => openProject(project)}><Play size={14} /> Open</button>
        ) : project.onServer ? (
          <button className="btn small" disabled={!!working} onClick={() => clone(project)}>{working ? <Spinner size={14} /> : <Download size={14} />} Clone</button>
        ) : null}
      </div>
    </div>
  );
}

export function Projects() {
  const { projects, settings, update, loading, reload, go, server, session, remoteError, category, setCategory } = useStore();
  const [query, setQuery] = useState("");
  const [filter, setFilter] = useState<Filter>("all");
  const [signIn, setSignIn] = useState(false);
  const [adding, setAdding] = useState(false);

  const categoryIds = useMemo(() => {
    if (category === "all") return null;
    const ids = new Set<string>([category]);
    let grew = true;
    while (grew) {
      grew = false;
      for (const c of settings.categories) {
        if (c.parentId && ids.has(c.parentId) && !ids.has(c.id)) {
          ids.add(c.id);
          grew = true;
        }
      }
    }
    return ids;
  }, [category, settings.categories]);

  const shown = projects.filter((p) => {
    if (query && !p.name.toLowerCase().includes(query.toLowerCase())) return false;
    if (filter === "local" && !p.local) return false;
    if (filter === "remote" && p.local) return false;
    if (filter === "unlinked" && p.onServer) return false;
    if (filter === "favorites" && !(p.id && settings.favorites.includes(p.id))) return false;
    if (categoryIds) {
      const assigned = p.id ? settings.assignments[p.id] : undefined;
      if (!assigned || !categoryIds.has(assigned)) return false;
    }
    return true;
  });

  const title = category === "all" ? "Projects" : settings.categories.find((c) => c.id === category)?.name ?? "Projects";
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
          {chip("favorites", "Favorites")}
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
          <Empty icon={filter === "favorites" ? <Heart size={28} /> : <Boxes size={28} />} title={projects.length === 0 ? "No projects yet" : "Nothing matches"}>
            {projects.length === 0 ? "Sign in to a server, or add a project from this PC." : "Try another filter or search."}
          </Empty>
        ) : settings.view === "grid" ? (
          <div className="grid">{shown.map((p) => <ProjectCard key={p.key} project={p} onOpen={() => go("project", p.key)} />)}</div>
        ) : (
          <div className="list">{shown.map((p) => <ListRow key={p.key} project={p} onOpen={() => go("project", p.key)} />)}</div>
        )}
      </div>
      {signIn && server && <SignInModal server={server} onClose={() => setSignIn(false)} />}
      {adding && <AddProjectModal onClose={() => setAdding(false)} />}
    </>
  );
}

export { open };

