import { useState } from "react";
import { Box, ChevronsUpDown, LayoutGrid, Play, Plus, RefreshCw, Settings as SettingsIcon, ShieldCheck, Star, Users } from "lucide-react";
import { api } from "../lib/api";
import { useProjectActions } from "../lib/actions";
import { useStore, type View } from "../lib/store";
import { useUpdates } from "../lib/updates";
import { isUefnBackup } from "../lib/uefn";
import { readableOn } from "../lib/format";
import { CategoryAccessModal, CategoryModal } from "./Categories";
import { ServerModal, SignInModal } from "./Servers";
import { Logo } from "./Logo";
import { useMenu } from "./ui";
import { DRAG_TYPE } from "../views/Projects";
import type { Category } from "../lib/types";

export function Sidebar() {
  const { settings, update, server, session, view, go, category, setCategory, projects, uefns, isAdmin, setServer, categories, sharedActive, projectKey } = useStore();
  const [over, setOver] = useState<string | null>(null);
  const { openProject } = useProjectActions();
  const canEdit = !sharedActive || isAdmin;
  const updates = useUpdates();
  const menu = useMenu();
  const [editing, setEditing] = useState<{ category?: Category; parentId?: string } | null>(null);
  const [access, setAccess] = useState<Category | null>(null);
  const [addServer, setAddServer] = useState(false);
  const [signIn, setSignIn] = useState(false);

  const nav = (id: View, label: string, icon: React.ReactNode, extra?: React.ReactNode) => (
    <button className={`nav-item ${view === id || (id === "projects" && view === "project" && category === "all") ? "active" : ""}`} onClick={() => go(id)}>
      {icon}
      {label}
      {extra}
    </button>
  );

  const countIn = (id: string) => projects.filter((p) => p.id && settings.assignments[p.id] === id).length;
  const roots = categories.filter((c) => !c.parentId);

  /** Dropping a project on a category (or on "All projects" to take it out of its category). */
  const dropTarget = (target: string | null) => ({
    onDragOver: (e: React.DragEvent) => {
      if (!canEdit || !e.dataTransfer.types.includes(DRAG_TYPE)) return;
      e.preventDefault();
      e.dataTransfer.dropEffect = "move";
      setOver(target ?? "all");
    },
    onDragLeave: () => setOver(null),
    onDrop: (e: React.DragEvent) => {
      const id = e.dataTransfer.getData(DRAG_TYPE);
      setOver(null);
      if (!id || !canEdit) return;
      e.preventDefault();
      const next = { ...settings.assignments };
      if (target) next[id] = target;
      else delete next[id];
      update({ assignments: next });
    },
  });

  const favorites = projects.filter((p) => p.id && settings.favorites.includes(p.id));

  const renderCategory = (c: Category, depth: number): React.ReactNode => (
    <div key={c.id}>
      <button
        {...dropTarget(c.id)}
        className={`nav-item ${over === c.id ? "drop" : ""} ${category === c.id && (view === "projects" || view === "project") ? "active" : ""}`}
        style={{ paddingLeft: 12 + depth * 16 }}
        onClick={() => {
          setCategory(c.id);
          go("projects");
        }}
        onContextMenu={(e) =>
          canEdit && menu.open(e, [
            { label: "Edit category", onClick: () => setEditing({ category: c }) },
            { label: "New category inside", onClick: () => setEditing({ parentId: c.id }) },
            { label: "Who has access", icon: <Users size={15} />, onClick: () => setAccess(c) },
          ])
        }
      >
        <span className="cat-row"><span className="cat-dot" style={{ background: c.color }} />{c.name}</span>
        <span className="count">{countIn(c.id) || ""}</span>
      </button>
      {categories.filter((child) => child.parentId === c.id).map((child) => renderCategory(child, depth + 1))}
    </div>
  );

  return (
    <aside className="sidebar">
      <div className="brand">
        <Logo size={34} />
        <div><div className="brand-name">Lore<span className="plus">+</span></div><div className="brand-sub">Revision Control</div></div>
      </div>

      <button
        className="server-switch"
        onClick={(e) => {
          const rect = e.currentTarget.getBoundingClientRect();
          menu.open({ ...e, clientX: rect.left, clientY: rect.bottom + 6, preventDefault: () => {}, stopPropagation: () => {} } as React.MouseEvent, [
            ...settings.servers.map((s) => ({ label: s.label, icon: <span className={`server-dot ${s.id === server?.id ? "on" : ""}`} />, onClick: () => setServer(s.id) })),
            { label: "", divider: true, onClick: () => {} },
            ...(server && !session ? [{ label: `Sign in to ${server.label}`, icon: <Users size={15} />, onClick: () => setSignIn(true) }] : []),
            { label: "Add a server", icon: <Plus size={15} />, onClick: () => setAddServer(true) },
          ]);
        }}
      >
        <span className={`server-dot ${session ? "on" : ""}`} />
        <span className="server-text">
          <div className="server-label">{server?.label ?? "No server"}</div>
          <div className="server-user">{session ? session.userName : "Not signed in"}</div>
        </span>
        <ChevronsUpDown size={15} color="var(--faint)" />
      </button>

      <div className="nav-scroll">
        {nav("projects", "Projects", <LayoutGrid size={18} />, <span className="count">{projects.length}</span>)}
        <div className="nav-group-title row between" style={{ paddingRight: 4 }}>
          <span>Categories</span>
          {canEdit && <button className="btn icon small ghost" style={{ width: 24, height: 24 }} title="New category" onClick={() => setEditing({})}><Plus size={14} /></button>}
        </div>
        {categories.length > 0 && (
          <button {...dropTarget(null)} className={`nav-item ${over === "all" ? "drop" : ""} ${category === "all" && view === "projects" ? "active" : ""}`} onClick={() => { setCategory("all"); go("projects"); }}>
            <span className="cat-row"><span className="cat-dot" style={{ background: "var(--faint)" }} />All projects</span>
          </button>
        )}
        {roots.map((c) => renderCategory(c, 0))}
        {categories.length === 0 && <div className="faint" style={{ padding: "2px 12px", fontSize: 12.5 }}>Group your projects, for example by product.</div>}
        {(uefns.length > 0 || projects.some((p) => isUefnBackup(p.name))) && (
          <>
            <div className="nav-group-title"><span>Other editors</span></div>
            <button className={`nav-item ${category === "uefn" && (view === "projects" || view === "project") ? "active" : ""}`} onClick={() => { setCategory("uefn"); go("projects"); }}>
              <span className="cat-row"><Box size={14} />UEFN</span>
              <span className="count">{projects.filter((p) => p.uefn || isUefnBackup(p.name)).length || ""}</span>
            </button>
          </>
        )}
        {favorites.length > 0 && (
          <>
            <div className="nav-group-title"><span>Favorites</span></div>
            {favorites.map((p) => (
              <div key={p.key} className="nav-row">
                <button className={`nav-item ${view === "project" && projectKey === p.key ? "active" : ""}`} onClick={() => go("project", p.key)}>
                  <span className="cat-row"><Star size={13} fill="currentColor" color="var(--warn)" />{settings.overrides[p.id!]?.displayName || p.name}</span>
                </button>
                {(p.local || p.uefn) && (
                  <button
                    className="row-play"
                    title={p.uefn ? "Open in UEFN" : p.local!.uproject ? "Open in Unreal Engine" : "Open the folder"}
                    onClick={() => (p.uefn || p.local!.uproject ? openProject(p) : api.openPath(p.local!.path))}
                  >
                    <Play size={13} fill="currentColor" />
                  </button>
                )}
              </div>
            ))}
          </>
        )}
      </div>

      <div className="sidebar-foot">
        {isAdmin && nav("admin", "Administration", <ShieldCheck size={18} />)}
        {nav("updates", "Updates", <RefreshCw size={18} />, updates.available > 0 ? <span className="nav-badge" style={{ color: readableOn("#14a0a5") }}>{updates.available}</span> : undefined)}
        {nav("settings", "Settings", <SettingsIcon size={18} />)}
      </div>

      {menu.element}
      {editing && <CategoryModal category={editing.category} parentId={editing.parentId} onClose={() => setEditing(null)} />}
      {access && <CategoryAccessModal category={access} onClose={() => setAccess(null)} />}
      {addServer && <ServerModal onClose={() => setAddServer(false)} onSaved={() => setSignIn(true)} />}
      {signIn && server && <SignInModal server={server} onClose={() => setSignIn(false)} />}
    </aside>
  );
}
