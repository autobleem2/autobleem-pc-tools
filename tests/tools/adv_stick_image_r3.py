#!/usr/bin/env python3
"""Adversarial round 3 for tools/make_stick_image.py. Runs the tool as a subprocess on trees built under a temp dir.

    python3 tests/tools/adv_stick_image_r3.py

Each check prints PASS or FAIL (FAIL = the tool let something through / broke its contract). Exit 1 on any FAIL.
"""
import json
import os
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "..", "tools", "make_stick_image.py")

WORK = tempfile.mkdtemp(prefix="advr3-")
RESULTS = []
N = [0]


def fresh():
    N[0] += 1
    base = os.path.join(WORK, "t%d" % N[0])
    stick = os.path.join(base, "stick")
    os.makedirs(os.path.join(stick, "Autobleem"))
    with open(os.path.join(stick, "Autobleem", "x.txt"), "w") as f:
        f.write("x")
    return base, stick


def put(stick, rel, data=b"x"):
    p = os.path.join(stick, *rel.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())
    return p


def run(stick, out, *extra, label="L"):
    p = subprocess.run([sys.executable, TOOL, stick, "--label", label, "--out", out] + list(extra),
                       capture_output=True, timeout=300)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def members(out, label="L"):
    with tarfile.open(os.path.join(out, "autobleem-stick-image-%s.tar.gz" % label)) as t:
        return t.getnames()


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print("%s: %s%s" % ("PASS" if ok else "FAIL", name, ("   [" + detail + "]") if detail and not ok else ""))


def marker_case(name, fname, content):
    """The content is irrelevant: a credential-like NAME is not copied, whatever the file holds."""
    base, stick = fresh()
    put(stick, "Apps/a/" + fname, content)
    rc, so, se = run(stick, base + "/out")
    copied = rc == 0 and ("Apps/a/" + fname) in members(base + "/out")
    check("name %s: %s is not copied whatever its content (%s)" % (fname, fname, name), not copied,
          "copied, rc=%d" % rc)


def name_case(rel, content=b"hunter2-private"):
    base, stick = fresh()
    put(stick, rel, content)
    rc, so, se = run(stick, base + "/out")
    copied = rc == 0 and rel in members(base + "/out")
    check("credential-like name %s is not copied" % rel, not copied, "copied")


