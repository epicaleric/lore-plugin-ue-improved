import { useEffect, useState } from "react";
import { Logo } from "./components/Logo";
import { Background } from "./components/Background";
import { Sidebar } from "./components/Sidebar";
import { ServerForm, SignInForm } from "./components/Servers";
import { Spinner } from "./components/ui";
import { useStore } from "./lib/store";
import { UpdatesProvider } from "./lib/updates";
import type { Server } from "./lib/types";
import { Admin } from "./views/Admin";
import { ProjectDetail } from "./views/ProjectDetail";
import { Projects } from "./views/Projects";
import { Settings } from "./views/Settings";
import { Updates } from "./views/Updates";

function Welcome() {
  const { settings, update, refreshSession, reload } = useStore();
  const [server, setServer] = useState<Server | null>(null);
  return (
    <div className="welcome">
      <div className="welcome-card">
        <div className="steps"><span className="step on" /><span className={`step ${server ? "on" : ""}`} /></div>
        <Logo size={56} />
        {!server ? (
          <>
            <h1>Welcome to Lore+</h1>
            <div className="muted" style={{ marginBottom: 22 }}>Connect to your Lore server to see all projects, sync them and manage who can use them.</div>
            <ServerForm
              onCancel={() => {}}
              onSave={(s) => {
                update({ servers: [...settings.servers, s], activeServer: s.id });
                setServer(s);
              }}
            />
          </>
        ) : (
          <>
            <h1>Sign in</h1>
            <div className="muted" style={{ marginBottom: 22 }}>{server.label} · {server.loreUrl}</div>
            <SignInForm
              server={server}
              onDone={async () => {
                await refreshSession();
                await reload();
              }}
            />
          </>
        )}
      </div>
    </div>
  );
}

export default function App() {
  const { ready, settings, view, toasts, back, forward } = useStore();

  // The side buttons of a mouse (and Alt + arrows) go back and forward, like in a browser.
  useEffect(() => {
    const press = (event: MouseEvent) => {
      if (event.button === 3 || event.button === 4) event.preventDefault();
    };
    const release = (event: MouseEvent) => {
      if (event.button === 3) { event.preventDefault(); back(); }
      if (event.button === 4) { event.preventDefault(); forward(); }
    };
    const keys = (event: KeyboardEvent) => {
      if (!event.altKey) return;
      if (event.key === "ArrowLeft") back();
      if (event.key === "ArrowRight") forward();
    };
    window.addEventListener("mousedown", press);
    window.addEventListener("mouseup", release);
    window.addEventListener("keydown", keys);
    return () => {
      window.removeEventListener("mousedown", press);
      window.removeEventListener("mouseup", release);
      window.removeEventListener("keydown", keys);
    };
  }, [back, forward]);
  if (!ready) return <div className="welcome"><Spinner size={28} /></div>;
  if (settings.servers.length === 0) {
    return <><Welcome /><div className="toasts">{toasts.map((t) => <div key={t.id} className={`toast ${t.kind}`}>{t.text}</div>)}</div></>;
  }
  return (
    <UpdatesProvider>
      <Background />
      <div className="app">
        <Sidebar />
        <main className="main">
          {view === "projects" && <Projects />}
          {view === "project" && <ProjectDetail />}
          {view === "admin" && <Admin />}
          {view === "updates" && <Updates />}
          {view === "settings" && <Settings />}
        </main>
      </div>
      <div className="toasts">{toasts.map((t) => <div key={t.id} className={`toast ${t.kind}`}>{t.text}</div>)}</div>
    </UpdatesProvider>
  );
}

