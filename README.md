# Lore tools for Unreal Engine

Two things for teams that use [Lore](https://github.com/EpicGames/lore) with Unreal Engine:

| | What it is |
| --- | --- |
| **[Lore+](Desktop/)** | A desktop app: all projects on your server as cards, sync and clone, history, users and access, categories, and one-click updates of the Lore client and the plugin. |
| **[Lore Source Control](Plugin/LoreSourceControl/)** | The Unreal Editor plugin: revision control provider plus a Lore Revision Control window with History, Changes, Locks, Server and Settings tabs, and AI-written descriptions. |

They work together but neither needs the other.

![Revision Control Settings panel showing Lore selected as the provider](Media/revision-control-settings.png)

## Lore+

- **Servers and sign-in** - add one or more Lore servers and sign in with your user name and password (or an API key). The password is never stored; the sign-in token goes in the Windows credential store.
- **Projects like the Unreal project browser** - thumbnail, name and Unreal version from the project itself (both can be changed in the app), and whether you are up to date, behind, or not on this PC at all.
- **Sync, clone, open** - clone a project from the server, sync it, or open it in the right engine version.
- **Put a local project on the server** - the app creates the project, adds a `.loreignore` for Unreal's generated folders, commits and pushes.
- **History per project** - who changed what and when, with bullet lists shown properly.
- **Categories and sub-categories** - group projects (for example by product). Right-click a category to choose who may use all projects in it.
- **Administration** (administrators only) - add users, make administrators, disable or delete users, reset passwords, and set which projects each person can use, per project or as a matrix.
- **Updates** - checks the Lore client, the Unreal plugin and the app itself, installs them, and can do so automatically. The plugin can be installed into any Unreal Engine on the PC (built with that engine's own tools) or into a single project.

Users, grants and projects are managed through the admin API of
[lore-auth-server](https://github.com/dy-sh/lore-auth-server) (`admin_api = true` in its config), so Lore+ fits servers that use that authentication service. Everything else works through the `lore` command-line client.

Windows only for now. Build instructions are in [Desktop/README.md](Desktop/README.md).

## Lore Source Control (Unreal plugin)

See [Plugin/LoreSourceControl/README.md](Plugin/LoreSourceControl/README.md).

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

## Credits and license

The Lore provider inside the plugin started from [ScottRaffertyCG/lore-ue](https://github.com/ScottRaffertyCG/lore-ue) (MIT, Pyre Labs).
Everything else here is new. Released under the MIT License, see [LICENSE](LICENSE) and [NOTICE](NOTICE).
Unreal Engine and Epic Games are trademarks of Epic Games, Inc. Lore is a project of Epic Games, Inc. These tools are independent.
