import { useState } from "react";
import { KeyRound, LogIn } from "lucide-react";
import { api } from "../lib/api";
import { hostOf, uid } from "../lib/format";
import { useStore } from "../lib/store";
import type { Server } from "../lib/types";
import { Field, Modal, Spinner } from "./ui";

/** lores://host:41337 -> https://host:41342 (auth) and https://host:41343 (login page + admin API). */
export function deriveServer(loreUrl: string): Pick<Server, "authUrl" | "webUrl"> {
  const host = hostOf(loreUrl);
  return { authUrl: `https://${host}:41342`, webUrl: `https://${host}:41343` };
}

export function normalizeLoreUrl(input: string): string {
  const trimmed = input.trim();
  if (!trimmed) return "";
  const withScheme = /^[a-z]+:\/\//i.test(trimmed) ? trimmed : `lores://${trimmed}`;
  return /:\d+$/.test(withScheme) ? withScheme : `${withScheme}:41337`;
}

export function SignInForm({ server, onDone, onCancel }: { server: Server; onDone: () => void; onCancel?: () => void }) {
  const { toast } = useStore();
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [apiKey, setApiKey] = useState("");
  const [useKey, setUseKey] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  const submit = async () => {
    setBusy(true);
    setError("");
    try {
      const info = useKey ? await api.signInApiKey(server, apiKey.trim()) : await api.signIn(server, username.trim(), password);
      toast(`Signed in as ${info.userName}`, "good");
      onDone();
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <form
      onSubmit={(e) => {
        e.preventDefault();
        submit();
      }}
    >
      {!useKey ? (
        <>
          <Field label="User name">
            <input className="input" autoFocus value={username} onChange={(e) => setUsername(e.target.value)} autoComplete="username" />
          </Field>
          <Field label="Password">
            <input className="input" type="password" value={password} onChange={(e) => setPassword(e.target.value)} autoComplete="current-password" />
          </Field>
        </>
      ) : (
        <Field label="API key" hint="Created by an administrator, for scripts and build machines.">
          <input className="input" type="password" value={apiKey} onChange={(e) => setApiKey(e.target.value)} />
        </Field>
      )}
      {error && <div className="banner bad">{error}</div>}
      <div className="row between" style={{ marginTop: 6 }}>
        <button type="button" className="btn ghost small" onClick={() => setUseKey(!useKey)}>
          <KeyRound size={15} /> {useKey ? "Use user name and password" : "Use an API key"}
        </button>
        <div className="row gap-s">
          {onCancel && (
            <button type="button" className="btn" onClick={onCancel}>
              Cancel
            </button>
          )}
          <button type="submit" className="btn primary" disabled={busy || (useKey ? !apiKey : !username || !password)}>
            {busy ? <Spinner /> : <LogIn size={16} />} Sign in
          </button>
        </div>
      </div>
    </form>
  );
}

export function SignInModal({ server, onClose }: { server: Server; onClose: () => void }) {
  const { refreshSession, reload } = useStore();
  return (
    <Modal title={`Sign in to ${server.label}`} sub={server.loreUrl} onClose={onClose}>
      <SignInForm
        server={server}
        onCancel={onClose}
        onDone={async () => {
          await refreshSession();
          await reload();
          onClose();
        }}
      />
    </Modal>
  );
}

export function ServerForm({ initial, onSave, onCancel }: { initial?: Server; onSave: (server: Server) => void; onCancel?: () => void }) {
  const [label, setLabel] = useState(initial?.label ?? "");
  const [loreUrl, setLoreUrl] = useState(initial?.loreUrl ?? "");
  const [authUrl, setAuthUrl] = useState(initial?.authUrl ?? "");
  const [webUrl, setWebUrl] = useState(initial?.webUrl ?? "");
  const [advanced, setAdvanced] = useState(!!initial);

  const normalized = normalizeLoreUrl(loreUrl);
  const derived = normalized ? deriveServer(normalized) : { authUrl: "", webUrl: "" };

  const save = () => {
    const url = normalizeLoreUrl(loreUrl);
    onSave({
      id: initial?.id ?? uid(),
      label: label.trim() || hostOf(url),
      loreUrl: url,
      authUrl: (authUrl.trim() || derived.authUrl).replace(/\/$/, ""),
      webUrl: (webUrl.trim() || derived.webUrl).replace(/\/$/, ""),
    });
  };

  return (
    <form
      onSubmit={(e) => {
        e.preventDefault();
        save();
      }}
    >
      <Field label="Name" hint="Only for you, for example Studio or Home server.">
        <input className="input" autoFocus value={label} onChange={(e) => setLabel(e.target.value)} placeholder="My Lore server" />
      </Field>
      <Field label="Server address" hint="The address you use with lore, for example lore.example.com or lores://lore.example.com:41337">
        <input className="input" value={loreUrl} onChange={(e) => setLoreUrl(e.target.value)} placeholder="lore.example.com" />
      </Field>
      <button type="button" className="btn ghost small" onClick={() => setAdvanced(!advanced)} style={{ marginBottom: 12 }}>
        {advanced ? "Hide" : "Show"} advanced
      </button>
      {advanced && (
        <>
          <Field label="Authentication service" hint="Where signing in happens (gRPC). Default: same host, port 41342.">
            <input className="input" value={authUrl} onChange={(e) => setAuthUrl(e.target.value)} placeholder={derived.authUrl || "https://host:41342"} />
          </Field>
          <Field label="Login page and admin API" hint="Default: same host, port 41343.">
            <input className="input" value={webUrl} onChange={(e) => setWebUrl(e.target.value)} placeholder={derived.webUrl || "https://host:41343"} />
          </Field>
        </>
      )}
      <div className="actions" style={{ display: "flex", justifyContent: "flex-end", gap: 10, marginTop: 6 }}>
        <button type="button" className="btn" onClick={onCancel}>
          Cancel
        </button>
        <button type="submit" className="btn primary" disabled={!loreUrl.trim()}>
          {initial ? "Save" : "Add server"}
        </button>
      </div>
    </form>
  );
}

export function ServerModal({ initial, onClose, onSaved }: { initial?: Server; onClose: () => void; onSaved?: (server: Server) => void }) {
  const { settings, update } = useStore();
  return (
    <Modal title={initial ? "Edit server" : "Add a server"} onClose={onClose}>
      <ServerForm
        initial={initial}
        onCancel={onClose}
        onSave={(server) => {
          const servers = initial ? settings.servers.map((s) => (s.id === server.id ? server : s)) : [...settings.servers, server];
          update({ servers, activeServer: server.id });
          onSaved?.(server);
          onClose();
        }}
      />
    </Modal>
  );
}

