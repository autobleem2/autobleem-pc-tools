#!/usr/bin/env bash
# fetch_platform_tools.sh DEST - Android's fastboot client for LastResortRecovery, into DEST/platform-tools/:
# fastboot.exe, the two DLLs it needs (AdbWinApi.dll loads AdbWinUsbApi.dll), Google's NOTICE.txt (the
# licences, Apache-2.0 and the rest) and source.properties (which release). Pinned to one release and its
# sha256, so a package never changes under us; bump both together (the zip is cached in
# ${AB_PLATFORM_TOOLS_CACHE:-~/.cache/autobleem-platform-tools}).
#
# The package step of build.yml calls it for the staged LastResortRecovery/ folder; locally:
#   tools/fetch_platform_tools.sh build_dev/apps/lastresort
set -euo pipefail

VERSION=37.0.1
SHA256=45f4d63113e895ebde0c90f194099a4676b6ac653bd28d54314a9e022bbc1a99
URL="https://dl.google.com/android/repository/platform-tools_r${VERSION}-win.zip"
FILES=(fastboot.exe AdbWinApi.dll AdbWinUsbApi.dll NOTICE.txt source.properties)

dest="${1:?usage: fetch_platform_tools.sh DEST}"
cache="${AB_PLATFORM_TOOLS_CACHE:-$HOME/.cache/autobleem-platform-tools}"
zip="$cache/platform-tools_r${VERSION}-win.zip"
mkdir -p "$cache"

sum() { sha256sum "$1" | cut -d' ' -f1; }
if [ ! -s "$zip" ] || [ "$(sum "$zip")" != "$SHA256" ]; then
    echo "==> downloading platform-tools r$VERSION"
    curl -fsSL --retry 3 -o "$zip.part" "$URL"
    got="$(sum "$zip.part")"
    [ "$got" = "$SHA256" ] || { echo "platform-tools r$VERSION: sha256 $got, expected $SHA256" >&2; rm -f "$zip.part"; exit 1; }
    mv "$zip.part" "$zip"
fi

out="$dest/platform-tools"
rm -rf "$out" && mkdir -p "$out"
if command -v unzip >/dev/null 2>&1; then
    for f in "${FILES[@]}"; do unzip -q -j -o "$zip" "platform-tools/$f" -d "$out"; done
else
    "$(command -v python3 || command -v python)" -c '
import sys, zipfile, os
zip, out, names = sys.argv[1], sys.argv[2], sys.argv[3:]
with zipfile.ZipFile(zip) as z:
    for n in names:
        with open(os.path.join(out, n), "wb") as f:
            f.write(z.read("platform-tools/" + n))
' "$zip" "$out" "${FILES[@]}"
fi
for f in "${FILES[@]}"; do [ -s "$out/$f" ] || { echo "platform-tools lacks $f" >&2; exit 1; }; done
echo "==> $out: $(ls "$out" | tr '\n' ' ')"
