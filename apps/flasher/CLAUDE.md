# AutoBleemFlasher (`apps/flasher`, 2026-09-23)

Writes the PC USB stick's image (`autobleem-<v>-pcusb-i386.img.xz`) onto a stick from Windows - the
PC-stick counterpart of AutoBleemInstaller, in its look. Linux and macOS users keep `xzcat | dd`.

- **The job is in autobleem-core**: `installer/flasher_job.*` (`FlasherJob`, `DiskTarget`, `FlashOptions`),
  tested there over a memory disk (`tests/installer/test_flasher_job.cpp`). The channel's image comes from
  `pc/images/release.json` / `testing.json` or `nightly/latest.json`'s `pc-i386` (each falling back like the
  installers' channels; autobleem-repo's `repo_index.py` writes them); it is downloaded to
  `%TEMP%\AutoBleemFlasher` (kept for the next stick, an older image removed), checked against its sha256,
  decoded with `ableem::XzFile` as it is written in 4 MiB chunks - **the first chunk (the partition table)
  last**, so Windows mounts nothing half written - and read back and compared.
- **This app** is the Windows side: `win32_disk.*` (the disks - USB/SD/MMC bus, media in, not the Windows
  disk, and *removable* unless "Show USB hard drives too" / `--allow-fixed`: the owner's 2 TB USB backup
  drive is on the same bus as a stick and only the removable-media flag tells them apart; `WindowsDisk`
  locks and dismounts **every** volume on the disk, letterless ones too, then writes `\\.\PhysicalDriveN`
  unbuffered through a page-aligned buffer), `flasher_window.*` (channel + "An image file on this PC...",
  the stick, the verify box, two confirmations before a write, a harder one for a hard drive), `main.cpp`
  (`--list`, `--quiet --disk N --yes`). WinINet comes from `../installer/src/win32_platform.cpp`.
- **requireAdministrator** in `flasher.manifest` - the one AutoBleem program that asks (the owner agreed:
  a raw disk cannot be written per user). To look at the window without elevation, run it with
  `__COMPAT_LAYER=RunAsInvoker` in the environment: the listing works, a write is refused.
- **Never point a test write at a real disk of the PC** (the dev PC's disk 6 is the owner's backup drive).
  The write path is proven by the core suite and, on hardware, by a stick the owner writes and boots.
- Shipped as `AutoBleemFlasher/` in `pc-tools-win64-<v>.zip`; autobleem-appliance's pcusb assembly makes
  `AutoBleemFlasher-<v>.zip` of it, the site's `flasher` package kind (the PC stick panel).