def main():
    pem = "-----BEGIN PRIVATE KEY-----\nMIIE\n-----END PRIVATE KEY-----\n"
    # 1. best effort: the NAME decides, the content is never read
    marker_case("UTF-16LE PEM", "token.png", pem.encode("utf-16-le"))
    marker_case("clean png bytes", "secret.bin", b"\x89PNG" + b"\0" * 50)
    marker_case("psk= wifi", "wpa_supplicant.png", b'network={\n ssid="home"\n psk="correct horse battery"\n}\n')
    marker_case("gzip-compressed PEM", "secret.bin", __import__("gzip").compress(pem.encode()))
    marker_case("empty", "token.zip", b"")
    marker_case("media name with a credential word", "secret-garden.png", b"\x89PNG")
    # an innocently named file with a secret inside IS copied: the tool is documented as best effort
    base, stick = fresh()
    put(stick, "Apps/a/notes.txt", pem)
    rc, so, se = run(stick, base + "/out")
    check("innocent name with a PEM inside is copied (best effort, names only)",
          rc == 0 and "Apps/a/notes.txt" in members(base + "/out"), "rc=%d" % rc)
    # the owner's allow-list lets exact paths through; the manifest names them
    base, stick = fresh()
    put(stick, "Apps/a/token.png", b"\x89PNG")
    put(stick, "Apps/b/token.png", b"\x89PNG")
    allow = os.path.join(base, "allow.txt")
    with open(allow, "w") as f:
        f.write("Apps/a/token.png\n")
    rc, so, se = run(stick, base + "/out", "--allow-suspicious", allow)
    names = members(base + "/out") if rc == 0 else []
    check("allow-list: the listed path is copied, an unlisted twin is not",
          rc == 0 and "Apps/a/token.png" in names and "Apps/b/token.png" not in names, "rc=%d %s" % (rc, se[-120:]))
    base, stick = fresh()
    put(stick, "Apps/a/token.png", b"\x89PNG")
    rc, so, se = run(stick, base + "/out")
    man = json.load(open(base + "/out/autobleem-stick-image-L.MANIFEST.json"))
    check("the manifest lists every skipped suspicious name",
          [c["path"] for c in man["credential_names_not_copied"]] == ["Apps/a/token.png"])

    # 2. credential names not in the list
    for rel in ["Apps/a/authorized_keys", "Apps/a/.envrc", "Apps/a/kubeconfig", "Apps/a/wallet.dat",
                "Apps/a/.bash_history", "Apps/a/terraform.tfstate", "Apps/a/prod.tfvars", "Apps/a/client.ovpn",
                "Apps/a/mail.gpg", "Apps/a/.s3cfg", "Apps/a/.dockercfg", "Apps/a/service-account.json",
                "Apps/a/private_key", "Apps/a/privkey", "Apps/a/known_hosts", "Apps/a/.boto", "Apps/a/login.keychain",
                "Apps/a/key.p8", "Apps/a/key.asc", "Apps/a/.my.cnf", "Apps/a/htpasswd", "Apps/a/htpasswd"]:
        name_case(rel)
    # credential-like FOLDER names: left out whole
    for d in ["credentials", "tokens", "keys", "private", ".password-store", "certs"]:
        name_case("Apps/a/%s/notes.txt" % d)

    # 3. names with control characters reach the terminal / forge lines
    base, stick = fresh()
    put(stick, "Apps/a/token\x1b[2Jevil.txt")
    put(stick, "Apps/a/secret\nwrote_forged_line.txt")
    rc, so, se = run(stick, base + "/out")
    check("a name with control characters is refused (or skipped) and never echoed raw (stdout and stderr)",
          "\x1b" not in so + se and "\nwrote_forged_line" not in so + se,
          "rc=%d raw ESC / forged line echoed: %r" % (rc, (so + se)[-200:]))

    # 4. --force with the manifest name occupied by a folder: the tar must not be replaced alone
    base, stick = fresh()
    out = base + "/out"
    rc0, _, _ = run(stick, out)
    tar = os.path.join(out, "autobleem-stick-image-L.tar.gz")
    man = os.path.join(out, "autobleem-stick-image-L.MANIFEST.json")
    before = open(tar, "rb").read()
    os.remove(man)
    os.mkdir(man)
    put(stick, "Apps/new/n.txt", "new")
    rc, so, se = run(stick, out, "--force")
    after = open(tar, "rb").read()
    check("--force with a folder in the manifest's place: refused and the old tar kept", rc == 2 and before == after,
          "rc=%d tar replaced=%s" % (rc, before != after))

    # 5. non-UTF8 names
    base, stick = fresh()
    try:
        os.makedirs(os.path.join(os.fsencode(stick), b"Apps", b"a"))
        with open(os.path.join(os.fsencode(stick), b"Apps", b"a", b"secret\xff.png"), "wb") as f:
            f.write(b"x")
        rc, so, se = run(stick, base + "/out")
        check("non-UTF-8 credential name: clean outcome", rc in (0, 2) and "Traceback" not in se, se[-200:])
    except OSError as exc:
        print("SKIP non-UTF-8 names (%s)" % exc)

    # 6. reserved Windows names the check misses
    for nm in ["COM¹.txt", "LPT²", "CONIN$", "CONOUT$", "con.", "Con .txt", "a:b", "‮txt.exe"]:
        base, stick = fresh()
        try:
            put(stick, "Apps/a/" + nm)
        except OSError:
            continue
        rc, so, se = run(stick, base + "/out")
        bad = nm in ("COM¹.txt", "LPT²", "CONIN$", "CONOUT$")
        if bad:
            check("Windows-reserved name %r refused" % nm, rc == 2, "rc=%d" % rc)

    # 10. determinism and manifest equality
    base, stick = fresh()
    put(stick, "Apps/a/b.txt", "b")
    run(stick, base + "/o1")
    run(stick, base + "/o2")
    t1 = open(base + "/o1/autobleem-stick-image-L.tar.gz", "rb").read()
    t2 = open(base + "/o2/autobleem-stick-image-L.tar.gz", "rb").read()
    check("two runs give identical tar bytes", t1 == t2)
    with tarfile.open(base + "/o1/autobleem-stick-image-L.tar.gz") as t:
        last = t.getmembers()[-1]
        data = t.extractfile(last).read()
    check("manifest file equals the tar's last member",
          last.name == "MANIFEST.json" and data == open(base + "/o1/autobleem-stick-image-L.MANIFEST.json", "rb").read())

    # 11. label edge cases
    for lab, want in [("a-OWNER", 2), ("a\n", 2), ("a-owner.1", None), ("..", 2), ("A" * 41, 2), ("a b", 2)]:
        base, stick = fresh()
        rc, so, se = run(stick, base + "/out", label=lab)
        if want is not None:
            check("label %r refused" % lab, rc == want, "rc=%d" % rc)
        else:
            print("   info: label %r -> rc=%d (a non-owner image named like an owner one)" % (lab, rc))

    # 12. symlink and special-file cases
    base, stick = fresh()
    os.symlink("../../outside", os.path.join(stick, "Apps"))
    rc, so, se = run(stick, base + "/out")
    check("symlink Apps -> outside refused", rc == 2, "rc=%d" % rc)
    base, stick = fresh()
    os.makedirs(os.path.join(stick, "Apps", "a"))
    os.mkfifo(os.path.join(stick, "Apps", "a", "token.png"))
    rc, so, se = run(stick, base + "/out")
    check("fifo named token.png does not hang or copy", rc == 0 and "Apps/a/token.png" not in members(base + "/out"))
    base, stick = fresh()
    os.symlink("secret.png", os.path.join(stick, "Apps-loop.png"))
    rc, so, se = run(stick, base + "/out")
    check("self-referencing symlink: clean outcome", rc in (0, 2) and "Traceback" not in se, se[-200:])

    # 13. owner data hidden under other spellings
    for rel in ["Games/a/b.bin", "GAMES/a.bin", "RetroArch/roms/x.bin", "retroarch/BIOS/s.bin", "RetroArch/bin/saves/s.srm",
                "RetroArch/bin/savestates/s.state"]:
        base, stick = fresh()
        put(stick, rel)
        put(stick, "Apps/keep.txt")
        rc, so, se = run(stick, base + "/out")
        check("owner data %s left out by default" % rel, rc == 0 and rel not in members(base + "/out"))

    print("\nWORK=%s" % WORK)
    bad = RESULTS.count(False)
    print("%d checks, %d FAIL" % (len(RESULTS), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
