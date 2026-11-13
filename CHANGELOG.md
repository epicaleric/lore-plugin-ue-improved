# Changelog

All notable changes. The plugin and the app are versioned separately: the plugin by `VersionName` in `LoreSourceControl.uplugin`, the app by the `desktop-v` tags.

## Plugin 0.3.5

- Sync from the editor can no longer hang: `lore` runs with `--non-interactive` and no input, every call has a time limit, and the progress window has a Cancel button.
- When a sync stops because files were changed on this PC and on the server, the editor names the files (and who holds their locks) and offers "Take theirs" (your version is kept as a copy in `Saved/Lore/Conflicts`) or "Keep mine" (your version goes back on top so you can submit it). Files the editor has open are let go of first.
- Assets locked by another user cannot be saved: the editor refuses the save and says who holds the lock (`Lore.BlockSaveOfLockedFiles 0` turns this off). A failed checkout names the lock holder.
- Sync works while files are staged, and the Lore window's Sync Latest uses the editor's Sync Content.

## Lore+ 0.2.0

- Finds the newest version correctly (GitHub does not list releases by version), and keeps checking while the app is open.
- Ready-made plugin builds: installing into a project or engine needs no C++ tools, a project with a damaged plugin is repaired, and a project that needs the plugin gets it in the engine.
- Changes tab: save what changed as a new version and send it to the server.
- GitHub mirror: every new version of a project that is also a Git repository can go to GitHub.
- Sync works with staged files and puts plugin files back when they get in the way.

## Plugin 0.3.2

- Gemini: stable models first, the lighter model before the full one, a model that does not answer in time is skipped, and the model that worked is tried first next time.
- Submit Content logs what a status pass found.
- Ready-made builds per Unreal version are published as plugin-v releases.

## Lore Hub 0.1.0

First version of the desktop app: servers and sign-in, project cards with thumbnails and Unreal versions, sync and clone, upload of a local project, history, categories, user and access administration, and automatic updates of the Lore client, the plugin and the app.

## Plugin 0.3.1

- The repository was reorganised: the plugin now lives in `Plugin/LoreSourceControl`. The update check follows the new location.


## 0.3.0

- Settings tab: choose any accent colour and avatar colour, with presets and the editor's colour picker.
  New users get a random avatar colour that no other known author has.
- Text on coloured backgrounds (avatars, badges) switches between dark and light so it stays readable.
- The AI button is now a pill-shaped **Generate** button in the description box.
- Description language is free text, so any language can be used.
- Gemini: uses the newest flash model the key can use and falls back to the next flash model when a limit is
  reached. The model setting is gone.
- Server tab: the plugin version is compared with this repository, the Lore client with EpicGames/lore.
- Headings use a lighter accent tint, spacing is more relaxed on every tab, the server address has a copy icon,
  and the "About locks" note is removed.

## 0.2.0

- New Settings tab with Identity, server address and Gemini settings.
- Revision descriptions keep line breaks and bullets, and are shown as a list.
- Server tab with update checks for the Lore client and the plugin.

## 0.1.x

- Lore History window in the style of UEFN's revision control: History, Changes, Locks and Server tabs.
- One-click Submit (stage, commit, push) with optional Gemini description.
- Own locks are remembered and restored after a server restart; locks are released per file after a submit.
- File type colours, readable asset names, and revert/restore without rewriting history.
- Lore provider for Unreal Engine 5.5 - 5.8 from [ScottRaffertyCG/lore-ue](https://github.com/ScottRaffertyCG/lore-ue).
