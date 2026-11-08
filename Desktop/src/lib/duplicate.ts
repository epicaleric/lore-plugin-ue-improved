import { api } from "./api";
import { squarePng } from "./image";
import { endJob, startJob } from "./jobs";
import type { LocalProject, Server } from "./types";

/** What Unreal accepts as a project name. */
export const validProjectName = (name: string) => /^[A-Za-z][A-Za-z0-9_]{0,19}$/.test(name);

const newProjectId = () => crypto.randomUUID().replace(/-/g, "").toUpperCase();

/** Changes a text file in place; a file that is not there or cannot be read as text is left alone. */
async function rewrite(path: string, change: (text: string) => string) {
  const text = await api.readText(path);
  if (text === null) return;
  const next = change(text);
  if (next !== text) await api.writeText(path, next);
}

/**
 * Makes a new Unreal project from the copy on this PC: the files are copied without generated folders and
 * Lore's data, and the project gets its own name, its own project id and, when chosen, its own thumbnail.
 * With a server it is put there as a new project with a fresh history.
 */
export async function duplicateProject(
  source: LocalProject,
  options: { name: string; parent: string; thumbnail: string | null; server: Server | null },
  onStep: (text: string) => void,
) {
  const { name, parent, thumbnail, server } = options;
  const target = `${parent.replace(/[\\/]$/, "")}\\${name}`;
  if (!source.uproject) throw new Error("Only an Unreal project can be duplicated.");
  if (await api.exists(target)) throw new Error(`${target} already exists.`);
  const oldName = source.uproject.split(/[\\/]/).pop()!.replace(/\.uproject$/i, "");
  if (!startJob(target)) throw new Error("This copy is already being made.");
  try {
    onStep("Copying the files");
    await api.copyProject(source.path, target);

    onStep("Giving the project its own name");
    await api.renamePath(`${target}\\${oldName}.uproject`, `${target}\\${name}.uproject`);
    if (thumbnail) await api.writeBinary(`${target}/${name}.png`, await squarePng(thumbnail));
    else if (await api.exists(`${target}/${oldName}.png`)) await api.renamePath(`${target}\\${oldName}.png`, `${target}\\${name}.png`);
    if (thumbnail && (await api.exists(`${target}/thumbnail.png`))) await api.writeBinary(`${target}/thumbnail.png`, await squarePng(thumbnail));
    await rewrite(`${target}/Config/DefaultGame.ini`, (text) =>
      text.replace(/^ProjectName=.*$/m, `ProjectName=${name}`).replace(/^ProjectID=.*$/m, `ProjectID=${newProjectId()}`));
    await rewrite(`${target}/Config/DefaultSourceControlSettings.ini`, (text) =>
      text.replace(new RegExp(`(RepositoryUrl=.*/)${oldName}\\s*$`, "m"), `$1${name}`));

    if (!server) return target;
    const fail = (result: { stderr: string; stdout: string }) => new Error((result.stderr || result.stdout).trim().split("\n").slice(0, 3).join(" "));
    const work = (args: string[]) => api.loreStream(args, target, server.id, onStep);
    onStep("Creating the project on the server");
    const created = await api.lore(["repository", "create", `${server.loreUrl}/${name}`], target, server.id);
    if (!created.ok) throw fail(created);
    let result = await work(["stage", "--scan", "."]);
    if (!result.ok) throw fail(result);
    result = await work(["commit", `New project ${name}, made from ${oldName}`]);
    if (!result.ok) throw fail(result);
    onStep("Sending it to the server");
    result = await work(["push"]);
    if (!result.ok) throw fail(result);
    return target;
  } finally {
    endJob(target);
  }
}
