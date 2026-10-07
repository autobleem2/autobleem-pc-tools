#!/usr/bin/env bash
# The installer for a release, locally: AutoBleemInstaller-<version>.zip with AutoBleemInstaller.exe (a plain Win32
# program linked statically - no SDL, no MSYS2 DLLs - built as Release and stripped; never UPX-packed:
# Defender quarantines a packed, unsigned exe as Trojan:Win32/Wacatac.C!ml), README.txt
# and the console package it installs from, autobleem-psc-<version>.tar.gz. The exe looks for the package
# next to itself, so the two stay together.
#
# With --payload the installer that carries everything: AutoBleemInstaller-<version>-full.zip, the same exe and files
# plus the folder autobleem-appliance's tools/psc_bundle.py made (payload/ with bundle.json and the packs the exe
# would download, its README for the full download, the licence files). The exe installs from payload/ when it is next
# to it (no channel; only the BIOS files still come over the net); --online makes it go to the site as the small one
# does. autobleem-appliance's assemble-psc.sh builds the same two zips for a release from the signed exe.
#
#   tools/make_installer_bundle.sh <autobleem-psc-<version>.tar.gz> [--out dist/installer] [--clean]
#   tools/make_installer_bundle.sh <autobleem-psc-<version>.tar.gz> --exe dist/win/AutoBleemInstaller.exe
#   tools/make_installer_bundle.sh <autobleem-psc-<version>.tar.gz> --exe <exe> --payload <psc_bundle.py's out dir>
#
# Run from the MSYS2 UCRT64 shell. The Release build goes into build_installer/ (incremental; --clean
# wipes it), separate from build_win/'s Debug one. The package comes
# from the build server's ci/build.sh psc (dist/psc/), or the download repository's pre-release. --exe
# takes an installer already built (ci/build.sh win leaves one in dist/win/, stripped) and
# builds nothing - the workflow's site job, any Linux host with python3.
set -e
cd "$(dirname "$0")/.."

PACKAGE=""
OUT=dist/installer
CLEAN=0
EXE=""
PAYLOAD=""
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --clean) CLEAN=1; shift ;;
        --exe) EXE="$2"; shift 2 ;;
        --payload) PAYLOAD="$2"; shift 2 ;;
        *) PACKAGE="$1"; shift ;;
    esac
done
[ -n "$PACKAGE" ] && [ -f "$PACKAGE" ] || { echo "usage: $0 <autobleem-psc-<version>.tar.gz> [--out DIR] [--clean] [--exe EXE] [--payload DIR]" >&2; exit 2; }
[ -z "$PAYLOAD" ] || [ -f "$PAYLOAD/payload/bundle.json" ] || { echo "no $PAYLOAD/payload/bundle.json - the folder psc_bundle.py makes" >&2; exit 2; }
NAME=$(basename "$PACKAGE")
VERSION=${NAME#autobleem-psc-}
VERSION=${VERSION%.tar.gz}
PY=$(command -v python3 || command -v python)

BUILD=build_installer
if [ -n "$EXE" ]; then
    [ -f "$EXE" ] || { echo "no $EXE" >&2; exit 2; }
else
    [ "$CLEAN" = 1 ] && rm -rf "$BUILD"
    mkdir -p "$BUILD"
    cmake -G Ninja -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DAB_BUILD_TESTS=OFF -DAB_ENABLE_CHD=ON >/dev/null
    ninja -C "$BUILD" installer
    EXE="$BUILD/apps/installer/AutoBleemInstaller.exe"
fi

STAGE="$BUILD/bundle/AutoBleemInstaller"
rm -rf "$BUILD/bundle" "$BUILD/bundle-full"
mkdir -p "$STAGE" "$OUT"
cp "$EXE" "$STAGE/AutoBleemInstaller.exe"
cp -r apps/installer/resources/. "$STAGE/"
cp apps/common/resources/RedHatText-OFL.txt "$STAGE/" # the licence of the font inside the exe
cp "$PACKAGE" "$STAGE/$NAME"
# a fresh build is stripped here; one from --exe already is
if [ "$EXE" = "$BUILD/apps/installer/AutoBleemInstaller.exe" ]; then
    strip "$STAGE/AutoBleemInstaller.exe"
fi
ZIP="$PWD/$OUT/AutoBleemInstaller-$VERSION.zip"
"$PY" tools/zip_tree.py "$BUILD/bundle" AutoBleemInstaller "$ZIP"
echo "==> $ZIP ($(du -h "$ZIP" | cut -f1)): AutoBleemInstaller.exe ($(du -h "$STAGE/AutoBleemInstaller.exe" | cut -f1)), README.txt, $NAME"

if [ -n "$PAYLOAD" ]; then
    # the full download: the exe and the font licence, then the payload folder's files over them - its README is the
    # full download's, the package is inside payload/ (the exe finds it through bundle.json), so none loose beside it
    FULL="$BUILD/bundle-full/AutoBleemInstaller"
    mkdir -p "$FULL"
    cp "$EXE" "$FULL/AutoBleemInstaller.exe"
    [ "$EXE" != "$BUILD/apps/installer/AutoBleemInstaller.exe" ] || strip "$FULL/AutoBleemInstaller.exe"
    cp apps/common/resources/RedHatText-OFL.txt "$FULL/"
    cp -r "$PAYLOAD/." "$FULL/"
    FULLZIP="$PWD/$OUT/AutoBleemInstaller-$VERSION-full.zip"
    "$PY" tools/zip_tree.py "$BUILD/bundle-full" AutoBleemInstaller "$FULLZIP"
    echo "==> $FULLZIP ($(du -h "$FULLZIP" | cut -f1)): AutoBleemInstaller.exe, payload/ ($(du -sh "$FULL/payload" | cut -f1))"
fi
