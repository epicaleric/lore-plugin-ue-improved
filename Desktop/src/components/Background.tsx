import { useEffect, useRef } from "react";
import { invalidateStatus } from "../lib/actions";
import { jobsRunning } from "../lib/jobs";
import { runMirrors } from "../lib/mirror";
import { api } from "../lib/api";
import { backupUefn } from "../lib/uefn";
import { useStore } from "../lib/store";

const EVERY = 5 * 60 * 1000;
const DAY = 24 * 60 * 60 * 1000;

/** While the app is open: the project list stays current and projects with a GitHub mirror are sent along. */
export function Background() {
  const store = useStore();
  const latest = useRef(store);
  latest.current = store;

  useEffect(() => {
    const tick = async () => {
      if (jobsRunning()) return;
      const { server, session, settings, locals, update, toast, reload } = latest.current;
      if (!server || !session) return;
      try {
        const sent = await runMirrors(server, settings, locals);
        const ids = Object.keys(sent);
        if (ids.length) {
          const mirrors = { ...latest.current.settings.mirrors };
          for (const id of ids) mirrors[id] = { ...mirrors[id], revision: sent[id] };
          update({ mirrors });
          toast("New versions were sent to GitHub", "good");
        }
      } catch (error) {
        toast(`GitHub: ${String(error).replace(/^Error: /, "")}`, "bad");
      }
      // UEFN's main copy is on Epic's server, so its backups are only made now and then, when UEFN is closed.
      for (const project of latest.current.uefns) {
        const backup = latest.current.settings.uefnBackups[project.id];
        if (!backup?.daily || Date.now() - (backup.at ?? 0) < DAY || (await api.uefnRunning())) continue;
        try {
          await backupUefn(server, project, backup);
          update({ uefnBackups: { ...latest.current.settings.uefnBackups, [project.id]: { ...backup, at: Date.now() } } });
        } catch (error) {
          toast(`Backup of ${project.name}: ${String(error).replace(/^Error: /, "")}`, "bad");
        }
      }
      invalidateStatus();
      await reload();
    };
    const first = setTimeout(tick, 20 * 1000);
    const timer = setInterval(tick, EVERY);
    return () => {
      clearTimeout(first);
      clearInterval(timer);
    };
  }, []);
  return null;
}
