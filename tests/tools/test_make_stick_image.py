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
            "RetroArch/bin/retroarch.cfg", "System/Databases/regional.db", "Themes/Mine/token.png"})
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
        self.assertEqual(reported, {"Apps/MyApp/api_token.txt", "Apps/MyApp/server.pem"})
        self.assertNotIn("Apps/MyApp/api_token.txt", {r["path"] for r in man["files"]})
        self.assertIn("Themes/Mine/token.png", {r["path"] for r in man["files"]})  # plain media file

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

    def test_bad_label(self):
        self.fake_stick()
        for label in ("", "../x", "a b", ".hidden", "x" * 41, "a/b"):
            with self.assertRaises(msi.Refused, msg=label):
                self.build(label=label)

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
