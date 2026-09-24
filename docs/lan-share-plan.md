# AutoBleem LAN Share - plan

A Windows program that shares the owner's PS1 games with the AutoBleem Store on the home network. It is
`abstored` (ext_store's LAN server, `server/`) with a window: it picks the game folders, serves them, sits in
the tray, and reads a game straight from the PC's CD drive into the library. Asked for by the owner on
2026-09-24, once the Store could read a LAN source and abstored ran on a PC and a Pi.

## What it does

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
  the rdb. A game on several discs asks for the next one and puts them together as `<Title> (Disc N)`, the
  layout the launcher's scanner merges. The new game is served as soon as it is written.

Nothing here decrypts or unlocks anything. A PS1 disc is plain data; the subchannel that LibCrypt games
check is read as it is on the disc, which is what `.sbi` files hold and what emulators expect.

## Shape

- **In `autobleem2/autobleem-pc-tools`**, as `apps/lanshare/` (`LanShare.exe`), next to UpdateRoms. The same
  build as UpdateRoms: a plain Win32 window (no SDL), `-mwindows`, statically linked, one exe, signed through
  SignPath like the others, never UPX-packed.
- **The server code is shared, not copied.** Today `LanLibrary`, `HttpServer` and the status page are
  ext_store's `abstored_core` (`server/src/`). They move into **autobleem-core** as an SDL-free library,
  `ab_lanserver`, linking `ableem_engine` only, with their tests (`test_store_server` today). ext_store's
  `abstored` then links it from core, and LAN Share takes it through pc-tools' `autobleem-core` submodule.
  One server, two front ends.
- **Several folders**: `LanLibrary` serves one `gamesDir` today. It grows a list of roots. A game's id and
  its `/files/` path are prefixed by its root's name when there is more than one root, so two libraries can
  both hold `Tekken 3/`. abstored keeps a single root and unchanged URLs.
- **The disc reader** is `DiscReader` in core's engine (the interface, the `.cue`/`.sbi` writing and the
  multi-disc naming, all tested against a fake drive) with one Windows back end in pc-tools:
  - `CreateFile("\\.\D:")` + `IOCTL_CDROM_READ_TOC_EX` (the full TOC: every track, data or audio, and the
    lead-out).
  - Each sector through `IOCTL_CDROM_RAW_READ` at 2352 bytes (`YellowMode2` for data, `CDDA` for audio), with
    `RawWithSubCode` (2448) when the drive supports it, for the subchannel.
  - LibCrypt: the sectors whose subchannel Q is not what its position says are written to `.sbi`. Only when
    the serial is a known LibCrypt title, or the Q data says so. A drive that cannot return subchannel data
    still makes a playable image for every other game, and says so for a LibCrypt one.
  - Read errors are retried, then reported with the sector. The image is checked against the rdb's CRC for
    that serial when the rdb has one: "matches the known good dump" or "does not match".

## Steps

Each step is one commit, or a core commit plus a submodule bump, with its tests.

1. **`ab_lanserver` in autobleem-core**: `LanLibrary`, `HttpServer` and the status page moved from ext_store,
   and their tests with them. ext_store's `server/` becomes `main.cpp` over core's library. Its standalone
   build and `INSTALL-linux.md` stay as they are, and abstored's output is byte-identical before and after.
2. **Several roots** in `LanLibrary`: `Config::roots` (name + folder), prefixed ids and paths when there is
   more than one root, and the fingerprint over all of them. Tested with two roots holding the same game
   folder name.
3. **`DiscReader`** (core, engine): the `CdDrive` interface (TOC, raw sector, subchannel), `.bin`/`.cue`
   writing (multi-track, audio pregaps), `.sbi` from the subchannel, the rdb check, the title, and the
   multi-disc naming. Tested against a fake drive built from `tests/data` images.
4. **The Windows drive** (`WinCdDrive`, pc-tools): the IOCTLs above, drive listing (`GetLogicalDrives` +
   `GetDriveType == DRIVE_CDROM`), media change. Tested by hand with a real disc (see step 7).
5. **`LanShare.exe`** (pc-tools `apps/lanshare/`): the window - the libraries list (Add / Remove), the
   status (URL + Copy, port, games / problems counts, the current downloads), the games list with each
   game's problems, **Open the status page**, **Read a disc** (drive, progress, per-disc prompt, result), the
   tray icon and menu, **Start with Windows**, the settings in `%LOCALAPPDATA%`. The first start triggers
   Windows' own firewall prompt: the window says to allow **private** networks only.
6. **Packaging and CI**: `tools/make_lanshare_bundle.sh`, the workflow's build + SignPath + release, and
   the download site - an entry next to UpdateRoms and a line on the Store page's LAN server tab.
7. **On hardware**: a PC with a DVD drive and real discs - a single-disc game, a multi-disc game, a game with
   CD audio, a LibCrypt game (PAL) - read, served, installed through the Store on the Pi 400 and the console,
   and played.
8. **Docs**: a LAN Share section in the manuals (English and Polish, with shots), pc-tools' CLAUDE.md, and
   the Store plan's LAN source part pointing here.

## Open questions

- **CHD output**: a read disc as `.chd` would be a third of the size. The vendored libchdr only reads.
  Writing CHD means vendoring chdman's compressor (MAME's code, a much larger import) or shipping `chdman.exe`.
  For now `.bin`/`.cue`, and CHD can come later.
- **HTTPS / a password**: abstored is plain HTTP for a home network, and LAN Share is the same. If LAN Share
  is ever meant to be reachable from outside, that is a separate design, not an option here.
- **A drive's read offset**: an exact dump (redump-grade) needs the drive's read offset and audio
  correction. The rdb CRC check shows whether a dump is exact. Offset correction is a later step, if it is
  needed at all for playing.
