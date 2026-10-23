#!/usr/bin/env python3
"""make_stick_image.py - build a "stick image" layer from a copy of an AutoBleem stick (TOOLS-6, phase 1).

    make_stick_image.py TREE --label LABEL --out DIR [--include-owner-data] [--force]

TREE is a folder holding a copy of a stick's root (it must have Autobleem/). It is only read, never written.
DIR gets two files:

    autobleem-stick-image-<label>[-owner].tar.gz              the layer, paths relative to the stick's root
    autobleem-stick-image-<label>[-owner].MANIFEST.json       the same MANIFEST.json that is the tar's last member

The layer goes on top of the release tarball (autobleem-psc-<version>.tar.gz); see docs/proposals/stick-image.md
for the layout and the exclusion list. Games, BIOS files and saves are left out unless --include-owner-data is
given - such an image is the owner's own and its name ends in -owner.

stdlib only. The output is deterministic: sorted members, fixed mtime, uid/gid 0, gzip without a name or time.
Exit code 0 = built, 2 = refused (the reason on stderr), nothing written.
"""
import argparse
import fnmatch
import gzip
import hashlib
import io
import json
import os
import re
import sys
import tarfile
import tempfile

TOOL_NAME = "make_stick_image"
TOOL_VERSION = "1.0"
MANIFEST_FORMAT = 1
MANIFEST_NAME = "MANIFEST.json"
FIXED_MTIME = 1577836800  # 2020-01-01T00:00:00Z
MAX_REL_PATH = 1024

# --- exclusions -------------------------------------------------------------------------------------------------
# Matched case-insensitively (a FAT stick copied on Windows keeps whatever case it had).

# a folder or file of this name, at any depth
EXCLUDE_ANYWHERE_NAMES = {
    "$recycle.bin": "windows-system",
    "system volume information": "windows-system",
    "logs": "logs",
    "thumbs.db": "thumbs-db",
}
# a file name pattern, at any depth
EXCLUDE_FILE_PATTERNS = [("*.log", "logs"), ("*.tmp", "tmp")]
# a top-level entry (chkdsk's FOUND.000 ...)
EXCLUDE_TOP_PATTERNS = [("found.*", "found")]
# a path from the stick's root (folder or file, everything below it)
EXCLUDE_PATHS = {
    "system/ssh": "ssh-key",  # a temporary key
    "system/bios": "console-state",
    "system/preferences": "console-state",
    "system/region": "console-state",
    "system/ui": "console-state",
    "system/install": "installer-scratch",  # the installer's download folder
}
# the owner's own data: out of every image unless --include-owner-data
OWNER_DATA_PATHS = [
    "games",  # games, Games/!SaveStates, Games/!MemCards
    "retroarch/roms",
    "retroarch/bios",
    "retroarch/bin/saves",
    "retroarch/bin/savestates",
]

# names that look like a credential. Not copied - unless the file is a plain game/media file by its extension
CREDENTIAL_PATTERNS = [
    ".env", ".env.*", "*token*", "*credential*", "*secret*", "*.pem", "*.key", "*.pfx", "*.p12", "*.ppk",
    "id_rsa*", "id_ed25519*", "id_ecdsa*", "id_dsa*", ".netrc", ".git-credentials",
    "*.pem.*", "*.key.*", "*passw*", ".htpasswd", "passwd*", "shadow", "shadow-", "wpa_supplicant*", "*.kdbx", "*.kdb",
    "*.jks", "*.keystore", "keystore*", "api_key*", "apikey*", "api-key*", ".npmrc", ".pypirc", ".pgpass",
    "cookies.sqlite", "cookies*.sqlite",
]
# folders (or files) of these names hold credentials: left out whole, at any depth, and reported
CREDENTIAL_DIR_NAMES = {".ssh", ".git", ".aws", ".gnupg", ".config", ".kube", ".docker", "secret", "secrets"}
MEDIA_EXTENSIONS = {
    ".bin", ".cue", ".img", ".iso", ".chd", ".pbp", ".ecm", ".m3u", ".sbi", ".cdi", ".gdi", ".mdf", ".mds", ".ccd",
    ".sub", ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".ico", ".mp3", ".ogg", ".wav", ".flac", ".mp4",
    ".mkv", ".avi", ".zip", ".7z", ".rar", ".srm", ".state", ".mcr", ".mcd", ".ttf",
}
ARCHIVE_EXTENSIONS = {".zip", ".7z", ".rar"}
# dotfiles and key-file names are never media, whatever the extension
MEDIA_NEVER = re.compile(r"^(\.|id_(rsa|dsa|ecdsa|ed25519))", re.I)
# the secret markers of contractorfiles' media_ok (the same set): a media-named credential file passes only without them
SECRET_MARKER = re.compile(
    rb"-----BEGIN |AKIA[0-9A-Z]{16}|gh[pousr]_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]{22,}|xox[abp]-[A-Za-z0-9-]{10,}"
    rb"|sk-ant-[A-Za-z0-9_-]{10,}|eyJ[A-Za-z0-9_-]{5,}\.eyJ[A-Za-z0-9_-]{5,}\.[A-Za-z0-9_-]*"
    rb"|(?i:passw(?:or)?d|token|secret|api[_-]?key)[ \t]*=[ \t]*[^\s]"
    rb"|[A-Z][A-Z0-9_]{2,}=\S+\r?\n[A-Z][A-Z0-9_]{2,}=\S")  # two KEY=VALUE lines in a row (.env)
