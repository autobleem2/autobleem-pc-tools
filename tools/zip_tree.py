#!/usr/bin/env python3
"""zip_tree.py PARENT TOP OUT - PARENT/TOP as TOP/... in the zip OUT (zip64 where needed).

Already-compressed files (.gz .zip .png .jpg .xz .7z) are stored, everything else deflated: deflating a 300 MB
.tar.gz a second time costs minutes and saves nothing. What tools/make_installer_bundle.sh zips with, where MSYS2 has
python but not always zip. Only the standard library is needed.
"""
import os
import sys
import zipfile

STORED = (".gz", ".zip", ".png", ".jpg", ".xz", ".7z")


def zip_tree(parent, top, out):
    if os.path.exists(out):
        os.remove(out)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9, allowZip64=True) as z:
        for base, _dirs, files in os.walk(os.path.join(parent, top)):
            for name in sorted(files):
                path = os.path.join(base, name)
                kind = zipfile.ZIP_STORED if name.lower().endswith(STORED) else zipfile.ZIP_DEFLATED
                z.write(path, os.path.relpath(path, parent).replace(os.sep, "/"), compress_type=kind)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    zip_tree(*sys.argv[1:])
