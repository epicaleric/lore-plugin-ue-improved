import { Check, Download, ExternalLink, Package, RefreshCw, Rocket, Terminal } from "lucide-react";
import { openUrl } from "@tauri-apps/plugin-opener";
import { Pill, Spinner, Switch } from "../components/ui";
import { useStore } from "../lib/store";
import { pluginIsOutdated, useUpdates, type Component } from "../lib/updates";

function Log({ lines }: { lines?: string[] }) {
  if (!lines || lines.length === 0) return null;
  return <div className="log" style={{ marginTop: 12 }}>{lines.join("\n")}</div>;
}

function StatusLine({ c }: { c: Component }) {
  if (c.error) return <Pill tone="muted">{c.error}</Pill>;
  if (!c.latest) return <Pill tone="muted">Not checked</Pill>;
  if (c.outdated) return <Pill tone="warn">Update available · {c.latest}</Pill>;
  return <Pill tone="good"><Check size={12} /> Up to date</Pill>;
}

export function Updates() {
  const { settings, update, engines, locals } = useStore();
  const u = useUpdates();

  return (
    <>
      <div className="topbar">
        <div><h1>Updates</h1><div className="sub">Lore client, Unreal plugin and this app</div></div>
        <div className="spacer" />
        <button className="btn" onClick={u.check}>{u.checking ? <Spinner /> : <RefreshCw size={16} />} Check for updates</button>
      </div>
      <div className="content">
        <div className="panel">
          <div className="row between">
            <div className="row">
              <div className="updates-icon"><Rocket size={20} /></div>
              <div>
                <h3 style={{ margin: 0 }}>Lore+</h3>
                <div className="muted">Installed {u.app.installed ?? "?"}{u.app.latest ? ` · latest ${u.app.latest}` : ""}</div>
              </div>
            </div>
            <div className="row gap-s">
              <StatusLine c={u.app} />
              <button className="btn small ghost" onClick={() => openUrl(u.app.url ?? "https://github.com/epicaleric/lore-plugin-ue-improved/releases")}><ExternalLink size={14} /> Releases</button>
              <button className="btn primary small" disabled={!u.app.outdated || !u.app.asset || u.busy["app"]} onClick={u.installApp}>
                {u.busy["app"] ? <Spinner size={14} /> : <Download size={14} />} Update and restart
              </button>
            </div>
          </div>
          <div className="row" style={{ marginTop: 14 }}>
            <Switch on={settings.autoUpdate.app} onChange={(on) => update({ autoUpdate: { ...settings.autoUpdate, app: on } })} />
            <span>Update automatically when the app starts</span>
          </div>
          <Log lines={u.logs["app"]} />
        </div>

        <div className="panel">
          <div className="row between">
            <div className="row">
              <div className="updates-icon"><Terminal size={20} /></div>
              <div>
                <h3 style={{ margin: 0 }}>Lore client</h3>
                <div className="muted">Installed {u.lore.installed ?? "unknown"}{u.lore.latest ? ` · latest ${u.lore.latest}` : ""}</div>
              </div>
            </div>
            <div className="row gap-s">
              <StatusLine c={u.lore} />
              {u.lore.url && <button className="btn small ghost" onClick={() => openUrl(u.lore.url!)}><ExternalLink size={14} /> Release notes</button>}
              <button className="btn primary small" disabled={!u.lore.outdated || !u.lore.asset || u.busy["lore-client"]} onClick={u.installLore}>
                {u.busy["lore-client"] ? <Spinner size={14} /> : <Download size={14} />} Update
              </button>
            </div>
          </div>
          <div className="row" style={{ marginTop: 14 }}>
            <Switch on={settings.autoUpdate.lore} onChange={(on) => update({ autoUpdate: { ...settings.autoUpdate, lore: on } })} />
            <span>Update automatically when the app starts <span className="faint">(Windows asks for permission)</span></span>
          </div>
          <div className="faint" style={{ fontSize: 12.5, marginTop: 8 }}>Keep the Lore server on the same version as the client, otherwise commands can fail.</div>
          <Log lines={u.logs["lore-client"]} />
        </div>

        <div className="panel">
          <div className="row between">
            <div className="row">
              <div className="updates-icon"><Package size={20} /></div>
              <div>
                <h3 style={{ margin: 0 }}>Unreal plugin</h3>
                <div className="muted">{u.plugin.latest ? `Latest ${u.plugin.latest}` : "Latest version unknown"}</div>
              </div>
            </div>
            <div className="row gap-s">
              <Switch on={settings.autoUpdate.plugin} onChange={(on) => update({ autoUpdate: { ...settings.autoUpdate, plugin: on } })} />
              <span>Update automatically</span>
            </div>
          </div>

          <div className="section-title">Unreal Engine installations</div>
          {engines.length === 0 ? <div className="muted">No Unreal Engine found on this PC.</div> : (
            <table className="table"><tbody>
              {engines.map((engine) => {
                const old = pluginIsOutdated(engine.plugin, u.plugin.latest);
                const key = `plugin:${engine.path}`;
                return (
                  <tr key={engine.path}>
                    <td style={{ width: 120 }}><b>Unreal {engine.version}</b></td>
                    <td className="muted mono">{engine.path}</td>
                    <td style={{ width: 190 }}>{engine.plugin ? (old ? <Pill tone="warn">{engine.plugin} → {u.plugin.latest}</Pill> : <Pill tone="good"><Check size={12} /> {engine.plugin}</Pill>) : <Pill tone="muted">Not installed</Pill>}</td>
                    <td style={{ width: 130, textAlign: "right" }}>
                      <button className="btn small" disabled={u.busy[key]} onClick={() => u.installPlugin("engine", engine.path, engine.version)}>
                        {u.busy[key] ? <Spinner size={14} /> : <Download size={14} />} {engine.plugin ? (old ? "Update" : "Rebuild") : "Install"}
                      </button>
                    </td>
                  </tr>
                );
              })}
            </tbody></table>
          )}
          <div className="faint" style={{ fontSize: 12.5, marginTop: 10 }}>A ready-made build is used when there is one for that Unreal version. Otherwise the plugin is built with the engine's own tools, which takes a few minutes and needs the C++ tools of Visual Studio.</div>
          {engines.map((engine) => <Log key={engine.path} lines={u.logs[`plugin:${engine.path}`]} />)}

          <div className="section-title">Projects on this PC</div>
          <div className="faint" style={{ fontSize: 12.5, marginBottom: 8 }}>Projects are only changed when you press Update here. The plugin in a project is saved in Lore, so a new version of it is a change you save as a version afterwards.</div>
          {locals.length === 0 ? <div className="muted">No projects found yet.</div> : (
            <table className="table"><tbody>
              {locals.map((project) => {
                const old = pluginIsOutdated(project.plugin, u.plugin.latest);
                const key = `plugin:${project.path}`;
                return (
                  <tr key={project.path}>
                    <td style={{ width: 220 }}><b>{project.repoName ?? project.name}</b></td>
                    <td style={{ width: 190 }}>{project.pluginBroken ? <Pill tone="bad">Needs a repair</Pill> : project.plugin ? (old ? <Pill tone="warn">{project.plugin} → {u.plugin.latest}</Pill> : <Pill tone="good"><Check size={12} /> {project.plugin}</Pill>) : <Pill tone="muted">Not in project</Pill>}</td>
                    <td className="muted" style={{ fontSize: 12.5 }}>Source copy in the project's Plugins folder</td>
                    <td style={{ width: 130, textAlign: "right" }}>
                      <button className="btn small" disabled={u.busy[key] || !project.uproject} onClick={() => u.installPlugin("project", project.path, project.engine)}>
                        {u.busy[key] ? <Spinner size={14} /> : <Download size={14} />} {project.pluginBroken ? "Repair" : project.plugin ? (old ? "Update" : "Reinstall") : "Install"}
                      </button>
                    </td>
                  </tr>
                );
              })}
            </tbody></table>
          )}
          {locals.map((project) => <Log key={project.path} lines={u.logs[`plugin:${project.path}`]} />)}
        </div>
      </div>
    </>
  );
}