MARKER_OVERLAP = 1024

WINDOWS_RESERVED = {"con", "prn", "aux", "nul"} | {"com%d" % i for i in range(1, 10)} | {"lpt%d" % i for i in range(1, 10)}
WINDOWS_BAD_CHARS = set('<>:"|?*\\')


class Refused(Exception):
    """The input or the request is not acceptable; nothing is written."""


def shown(text):
    """A name for a message or the manifest: never fails on bytes that are not UTF-8."""
    return text.encode("utf-8", "backslashreplace").decode("utf-8")


def exclusion_rule(parts, is_dir, include_owner):
    """The rule that keeps the entry out of the image, or None. parts = the path below the stick's root."""
    low = [p.lower() for p in parts]
    rel = "/".join(low)
    name = low[-1]
    for prefix, rule in EXCLUDE_PATHS.items():
        if rel == prefix or rel.startswith(prefix + "/"):
            return rule
    if len(low) >= 2 and "/".join(low[-2:]) == "system/ssh":  # also Autobleem/System/ssh, at any depth
        return EXCLUDE_PATHS["system/ssh"]
    if not include_owner:
        for prefix in OWNER_DATA_PATHS:
            if rel == prefix or rel.startswith(prefix + "/"):
                return "owner-data"
    if name in EXCLUDE_ANYWHERE_NAMES:
        return EXCLUDE_ANYWHERE_NAMES[name]
    if len(parts) == 1:
        for pattern, rule in EXCLUDE_TOP_PATTERNS:
            if fnmatch.fnmatchcase(name, pattern):
                return rule
    if not is_dir:
        for pattern, rule in EXCLUDE_FILE_PATTERNS:
            if fnmatch.fnmatchcase(name, pattern):
                return rule
    return None


def credential_rule(name):
    """The pattern a FILE name matches as a credential name, else None (the extension does not matter here)."""
    low = name.lower()
    for pattern in CREDENTIAL_PATTERNS:
        if fnmatch.fnmatchcase(low, pattern):
            return pattern
    return None


def has_secret_marker(path):
    """True when a secret marker occurs anywhere in the file (read in chunks with an overlap)."""
    tail = b""
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                return False
            buf = tail + chunk
            if SECRET_MARKER.search(buf):
                return True
            tail = buf[-MARKER_OVERLAP:]


def credential_verdict(name, path):
    """None when the name is not credential-like. Else (action, pattern): "skip" (not copied), "copy" (a media/game
    file whose whole content has no secret marker) or "copy-unscanned" (the same, but an archive: its compressed content
    cannot be scanned, so it is copied and listed in the manifest)."""
    pattern = credential_rule(name)
    if not pattern:
        return None
    ext = os.path.splitext(name.lower())[1]
    if ext not in MEDIA_EXTENSIONS or MEDIA_NEVER.search(name):
        return "skip", pattern
    if has_secret_marker(path):
        return "skip", pattern + " (secret marker in the content)"
    return ("copy-unscanned" if ext in ARCHIVE_EXTENSIONS else "copy"), pattern


