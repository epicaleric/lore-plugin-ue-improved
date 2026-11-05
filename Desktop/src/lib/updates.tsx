import { jobsRunning } from "./jobs";
import { createContext, useCallback, useContext, useEffect, useRef, useState, type ReactNode } from "react";
import { listen } from "@tauri-apps/api/event";
import { getVersion } from "@tauri-apps/api/app";
import { api } from "./api";
import { compareVersions } from "./parse";
import { useStore } from "./store";

export const REPO = "epicaleric/lore-plugin-ue-improved";
export const REPO_URL = `https://github.com/${REPO}`;
const PLUGIN_UPLUGIN = `https://raw.githubusercontent.com/${REPO}/main/Plugin/LoreSourceControl/LoreSourceControl.uplugin`;

export interface Component {
  name: string;
  installed: string | null;
  latest: string | null;
  url?: string;
  asset?: string;
  error?: string;
  outdated: boolean;
  /** Unreal version ("5.8") -> download of the ready-made plugin */
  builds?: Record<string, string>;
}

/** Releases are not sorted by version on GitHub, so the newest one has to be looked for. */
function newest(releases: any[], prefix: string) {
  return releases
    .filter((r) => String(r.tag_name).startsWith(prefix) && !r.draft && !r.prerelease)
    .sort((a, b) => compareVersions(String(b.tag_name).slice(prefix.length), String(a.tag_name).slice(prefix.length)))[0];
}

const unrealVersion = (version: string) => version.split(".").slice(0, 2).join(".");

interface UpdatesState {
  lore: Component;
  plugin: Component;
  app: Component;
  checking: boolean;
  available: number;
  logs: Record<string, string[]>;
  busy: Record<string, boolean>;
  check: () => Promise<void>;
  installLore: () => Promise<void>;
  installApp: () => Promise<void>;
  installPlugin: (kind: "engine" | "project", target: string, engine: string | null) => Promise<void>;
}

const empty = (name: string): Component => ({ name, installed: null, latest: null, outdated: false });

const Ctx = createContext<UpdatesState>(null as unknown as UpdatesState);
export const useUpdates = () => useContext(Ctx);

