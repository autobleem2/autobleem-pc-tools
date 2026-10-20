# autobleem-pc-tools - developer map

PC-side programs for AutoBleem: small Win32 GUI tools with no SDL and no AutoBleem theme. Each is one
statically linked exe. The jobs behind them (install, flash, update) mostly live in the `autobleem-core`
submodule, which supplies `ab_core` and `ab_installer`. This repo holds the windows, the tools' own cores
and the packaging. The top `CMakeLists.txt` header describes the repo the same way.

## The tools

| Exe (OUTPUT_NAME) | CMake target | Folder | What it is |
|---|---|---|---|
| `AutoBleemInstaller.exe` | `installer` | `apps/installer` | Installs AutoBleem on a PlayStation Classic stick |
| `AutoBleemWinSetup.exe` | `winsetup` | `apps/installer` | Setup helper of the Windows product (run by `installer/windows/autobleem.nsi`) |
| `UpdateRoms.exe` | `updateroms` | `apps/updateroms` | Scans the stick's ROMs on the PC and writes the playlists |
| `AutoBleemFlasher.exe` | `flasher` | `apps/flasher` | Writes the PC USB stick image from Windows |
| `LastResortRecovery.exe` | `lastresort` | `apps/lastresort` | Restores a console that no longer starts, from its LBOOT.EPB over fastboot |
| `LanShare.exe` | `lanshare` | `apps/lanshare` | Publishes games to an abstored LAN server (plan: `docs/lan-share-plan.md`) |

Each `apps/<tool>/CLAUDE.md` has the detail for that tool. Read it before changing the tool.

## Layout

- `CMakeLists.txt` - top level. Sets `AB_TARGET`, turns the core's tests and SDL UI off, adds `autobleem-core`
  and the `apps/` folders, then `tests/`.
- `autobleem-core/` - git submodule (see below).
- `apps/common/` - code shared by the tools. `ui_theme` (Red Hat Text fonts, icons, Windows only) and
  `channel_choice` (Release / Testing / Nightly choice, built on every host).
- `apps/<tool>/src` - the Win32 window, `main.cpp`, `.rc` and `.manifest`. `src/core/` (where present)
  holds the portable logic that the tests build natively.
- `apps/<tool>/resources/` - `README.txt` and the like. CI copies the folder next to the exe.
- `apps/lastresort/third_party/libwdi/` - vendored libwdi sources and headers (LGPL, `COPYING-LGPL`).
- `tests/` - the tools' own unit tests (`tests/apps/test_*.cpp`).
- `tools/` - `fetch_platform_tools.sh`, `make_installer_bundle.sh`, `make_updateroms_bundle.sh`,
  `make_pcb_diagram.py`.
- `installer/windows/autobleem.nsi` - NSIS script for the Windows product. Its header says the staged folder
  comes from `tools/make_win_package.sh`, which is not in this repo.
- `.signpath/artifact-configuration.xml` - SignPath config for the six exes. It is a copy for reference,
  because SignPath reads its own copy.
- `docs/lan-share-plan.md` - the only file in `docs/`.

## Build

CI is `.github/workflows/build.yml`. Every job is gated by `vars.AB_CI_ENABLED == 'true'` except `release`,
`nightly` and `preview`, which are gated only by their ref and event conditions. The build and test jobs run in the
`ghcr.io/autobleem2/autobleem-build` image (`develop` tag, `latest` on `v*` tags). They check out with
`submodules: recursive`.

**Windows exes: a Linux cross-compile** (llvm-mingw, UCRT, toolchain file `/opt/win-llvm.cmake` in the image):

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=/opt/win-llvm.cmake
    cmake --build build

The `package` job builds Release and runs
`cmake --build build_rel --target installer winsetup updateroms flasher lastresort lanshare`.
It then strips the exes (not UPX-packed, because Defender flags packed unsigned exes), runs
`tools/fetch_platform_tools.sh` for `LastResortRecovery/platform-tools/` and uploads the staged folder.
`sign`, `release` (`v*` tags), `nightly` (`develop`), `preview` (manual run with `channel=preview`) and `site`
(publishes LAN Share) follow it.

