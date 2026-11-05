import { useState } from "react";
import { ChevronsUpDown, Folder, LayoutGrid, Plus, RefreshCw, Settings as SettingsIcon, ShieldCheck, Users } from "lucide-react";
import { useStore, type View } from "../lib/store";
import { useUpdates } from "../lib/updates";
import { readableOn } from "../lib/format";
import { CategoryAccessModal, CategoryModal } from "./Categories";
import { ServerModal, SignInModal } from "./Servers";
import { useMenu } from "./ui";
import type { Category } from "../lib/types";

export function Sidebar() {
  const { settings, server, session, view, go, category, setCategory, projects, isAdmin, setServer } = useStore();
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
  const roots = settings.categories.filter((c) => !c.parentId);

  const renderCategory = (c: Category, depth: number): React.ReactNode => (
    <div key={c.id}>
      <button
        className={`nav-item ${category === c.id && (view === "projects" || view === "project") ? "active" : ""}`}
        style={{ paddingLeft: 12 + depth * 16 }}
        onClick={() => {
          setCategory(c.id);
          go("projects");
        }}
        onContextMenu={(e) =>
          menu.open(e, [
            { label: "Edit category", onClick: () => setEditing({ category: c }) },
            { label: "New category inside", onClick: () => setEditing({ parentId: c.id }) },
            { label: "Who has access", icon: <Users size={15} />, onClick: () => setAccess(c) },
          ])
        }
      >
        <span className="cat-row"><span className="cat-dot" style={{ background: c.color }} />{c.name}</span>
        <span className="count">{countIn(c.id) || ""}</span>
      </button>
      {settings.categories.filter((child) => child.parentId === c.id).map((child) => renderCategory(child, depth + 1))}
    </div>
  );

  return (
    <aside className="sidebar">
      <div className="brand">
        <div className="brand-mark"><Folder size={19} strokeWidth={2.4} /></div>
        <div><div className="brand-name">Lore Hub</div><div className="brand-sub">Projects and people</div></div>
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
          <button className="btn icon small ghost" style={{ width: 24, height: 24 }} title="New category" onClick={() => setEditing({})}><Plus size={14} /></button>
        </div>
        {settings.categories.length > 0 && (
          <button className={`nav-item ${category === "all" && view === "projects" ? "active" : ""}`} onClick={() => { setCategory("all"); go("projects"); }}>
            <span className="cat-row"><span className="cat-dot" style={{ background: "var(--faint)" }} />All projects</span>
          </button>
        )}
        {roots.map((c) => renderCategory(c, 0))}
        {settings.categories.length === 0 && <div className="faint" style={{ padding: "2px 12px", fontSize: 12.5 }}>Group your projects, for example by product.</div>}
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
