"""Tests of tools/make_stick_image.py on a fake stick tree built in a temp folder.

Run: python3 -m unittest discover -s tests/tools   (from the repo root)
"""
import hashlib
import importlib.util
import io
import json
import os
import tarfile
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "..", "tools", "make_stick_image.py")
spec = importlib.util.spec_from_file_location("make_stick_image", TOOL)
msi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(msi)


def put(root, rel, data=b"x"):
    path = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def tree_state(root):
    out = {}
    for here, dirs, names in os.walk(root):
        for n in dirs + names:
            p = os.path.join(here, n)
            st = os.lstat(p)
            out[os.path.relpath(p, root)] = (st.st_size, st.st_mtime_ns)
    return out


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


class Base(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = self._tmp.name
        self.tree = os.path.join(self.tmp, "stick")
        self.out = os.path.join(self.tmp, "out")
        os.makedirs(self.tree)

    def fake_stick(self):
        files = {
            "VERSION": b"2.0.1\n",
            "Autobleem/bin/autobleem/autobleem-gui": b"elf",
            "Autobleem/bin/autobleem/config.ini": b"theme=x\n",
            "Apps/MyApp/app.sh": b"#!/bin/sh\n",
            "Themes/Mine/theme.ini": b"a=b\n",
            "RetroArch/bin/playlists/PSX.lpl": b"{}",
            "RetroArch/bin/retroarch.cfg": b"k = v\n",
            # excluded always
            "System/ssh/id_tmp": b"KEY",
            "System/Bios/scph1001.bin": b"bios",
            "System/Preferences/p.dat": b"p",
            "System/Region": b"EU",
            "System/UI/ui.dat": b"u",
            "System/Logs/installer.log": b"log",
            "System/Install/pkg.tar.gz": b"scratch",
            "System/Databases/regional.db": b"db",
            "FOUND.000/FILE0000.CHK": b"c",
            "$RECYCLE.BIN/S-1/x": b"r",
            "System Volume Information/IndexerVolumeGuid": b"g",
            "Apps/MyApp/debug.log": b"l",
            "Apps/MyApp/cache.tmp": b"t",
            "Themes/Mine/Thumbs.db": b"t",
            "Apps/MyApp/Logs/a.txt": b"l",
            # owner data
            "Games/Crash/Crash.cue": b"cue",
            "Games/Crash/Crash.bin": b"bin" * 10,
            "Games/!SaveStates/s.state": b"st",
            "RetroArch/roms/psx/game.chd": b"chd",
            "RetroArch/bios/scph5501.bin": b"bios2",
            "RetroArch/bin/saves/x.srm": b"srm",
            # credential-like
            "Apps/MyApp/api_token.txt": b"secret",
            "Apps/MyApp/server.pem": b"pem",
            "Themes/Mine/token.png": b"png",
        }
        for rel, data in files.items():
            put(self.tree, rel, data)

    def build(self, **kw):
        kw.setdefault("label", "t1")
        return msi.build(self.tree, kw.pop("label"), kw.pop("out", self.out), **kw)

    def cli(self, out, *extra):
        return msi.main([self.tree, "--label", "t1", "--out", out] + list(extra))

    def members(self, tar_path):
        with tarfile.open(tar_path, "r:gz") as tf:
            return {m.name: m for m in tf.getmembers()}, tf


class ExclusionTests(Base):
    def test_default_exclusions(self):
        self.fake_stick()
        tar_path, _, man = self.build()
        names, _ = self.members(tar_path)
        files = {r["path"] for r in man["files"]}
        self.assertEqual(files, {
            "VERSION", "Autobleem/bin/autobleem/autobleem-gui", "Autobleem/bin/autobleem/config.ini",
            "Apps/MyApp/app.sh", "Themes/Mine/theme.ini", "RetroArch/bin/playlists/PSX.lpl",
            "RetroArch/bin/retroarch.cfg", "System/Databases/regional.db"})
        for bad in ("System/ssh", "System/Bios", "System/Preferences", "System/Region", "System/UI", "System/Logs",
                    "System/Install", "FOUND.000", "$RECYCLE.BIN", "System Volume Information", "Games",
                    "RetroArch/roms", "RetroArch/bios", "RetroArch/bin/saves", "Apps/MyApp/Logs"):
            self.assertFalse(any(n == bad or n.startswith(bad + "/") for n in names), bad)
        for bad in ("debug.log", "cache.tmp", "Thumbs.db"):
            self.assertFalse(any(n.endswith(bad) for n in names), bad)

    def test_credential_names_reported_not_copied(self):
        self.fake_stick()
        _, _, man = self.build()
        reported = {c["path"] for c in man["credential_names_not_copied"]}
        self.assertEqual(reported, {"Apps/MyApp/api_token.txt", "Apps/MyApp/server.pem", "Themes/Mine/token.png"})
        self.assertNotIn("Apps/MyApp/api_token.txt", {r["path"] for r in man["files"]})
        self.assertNotIn("Themes/Mine/token.png", {r["path"] for r in man["files"]})  # media name: still not copied

    def test_exclusions_case_insensitive(self):
        put(self.tree, "Autobleem/a", b"a")
        put(self.tree, "system/SSH/key", b"k")
        put(self.tree, "found.001/x", b"x")
        _, _, man = self.build()
        self.assertEqual([r["path"] for r in man["files"]], ["Autobleem/a"])

    def test_excluded_report_counts(self):
        self.fake_stick()
        _, _, man = self.build()
        games = [x for x in man["excluded"] if x["path"] == "Games/"][0]
        self.assertEqual(games["rule"], "owner-data")
        self.assertEqual(games["files"], 3)


class CredentialNameTests(Base):
    """Point 2: secret-like names and folders outside the first pattern list."""

    FILES = [".ssh/known", "secrets/db.txt", ".git/config", "Autobleem/wpa_supplicant.conf", "Autobleem/.htpasswd",
             "Autobleem/passwords.txt", "Autobleem/vault.kdbx", ".aws/config", ".gnupg/trustdb.gpg",
             "Autobleem/keystore.jks", "Autobleem/api_key.txt", "Autobleem/apikey.json", "Autobleem/.npmrc",
             "Autobleem/client_secret.json", "Autobleem/passwd", "Autobleem/shadow", "Autobleem/cookies.sqlite",
             "Apps/deep/.ssh/id", "Apps/deep/.config/x/y"]

    def test_none_copied_all_reported(self):
        put(self.tree, "Autobleem/ok.txt", b"ok")
        for rel in self.FILES:
            put(self.tree, rel, b"x")
        _, _, man = self.build()
        self.assertEqual([r["path"] for r in man["files"]], ["Autobleem/ok.txt"])
        reported = {c["path"] for c in man["credential_names_not_copied"]}
        for top in (".ssh/", "secrets/", ".git/", ".aws/", ".gnupg/", "Apps/deep/.ssh/", "Apps/deep/.config/",
                    "Autobleem/passwd", "Autobleem/shadow", "Autobleem/.npmrc"):
            self.assertIn(top, reported)
        self.assertEqual(man["counts"]["credential_names_not_copied"], len(man["credential_names_not_copied"]))

    def test_folder_content_not_walked(self):
        put(self.tree, "Autobleem/ok", b"ok")
        put(self.tree, ".git/objects/CON", b"x")  # a bad name inside is not a reason to refuse
        self.build()

    def test_nested_system_ssh_excluded_but_bios_stays_root_only(self):
        put(self.tree, "Autobleem/ok", b"ok")
        put(self.tree, "Autobleem/System/ssh/k", b"k")
        put(self.tree, "Autobleem/System/Bios/b.bin", b"b")
        _, _, man = self.build()
        files = {r["path"] for r in man["files"]}
        self.assertNotIn("Autobleem/System/ssh/k", files)
        self.assertIn("Autobleem/System/Bios/b.bin", files)

    def test_gitignore_is_not_a_git_folder(self):
        put(self.tree, "Autobleem/.gitignore", b"x")
        _, _, man = self.build()
        self.assertIn("Autobleem/.gitignore", {r["path"] for r in man["files"]})


class SuspiciousPolicyTests(Base):
    """The policy: a credential-like NAME is not copied, whatever the content, unless the owner's allow-list names it."""

    def files(self, man):
        return {r["path"] for r in man["files"]}

    def allow_file(self, *lines):
        path = os.path.join(self.tmp, "allow.txt")
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write("\n".join(lines) + "\n")
        return path

    def test_media_named_suspicious_files_are_not_copied(self):
        put(self.tree, "Autobleem/ok", b"ok")
        for rel in ("id_rsa.png", "secret.bin", "token.zip", "prod.pem.jpg", "my_token.ogg", "secret-garden.png"):
            put(self.tree, "Autobleem/" + rel, b"\x89PNG" + b"\0" * 50)  # clean content: the name decides
        _, _, man = self.build()
        self.assertEqual(self.files(man), {"Autobleem/ok"})
        self.assertEqual(len(man["credential_names_not_copied"]), 6)

    def test_content_is_never_read(self):
        self.assertFalse(hasattr(msi, "has_secret_marker"))
        self.assertFalse(hasattr(msi, "SECRET_MARKER"))
        put(self.tree, "Autobleem/ok", b"ok")
        put(self.tree, "Autobleem/notes.txt", b"-----BEGIN PRIVATE KEY-----\npassword=x")  # innocent name
        _, _, man = self.build()
        self.assertIn("Autobleem/notes.txt", self.files(man))
        self.assertNotIn("unscanned-archive", man)
        self.assertNotIn("unscanned-timeout", man)

    def test_allow_list_lets_exact_paths_through(self):
        put(self.tree, "Autobleem/ok", b"ok")
        put(self.tree, "Themes/token.png", b"png")
        put(self.tree, "Themes/other/token.png", b"png")
        put(self.tree, "Themes/secrets/a.txt", b"a")
        allow = self.allow_file("Themes/token.png", "Themes/secrets/", "token.png")
        _, _, man = self.build(allow_file=allow)
        self.assertEqual(self.files(man), {"Autobleem/ok", "Themes/token.png", "Themes/secrets/a.txt"})
        self.assertEqual([c["path"] for c in man["credential_names_not_copied"]], ["Themes/other/token.png"])
        self.assertEqual([c["path"] for c in man["suspicious_names_allowed"]], ["Themes/secrets/", "Themes/token.png"])
        self.assertEqual(man["counts"]["suspicious_names_allowed"], 2)

    def test_allow_list_is_exact_not_a_pattern_or_case_folded(self):
        put(self.tree, "Autobleem/ok", b"ok")
        put(self.tree, "Themes/Token.png", b"png")
        allow = self.allow_file("Themes/token.png", "Themes/*", "Themes")
        _, _, man = self.build(allow_file=allow)
        self.assertEqual(self.files(man), {"Autobleem/ok"})

    def test_allowed_folder_content_is_still_judged(self):
        put(self.tree, "Autobleem/ok", b"ok")
        put(self.tree, "Themes/keys/a.txt", b"a")
        put(self.tree, "Themes/keys/id_rsa", b"k")
        _, _, man = self.build(allow_file=self.allow_file("Themes/keys"))
        self.assertEqual(self.files(man), {"Autobleem/ok", "Themes/keys/a.txt"})
        self.assertIn("Themes/keys/id_rsa", {c["path"] for c in man["credential_names_not_copied"]})

    def test_allow_list_errors_refuse(self):
        put(self.tree, "Autobleem/ok", b"ok")
        with self.assertRaises(msi.Refused):
            self.build(allow_file=os.path.join(self.tmp, "missing.txt"))
        bad = os.path.join(self.tmp, "bad.txt")
        with open(bad, "wb") as f:
            f.write(b"\xff\xfe")
        with self.assertRaises(msi.Refused):
            self.build(allow_file=bad)
        self.assertFalse(os.path.exists(self.out))

    def test_cli_allow_option(self):
        put(self.tree, "Autobleem/ok", b"ok")
        put(self.tree, "Autobleem/.envrc", b"x")
        allow = self.allow_file("Autobleem/.envrc")
        rc = msi.main([self.tree, "--label", "a", "--out", self.out, "--allow-suspicious", allow])
        self.assertEqual(rc, 0)
        with open(os.path.join(self.out, "autobleem-stick-image-a.MANIFEST.json")) as f:
            self.assertIn("Autobleem/.envrc", {r["path"] for r in json.load(f)["files"]})


class NameListTests(Base):
    NAMES = ["authorized_keys", "known_hosts", ".envrc", "kubeconfig", "wallet.dat", ".bash_history",
             "terraform.tfstate", "a.tfvars", "a.ovpn", "a.gpg", "a.asc", "a.p8", "a.keychain", ".s3cfg", ".dockercfg",
             ".boto", ".my.cnf", "service-account.json", "private_key", "privkey", "Authorized_Keys", "PRIVKEY.txt"]
    DIRS = ["credentials", "tokens", "keys", "private", "certs", ".password-store", "Keys", "secrets", ".kube"]

    def test_names(self):
        for n in self.NAMES:
            self.assertIsNotNone(msi.credential_rule(n), n)
        for n in self.DIRS:
            self.assertIsNotNone(msi.credential_rule(n), n)
        for n in ("config.ini", "keyboard.png", "private.txt", "monkeys", "certificates"):
            self.assertIsNone(msi.credential_rule(n), n)

    def test_folders_left_out_whole_at_any_depth(self):
        put(self.tree, "Autobleem/ok", b"ok")
        for d in self.DIRS:
            put(self.tree, "Apps/x/%s/inner/a.txt" % d, b"a")
        _, _, man = self.build()
        self.assertEqual({r["path"] for r in man["files"]}, {"Autobleem/ok"})
        reported = {c["path"] for c in man["credential_names_not_copied"]}
        for d in self.DIRS:
            self.assertIn("Apps/x/%s/" % d, reported)
        self.assertFalse(any("inner" in r for r in reported))


class OwnerDataTests(Base):
    def test_default_leaves_owner_data_out(self):
        self.fake_stick()
        tar_path, _, man = self.build()
        self.assertFalse(man["owner_data_included"])
        self.assertTrue(tar_path.endswith("autobleem-stick-image-t1.tar.gz"))
        self.assertNotIn("-owner", os.path.basename(tar_path))

    def test_flag_includes_and_names_owner(self):
        self.fake_stick()
        tar_path, man_path, man = self.build(include_owner=True)
        self.assertTrue(tar_path.endswith("autobleem-stick-image-t1-owner.tar.gz"))
        files = {r["path"] for r in man["files"]}
        for rel in ("Games/Crash/Crash.bin", "Games/!SaveStates/s.state", "RetroArch/roms/psx/game.chd",
                    "RetroArch/bios/scph5501.bin", "RetroArch/bin/saves/x.srm"):
            self.assertIn(rel, files)
        self.assertTrue(man["owner_data_included"])
        # the always-excluded stay out even with the flag
        self.assertNotIn("System/ssh/id_tmp", files)
        self.assertNotIn("System/Bios/scph1001.bin", files)

    def test_cli_flag(self):
        self.fake_stick()
        rc = msi.main([self.tree, "--label", "c", "--out", self.out, "--include-owner-data"])
        self.assertEqual(rc, 0)
        self.assertTrue(os.path.exists(os.path.join(self.out, "autobleem-stick-image-c-owner.tar.gz")))


class ManifestTests(Base):
    def test_manifest_matches_tar(self):
        self.fake_stick()
        tar_path, man_path, man = self.build()
        with tarfile.open(tar_path, "r:gz") as tf:
            members = tf.getmembers()
            self.assertEqual(members[-1].name, "MANIFEST.json")
            inner = json.loads(tf.extractfile("MANIFEST.json").read())
            self.assertEqual(inner, man)
            with open(man_path, "rb") as f:
                self.assertEqual(json.loads(f.read()), man)
            tar_files = {}
            for m in members[:-1]:
                if m.isfile():
                    data = tf.extractfile(m).read()
                    tar_files[m.name] = (len(data), hashlib.sha256(data).hexdigest())
                else:
                    self.assertTrue(m.isdir())
                    self.assertIn(m.name, man["dirs"])
        self.assertEqual(tar_files, {r["path"]: (r["size"], r["sha256"]) for r in man["files"]})
        self.assertEqual(man["counts"]["files"], len(man["files"]))
        self.assertEqual(man["counts"]["bytes"], sum(r["size"] for r in man["files"]))
        self.assertEqual(man["label"], "t1")
        self.assertEqual(man["tool_version"], msi.TOOL_VERSION)
        self.assertEqual([r["path"] for r in man["files"]], sorted(r["path"] for r in man["files"]))

    def test_manifest_has_no_input_path(self):
        self.fake_stick()
        _, man_path, _ = self.build()
        with open(man_path, "r") as f:
            self.assertNotIn(self.tree, f.read())


class DeterminismTests(Base):
    def test_two_runs_same_sha256(self):
        self.fake_stick()
        a, _, _ = self.build(out=os.path.join(self.tmp, "o1"))
        os.utime(os.path.join(self.tree, "VERSION"), (12345, 12345))  # mtimes must not matter
        b, _, _ = self.build(out=os.path.join(self.tmp, "o2"))
        self.assertEqual(sha(a), sha(b))

    def test_fixed_member_metadata(self):
        self.fake_stick()
        tar_path, _, _ = self.build()
        with tarfile.open(tar_path, "r:gz") as tf:
            for m in tf.getmembers():
                self.assertEqual((m.mtime, m.uid, m.gid, m.uname, m.gname), (msi.FIXED_MTIME, 0, 0, "", ""))


class RefusalTests(Base):
    def refuses(self, text=None, **kw):
        with self.assertRaises(msi.Refused) as cm:
            self.build(**kw)
        if text:
            self.assertIn(text, str(cm.exception))
        self.assertFalse(os.path.exists(self.out) and os.listdir(self.out), "nothing may be written")

    def test_missing_tree(self):
        os.rmdir(self.tree)
        self.refuses("not a folder")

    def test_empty_tree(self):
        self.refuses("empty")

    def test_not_a_stick(self):
        put(self.tree, "Games/a", b"a")
        self.refuses("no Autobleem/")

    def test_only_excluded(self):
        os.makedirs(os.path.join(self.tree, "Autobleem"))
        put(self.tree, "System/ssh/k", b"k")
        self.refuses("nothing to put")

    @unittest.skipUnless(hasattr(os, "symlink"), "no symlinks")
    def test_symlink_out_of_tree(self):
        put(self.tree, "Autobleem/a", b"a")
        outside = os.path.join(self.tmp, "outside.txt")
        with open(outside, "w") as f:
            f.write("o")
        try:
            os.symlink(outside, os.path.join(self.tree, "Apps_link"))
        except OSError:
            self.skipTest("cannot symlink")
        self.refuses("symlink out of the tree")

    @unittest.skipUnless(hasattr(os, "symlink"), "no symlinks")
    def test_dir_symlink_out_of_tree(self):
        put(self.tree, "Autobleem/a", b"a")
        outside = os.path.join(self.tmp, "outdir")
        os.makedirs(outside)
        try:
            os.symlink(outside, os.path.join(self.tree, "Themes"))
        except OSError:
            self.skipTest("cannot symlink")
        self.refuses("symlink out of the tree")

    @unittest.skipUnless(hasattr(os, "symlink"), "no symlinks")
    def test_symlink_inside_tree_skipped_not_followed(self):
        put(self.tree, "Autobleem/a", b"a")
        try:
            os.symlink("Autobleem", os.path.join(self.tree, "alias"))
        except OSError:
            self.skipTest("cannot symlink")
        _, _, man = self.build()
        self.assertEqual([r["path"] for r in man["files"]], ["Autobleem/a"])
        self.assertEqual([s["path"] for s in man["skipped"]], ["alias"])

    def test_dotdot_component(self):
        for parts in (("..", "x"), ("a", "..", "x"), (".",), ("",)):
            with self.assertRaises(msi.Refused):
                msi.check_relpath(parts)

    def test_reserved_windows_names(self):
        for name in ("CON", "nul.txt", "Aux", "COM1", "lpt9.bin", "prn.", "a:b", "x*y", "q?", "trail ", "dot."):
            with self.assertRaises(msi.Refused, msg=name):
                msg = msi.check_name(name)
        msi.check_name("CONSOLE")
        msi.check_name("com10")
        msi.check_name("a b.txt")

    def test_reserved_names_superscript_and_console_handles(self):
        for name in ("COM¹", "com².txt", "LPT³", "lpt¹.bin", "CONIN$", "conout$", "CONIN$.txt",
                     "ＣＯＭ1"):
            with self.assertRaises(msi.Refused, msg=name):
                msi.check_name(name)

    def test_shown_escapes_every_control_character(self):
        for c in ("\n", "\r", "\x1b", "\x00", "\x7f", "\x85", "\x9b", " ", "‮"):
            text = msi.shown("a%sb" % c)
            self.assertNotIn(c, text)
            self.assertTrue(text.isprintable(), repr(text))
        self.assertEqual(msi.shown("a\x1b[2Jb"), "a\\x1b[2Jb")
        self.assertEqual(msi.shown("café"), "café")
        with self.assertRaises(msi.Refused) as cm:
            msi.check_name("bad\nname<")
        self.assertNotIn("\n", str(cm.exception))

    def test_reserved_name_in_tree(self):
        put(self.tree, "Autobleem/a", b"a")
        put(self.tree, "Apps/NUL.txt", b"x")
        self.refuses("reserved Windows name")

    def test_reserved_name_inside_excluded_is_fine(self):
        put(self.tree, "Autobleem/a", b"a")
        put(self.tree, "$RECYCLE.BIN/aux", b"x")
        self.build()

    def test_case_collision(self):
        put(self.tree, "Autobleem/a", b"a")
        put(self.tree, "Apps/X", b"1")
        try:
            put(self.tree, "Apps/x", b"2")
        except OSError:
            self.skipTest("case-insensitive file system")
        if len(os.listdir(os.path.join(self.tree, "Apps"))) < 2:
            self.skipTest("case-insensitive file system")
        self.refuses("differ only by case")

    def test_unicode_normalization_collision(self):
        put(self.tree, "Autobleem/a", b"a")
        put(self.tree, "Apps/café", b"1")
        put(self.tree, "Apps/café", b"2")
        self.assertEqual(len(os.listdir(os.path.join(self.tree, "Apps"))), 2)
        self.refuses("Unicode normalization")

    def test_bad_label(self):
        self.fake_stick()
        for label in ("", "../x", "a b", ".hidden", "x" * 41, "a/b"):
            with self.assertRaises(msi.Refused, msg=label):
                self.build(label=label)

    def test_label_ending_in_owner(self):
        self.fake_stick()
        for label in ("mine-owner", "x-OWNER", "-owner"):
            with self.assertRaises(msi.Refused, msg=label):
                self.build(label=label)
            with self.assertRaises(msi.Refused, msg=label):
                self.build(label=label, include_owner=True)
        self.assertFalse(os.path.exists(self.out))
        self.build(label="owner-notes")  # only the suffix is reserved

    def test_output_inside_tree(self):
        self.fake_stick()
        with self.assertRaises(msi.Refused):
            self.build(out=os.path.join(self.tree, "out"))
        self.assertFalse(os.path.exists(os.path.join(self.tree, "out")))

    def test_existing_output_needs_force(self):
        self.fake_stick()
        self.build()
        with self.assertRaises(msi.Refused):
            self.build()
        self.build(force=True)

    def test_force_is_atomic_when_the_manifest_cannot_be_replaced(self):
        self.fake_stick()
        tar_path, man_path, _ = self.build()
        os.remove(man_path)
        os.makedirs(os.path.join(man_path, "keep"))  # a folder where the manifest goes
        old_tar = sha(tar_path)
        put(self.tree, "Autobleem/new.txt", b"new")  # a forced build would differ from the old one
        self.assertEqual(self.cli(self.out, "--force"), 2)
        self.assertEqual(sha(tar_path), old_tar)
        self.assertTrue(os.path.isdir(os.path.join(man_path, "keep")))
        self.assertEqual(sorted(os.listdir(self.out)),
                         sorted([os.path.basename(tar_path), os.path.basename(man_path)]))

    def test_force_rollback_keeps_old_manifest_file(self):
        self.fake_stick()
        tar_path, man_path, _ = self.build()
        old = sha(tar_path), sha(man_path)
        put(self.tree, "Autobleem/new.txt", b"new")
        real = msi.os.replace

        def boom(src, dst, *a, **k):
            if dst == man_path:
                raise OSError("cannot replace")
            return real(src, dst, *a, **k)
        msi.os.replace = boom
        try:
            self.assertEqual(self.cli(self.out, "--force"), 2)
        finally:
            msi.os.replace = real
        self.assertEqual((sha(tar_path), sha(man_path)), old)
        self.assertEqual(len(os.listdir(self.out)), 2)

    def test_force_failure_without_old_tar_leaves_no_new_tar(self):
        self.fake_stick()
        tar_path, man_path, _ = self.build()
        os.remove(tar_path)
        os.remove(man_path)
        os.makedirs(man_path)
        self.assertEqual(self.cli(self.out, "--force"), 2)
        self.assertFalse(os.path.lexists(tar_path))
        self.assertEqual(os.listdir(self.out), [os.path.basename(man_path)])

    def test_manifest_name_at_top(self):
        put(self.tree, "Autobleem/a", b"a")
        put(self.tree, "manifest.json", b"m")
        self.refuses("manifest")

    def test_cli_returns_2_on_refusal(self):
        rc = msi.main([os.path.join(self.tmp, "nope"), "--label", "x", "--out", self.out])
        self.assertEqual(rc, 2)

    def test_path_too_long(self):
        put(self.tree, "Autobleem/a", b"a")
        deep = "/".join(["d" * 50] * 25) + "/f"
        try:
            put(self.tree, "Apps/" + deep, b"a")
        except OSError:
            self.skipTest("path too long for this file system")
        self.refuses("longer than")


class InputSafetyTests(Base):
    def test_input_tree_untouched(self):
        self.fake_stick()
        before = tree_state(self.tree)
        self.build()
        self.build(out=os.path.join(self.tmp, "o2"), include_owner=True)
        self.assertEqual(before, tree_state(self.tree))

    def test_no_part_files_left(self):
        self.fake_stick()
        self.build()
        self.assertEqual(sorted(os.listdir(self.out)),
                         ["autobleem-stick-image-t1.MANIFEST.json", "autobleem-stick-image-t1.tar.gz"])


@unittest.skipUnless(hasattr(os, "symlink"), "no symlinks")
class OutputSymlinkTests(Base):
    """Point 1: nothing planted in OUT may lead a write into the input tree."""

    def plant(self, *names):
        put(self.tree, "Autobleem/config.ini", b"theme=a\n")
        victim = os.path.join(self.tree, "Autobleem", "config.ini")
        os.makedirs(self.out)
        for n in names:
            try:
                os.symlink(victim, os.path.join(self.out, n))
            except OSError:
                self.skipTest("cannot symlink")
        return victim

    def test_planted_part_symlinks_do_not_truncate(self):
        victim = self.plant("autobleem-stick-image-t1.tar.gz.part", "autobleem-stick-image-t1.MANIFEST.json.part")
        with self.assertRaises(msi.Refused):
            self.build()
        with open(victim, "rb") as f:
            self.assertEqual(f.read(), b"theme=a\n")

    def test_planted_artifact_symlink_refused_even_with_force(self):
        victim = self.plant("autobleem-stick-image-t1.tar.gz")
        with self.assertRaises(msi.Refused):
            self.build(force=True)
        with open(victim, "rb") as f:
            self.assertEqual(f.read(), b"theme=a\n")

    def test_planted_manifest_symlink_refused(self):
        victim = self.plant("autobleem-stick-image-t1.MANIFEST.json")
        with self.assertRaises(msi.Refused):
            self.build(force=True)
        with open(victim, "rb") as f:
            self.assertEqual(f.read(), b"theme=a\n")

    def test_final_names_are_regular_files(self):
        put(self.tree, "Autobleem/a", b"a")
        tar_path, man_path, _ = self.build()
        self.assertFalse(os.path.islink(tar_path) or os.path.islink(man_path))
        self.assertEqual(sorted(os.listdir(self.out)), sorted(os.path.basename(p) for p in (tar_path, man_path)))


class OutputFailureTests(Base):
    """Points 5 and 6: a bad --out or an unreadable file is a 'refused:' exit 2, nothing left behind."""

    def test_out_is_a_file(self):
        self.fake_stick()
        target = os.path.join(self.tmp, "afile")
        with open(target, "wb") as f:
            f.write(b"keep")
        with self.assertRaises(msi.Refused):
            self.build(out=target)
        self.assertEqual(self.cli(target), 2)
        with open(target, "rb") as f:
            self.assertEqual(f.read(), b"keep")

    def test_unreadable_file(self):
        self.fake_stick()
        victim = os.path.join(self.tree, "Apps", "MyApp", "app.sh")
        os.chmod(victim, 0)
        self.addCleanup(os.chmod, victim, 0o644)
        self.assertEqual(self.cli(self.out), 2)
        self.assertFalse(os.path.exists(self.out))

    def test_oserror_while_writing_leaves_nothing(self):
        self.fake_stick()
        os.makedirs(self.out)
        real = msi.os.replace

        def boom(*a, **k):
            raise OSError("disk full")
        msi.os.replace = boom
        try:
            self.assertEqual(self.cli(self.out), 2)
        finally:
            msi.os.replace = real
        self.assertEqual(os.listdir(self.out), [])

    def test_new_out_folder_removed_on_failure(self):
        self.fake_stick()
        real = msi.os.replace

        def boom(*a, **k):
            raise OSError("disk full")
        msi.os.replace = boom
        try:
            self.assertEqual(self.cli(self.out), 2)
        finally:
            msi.os.replace = real
        self.assertFalse(os.path.exists(self.out))


class AwkwardFileTests(Base):
    def test_space_nonascii_empty_deep(self):
        put(self.tree, "Autobleem/a", b"a")
        put(self.tree, "Apps/My App/hello world.txt", b"hi")
        put(self.tree, "Themes/Zażółć gęślą jaźń/ścieżka ☃.png", b"snow")
        put(self.tree, "Apps/My App/empty.dat", b"")
        deep = "/".join("level%02d" % i for i in range(30)) + "/deep file.txt"
        put(self.tree, "Extensions/" + deep, b"deep")
        long_name = "n" * 120 + ".txt"  # forces a GNU long name only if the path passes 100 chars
        put(self.tree, "Apps/" + long_name, b"long")
        tar_path, _, man = self.build()
        with tarfile.open(tar_path, "r:gz") as tf:
            names = set(tf.getnames())
            for rel, data in (("Apps/My App/hello world.txt", b"hi"),
                              ("Themes/Zażółć gęślą jaźń/ścieżka ☃.png", b"snow"),
                              ("Apps/My App/empty.dat", b""),
                              ("Extensions/" + deep, b"deep"),
                              ("Apps/" + long_name, b"long")):
                self.assertIn(rel, names)
                self.assertEqual(tf.extractfile(rel).read(), data)
        rec = {r["path"]: r for r in man["files"]}
        self.assertEqual(rec["Apps/My App/empty.dat"]["size"], 0)
        self.assertEqual(rec["Apps/My App/empty.dat"]["sha256"], hashlib.sha256(b"").hexdigest())
        self.assertIn("Themes/Zażółć gęślą jaźń/ścieżka ☃.png", rec)

    def test_empty_dir_kept(self):
        put(self.tree, "Autobleem/a", b"a")
        os.makedirs(os.path.join(self.tree, "Apps", "Empty"))
        tar_path, _, man = self.build()
        self.assertIn("Apps/Empty", man["dirs"])
        names, _ = self.members(tar_path)
        self.assertTrue(names["Apps/Empty"].isdir())


if __name__ == "__main__":
    unittest.main()