def check_name(name):
    """Raise Refused for a path component that is unsafe on the stick (FAT/Windows) or in a tar."""
    if name in ("", ".", ".."):
        raise Refused("path component %r is not allowed" % name)
    try:
        name.encode("utf-8")
    except UnicodeEncodeError:
        raise Refused("name is not valid UTF-8: %s" % shown(name))
    if "/" in name or any(ord(c) < 32 or c in WINDOWS_BAD_CHARS for c in name):
        raise Refused("name has a character a Windows/FAT stick cannot hold: %s" % shown(name))
    if name != name.rstrip(" ."):
        raise Refused("name ends in a dot or a space: %s" % shown(name))
    if name.split(".")[0].strip().lower() in WINDOWS_RESERVED:
        raise Refused("reserved Windows name: %s" % shown(name))


def check_relpath(parts):
    """The checks on a whole relative path (also usable on its own)."""
    for part in parts:
        check_name(part)
    if len("/".join(parts)) > MAX_REL_PATH:
        raise Refused("path longer than %d characters: %s" % (MAX_REL_PATH, shown("/".join(parts)[:80] + "...")))


def count_below(path):
    """(files, bytes) of an excluded entry, links not followed."""
    try:
        st = os.lstat(path)
    except OSError:
        return 0, 0
    if not os.path.isdir(path) or os.path.islink(path):
        return 1, st.st_size
    files = size = 0
    for here, dirs, names in os.walk(path, followlinks=False):
        for n in names:
            try:
                size += os.lstat(os.path.join(here, n)).st_size
            except OSError:
                pass
            files += 1
    return files, size


def inside(path, base):
    path, base = os.path.normcase(path), os.path.normcase(base)
    return path == base or path.startswith(base.rstrip(os.sep) + os.sep)


class Plan:
    def __init__(self):
        self.dirs = []        # tuples of parts
        self.files = []       # (parts, abspath, size)
        self.excluded = []    # dicts
        self.credentials = []  # dicts
        self.skipped = []     # dicts
        self.unscanned = []   # credential-named archives, copied, content not scannable


def scan(tree, include_owner):
    """Walk the stick tree (read only) into a Plan; raise Refused on anything the tool will not take."""
    if not os.path.isdir(tree):
        raise Refused("not a folder: %s" % shown(tree))
    root = os.path.realpath(tree)
    try:
        top = sorted(os.listdir(root))
    except OSError as exc:
        raise Refused("cannot read %s: %s" % (shown(tree), exc))
    if not top:
        raise Refused("the tree is empty: %s" % shown(tree))
    if not any(n.lower() == "autobleem" and os.path.isdir(os.path.join(root, n)) for n in top):
        raise Refused("not a stick: no Autobleem/ folder at the top of %s" % shown(tree))

    plan = Plan()
    stack = [((), root)]
    while stack:
        parts, path = stack.pop()
        try:
            with os.scandir(path) as it:
                entries = sorted(it, key=lambda e: e.name)
        except OSError as exc:
            raise Refused("cannot read %s: %s" % (shown("/".join(parts) or "."), exc))
        seen = {}
        for e in entries:
            cparts = parts + (e.name,)
            rel = "/".join(cparts)
            is_link = e.is_symlink()
            is_dir = (not is_link) and e.is_dir(follow_symlinks=False)
            rule = exclusion_rule(cparts, is_dir, include_owner)
            if rule:
                files, size = count_below(e.path)
                plan.excluded.append({"path": shown(rel) + ("/" if is_dir else ""), "rule": rule,
                                      "files": files, "bytes": size})
                continue
            if e.name.lower() in CREDENTIAL_DIR_NAMES:
                plan.credentials.append({"path": shown(rel) + ("/" if is_dir else ""), "rule": "credential folder name"})
                continue
            check_relpath(cparts)
            folded = e.name.lower()
            if folded in seen:
                raise Refused("two names differ only by case (a FAT stick cannot hold both): %s and %s"
                              % (shown(seen[folded]), shown(e.name)))
            seen[folded] = e.name
            if len(cparts) == 1 and folded == MANIFEST_NAME.lower():
                raise Refused("the tree has a %s at its top, the name the image's manifest uses" % MANIFEST_NAME)
            if is_link:
                target = os.path.realpath(e.path)
                if not inside(target, root):
                    raise Refused("symlink out of the tree: %s" % shown(rel))
                plan.skipped.append({"path": shown(rel), "reason": "symlink inside the tree, not followed"})
                continue
            if is_dir:
                plan.dirs.append(cparts)
                stack.append((cparts, e.path))
            elif e.is_file(follow_symlinks=False):
                verdict = credential_verdict(e.name, e.path)
                if verdict and verdict[0] == "skip":
                    plan.credentials.append({"path": shown(rel), "rule": verdict[1]})
                    continue
                if verdict and verdict[0] == "copy-unscanned":
                    plan.unscanned.append({"path": shown(rel), "rule": verdict[1]})
                plan.files.append((cparts, e.path, e.stat(follow_symlinks=False).st_size))
            else:
                plan.skipped.append({"path": shown(rel), "reason": "not a regular file"})
    if not plan.files:
        raise Refused("nothing to put in the image: every file of the tree is excluded")
    plan.dirs.sort()
    plan.files.sort(key=lambda f: f[0])
    plan.excluded.sort(key=lambda x: x["path"])
    plan.credentials.sort(key=lambda x: x["path"])
    plan.skipped.sort(key=lambda x: x["path"])
    plan.unscanned.sort(key=lambda x: x["path"])
    return plan


