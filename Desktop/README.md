# Lore Hub

A desktop app for Lore servers. See the [main README](../README.md) for what it does.

## Build

Needs Windows 10/11, [Node.js](https://nodejs.org) 20+, [Rust](https://rustup.rs) and the Microsoft C++ build tools.

```bash
npm install
npm run tauri dev      # run with hot reload
npm run tauri build    # installer in src-tauri/target/release/bundle/nsis
```

## How it works

- **Frontend:** React and TypeScript in `src/`, styled with plain CSS tokens in `src/styles.css`.
- **Backend:** Rust (Tauri 2) in `src-tauri/src/`.
  - `lore.rs` runs the `lore` command-line client. The sign-in token is handed to it through the `LORE_IDENTITY_TOKEN` environment variable, so it never appears in a command line.
  - `grpc.rs` is a tiny gRPC client for the auth service (start a sign-in session, collect the token, exchange an API key). Messages are encoded by hand, so no protobuf code generation is needed.
  - `lib.rs` has the commands the UI calls: settings, sign-in, admin API requests and health checks. Tokens are kept in the Windows credential store.
  - `system.rs` finds Unreal installations and projects, downloads and installs updates, and installs the plugin.
- **Settings** (servers, folders, categories, accent colour) are stored in the app data folder as `settings.json`. Categories are stored by this app, not by Lore.

## Sign-in flow

1. The app asks the auth service (gRPC `StartAuthSession`) for a sign-in session.
2. It posts the session, user name and password to the service's `/api/sign-in` page, the same checks as the browser form.
3. It collects the user token (`GetAuthSession`) and stores it in the Windows credential store.

## Admin API

Needs `admin_api = true` under `[server]` in the auth service's `config.toml`. Only administrators can use it.
