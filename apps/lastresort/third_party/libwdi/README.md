# libwdi's pki.c (vendored)

From [pbatard/libwdi](https://github.com/pbatard/libwdi) at `30df0c0e051b0132c4b9ebed8c054bc8eb3aaaec`
(2025-07-17), LGPL-3.0-or-later (`COPYING-LGPL`). Unchanged: `pki.c`, `mssign32.h`, `msapi_utf8.h`,
`stdfn.h`, `installer.h`, `libwdi.h`. Ours: `config.h` (empty - no autotools), `logging.h` (libwdi's
`wdi_*` log macros onto one callback), `pki_glue.c` (the Windows version pki.c asks for, the log sink,
`wdi_windows_error_str`) and `pki.h` (the two functions LastResortRecovery calls).

What it is used for: `CreateCat` makes the security catalog of the console's WinUSB driver package and
`SelfSignFile` signs it with a certificate made for it and trusted by the machine, whose private key is
then deleted - Zadig's way of installing WinUSB for a device without test signing. See
`../../src/win32_console.cpp` (`installConsoleDriver`).