def active_rules(include_owner):
    rules = sorted(set(EXCLUDE_ANYWHERE_NAMES.values()) | {r for _, r in EXCLUDE_FILE_PATTERNS}
                   | {r for _, r in EXCLUDE_TOP_PATTERNS} | set(EXCLUDE_PATHS.values()))
    if not include_owner:
        rules.append("owner-data")
    return rules


class _HashingReader:
    def __init__(self, fileobj):
        self.f = fileobj
        self.sha = hashlib.sha256()
        self.count = 0

    def read(self, n=-1):
        data = self.f.read(n)
        self.sha.update(data)
        self.count += len(data)
        return data


def artifact_base(label, include_owner):
    return "autobleem-stick-image-%s%s" % (label, "-owner" if include_owner else "")


def build(tree, label, out_dir, include_owner=False, force=False):
    """Make the image. Returns (tar path, manifest path, manifest dict)."""
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,39}", label or ""):
        raise Refused("label must be 1-40 characters of A-Z a-z 0-9 . _ - and start with a letter or digit")
    plan = scan(tree, include_owner)
    root = os.path.realpath(tree)
    out_real = os.path.realpath(out_dir)
    if inside(out_real, root):
        raise Refused("the output folder is inside the input tree; the tree is never written to")
    base = artifact_base(label, include_owner)
    tar_path = os.path.join(out_dir, base + ".tar.gz")
    man_path = os.path.join(out_dir, base + ".MANIFEST.json")
    # a link planted under any of the artifact's names (or their .part names) could lead a write into the tree
    for name in (tar_path, man_path, tar_path + ".part", man_path + ".part"):
        if os.path.islink(name):
            raise Refused("%s is a symlink; the output folder is not trusted" % shown(name))
    if not force and (os.path.exists(tar_path) or os.path.exists(man_path)):
        raise Refused("%s already exists (use --force to replace it)" % shown(tar_path))
    os.makedirs(out_dir, exist_ok=True)

    # temporary files are made fresh (O_EXCL, unique name) so no planted link or file is ever opened for writing
    tar_fd, tmp_tar = tempfile.mkstemp(prefix=base + ".", suffix=".tar.part", dir=out_dir)
    man_fd, tmp_man = tempfile.mkstemp(prefix=base + ".", suffix=".man.part", dir=out_dir)
    raw, mf = os.fdopen(tar_fd, "wb"), os.fdopen(man_fd, "wb")
    entries = sorted([(p, None) for p in plan.dirs] + [(p, (a, s)) for p, a, s in plan.files], key=lambda x: x[0])
    file_records = []
    try:
        with raw:
            gz =gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9)
            tf = tarfile.open(fileobj=gz, mode="w", format=tarfile.GNU_FORMAT, encoding="utf-8")
            for parts, info in entries:
                ti = tarfile.TarInfo("/".join(parts))
                ti.mtime, ti.uid, ti.gid, ti.uname, ti.gname = FIXED_MTIME, 0, 0, "", ""
                if info is None:
                    ti.type, ti.mode = tarfile.DIRTYPE, 0o755
                    tf.addfile(ti)
                    continue
                abspath, size = info
                ti.type, ti.mode, ti.size = tarfile.REGTYPE, 0o644, size
                with open(abspath, "rb") as src:
                    reader = _HashingReader(src)
                    tf.addfile(ti, reader)
                if reader.count != size or os.stat(abspath).st_size != size:
                    raise Refused("file changed while it was read: %s" % shown("/".join(parts)))
                file_records.append({"path": "/".join(parts), "size": size, "sha256": reader.sha.hexdigest()})
            manifest = {
                "tool": TOOL_NAME,
                "tool_version": TOOL_VERSION,
                "format": MANIFEST_FORMAT,
                "label": label,
                "owner_data_included": bool(include_owner),
                "counts": {
                    "files": len(file_records),
                    "dirs": len(plan.dirs),
                    "bytes": sum(r["size"] for r in file_records),
                    "excluded_entries": len(plan.excluded),
                    "excluded_files": sum(x["files"] for x in plan.excluded),
                    "credential_names_not_copied": len(plan.credentials),
                    "skipped": len(plan.skipped),
                    "unscanned_archives": len(plan.unscanned),
                },
                "exclusion_rules": active_rules(include_owner),
                "excluded": plan.excluded,
                "credential_names_not_copied": plan.credentials,
                "skipped": plan.skipped,
                "unscanned-archive": plan.unscanned,
                "dirs": ["/".join(p) for p in plan.dirs],
                "files": file_records,
            }
            data = (json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=True) + "\n").encode("utf-8")
            mi = tarfile.TarInfo(MANIFEST_NAME)
            mi.mtime, mi.uid, mi.gid, mi.uname, mi.gname = FIXED_MTIME, 0, 0, "", ""
            mi.type, mi.mode, mi.size = tarfile.REGTYPE, 0o644, len(data)
            tf.addfile(mi, io.BytesIO(data))
            tf.close()
            gz.close()
        with mf:
            mf.write(data)
        for tmp in (tmp_tar, tmp_man):  # mkstemp makes them 0600
            os.chmod(tmp, 0o644)
        for name in (tar_path, man_path):  # re-checked right before the rename
            if os.path.islink(name):
                raise Refused("%s is a symlink; the output folder is not trusted" % shown(name))
        os.replace(tmp_tar, tar_path)  # a rename replaces the name itself, it never writes through a link
        os.replace(tmp_man, man_path)
    finally:
        for fobj in (raw, mf):  # closing twice is harmless; open only when we failed before writing
            fobj.close()
        for leftover in (tmp_tar, tmp_man):
            if os.path.lexists(leftover):
                os.remove(leftover)
    return tar_path, man_path, manifest


