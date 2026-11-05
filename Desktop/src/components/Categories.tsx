import { useEffect, useState } from "react";
import { Lock, Trash2 } from "lucide-react";
import { adminApi } from "../lib/api";
import { uid } from "../lib/format";
import { useStore } from "../lib/store";
import type { AdminUser, Category, Grant } from "../lib/types";
import { Avatar, Field, Modal, Pill, Spinner } from "./ui";

export const CATEGORY_COLORS = ["#14a0a5", "#2f7de1", "#9664dc", "#28aa5f", "#e19620", "#e14b73", "#73829b"];

/** The category and everything below it. */
export function descendants(categories: Category[], id: string): Set<string> {
  const ids = new Set<string>([id]);
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
}

export function CategoryModal({ category, parentId, onClose }: { category?: Category; parentId?: string; onClose: () => void }) {
  const { settings, update, setCategory } = useStore();
  const [name, setName] = useState(category?.name ?? "");
  const [color, setColor] = useState(category?.color ?? CATEGORY_COLORS[settings.categories.length % CATEGORY_COLORS.length]);
  const [parent, setParent] = useState(category?.parentId ?? parentId ?? "");
  const blocked = category ? descendants(settings.categories, category.id) : new Set<string>();

  const save = () => {
    const next: Category = { id: category?.id ?? uid(), name: name.trim(), color, parentId: parent || undefined };
    update({ categories: category ? settings.categories.map((c) => (c.id === next.id ? next : c)) : [...settings.categories, next] });
    onClose();
  };

  const remove = () => {
    if (!category || !window.confirm(`Remove the category ${category.name}? The projects in it are not touched.`)) return;
    const assignments = { ...settings.assignments };
    for (const key of Object.keys(assignments)) if (assignments[key] === category.id) delete assignments[key];
    update({
      categories: settings.categories.filter((c) => c.id !== category.id).map((c) => (c.parentId === category.id ? { ...c, parentId: category.parentId } : c)),
      assignments,
    });
    setCategory("all");
    onClose();
  };

  return (
    <Modal
      title={category ? "Edit category" : "New category"}
      sub="Group projects, for example by product or team. Categories can sit inside other categories."
      onClose={onClose}
      actions={
        <>
          {category && <button className="btn danger" onClick={remove} style={{ marginRight: "auto" }}><Trash2 size={15} /> Remove</button>}
          <button className="btn" onClick={onClose}>Cancel</button>
          <button className="btn primary" disabled={!name.trim()} onClick={save}>Save</button>
        </>
      }
    >
      <Field label="Name"><input className="input" autoFocus value={name} onChange={(e) => setName(e.target.value)} onKeyDown={(e) => e.key === "Enter" && name.trim() && save()} /></Field>
      <Field label="Inside">
        <select className="select" value={parent} onChange={(e) => setParent(e.target.value)}>
          <option value="">Top level</option>
          {settings.categories.filter((c) => !blocked.has(c.id)).map((c) => <option key={c.id} value={c.id}>{c.name}</option>)}
        </select>
      </Field>
      <Field label="Colour">
        <div className="swatches">
          {CATEGORY_COLORS.map((c) => <button key={c} className={`swatch ${c === color ? "on" : ""}`} style={{ background: c }} onClick={() => setColor(c)} />)}
        </div>
      </Field>
    </Modal>
  );
}

/** Who can use the projects in a category. Changes are applied to every project in it. */
export function CategoryAccessModal({ category, onClose }: { category: Category; onClose: () => void }) {
  const { server, isAdmin, settings, projects, toast } = useStore();
  const [users, setUsers] = useState<AdminUser[]>([]);
  const [grants, setGrants] = useState<Grant[]>([]);
  const [busy, setBusy] = useState<string | null>(null);
  const ids = descendants(settings.categories, category.id);
  const repos = projects.filter((p) => p.id && settings.assignments[p.id] && ids.has(settings.assignments[p.id]));

  const load = async () => {
    if (!server) return;
    const [u, g] = await Promise.all([adminApi.users(server), adminApi.grants(server)]);
    setUsers(u);
    setGrants(g);
  };
  useEffect(() => { load().catch((e) => toast(String(e), "bad")); }, []);

  const count = (user: AdminUser) => repos.filter((r) => grants.some((g) => g.user === user.name && (g.repository === r.id || g.repository === "*"))).length;

  const toggle = async (user: AdminUser) => {
    if (!server) return;
    const all = count(user) === repos.length;
    setBusy(user.name);
    try {
      for (const repo of repos) {
        if (all) await adminApi.revoke(server, user.name, repo.id!).catch(() => {});
        else await adminApi.grant(server, user.name, repo.id!);
      }
      await load();
    } catch (e) { toast(String(e), "bad"); } finally { setBusy(null); }
  };

  return (
    <Modal wide title={`Access to ${category.name}`} sub={`${repos.length} project${repos.length === 1 ? "" : "s"} in this category${repos.length ? ": " + repos.map((r) => r.name).join(", ") : ""}`} onClose={onClose} actions={<button className="btn primary" onClick={onClose}>Done</button>}>
      {!isAdmin ? (
        <div className="banner warn"><Lock size={16} /> Only administrators can change who has access.</div>
      ) : repos.length === 0 ? (
        <div className="banner">Put projects in this category first (project, Settings, Category).</div>
      ) : (
        <table className="table"><tbody>
          {users.map((user) => {
            const n = count(user);
            return (
              <tr key={user.name}>
                <td style={{ width: 44 }}><Avatar name={user.name} /></td>
                <td><b>{user.displayName || user.name}</b>{user.admin && <> <Pill tone="accent">admin</Pill></>}</td>
                <td className="muted">{user.admin ? "All projects" : n === repos.length ? "All of them" : n === 0 ? "None" : `${n} of ${repos.length}`}</td>
                <td style={{ width: 60, textAlign: "right" }}>
                  {user.admin ? null : busy === user.name ? <Spinner /> : <button className={`switch ${n === repos.length ? "on" : ""}`} onClick={() => toggle(user)} />}
                </td>
              </tr>
            );
          })}
        </tbody></table>
      )}
    </Modal>
  );
}
