# LAN Share - the Windows app for the Store's LAN server

`LanShare.exe` puts the owner's PS1 games on an **abstored** running elsewhere on the home network (a Pi, a NAS,
another PC) and reads a PS1 disc from the PC's drive for it. The design and its history is
`docs/lan-share-plan.md`; the user's side is the manuals' section 5.2 and `resources/README.txt`.

## What is where

- **The server side is core's** (`autobleem-core`, `lib_ableem/.../lanserver/`, namespace `ableem`):
  - `LanLibrary` (the read-only scan, several roots), `HttpServer`, `LanServer` (the routes, the watcher, the
    hasher, `/status.json`, the opt-in uploads and `DELETE /games/<id>`);
  - `LanClient` (plain HTTP to a server: status, upload with resume, commit, remove) and `Publisher` (a game
    to a server - through its share, else uploaded - `remove`, `filesOf`, `serverHas`, `folderNameFor`).
  - ext_store's `abstored` is the command line around `LanServer`.
- **The disc reader is core's** `DiscReader` (`ab_core`, `core/services/disc_reader.*`) over the `CdDrive`
  interface; this app's `WinCdDrive` (`src/win_cd_drive.*`) is the Windows back end (the storage stack's
  CD-ROM IOCTLs, `RawWithSubCode` for the Q channel, 20 sectors a request).
- **Here**:
  - `lanshare_core` (`src/core/lanshare_settings.*`): `settings.ini` in `%LOCALAPPDATA%\AutoBleem LAN Share\`
    and the local server's `LanServer::Config`. Builds everywhere; tested in `tests/apps/test_lanshare_core.cpp`.
  - `src/win32_window.cpp`: the window. Nothing slow on its thread - the server's status (every 10 s), the
    folder's scan, the local server's start and the job (a disc read, a publish, a removal) each have a
    thread; a 500 ms timer moves their state into the controls.
  - `src/main.cpp`: the window, `--tray` (Windows' start-up entry), and `--list-drives` / `--read-disc` for
    reading a disc from a console.

## Rules

- **The server is remote**; sharing from this PC is an option, off by default (the owner, 2026-09-24).
- **Never a second copy**: a game the server has (by serial, else title) is skipped, a read disc it has is not
  sent. **Never a deletion**: removal moves a game into the server's `.removed/`.
- **Plain HTTP, a home network**: no TLS, the upload token is the only guard; abstored is read only unless
  started with `--allow-uploads`.
- Nothing is decrypted: a PS1 disc is plain data, the Q channel is read as it is.
- Windows only (the window, the drive); built like UpdateRoms (`-mwindows`, static, one exe, never UPX).
  CI: built and staged into `pc-tools-win64-<v>.zip` (`stage/LanShare/`), signed when SignPath is on, and
  published to the site as `lanshare-windows-x86_64-<v>.zip` (the `site` job; the Store page's LAN server tab).