def main(argv=None):
    ap = argparse.ArgumentParser(description="Build an AutoBleem stick image layer from a copy of a stick.")
    ap.add_argument("tree", help="folder with a copy of the stick's root (read only)")
    ap.add_argument("--label", required=True, help="name of the image, e.g. owner-2026-10")
    ap.add_argument("--out", required=True, help="folder for the .tar.gz and the manifest (outside the tree)")
    ap.add_argument("--include-owner-data", action="store_true",
                    help="also put the owner's games, BIOS files and saves in; the artifact name gets -owner")
    ap.add_argument("--force", action="store_true", help="replace an existing image of the same name")
    args = ap.parse_args(argv)
    try:
        tar_path, man_path, m = build(args.tree, args.label, args.out, args.include_owner_data, args.force)
    except Refused as exc:
        print("refused: %s" % exc, file=sys.stderr)
        return 2
    c = m["counts"]
    print("wrote %s" % tar_path)
    print("wrote %s" % man_path)
    print("%d files, %d dirs, %d bytes; %d excluded entries (%d files); owner data %s"
          % (c["files"], c["dirs"], c["bytes"], c["excluded_entries"], c["excluded_files"],
             "INCLUDED" if m["owner_data_included"] else "left out"))
    for cred in m["credential_names_not_copied"]:
        print("credential-like name not copied: %s (%s)" % (cred["path"], cred["rule"]))
    for u in m["unscanned-archive"]:
        print("unscanned-archive copied: %s (%s)" % (u["path"], u["rule"]))
    for s in m["skipped"]:
        print("skipped: %s (%s)" % (s["path"], s["reason"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
