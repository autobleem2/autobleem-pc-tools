# AutoBleem LAN Share - plan

A Windows program that puts the owner's PS1 games on the **LAN server** the AutoBleem Store reads - an
`abstored` (ext_store's `server/`) running on the Pi, a NAS or another PC - and reads a game straight from the
PC's CD drive for it. Asked for by the owner on 2026-09-24, once the Store could read a LAN source and abstored
ran on a PC and a Pi.

**The server is remote** (the owner, 2026-09-24, after the first window served from the PC itself): the app
connects to an abstored on the network by the address typed in, shows its games and problems, and **publishes**
games to it - a disc it read, or games found by scanning a local folder - through the server's **network share**
when one is given (`\\raspberrypi\games`: copied there, then a rescan asked for) or **uploaded over HTTP** to
abstored otherwise (an endpoint abstored offers only when started with `--allow-uploads`, behind a token).
Serving from the PC itself stays as an option, **"Also share from this PC", off by default**.

## What it does

(Steps 1-5 below built the first shape - the app as the server. Steps 9 on make it the remote server's
manager; the bullets here describe the first shape, the "Remote server" section the second.)

- **Libraries**: one or more folders of games, added and removed in the window. Each is served exactly as
  abstored serves a games folder: one folder per game, any depth, read only, the problems shown next to
  each game (a `.cue` naming a missing file, an empty image, a game without a serial).
- **Serving**: the same `/store.tsv`, `/files/`, `/cover/`, `/rescan` and status page as abstored, from the
  same code. The window shows the source URL to type in the Store (`http://<this PC>:8124/store.tsv`, every
  IPv4 address of the PC, a **Copy** button), the port, and what is being downloaded right now.
- **In the background**: closing the window keeps it serving from the tray. **Start with Windows** is a
  checkbox (the current user's `Run` key, nothing machine-wide). The folders are watched as abstored watches
  them, and the checksums are cached in `%LOCALAPPDATA%\AutoBleem LAN Share\`.
- **Read a disc**: with a PS1 disc in the drive, **Read a disc** makes `<library>\<Title>\<Title>.cue` +
  `.bin` (and `.sbi` when the disc has LibCrypt), titled from the serial through the covers databases and
  the rdb. A game on several discs asks for the next one and puts it in the same folder as
  `<Title> (Disc N).bin/.cue`, which the library serves as one game with its discs in order. The new game is
  served as soon as it is written.

Nothing here decrypts or unlocks anything. A PS1 disc is plain data; the subchannel that LibCrypt games
check is read as it is on the disc, which is what `.sbi` files hold and what emulators expect.

## Shape

- **In `autobleem2/autobleem-pc-tools`**, as `apps/lanshare/` (`LanShare.exe`), next to UpdateRoms. The same
  build as UpdateRoms: a plain Win32 window (no SDL), `-mwindows`, statically linked, one exe, signed through
  SignPath like the others, never UPX-packed.
- **The server code is shared, not copied.** Today `LanLibrary`, `HttpServer` and the status page are
  ext_store's `abstored_core` (`server/src/`). They move into **autobleem-core** as an SDL-free library,
  `ableem_lanserver` (in lib_ableem, `<ableem/lanserver/...>`), linking `ableem_engine` only, with their tests (`test_store_server` today). ext_store's
  `abstored` then links it from core, and LAN Share takes it through pc-tools' `autobleem-core` submodule.
  One server, two front ends.
- **Several folders**: `LanLibrary` serves one `gamesDir` today. It grows a list of roots. A game's id and
  its `/files/` path are prefixed by its root's name when there is more than one root, so two libraries can
  both hold `Tekken 3/`. abstored keeps a single root and unchanged URLs.
- **The disc reader** is `DiscReader` in core's `ab_core` (`core/services/disc_reader.*`: the interface, the `.cue`/`.sbi` writing and the
  multi-disc naming, all tested against a fake drive) with one Windows back end in pc-tools:
  - `CreateFile("\\.\D:")` + `IOCTL_CDROM_READ_TOC_EX` (the full TOC: every track, data or audio, and the
    lead-out).
  - Each sector through `IOCTL_CDROM_RAW_READ` at 2352 bytes (`YellowMode2` for data, `CDDA` for audio), with
    `RawWithSubCode` (2448) when the drive supports it, for the subchannel.
  - LibCrypt: the data track's sectors whose subchannel Q is not what its position says (a bad CRC, another
    address) are written to `.sbi`; more than 64 of them is the drive's noise, not LibCrypt, and no `.sbi` is
    written. A drive that cannot return subchannel data still makes a playable image for every other game.
  - Read errors are retried, then reported with the sector. The image is checked against the rdb's CRC for
    that serial when the rdb has one: "matches the known good dump" or "does not match".

## Steps

Each step is one commit, or a core commit plus a submodule bump, with its tests.

1. **Done** (2026-09-24, core `6e466f5`, ext_store `311115a`). **`ableem_lanserver` in autobleem-core**: `LanLibrary`, `HttpServer` and the status page moved from ext_store,
   and their tests with them. ext_store's `server/` becomes `main.cpp` over core's library. Its standalone
   build and `INSTALL-linux.md` stay as they are, and abstored's output is byte-identical before and after.
2. **Done** (2026-09-24, core `5a61aeb`). **Several roots** in `LanLibrary`: `Config::roots` (name + folder), prefixed ids and paths when there is
   more than one root, and the fingerprint over all of them. Tested with two roots holding the same game
   folder name.
3. **Done** (2026-09-24, core `9e11c36`). **`DiscReader`** (`ab_core`): the `CdDrive` interface (TOC, raw sector, subchannel), `.bin`/`.cue`
   writing (multi-track, audio pregaps), `.sbi` from the subchannel, the rdb check, the title, and the
   multi-disc naming. Tested against a fake drive over the fake game's real MODE2 image (`test_disc_reader`).
4. **Done** (2026-09-24, pc-tools `0671fdf`; `LanShare.exe --list-drives` / `--read-disc` until the window). The owner's first disc, Resident Evil 3 - Nemesis (SLES-02698), read bit-perfect: 712,491,360 bytes, CRC-32 `7B248588`, the Redump record's; every Form 1 sector's EDC checked. **The Windows drive** (`WinCdDrive`, pc-tools): the IOCTLs above, drive listing (`GetLogicalDrives` +
   `GetDriveType == DRIVE_CDROM`), media change. Tested by hand with a real disc (see step 7).
5. **Done** (2026-09-24, pc-tools `63e889a`: the app as the server - reshaped by steps 9-13). **`LanShare.exe`** (pc-tools `apps/lanshare/`): the window - the libraries list (Add / Remove), the
   status (URL + Copy, port, games / problems counts, the current downloads), the games list with each
   game's problems, **Open the status page**, **Read a disc** (drive, progress, per-disc prompt, result), the
   tray icon and menu, **Start with Windows**, the settings in `%LOCALAPPDATA%`. The first start triggers
   Windows' own firewall prompt: the window says to allow **private** networks only.
6. **Done** (2026-09-25, pc-tools `edbfe58`, autobleem-repo `9e1bc9b`): no bundle script - the workflow builds
   `lanshare`, stages `LanShare/` (exe + README.txt) in `pc-tools-win64-<v>.zip` (in the SignPath
   configuration; signed once signing is on), and a `site` job publishes `lanshare-windows-x86_64-<v>.zip` to
   `extensions/lanshare/`, listed in the Store page's LAN server tab. **Packaging and CI**: `tools/make_lanshare_bundle.sh`, the workflow's build + SignPath + release, and
   the download site - an entry next to UpdateRoms and a line on the Store page's LAN server tab.
7. **A TODO for the testers** (the owner, 2026-09-25: autobleem-main's `docs/todo.md`, "Testers" - the first
   real disc, step 4, stands for now).
   **On hardware**: a PC with a DVD drive and real discs - a single-disc game, a multi-disc game, a game with
   CD audio, a LibCrypt game (PAL) - read, served, installed through the Store on the Pi 400 and the console,
   and played.
8. **Done** (2026-09-25): the manuals' chapter 5 is "On the PC" - 5.2 LAN Share, English and Polish with its
   picture (launcher `215bf95`, published); `apps/lanshare/CLAUDE.md`; the Store plan's table. **Docs**: a LAN Share section in the manuals (English and Polish, with shots), pc-tools' CLAUDE.md, and
   the Store plan's LAN source part pointing here.

## Remote server (steps 9-13)

- **abstored stays read-only unless told otherwise.** Two things are added to `LanServer` (so abstored and the
  app's optional local server have them alike):
  - `GET /status.json`: the name, the version, the games (id, title, serial, discs, size, the library), the
    problems, whether checksums are still being worked out, whether uploads are on, the free space of each
    library - what the app shows for a remote server, read by a program instead of a person.
  - **Uploads**, only with `--allow-uploads` (a random token made and kept in `--state`, printed at start;
    `--upload-token` sets one). `PUT /upload/<game folder>/<file>?offset=N` (`X-AB-Token`) appends to
    `<root>/.uploading/<game folder>/<file>` - a stopped upload goes on from what is there (`HEAD` says how
    much); `POST /upload/<game folder>?commit` moves the folder into the root under a FAT-safe name
    (" (2)" when taken), then rescans; `DELETE /upload/<game folder>` drops a staging folder. Refused: a
    wrong token, a path with `..` or a separator, a file larger than the free space. With several roots a
    `library=` query names the root.
- **The app** (`LanShare.exe`):
  - **Server**: the address (`http://192.168.68.144:8126`), the token when uploading, and optionally the
    share the server's games folder is on. **Connect** reads `/status.json`; the games and problems are the
    server's; its Store address is the one to copy.
  - **Publish**: **Read a disc** reads into a local staging folder (`%LOCALAPPDATA%\AutoBleem LAN Share\staging`), then publishes it; **Publish games...** scans a chosen local folder (core's `LanLibrary`, read
    only) and lists its games with their problems and whether the server has them already (by serial and
    title), and publishes the ticked ones. Publishing copies to the share when one is set and reachable, else
    uploads over HTTP with progress; then the server is asked to rescan and the list comes back from it.
  - **Also share from this PC** (off by default): the local server of steps 1-5, its folders and port.

## Steps (remote)

Steps 9-13 are **done** (2026-09-24/25; core develop `f681ac0`, ext_store `5c34e0d`, pc-tools `4927155`), with
two things the owner asked for on the way:
- **Removal**: `DELETE /games/<id>` (uploads on, the token) and `Publisher::remove` (the same through the
  share) move a game's folder into `.removed/` next to the games - never deleted; the window's "Remove from
  the server..." asks first.
- **No second copy**: publishing skips a game the server has already (by serial, else by title), and a read
  disc the server has is not sent - "Game (2)" only for two different games of one name.
And one race found by core's Linux CI: a stopped upload's request could still be writing when the resumed
one asked for the staged size. The server now takes one writer a file (409 with the size so far), the client
goes on from that size.

9. **`/status.json` in `LanServer`** (core), tested over HTTP.
10. **Uploads in `LanServer`** (core): the endpoints above, off unless `Config::uploads` is set, tested over HTTP
    (a whole game, a resumed file, a commit under a taken name, a wrong token, a path escape, no space);
    `HttpServer` learns PUT/POST/DELETE with a streamed body.
11. **abstored**: `--allow-uploads [--upload-token T]`; the status page says whether uploads are on;
    INSTALL-linux.md's service section (the upload token, and that the games folder must then be writable
    by the service account).
12. **`LanClient` and `Publisher`** (core): `LanClient` reads `/status.json` and uploads a file with resume over
    plain HTTP (sockets, no TLS - a home network); `Publisher` publishes a game folder through a share or the
    client and asks for a rescan. Tested against a `LanServer` in the same process.
13. **The window, remote first**: the server section, Publish games..., Read a disc publishing to the server,
    "Also share from this PC" off by default; the settings move (server address, token, share, localServer).

**The plan is done** (2026-09-25): steps 1-6 and 8-13; step 7, the hardware tests, is a TODO in autobleem-main's `docs/todo.md`.

## Open questions

- **CHD output**: a read disc as `.chd` would be a third of the size. The vendored libchdr only reads.
  Writing CHD means vendoring chdman's compressor (MAME's code, a much larger import) or shipping `chdman.exe`.
  For now `.bin`/`.cue`, and CHD can come later.
- **HTTPS / a password**: abstored is plain HTTP for a home network, and LAN Share is the same. If LAN Share
  is ever meant to be reachable from outside, that is a separate design, not an option here.
- **A drive's read offset**: an exact dump (redump-grade) needs the drive's read offset and audio
  correction. The rdb CRC check shows whether a dump is exact. Offset correction is a later step, if it is
  needed at all for playing.
