/** Work that must not be interrupted (an upload), and folders that already have a Lore command running. */
const folders = new Set<string>();

export const startJob = (folder: string) => {
  const key = folder.toLowerCase();
  if (folders.has(key)) return false;
  folders.add(key);
  return true;
};
export const endJob = (folder: string) => void folders.delete(folder.toLowerCase());
export const jobsRunning = () => folders.size > 0;
