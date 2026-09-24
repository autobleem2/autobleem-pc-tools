# LastResortRecovery (`apps/lastresort`, 2026-09-24)

For a PlayStation Classic that no longer starts and that Sony's recovery (LBOOT.EPB on the stick, MISC
set) cannot fix: the console's own backup written back over USB with Android's fastboot client, the
console in fastboot mode. Shipped inside `AutoBleemInstaller-<v>.zip` (the owner's call: the console
release's Windows zip) as `AutoBleemInstaller/LastResortRecovery/`.

- **The facts it rests on** (none of them were in our repos before; the sources are CyberCX's "PlayStation
  Classic Hacking" write-up and donluca/PSClassic-Debian):
  - fastboot mode is entered by bridging two round pads on **side A** of the board (LM-11,
    1-984-020-11), just right of the "LM-11" silkscreen, while the micro-USB port is connected to a PC.
  - It shows up as USB **0bb4:0c01** ("MediaTek", "Yocto").
  - Its partitions under fastboot are the by-partlabel names: BOOTIMG1/2, SEC_RO, MISC, TEE1/2,
    ROOTFS1/2, GAADATA, USRDATA.
  - The PCB picture (`src/pcb_fastboot.png`) is **our own drawing** (`tools/make_pcb_diagram.py`). The
    only public photo is CyberCX's and it is not ours to ship.
- **LBOOT.EPB** is abflashkit's (autobleem-console-tools, `lboot_backup.*`): a plain zip plus a 4 KB
  trailer ending "autobleem".
  - `boot.img`->BOOTIMG1, `tz.img`->TEE1, `rootfs.ext4`->ROOTFS1 (full backups only),
    `userdata.ext4`->USRDATA.
  - `LbootImage` maps them and flashes the kernel first and the user data last. Unknown entries are
    listed and left alone. A backup without `boot.img` is refused.
  - Another tool's backup (no trailer) is flashed too, after a warning.
  - "Recovery off" is abflashkit's `recovery-off.img` (sixteen zero bytes) flashed to MISC, done last.
- **The job is `RecoveryJob`** (`src/core/`, `lastresort_core`, tested in `tests/apps/test_lastresort_core.cpp`
  against a scripted `Fastboot`). Its order:
  1. The backup is checked, and the room to unpack it.
  2. The console is checked: `fastboot devices` (exactly one), `getvar product`, and `getvar
     partition-size:<P>` for each image. An image bigger than its partition is refused; a console that
     does not answer is flashed anyway.
  3. The backup is unpacked into `%TEMP%\LastResortRecovery\images` with `ZipArchive::extract` and
     removed afterwards.
  4. Each image is written with `fastboot flash`. From here on a stop request is refused.
  5. MISC is written.
  6. `fastboot reboot` runs; a failed reboot is only reported.

  `FastbootOutput` reads what the client prints: "Sending ... (N KB) ... OKAY" gives the bytes for the
  progress bar, "FAILED (...)" the reason, and "< waiting for any device >" means the console was lost
  (the client is then ended).
- **The Windows side** (`win32_console.*`):
  - `findConsoleUsb()` uses SetupDi and CM to report the device, the driver bound to it and any problem
    code.
  - `WindowsFastboot` runs `platform-tools/fastboot.exe` next to the exe and streams its output.
  - **The driver**: Windows has none for 0bb4:0c01, and Google's `android_winusb.inf` (r13) lists only
    18d1 devices, so `installConsoleDriver` writes Microsoft's Windows 8+ WinUSB .inf (`Include=winusb.inf`)
    for that id. The .inf sets `DeviceInterfaceGUIDs` to **AdbWinApi's `{F72FE0D4-...}`**, because
    fastboot.exe enumerates only that interface; plain Zadig WinUSB binds but fastboot sees nothing.
  - The package's catalog is made and self-signed with libwdi's `pki.c` (`third_party/libwdi`,
    LGPL-3.0). The certificate goes into Root + TrustedPublisher and its private key is deleted.
  - The driver is installed with `UpdateDriverForPlugAndPlayDevicesW`, or staged with `SetupCopyOEMInfW`
    when the console is not plugged in.
  - Only that install needs an administrator. The program is `asInvoker` and re-runs itself elevated
    (`--install-driver DIR --log FILE`, `runas`), then reads the log back.
- **The window** (`recovery_window.*`) is the flasher's pattern (worker thread + `State` + 100 ms timer).
  It has four pages:
  1. The backup: `LBOOT.EPB` at the root of every removable drive, or browse.
  2. Fastboot mode: the board picture, the steps, and a live status. `fastboot devices` decides, once a
     second; `findConsoleUsb` only explains a missing driver, and "Install driver" appears then.
  3. The progress: the Stop button becomes "Writing..." once images are written, and close is refused.
  4. "Does the console start again?" (Yes/No, each with its message), then exit.

  The log is `%TEMP%\LastResortRecovery\LastResortRecovery.log`.
- **platform-tools** are Google's, unchanged, pinned by version and sha256 in `tools/fetch_platform_tools.sh`
  (r37.0.1): `fastboot.exe`, `AdbWinApi.dll`, `AdbWinUsbApi.dll`, `NOTICE.txt`, `source.properties`.
  `build.yml`'s package step puts them in `stage/LastResortRecovery/platform-tools/`.
- **Tested on the dev PC**:
  - The whole window, with a stand-in fastboot.exe.
  - `--probe`.
  - `CreateCat` for our .inf.
- **Not yet run**: against a console, and the signing/driver install itself. That needs a console in
  fastboot mode, and it changes the PC's certificate stores, which is the owner's call.
