import { useEffect, useState } from "react";
import { Ban, Check, KeyRound, Lock, MoreHorizontal, RefreshCw, ShieldCheck, ShieldOff, Trash2, UserPlus } from "lucide-react";
import { Avatar, Empty, Field, Modal, Pill, Spinner, Switch, useMenu } from "../components/ui";
import { adminApi, api } from "../lib/api";
import { useStore } from "../lib/store";
import { SHARED_NAME, shareWithEveryone } from "../lib/shared";
import type { AdminUser, Grant } from "../lib/types";

type Tab = "users" | "access" | "server";

function AddUser({ onClose, onDone }: { onClose: () => void; onDone: () => void }) {
  const { server, toast } = useStore();
  const [name, setName] = useState("");
  const [displayName, setDisplayName] = useState("");
  const [password, setPassword] = useState("");
  const [admin, setAdmin] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  const save = async () => {
    if (!server) return;
    setBusy(true);
    try {
      await adminApi.createUser(server, { name: name.trim(), password, displayName: displayName.trim() || undefined, admin });
      shareWithEveryone(server).catch(() => {});
      toast(`${name} was added`, "good");
      onDone();
      onClose();
    } catch (e) {
      setError(String(e).replace(/^Error: /, ""));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Modal title="Add a user" sub="They can sign in with these details in Unreal and in this app." onClose={onClose}
      actions={<><button className="btn" onClick={onClose}>Cancel</button><button className="btn primary" disabled={busy || !name.trim() || password.length < 4} onClick={save}>{busy ? <Spinner /> : <UserPlus size={16} />} Add user</button></>}>
      <Field label="User name"><input className="input" autoFocus value={name} onChange={(e) => setName(e.target.value)} /></Field>
      <Field label="Display name" hint="Optional."><input className="input" value={displayName} onChange={(e) => setDisplayName(e.target.value)} /></Field>
      <Field label="Password"><input className="input" type="password" value={password} onChange={(e) => setPassword(e.target.value)} /></Field>
      <div className="row between" style={{ marginBottom: 6 }}>
        <div><b>Administrator</b><div className="muted" style={{ fontSize: 12.5 }}>Can use every project and manage users.</div></div>
        <Switch on={admin} onChange={setAdmin} />
      </div>
      {error && <div className="banner bad" style={{ marginTop: 12 }}>{error}</div>}
    </Modal>
  );
}

function PasswordModal({ user, onClose }: { user: AdminUser; onClose: () => void }) {
  const { server, toast } = useStore();
  const [password, setPassword] = useState("");
  const [busy, setBusy] = useState(false);
  return (
    <Modal title={`New password for ${user.name}`} onClose={onClose}
      actions={<><button className="btn" onClick={onClose}>Cancel</button><button className="btn primary" disabled={busy || password.length < 4} onClick={async () => {
        if (!server) return;
        setBusy(true);
        try { await adminApi.setPassword(server, user.name, password); toast("Password changed", "good"); onClose(); } catch (e) { toast(String(e), "bad"); } finally { setBusy(false); }
      }}>{busy ? <Spinner /> : <KeyRound size={16} />} Change password</button></>}>
      <Field label="Password"><input className="input" type="password" autoFocus value={password} onChange={(e) => setPassword(e.target.value)} /></Field>
    </Modal>
  );
}

function Users({ users, grants, reload }: { users: AdminUser[]; grants: Grant[]; reload: () => void }) {
  const { server, session, toast } = useStore();
  const [adding, setAdding] = useState(false);
  const [passwordFor, setPasswordFor] = useState<AdminUser | null>(null);
  const menu = useMenu();

  const act = async (work: () => Promise<unknown>, done: string) => {
    try { await work(); toast(done, "good"); reload(); } catch (e) { toast(String(e).replace(/^Error: /, ""), "bad"); }
  };

  return (
    <>
      <div className="row between" style={{ marginBottom: 14 }}>
        <div className="muted">{users.length} user{users.length === 1 ? "" : "s"}</div>
        <button className="btn primary" onClick={() => setAdding(true)}><UserPlus size={16} /> Add user</button>
      </div>
      <div className="panel" style={{ padding: 0 }}>
        <table className="table">
          <thead><tr><th style={{ width: 56 }} /><th>User</th><th>Role</th><th>Projects</th><th style={{ width: 60 }} /></tr></thead>
          <tbody>
            {users.map((user) => {
              const own = grants.filter((g) => g.user === user.name);
              const all = user.admin || own.some((g) => g.repository === "*");
              const isMe = session?.userName === user.name;
              return (
                <tr key={user.name}>
                  <td><Avatar name={user.name} /></td>
                  <td><b>{user.displayName || user.name}</b>{isMe && <span className="faint"> (you)</span>}<div className="muted" style={{ fontSize: 12.5 }}>{user.name}</div></td>
                  <td>{user.disabled ? <Pill tone="bad"><Ban size={12} /> Disabled</Pill> : user.admin ? <Pill tone="accent"><ShieldCheck size={12} /> Administrator</Pill> : <Pill>Member</Pill>}</td>
                  <td className="muted">{all ? "All projects" : own.length === 0 ? "None yet" : own.map((g) => g.repositoryName ?? g.repository).join(", ")}</td>
                  <td>
                    <button className="btn icon small ghost" onClick={(e) => menu.open(e, [
                      { label: "Change password", icon: <KeyRound size={15} />, onClick: () => setPasswordFor(user) },
                      user.admin
                        ? { label: "Remove administrator rights", icon: <ShieldOff size={15} />, onClick: () => server && act(() => adminApi.updateUser(server, user.name, { admin: false }), "Rights updated") }
                        : { label: "Make administrator", icon: <ShieldCheck size={15} />, onClick: () => server && act(() => adminApi.updateUser(server, user.name, { admin: true }), "Rights updated") },
                      user.disabled
                        ? { label: "Enable sign-in", icon: <Check size={15} />, onClick: () => server && act(() => adminApi.updateUser(server, user.name, { disabled: false }), "User enabled") }
                        : { label: "Disable sign-in", icon: <Ban size={15} />, onClick: () => server && act(() => adminApi.updateUser(server, user.name, { disabled: true }), "User disabled") },
                      { label: "", divider: true, onClick: () => {} },
                      { label: "Delete user", icon: <Trash2 size={15} />, danger: true, onClick: () => { if (server && window.confirm(`Delete ${user.name}? This cannot be undone.`)) act(() => adminApi.deleteUser(server, user.name), "User deleted"); } },
                    ])}><MoreHorizontal size={17} /></button>
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
      {menu.element}
      {adding && <AddUser onClose={() => setAdding(false)} onDone={reload} />}
      {passwordFor && <PasswordModal user={passwordFor} onClose={() => setPasswordFor(null)} />}
    </>
  );
}

function Matrix({ users, grants, repos, reload }: { users: AdminUser[]; grants: Grant[]; repos: { id: string; name: string }[]; reload: () => void }) {
  const { server, toast } = useStore();
  const [busy, setBusy] = useState<string | null>(null);
  const has = (user: string, repo: string) => grants.some((g) => g.user === user && g.repository === repo);

  const toggle = async (user: AdminUser, repo: { id: string }) => {
    if (!server) return;
    const key = `${user.name}:${repo.id}`;
    setBusy(key);
    try {
      if (has(user.name, repo.id)) await adminApi.revoke(server, user.name, repo.id);
      else await adminApi.grant(server, user.name, repo.id);
      reload();
    } catch (e) { toast(String(e), "bad"); } finally { setBusy(null); }
  };

  if (repos.length === 0) return <Empty icon={<Lock size={28} />} title="No projects on the server yet" />;
  return (
    <div className="panel" style={{ padding: 0, overflow: "auto" }}>
      <table className="table matrix">
        <thead>
          <tr><th>User</th>{repos.map((r) => <th key={r.id} className="rot" title={r.name}>{r.name}</th>)}</tr>
        </thead>
        <tbody>
          {users.map((user) => {
            const all = user.admin || has(user.name, "*");
            return (
              <tr key={user.name}>
                <td><div className="row gap-s"><Avatar name={user.name} size="small" /><b>{user.displayName || user.name}</b>{user.admin && <Pill tone="accent">admin</Pill>}</div></td>
                {repos.map((repo) => (
                  <td key={repo.id} className="cell">
                    {all ? <Check size={16} color="var(--faint)" /> : busy === `${user.name}:${repo.id}` ? <Spinner size={14} /> : <input className="checkbox" type="checkbox" checked={has(user.name, repo.id)} onChange={() => toggle(user, repo)} />}
                  </td>
                ))}
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}

function SharedPanel() {
  const { sharedActive } = useStore();
  return (
    <div className="panel">
      <h3>Shared categories</h3>
      <div className="muted">
        {sharedActive
          ? <>Categories, and the projects in them, are kept in the project <span className="mono">{SHARED_NAME}</span> on this server, so everybody sees the same. Only administrators can change them. New users get read access to it automatically.</>
          : <>The project <span className="mono">{SHARED_NAME}</span> that keeps the categories on the server is created when an administrator signs in.</>}
      </div>
    </div>
  );
}

function ServerTab({ users, repos }: { users: AdminUser[]; repos: unknown[] }) {
  const { server } = useStore();
  const [health, setHealth] = useState<{ lore: { ok: boolean; ms: number }; auth: { ok: boolean; ms: number } } | null>(null);
  const [checking, setChecking] = useState(false);
  const check = async () => {
    if (!server) return;
    setChecking(true);
    setHealth(await api.health(server).catch(() => null));
    setChecking(false);
  };
  useEffect(() => { check(); }, [server?.id]);
  if (!server) return null;
  const row = (label: string, value: React.ReactNode) => <tr><td className="muted" style={{ width: 190 }}>{label}</td><td>{value}</td></tr>;
  const state = (h?: { ok: boolean; ms: number }) => !h ? <Spinner size={14} /> : h.ok ? <Pill tone="good">Online · {h.ms} ms</Pill> : <Pill tone="bad">Not reachable</Pill>;
  return (
    <div className="col">
      <div className="panel">
        <div className="row between"><h3>Status</h3><button className="btn small" onClick={check}>{checking ? <Spinner size={14} /> : <RefreshCw size={14} />} Check now</button></div>
        <table className="table"><tbody>
          {row("Lore server", state(health?.lore))}
          {row("Authentication service", state(health?.auth))}
          {row("Projects", repos.length)}
          {row("Users", users.length)}
        </tbody></table>
      </div>
      <SharedPanel />
      <div className="panel">
        <h3>Addresses</h3>
        <table className="table"><tbody>
          {row("Lore server", <span className="mono">{server.loreUrl}</span>)}
          {row("Sign-in (gRPC)", <span className="mono">{server.authUrl}</span>)}
          {row("Login page and admin API", <span className="mono">{server.webUrl}</span>)}
        </tbody></table>
      </div>
      <div className="panel">
        <h3>Good to know</h3>
        <div className="muted">The Lore server keeps file locks in memory, so they are lost when the server restarts. The Unreal plugin remembers each person's own locks and takes them back. User and access changes made here apply immediately; people who are already signed in keep access until their project token expires (about an hour).</div>
      </div>
    </div>
  );
}

export function Admin() {
  const { server, isAdmin, session } = useStore();
  const [tab, setTab] = useState<Tab>("users");
  const [users, setUsers] = useState<AdminUser[]>([]);
  const [grants, setGrants] = useState<Grant[]>([]);
  const [repos, setRepos] = useState<{ id: string; name: string }[]>([]);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(false);

  const load = async () => {
    if (!server || !isAdmin) return;
    setLoading(true);
    try {
      const [u, g, r] = await Promise.all([adminApi.users(server), adminApi.grants(server), adminApi.repositories(server)]);
      setUsers(u); setGrants(g); setRepos(r); setError("");
    } catch (e) { setError(String(e).replace(/^Error: /, "")); } finally { setLoading(false); }
  };
  useEffect(() => { load(); }, [server?.id, isAdmin]);

  return (
    <>
      <div className="topbar">
        <div><h1>Administration</h1><div className="sub">{server?.label} · users and access</div></div>
        <div className="spacer" />
        <button className="btn icon" onClick={load} title="Refresh">{loading ? <Spinner /> : <RefreshCw size={16} />}</button>
      </div>
      <div className="content">
        {!session ? (
          <Empty icon={<Lock size={28} />} title="Sign in first">Sign in to {server?.label} to manage it.</Empty>
        ) : !isAdmin ? (
          <Empty icon={<Lock size={28} />} title="Administrators only">Your account is not an administrator on this server.</Empty>
        ) : (
          <>
            {error && <div className="banner bad">{error} <span className="muted">Is the admin API switched on in the auth service (admin_api = true)?</span></div>}
            <div className="tabs">
              {([["users", "Users"], ["access", "Project access"], ["server", "Server"]] as [Tab, string][]).map(([id, label]) => (
                <button key={id} className={`tab ${tab === id ? "active" : ""}`} onClick={() => setTab(id)}>{label}</button>
              ))}
            </div>
            {tab === "users" && <Users users={users} grants={grants} reload={load} />}
            {tab === "access" && <Matrix users={users} grants={grants} repos={repos} reload={load} />}
            {tab === "server" && <ServerTab users={users} repos={repos} />}
          </>
        )}
      </div>
    </>
  );
}