export function UpdatesProvider({ children }: { children: ReactNode }) {
  const { settings, engines, locals, reloadEngines, reload, toast, ready } = useStore();
  const [lore, setLore] = useState<Component>(empty("Lore client"));
  const [plugin, setPlugin] = useState<Component>(empty("Unreal plugin"));
  const [app, setApp] = useState<Component>(empty("Lore+"));
  const [checking, setChecking] = useState(false);
  const [logs, setLogs] = useState<Record<string, string[]>>({});
  const [busy, setBusy] = useState<Record<string, boolean>>({});
  const autoClient = useRef(false);
  const autoApp = useRef(false);
  const handled = useRef(new Set<string>());

  useEffect(() => {
    const unlisten = listen<{ task: string; line: string }>("task-log", (event) => {
      setLogs((all) => ({ ...all, [event.payload.task]: [...(all[event.payload.task] ?? []).slice(-60), event.payload.line] }));
    });
    return () => {
      unlisten.then((f) => f());
    };
  }, []);

  const check = useCallback(async () => {
    setChecking(true);
    try {
      const [version, loreLatest, pluginText, releases, current] = await Promise.allSettled([
        api.lore(["--version"]),
        api.getJson("https://api.github.com/repos/EpicGames/lore/releases/latest"),
        api.getText(PLUGIN_UPLUGIN),
        api.getJson(`https://api.github.com/repos/${REPO}/releases?per_page=100`),
        getVersion(),
      ]);

      // Lore client
      const installed = version.status === "fulfilled" && version.value.ok ? version.value.stdout.trim().replace(/^lore\s+/, "") : null;
      if (loreLatest.status === "fulfilled") {
        const tag: string = loreLatest.value.tag_name ?? "";
        const msi = (loreLatest.value.assets ?? []).find((a: any) => /x86_64-pc-windows-msvc\.msi$/.test(a.name));
        setLore({
          name: "Lore client",
          installed,
          latest: tag.replace(/^v/, ""),
          url: loreLatest.value.html_url,
          asset: msi?.browser_download_url,
          outdated: !!installed && compareVersions(installed, tag) < 0,
        });
      } else {
        setLore({ ...empty("Lore client"), installed, error: "Could not check GitHub" });
      }

      // Plugin: the newest ready-made build, or else the version in the repository.
      const pluginRelease = releases.status === "fulfilled" ? newest(releases.value as any[], "plugin-v") : undefined;
      if (pluginRelease) {
        const builds: Record<string, string> = {};
        for (const asset of pluginRelease.assets ?? []) {
          const match = String(asset.name).match(/-UE(\d+\.\d+)-Win64\.zip$/i);
          if (match) builds[match[1]] = asset.browser_download_url;
        }
        setPlugin({ name: "Unreal plugin", installed: null, latest: String(pluginRelease.tag_name).replace(/^plugin-v/, ""), url: pluginRelease.html_url, outdated: false, builds });
      } else if (pluginText.status === "fulfilled") {
        let latest: string | null = null;
        try {
          latest = JSON.parse(pluginText.value.replace(/^\uFEFF/, "")).VersionName ?? null;
        } catch {
          latest = null;
        }
        setPlugin({ name: "Unreal plugin", installed: null, latest, url: REPO_URL, outdated: false, builds: {} });
      } else {
        setPlugin({ ...empty("Unreal plugin"), error: "Could not check GitHub", url: REPO_URL });
      }

      // The app itself
      const currentVersion = current.status === "fulfilled" ? current.value : null;
      if (releases.status === "fulfilled") {
        const release = newest(releases.value as any[], "desktop-v");
        const asset = release?.assets?.find((a: any) => /setup\.exe$/i.test(a.name));
        const latest = release ? String(release.tag_name).replace(/^desktop-v/, "") : null;
        setApp({
          name: "Lore+",
          installed: currentVersion,
          latest,
          url: release?.html_url ?? `${REPO_URL}/releases`,
          asset: asset?.browser_download_url,
          outdated: !!(currentVersion && latest && compareVersions(currentVersion, latest) < 0),
          error: release ? undefined : "No release published yet",
        });
      } else {
        setApp({ ...empty("Lore+"), installed: currentVersion, error: "Could not check GitHub" });
      }
    } finally {
      setChecking(false);
    }
  }, []);

  useEffect(() => {
    if (!ready) return;
    check();
    // A window that stays open still hears about new versions.
    const timer = setInterval(check, 30 * 60 * 1000);
    return () => clearInterval(timer);
  }, [ready]);

  // Which engines and projects have an older plugin than the latest one?
  const pluginOutdated = (installed: string | null) => !!installed && !!plugin.latest && compareVersions(installed, plugin.latest) < 0;
  const outdatedPlugins = engines.filter((e) => pluginOutdated(e.plugin)).length + locals.filter((l) => pluginOutdated(l.plugin)).length;
  const available = (lore.outdated ? 1 : 0) + (app.outdated ? 1 : 0) + outdatedPlugins;

  const withBusy = async (key: string, work: () => Promise<void>, done?: string) => {
    setBusy((b) => ({ ...b, [key]: true }));
    setLogs((all) => ({ ...all, [key]: [] }));
    try {
      await work();
      if (done) toast(done, "good");
    } catch (error) {
      toast(String(error), "bad");
      setLogs((all) => ({ ...all, [key]: [...(all[key] ?? []), String(error)] }));
    } finally {
      setBusy((b) => ({ ...b, [key]: false }));
    }
  };

  const installLore = useCallback(async () => {
    if (!lore.asset) return;
    await withBusy("lore-client", async () => {
      await api.installLoreClient(lore.asset!);
      await check();
    }, "Lore client updated");
  }, [lore.asset]);

  const installApp = useCallback(async () => {
    if (!app.asset || busy["app"]) return;
    if (jobsRunning()) {
      toast("An upload is running. Lore+ updates when it is done.", "info");
      return;
    }
    await withBusy("app", async () => {
      toast(`Updating Lore+ to ${app.latest}. The app closes for a moment and opens again by itself.`, "info");
      await new Promise((resolve) => setTimeout(resolve, 3500));
      await api.installAppUpdate(app.asset!);
    });
  }, [app.asset, busy["app"]]);

  const installPlugin = useCallback(
    async (kind: "engine" | "project", target: string, engine: string | null) => {
      const prebuilt = engine ? plugin.builds?.[unrealVersion(engine)] ?? null : null;
      await withBusy(`plugin:${target}`, async () => {
        await api.installPlugin(kind, target, prebuilt);
        await reloadEngines();
        await reload();
      }, "Plugin installed");
    },
    [reload, reloadEngines, plugin.builds],
  );

  // Automatic updates, once per start, for what the user switched on.
  useEffect(() => {
    if (autoClient.current || checking || !ready || !lore.latest) return;
    autoClient.current = true;
    if (settings.autoUpdate.lore && lore.outdated && lore.asset) installLore();
  }, [checking, ready, lore.latest]);

  useEffect(() => {
    if (autoApp.current || checking || !ready || !app.latest) return;
    autoApp.current = true;
    if (settings.autoUpdate.app && app.outdated && app.asset) installApp();
  }, [checking, ready, app.latest]);

  // The plugin: what is broken is repaired, what a project needs is put in the engine. Each thing once per start.
  useEffect(() => {
    if (!ready || !plugin.builds) return;
    const once = (key: string, work: () => void) => {
      if (handled.current.has(key)) return;
      handled.current.add(key);
      work();
    };
    for (const project of locals) {
      if (project.pluginBroken && project.engine && plugin.builds[unrealVersion(project.engine)]) {
        once(`repair:${project.path}`, () => installPlugin("project", project.path, project.engine));
      }
    }
    if (!settings.autoUpdate.plugin) return;
    for (const engine of engines) {
      const wanted = locals.some((l) => l.needsPlugin && !l.plugin && l.engine && unrealVersion(l.engine) === unrealVersion(engine.version));
      const available = !!plugin.builds[unrealVersion(engine.version)];
      if (available && (pluginOutdated(engine.plugin) || (!engine.plugin && wanted))) {
        once(`engine:${engine.path}`, () => installPlugin("engine", engine.path, engine.version));
      }
    }
  }, [ready, plugin.latest, plugin.builds, engines, locals]);

  const value: UpdatesState = { lore, plugin, app, checking, available, logs, busy, check, installLore, installApp, installPlugin };
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export function pluginIsOutdated(installed: string | null, latest: string | null) {
  return !!installed && !!latest && compareVersions(installed, latest) < 0;
}
