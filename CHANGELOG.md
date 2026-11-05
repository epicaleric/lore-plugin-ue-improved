# Changelog

All notable changes to this plugin. Versions follow `VersionName` in `LoreSourceControl.uplugin`.

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
