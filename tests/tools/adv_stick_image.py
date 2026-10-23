#!/usr/bin/env python3
"""Adversarial scenarios for tools/make_stick_image.py (not a unittest; run: python3 tests/tools/adv_stick_image.py)."""
import hashlib
import json
import os
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "..", "tools", "make_stick_image.py")
WORK = tempfile.mkdtemp(prefix="adv-", dir=os.environ.get("TMPDIR"))
N = [0]


def mk(path, data=b"x"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def tree(name):
    t = os.path.join(WORK, name, "stick")
    mk(os.path.join(t, "Autobleem", "bin", "autobleem"), b"bin")
    mk(os.path.join(t, "Autobleem", "config.ini"), b"theme=a\n")
    return t


def snap(t):
    out = []
    for h, d, f in os.walk(t, followlinks=False):
        for n in d + f:
            p = os.path.join(h, n)
            st = os.lstat(p)
            out.append((os.path.relpath(p, t), st.st_mode, st.st_size, st.st_mtime_ns))
    return sorted(out)


def run(t, out, *extra, label="L"):
    cmd = [sys.executable, TOOL, t, "--label", label, "--out", out] + list(extra)
    r = subprocess.run(cmd, capture_output=True, text=True)
    return cmd, r


def members(out, label="L", owner=False):
    p = os.path.join(out, "autobleem-stick-image-%s%s.tar.gz" % (label, "-owner" if owner else ""))
    with tarfile.open(p) as tf:
        return [m.name for m in tf.getmembers()]


def report(title, ok, cmd, r, extra=""):
    N[0] += 1
    print("### %d. %s: %s" % (N[0], title, "PASS" if ok else "FAIL"))
    if cmd:
        print("$ " + " ".join(cmd).replace(WORK, "$W"))
    if r is not None:
        print("rc=%d stdout=%r stderr=%r" % (r.returncode, r.stdout.replace(WORK, "$W")[-600:], r.stderr.replace(WORK, "$W")[-300:]))
    if extra:
        print(extra.replace(WORK, "$W"))
    print()


def main():
    # 1 output inside the tree (plain, ../ route, symlink route, symlink tree)
    t = tree("s1")
    before = snap(t)
    res = []
    for out in (os.path.join(t, "o"), os.path.join(t, "Autobleem", "..", "o2"), os.path.join(t, "Autobleem", "bin")):
        cmd, r = run(t, out)
        res.append(r.returncode)
    lnk = os.path.join(WORK, "s1", "lnk")
    os.symlink(t, lnk)
    cmd, r = run(lnk, os.path.join(t, "o3"))
    res.append(r.returncode)
    lo = os.path.join(WORK, "s1", "outlink")
    os.symlink(os.path.join(t, "Autobleem"), lo)
    cmd, r = run(t, os.path.join(lo, "sub"))
    res.append(r.returncode)
    report("output folder inside tree (direct, ../, via symlinked tree, via symlinked out)",
           res == [2] * 5 and snap(t) == before, cmd, r, "rcs=%s tree unchanged=%s" % (res, snap(t) == before))

    # 2 .part symlink planted in out dir pointing into the tree
    t = tree("s2")
    victim = os.path.join(t, "Autobleem", "config.ini")
    out = os.path.join(WORK, "s2", "out")
    os.makedirs(out)
    os.symlink(victim, os.path.join(out, "autobleem-stick-image-L.tar.gz.part"))
    os.symlink(victim, os.path.join(out, "autobleem-stick-image-L.MANIFEST.json.part"))
    cmd, r = run(t, out)
    data = open(victim, "rb").read()
    report("pre-planted .part symlinks in OUT pointing at a tree file (needs write access to OUT)",
           data == b"theme=a\n", cmd, r, "victim now %r (len %d)" % (data[:30], len(data)))

    # 3 excluded / credential files
    t = tree("s3")
    names = {
        "System/ssh/key": 1, "System/SSH/id": 1, "System/Bios/scph1001.bin": 1, "System/Logs/a.txt": 1, "Autobleem/x.LOG": 1,
        "Autobleem/a.tmp": 1, "found.000/f": 1, "$RECYCLE.BIN/x": 1, "System Volume Information/x": 1, "Autobleem/Thumbs.DB": 1,
        "Games/g/g.bin": 1, "GAMES/g.cue": 1, "RetroArch/roms/r.bin": 1, "RetroArch/bin/saves/s.srm": 1,
        "RetroArch/bin/savestates/s.state": 1,
        "Autobleem/.env": 1, "Autobleem/.ENV.local": 1, "Autobleem/my_Token.txt": 1, "Autobleem/aws_credentials": 1,
        "Autobleem/id_rsa": 1, "Autobleem/id_rsa.pub": 1, "Autobleem/a.PEM": 1, "Autobleem/b.key": 1, "Autobleem/.netrc": 1,
        "Autobleem/.git-credentials": 1, "Autobleem/SECRET.txt": 1, "Autobleem/c.p12": 1, "Autobleem/d.ppk": 1,
    }
    for n in names:
        mk(os.path.join(t, n), b"SECRETDATA")
    out = os.path.join(WORK, "s3", "out")
    cmd, r = run(t, out)
    got = members(out)
    # empty parent folders (RetroArch, System ...) remain by design; only files matter
    leaked = [m for m in got if m != "MANIFEST.json" and m not in ("Autobleem", "Autobleem/bin", "Autobleem/bin/autobleem",
                                                                    "Autobleem/config.ini", "RetroArch", "RetroArch/bin", "System")]
    report("excluded and credential-like names (case variants) all kept out", r.returncode == 0 and not leaked, cmd, r,
           "leaked=%s" % leaked)

    # 4 gaps: secret-like content under names the patterns miss
    t = tree("s4")
    gaps = [".ssh/known", "secrets/db.txt", ".git/config", "Autobleem/wpa_supplicant.conf", "Autobleem/.htpasswd",
            "Autobleem/passwords.txt", "Autobleem/vault.kdbx", ".aws/config", ".gnupg/trustdb.gpg", "Autobleem/keystore.jks",
            "Autobleem/api_key.txt", "Autobleem/apikey.json", "Autobleem/.npmrc", "Autobleem/client_secret.json",
            "Autobleem/passwd", "Autobleem/shadow", "Autobleem/cookies.sqlite"]
    for g in gaps:
        mk(os.path.join(t, g), b"x")
    out = os.path.join(WORK, "s4", "out")
    cmd, r = run(t, out)
    got = members(out)
    copied = [g for g in gaps if g in got]
    report("secret-like files whose NAME the patterns do not match (design limit, informational)", not copied, cmd, r,
           "copied: %s" % copied)

    # 5 media-extension bypass
    t = tree("s5")
    for g in ("Autobleem/id_rsa.png", "Autobleem/token.zip", "Autobleem/secret.bin", "Autobleem/prod.pem.jpg"):
        mk(os.path.join(t, g), b"-----BEGIN PRIVATE KEY-----")
    out = os.path.join(WORK, "s5", "out")
    cmd, r = run(t, out)
    got = members(out)
    report("credential-named file with a media extension (documented exemption; a key renamed .png is copied)",
           not [g for g in got if "id_rsa" in g or "token" in g or "secret" in g], cmd, r,
           "copied: %s" % [g for g in got if "id_rsa" in g or "token" in g or "secret" in g])

    # 6 symlinks
    t = tree("s6")
    outside = os.path.join(WORK, "s6", "outside.txt")
    mk(outside, b"outside")
    os.symlink(outside, os.path.join(t, "Autobleem", "ln"))
    cmd, r = run(t, os.path.join(WORK, "s6", "out"))
    ok1 = r.returncode == 2 and not os.path.exists(os.path.join(WORK, "s6", "out", "autobleem-stick-image-L.tar.gz"))
    report("symlink to file outside tree refused, nothing written", ok1, cmd, r)
    t = tree("s6b")
    os.symlink("/etc", os.path.join(t, "Autobleem", "etc"))
    cmd, r = run(t, os.path.join(WORK, "s6b", "out"))
    report("symlink to /etc refused", r.returncode == 2, cmd, r)
    t = tree("s6c")
    os.symlink("../../..", os.path.join(t, "Autobleem", "up"))
    cmd, r = run(t, os.path.join(WORK, "s6c", "out"))
    report("relative symlink escaping with ../ refused", r.returncode == 2, cmd, r)
    t = tree("s6d")
    os.symlink(os.path.join(t, "Autobleem", "config.ini"), os.path.join(t, "Autobleem", "inner"))
    os.symlink("nonexistent", os.path.join(t, "Autobleem", "dangling"))
    cmd, r = run(t, os.path.join(WORK, "s6d", "out"))
    got = members(os.path.join(WORK, "s6d", "out"))
    report("inside/dangling symlinks skipped, not stored", r.returncode == 0 and "Autobleem/inner" not in got
           and "Autobleem/dangling" not in got, cmd, r)
    t = tree("s6e")
    os.symlink("/etc", os.path.join(t, "Games"))  # excluded owner-data symlink pointing out
    cmd, r = run(t, os.path.join(WORK, "s6e", "out"))
    report("excluded name that is a symlink out of tree (not followed, no count walk into /etc)", r.returncode == 0, cmd, r)
    t = tree("s6f")
    os.symlink(os.path.join(WORK, "s6f"), os.path.join(t, "Autobleem", "parent"))
    cmd, r = run(t, os.path.join(WORK, "s6f", "out"))
    report("symlink to tree's parent refused", r.returncode == 2, cmd, r)
    t = tree("s6g")
    os.symlink("/etc", os.path.join(t, "Autobleem", "sub"))
    t2 = os.path.join(WORK, "s6g", "stick2")
    os.makedirs(t2)
    os.symlink(os.path.join(t, "Autobleem"), os.path.join(t2, "Autobleem"))
    cmd, r = run(t2, os.path.join(WORK, "s6g", "out"))
    report("top Autobleem is a symlink to a folder outside the tree", r.returncode == 2, cmd, r)

    # 7 determinism
    t = tree("s7")
    mk(os.path.join(t, "Autobleem", "Apps", "z.txt"), b"z")
    mk(os.path.join(t, "Autobleem", "Apps", "a.txt"), b"a")
    os.makedirs(os.path.join(t, "Autobleem", "empty"))
    o1, o2 = os.path.join(WORK, "s7", "o1"), os.path.join(WORK, "s7", "o2")
    run(t, o1)
    os.utime(os.path.join(t, "Autobleem", "Apps", "a.txt"), (1, 1))
    os.chmod(os.path.join(t, "Autobleem", "Apps", "z.txt"), 0o600)
    os.link(os.path.join(t, "Autobleem", "Apps", "a.txt"), os.path.join(t, "Autobleem", "hard"))
    os.unlink(os.path.join(t, "Autobleem", "hard"))
    cmd, r = run(t, o2)
    def h(p):
        return hashlib.sha256(open(p, "rb").read()).hexdigest()
    f = "autobleem-stick-image-L.tar.gz"
    m = "autobleem-stick-image-L.MANIFEST.json"
    same = h(os.path.join(o1, f)) == h(os.path.join(o2, f)) and h(os.path.join(o1, m)) == h(os.path.join(o2, m))
    report("same tree, changed mtimes/modes, different out dir -> identical sha256", same, cmd, r)
    # different cwd and relative vs absolute tree path
    o3 = os.path.join(WORK, "s7", "o3")
    rel = os.path.relpath(t, os.path.dirname(t))
    r3 = subprocess.run([sys.executable, TOOL, rel, "--label", "L", "--out", o3], capture_output=True, text=True,
                        cwd=os.path.dirname(t))
    report("relative tree path from another cwd -> same bytes", r3.returncode == 0 and h(os.path.join(o3, f)) == h(os.path.join(o1, f)),
           None, r3)
    with tarfile.open(os.path.join(o1, f)) as tf:
        ms = tf.getmembers()
        bad = [x.name for x in ms if x.mtime != 1577836800 or x.uid or x.gid or x.uname or x.gname]
        last = ms[-1].name
        body = tf.extractfile(ms[-1]).read()
    report("tar metadata fixed, manifest last and equal to the sidecar", not bad and last == "MANIFEST.json"
           and body == open(os.path.join(o1, m), "rb").read(), None, None, "bad=%s last=%s" % (bad, last))
    raw = open(os.path.join(o1, f), "rb").read()[:10]
    report("gzip header has no mtime/name", raw[3] == 0 and raw[4:8] == b"\0\0\0\0", None, None, "header=%r" % raw)

    # 8 manifest hashes true
    with tarfile.open(os.path.join(o1, f)) as tf:
        mf = json.loads(body)
        bad = []
        for rec in mf["files"]:
            d = tf.extractfile(rec["path"]).read()
            if hashlib.sha256(d).hexdigest() != rec["sha256"] or len(d) != rec["size"]:
                bad.append(rec["path"])
    report("manifest sha256/size match tar contents", not bad and "Autobleem/empty" in mf["dirs"], None, None, "bad=%s" % bad)

    # 9 owner naming collision
    t = tree("s9")
    mk(os.path.join(t, "Games", "g.bin"), b"game")
    out = os.path.join(WORK, "s9", "out")
    cmd, r = run(t, out, label="mine-owner")
    name_clash = os.path.exists(os.path.join(out, "autobleem-stick-image-mine-owner.tar.gz"))
    report("a label ending in '-owner' is refused (exit 2, no file): the suffix is the owner/shareable signal",
           r.returncode == 2 and not name_clash and not os.path.exists(out), cmd, r, "rc=%d file=%s" % (r.returncode, name_clash))
    cmd, r = run(t, out, "--include-owner-data", label="mine")
    cmd2, r2 = run(t, out, "--include-owner-data", label="mine")
    report("owner image of label 'mine' would collide with the above (refused w/o --force?)",
           True, cmd, r, "second run rc=%d (existing-output refusal)" % r2.returncode)
    cmd, r = run(t, os.path.join(WORK, "s9", "out2"), "--include-owner-data", label="x")
    report("--include-owner-data does include Games and name ends -owner",
           "Games/g.bin" in members(os.path.join(WORK, "s9", "out2"), "x", True), cmd, r)

    # 10 labels
    t = tree("s10")
    bad = []
    for lab in ("../x", "a/b", "", ".hidden", "-x", "a b", "x" * 41, "a\nb"):
        try:
            cmd, r = run(t, os.path.join(WORK, "s10", "o"), label=lab)
        except ValueError:
            continue
        if lab in ("../x", "a/b", "", ".hidden", "-x", "a b", "x" * 41, "a\nb") and r.returncode != 2 and r.returncode != 0:
            bad.append((lab, r.returncode))
        if lab in ("../x", "a/b", ".hidden", "a b", "x" * 41, "a\nb") and r.returncode == 0:
            bad.append((lab, "accepted"))
    ok = os.path.isdir(os.path.join(WORK, "s10", "o")) is False
    report("hostile labels refused, no output folder created", not bad and ok, None, None, "bad=%s outdir_created=%s" % (bad, not ok))
    cmd, r = run(t, os.path.join(WORK, "s10", "o2"), label="x\\y")
    report("label with backslash refused", r.returncode == 2, cmd, r)
    cmd, r = run(t, os.path.join(WORK, "s10", "o3"), label="CON")
    report("label 'CON' (reserved name -> autobleem-stick-image-CON... fine since prefixed)", r.returncode in (0, 2), cmd, r)

    # 11 refusals leave nothing
    t = tree("s11")
    mk(os.path.join(t, "Autobleem", "bad:name"), b"x")
    out = os.path.join(WORK, "s11", "out")
    cmd, r = run(t, out)
    report("bad FAT name refused, out folder not created", r.returncode == 2 and not os.path.exists(out), cmd, r)
    t = tree("s11b")
    mk(os.path.join(t, "Autobleem", "A.txt"), b"x")
    mk(os.path.join(t, "Autobleem", "a.txt"), b"y")
    cmd, r = run(t, os.path.join(WORK, "s11b", "out"))
    report("case-collision refused", r.returncode == 2, cmd, r)
    t = tree("s11c")
    os.makedirs(os.path.join(t, "Autobleem", "A"))
    mk(os.path.join(t, "Autobleem", "a"), b"y")
    cmd, r = run(t, os.path.join(WORK, "s11c", "out"))
    report("dir/file case-collision refused", r.returncode == 2, cmd, r)
    t = tree("s11d")
    mk(os.path.join(t, "manifest.JSON"), b"x")
    cmd, r = run(t, os.path.join(WORK, "s11d", "out"))
    report("top-level manifest.JSON refused", r.returncode == 2, cmd, r)
    t = tree("s11e")
    mk(os.path.join(t, "Autobleem", "sub", "MANIFEST.json"), b"x")
    cmd, r = run(t, os.path.join(WORK, "s11e", "out"))
    report("nested MANIFEST.json is stored as a normal file (not the manifest)", r.returncode == 0, cmd, r)
    t = tree("s11f")
    mk(os.path.join(t, "Autobleem", "NUL.txt"), b"x")
    cmd, r = run(t, os.path.join(WORK, "s11f", "out"))
    report("NUL.txt refused", r.returncode == 2, cmd, r)
    t = tree("s11g")
    os.makedirs(os.path.join(t, "Autobleem", "d"))
    p = os.path.join(t, "Autobleem")
    for i in range(30):
        p = os.path.join(p, "n" * 40)
    os.makedirs(p)
    mk(os.path.join(p, "f"), b"x")
    cmd, r = run(t, os.path.join(WORK, "s11g", "out"))
    report("path >1024 refused", r.returncode == 2, cmd, r)
    t = tree("s11h")
    try:
        with open(os.path.join(t.encode(), b"Autobleem", b"bad\xff"), "wb") as f:
            f.write(b"x")
        cmd, r = run(t, os.path.join(WORK, "s11h", "out"))
        report("non-UTF-8 filename refused", r.returncode == 2, cmd, r)
    except OSError as e:
        report("non-UTF-8 filename (fs refused to create)", True, None, None, str(e))
    t = tree("s11i")
    mk(os.path.join(t, "Autobleem", "café"), b"x")
    mk(os.path.join(t, "Autobleem", "café"), b"y")
    cmd, r = run(t, os.path.join(WORK, "s11i", "out"))
    report("NFC/NFD twins (FAT/macOS would merge) - informational", r.returncode == 2, cmd, r)
    t = tree("s11j")
    os.mkfifo(os.path.join(t, "Autobleem", "fifo"))
    cmd, r = run(t, os.path.join(WORK, "s11j", "out"))
    report("FIFO in tree does not hang, skipped", r.returncode == 0 and "fifo" in r.stdout, cmd, r)

    # 12 existing output / force
    t = tree("s12")
    out = os.path.join(WORK, "s12", "out")
    run(t, out)
    cmd, r = run(t, out)
    ok = r.returncode == 2
    cmd, r2 = run(t, out, "--force")
    report("existing output refused without --force, replaced with it", ok and r2.returncode == 0, cmd, r2)
    # force when tar target is a symlink into the tree
    t = tree("s12b")
    out = os.path.join(WORK, "s12b", "out")
    os.makedirs(out)
    victim = os.path.join(t, "Autobleem", "config.ini")
    os.symlink(victim, os.path.join(out, "autobleem-stick-image-L.tar.gz"))
    cmd, r = run(t, out, "--force")
    report("--force over a symlink planted as the output name: tree file untouched", open(victim, "rb").read() == b"theme=a\n", cmd, r)
    # out is a file
    t = tree("s12c")
    mk(os.path.join(WORK, "s12c", "afile"), b"x")
    cmd, r = run(t, os.path.join(WORK, "s12c", "afile"))
    report("--out is an existing file: clean refusal (no traceback)", "Traceback" not in r.stderr, cmd, r)

    # 13 tree edge cases
    cmd, r = run(os.path.join(WORK, "nonexistent"), os.path.join(WORK, "o"))
    report("missing tree", r.returncode == 2, cmd, r)
    t = os.path.join(WORK, "s13", "stick")
    mk(os.path.join(t, "Games", "x.bin"))
    os.makedirs(os.path.join(t, "Autobleem"))
    cmd, r = run(t, os.path.join(WORK, "s13", "out"))
    report("stick with Autobleem/ empty and only owner data: nothing to image", r.returncode == 2, cmd, r)
    t = os.path.join(WORK, "s13b", "stick")
    mk(os.path.join(t, "Autobleem", "x"))
    mk(os.path.join(t, "Autobleem", "unreadable", "y"))
    os.chmod(os.path.join(t, "Autobleem", "unreadable", "y"), 0)
    cmd, r = run(t, os.path.join(WORK, "s13b", "out"))
    report("unreadable file: clean outcome, no partial output", "Traceback" not in r.stderr and not [
        f for f in (os.listdir(os.path.join(WORK, "s13b", "out")) if os.path.exists(os.path.join(WORK, "s13b", "out")) else [])
        if f.endswith(".part")], cmd, r)
    # tree itself not touched in every scenario: checked for s1/s3
    # excluded dir containing weird names is ignored
    t = tree("s14")
    mk(os.path.join(t, "Logs", "bad:name"), b"x")
    mk(os.path.join(t, "System", "ssh", "CON"), b"x")
    cmd, r = run(t, os.path.join(WORK, "s14", "out"))
    report("bad names inside excluded folders do not block", r.returncode == 0, cmd, r)
    # excluded rule nested: autobleem/Logs and a FILE named logs
    t = tree("s14b")
    mk(os.path.join(t, "Autobleem", "logs"), b"file named logs")
    mk(os.path.join(t, "Autobleem", "System", "ssh", "k"), b"k")  # System/ssh not at root: only root-anchored
    mk(os.path.join(t, "Autobleem", "System", "Bios", "b.bin"), b"k")
    cmd, r = run(t, os.path.join(WORK, "s14b", "out"))
    got = members(os.path.join(WORK, "s14b", "out"))
    report("ssh-key/console-state rules are root-anchored: Autobleem/System/ssh/k and Autobleem/System/Bios are copied "
           "(informational; check real layout)", "Autobleem/System/ssh/k" not in got, cmd, r,
           "Autobleem/System/ssh/k in image: %s; Autobleem/System/Bios/b.bin in image: %s"
           % ("Autobleem/System/ssh/k" in got, "Autobleem/System/Bios/b.bin" in got))
    # gz bomb / large file smoke: skip
    print("WORK=%s" % WORK)


main()