**Native Linux or dev host** (what the `test` job does; `AB_TARGET` defaults to `dev`, and only `dev` and
`win` are accepted):

    cmake -S . -B build_test -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build_test --target test_updateroms_core test_lastresort_core test_lanshare_core test_channel_choice
    ctest --test-dir build_test --output-on-failure -j4

- The tool tests are on by default (`AB_TOOL_TESTS`). They use the core's `tests/harness.cmake` (`ab_add_test`).
- Four suites exist: `test_updateroms_core`, `test_lastresort_core`, `test_lanshare_core`, `test_channel_choice`.
- The installer and flasher job suites run in autobleem-core (`tests/CMakeLists.txt`). The Windows cross
  build only compiles; it cannot run tests.

**Windows with MSYS2 UCRT64**: the scripts in `tools/` say they are run from that shell. They configure
with Ninja and `-DAB_BUILD_TESTS=OFF -DAB_ENABLE_CHD=ON`.

- `tools/make_installer_bundle.sh <autobleem-psc-<v>.tar.gz> [--out DIR] [--clean] [--exe EXE]` builds
  `build_installer/` and writes `AutoBleemInstaller-<v>.zip`.
- `tools/make_updateroms_bundle.sh [--clean]` builds `build_updateroms/` and writes `build_win/UpdateRoms/`.
- These scripts are not run by CI. They still configure with `-DAB_ENABLE_CHD=ON`, which the current top-level
  `CMakeLists.txt` does not mention, so check that they still work before relying on them.

`apps/common` (`ui_theme`) and the tools' Win32 targets are `if (WIN32)` only. The `*_core` libraries and
`channel_choice` build on every host.

## The autobleem-core submodule

`.gitmodules` declares `autobleem-core` from `https://github.com/autobleem2/autobleem-core.git`. The pin is
a commit, with no branch tracking (`git submodule status` shows it).

    git submodule update --init            # first checkout
    git -C autobleem-core fetch origin
    git -C autobleem-core checkout <commit-or-tag>
    git add autobleem-core && git commit -m "Bump autobleem-core to <short sha>"

Build and run the tests after a bump, because the tools use core headers directly (`ab_core`, `ab_installer`).
Changes to an installer or flasher job are made in autobleem-core, not here.

## Strings and translations

None: the tools are English only. There are no translation tables or locale files; UI text is English
literals in the `*_window.cpp` files and in each `resources/README.txt`. The launcher's `_()` strings and
language files live in the launcher.

## Where else to look

- The core: `https://github.com/autobleem2/autobleem-core` (the submodule): its `CLAUDE.md` and
  `docs/developer-guide.md`, after `git submodule update --init`.
- CI image and shared workflows: `autobleem2/autobleem-build` (`route.yml`, the `signpath-sign` and
  `nightly-release` actions that `build.yml` uses).
- The launcher: `autobleem2/autobleem`. Several `apps/*/CLAUDE.md` files cite plans such as
  `docs/pc-targets-plan.md` and `docs/retroarch-scanner-plan.md`. Those files are not in this repo's
  `docs/`, so they live in another repo.
- Packaging consumers named in `build.yml`: `autobleem-appliance` (`assemble-psc.sh` builds the site zips) and
  `autobleem-repo` (download site, `tools/repo_publish.sh`).

## Host test
- `ci/host-test.sh [build-dir]`: cmake (Ninja, `AB_TARGET=dev`) configure, build, `ctest --output-on-failure`; the exit code is the result.
- Needs cmake, ninja and a C++14 compiler, and the submodule initialised (`git submodule update --init`).
- The Windows-only programs (`win32_setup_window.cpp`, the `.rc` files) are not covered: they need the llvm-mingw cross build (`build.yml`).
