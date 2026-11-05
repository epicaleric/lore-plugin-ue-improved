# Lore Source Control for Unreal Engine

Revision control for Unreal Engine projects on [Lore](https://github.com/EpicGames/lore), with a
**Lore History window** that works like the revision control in UEFN: a timeline of revisions, a check-in
tab with one-click submit, file locks, server status and AI-written descriptions.

It adds Lore as a provider next to Perforce, Git and Subversion in **Revision Control**, and adds a window
that shows what your team has done, lets you go back to an earlier version, and submits your own work.

![Revision Control Settings panel showing Lore selected as the provider](Media/revision-control-settings.png)

## What you get

- **History** - every revision with author, date and description. Expand one to see the files that changed.
  Go back to a revision for the whole project or for a single file. Nothing is ever rewritten: going back
  creates a new revision.
- **Changes** - tick the files you changed, write a description (or let the AI write it) and press **Submit**.
  The plugin stages, commits and pushes in one step. Optionally keep your files checked out.
- **Locks** - see who has which file checked out, and unlock files when needed. Your own locks are
  remembered and restored after the Lore server restarts.
- **Server** - whether the server is online, how fast it answers, repository facts, your login, and a check
  for newer versions of the Lore client and of this plugin.
- **Settings** - accent colour and avatar colour (any colour), identity name, the server address, and the
  Gemini settings.
- **Auto check-out** - saving an asset checks it out for you (`Lore.AutoCheckOutOnSave`, on by default).
- **Status overlays** in the Content Browser for modified, added, deleted and locked assets.

## AI descriptions (optional)

Press **Generate** in the Changes tab and Google Gemini writes the description from the file names and change
types. File contents are never sent.

- Written in the past tense, as a record of what was done, in any language you type in the settings.
- Uses the newest Gemini *flash* model your key can use, and moves on to the next flash model by itself when a
  usage limit is reached.
- The API key is typed by you and stored only in `Saved/Config` on your computer, never in the repository.
- On Google's free tier, prompts may be used to improve Google's products. Use a paid key if that matters.

## Requirements

- Unreal Engine 5.8 (see [compatibility](#compatibility))
- The [`lore` CLI](https://github.com/EpicGames/lore) on your `PATH`, or its path set in the plugin settings
- A Lore working copy for your project (clone your repository with `lore clone`)

## Install

1. Copy the `LoreSourceControl` folder into `<YourProject>/Plugins/`. This repository is that folder.
2. Regenerate project files and build the editor target.
3. In the editor choose **Revision Control > Connect to Revision Control**, then **Lore** as the provider.
4. Open the window from **Window > Developer Tools > Lore History**, or run `Lore.ShowHistory`.

If `lore` is not on your `PATH`, set the executable path in the Lore settings panel.

## Identity and login

The **Identity** setting is a display name passed to `lore --identity`. It is not a login. Authentication
against a Lore server is handled by the `lore` CLI (for example `lore login`). The plugin never stores or sends
passwords. Leave Identity empty to use the name you are logged in with.

## Compatibility

| Unreal Engine | Status |
| --- | --- |
| 5.8 | Built and used daily |
| 5.5 - 5.7 | Inherited from the original plugin, not tested with the History window |

## How it works

Every operation maps to a `lore` command. The plugin reads the command output to build Unreal's source control
state, runs work on background threads and refreshes the overlays on the game thread. Lore keeps locks in
memory on the server, so the plugin saves your own locks in `Saved/Lore/MyLocks.txt` and takes them back for
files you are still editing.

## Known limitations

- Lore is pre-1.0 and changes between releases. Keep the client and the server on the same version.
- The server address is stored in the working copy and cannot be changed from the plugin.
- Diff from the History window is not available yet.
- Changelists, shelving and cross-branch state are not implemented.

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

## Credits and license

This plugin started from [ScottRaffertyCG/lore-ue](https://github.com/ScottRaffertyCG/lore-ue) (MIT, Pyre Labs),
which provides the Lore provider itself. The History window, check-in tab, locks handling, AI descriptions,
settings and the rest of the editor UI were added on top of it here.

Released under the MIT License, see [LICENSE](LICENSE) and [NOTICE](NOTICE). Unreal Engine and Epic Games are
trademarks of Epic Games, Inc. Lore is a project of Epic Games, Inc. This plugin is an independent integration.
