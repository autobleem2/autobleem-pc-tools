# Stick image: the owner's known-good stick as a second layer (TOOLS-6, phase 1)

Status: design + a tool that builds the layer. Nothing in the installer is changed; applying the layer is phase 2,
after the owner's OK on this page. Source read: `autobleem-core` `src/code/installer/installer_job.{h,cpp}`,
`install_job_base.*`.

## 1. What the installer takes today

`InstallerJob::run` puts AutoBleem on a stick from **one package** plus downloads.

- **The package** is `autobleem-psc-<version>.tar.gz` (the release's "psc-fs" file). `packageNextTo` picks the newest
  one beside the program; with a channel it is downloaded (sha256-checked) into `System/Install`. `package()` lists
  it and requires the entry `Autobleem/bin/autobleem/autobleem-gui`; it also needs a `VERSION` entry.
  Entries are paths relative to the stick's root: `Autobleem/` (launcher, pcsx-ab, scripts, console tools, `lib`,
  `start.sh`), `Apps/` (the two console tools), `Themes/<name>/` (shipped themes), `Extensions/<name>/` (PSC-Bios,
  the Store), `Docs/`, `VERSION`.
- **Unpacked** by `untar(package, root)` straight over the stick's root (`TarArchive::extract`): files are written
  over existing ones, nothing is deleted by the extraction itself.
- **Other sources**, each on request: UpdateRoms (folder beside the installer or the site), cover DBs
  (`Autobleem/bin/db/covers{J,U,P}.db`), RetroArch with cores, libs, apps and libretro bundles
  (`RetroArch/bin/...`), BIOS files (`RetroArch/bios`), sample games (`Games/`, `System/samples.txt`).
- **Replaced on an update** (`prepare()` deletes them first, then the unpack lays the new ones):
  `Autobleem/bin/autobleem`, `Autobleem/bin/emu`, `Autobleem/bin/emunxt`, `Autobleem/rc`, `Apps/pscbios`,
  `Apps/abflashkit`, `Extensions/pscbios`, `Docs`, every `Extensions/<name>` and `Themes/<name>` the package ships,
  `Autobleem/lib/libs.tar.gz`, `Autobleem/start.sh`, `VERSION`; `UpdateRoms/` (swapped whole); RetroArch's binary,
  its theme files, `VERSION`.
- **Kept**: `Games/` (games, `!SaveStates`, `!MemCards`), `System/` (`Databases`, `Logs`, `Processors`), other
  `Apps/`, `Themes/`, `Extensions/`, RetroArch's saves, playlists, thumbnails, `roms/`, `bios/`, `retroarch.cfg`
  (only the build's own keys are merged in). `config.ini` is read before the wipe and written back after the unpack;
  then `theme` (once, if the default theme folder is new) and `emulator = pcsx-abnxt` (always) are set.

## 2. Where a stick-image layer hooks in

The image is the owner's *user layer* on top of the package: his Apps, themes, playlists / games list, config.
It is built by `tools/make_stick_image.py` (section 4) as `autobleem-stick-image-<label>.tar.gz`, a tar of paths
relative to the stick's root, **plus `MANIFEST.json` as its last member** (files with size and sha256, exclusions,
counts, label, tool version).

Recommended hook: **an optional second file beside the package, applied by a new phase, manifest-driven.**

- `InstallOptions::stickImageFile` (default `""` = no layer); a helper `stickImageNextTo(programPath)` like
  `packageNextTo`, matching `autobleem-stick-image-*.tar.gz`. `inspect()` reads its `MANIFEST.json` (like it reads
  `VERSION`) and reports it in `StickInfo`.
- `phasesFor` adds **"Applying the stick image"** right after "Unpacking AutoBleem" (so the later RetroArch phases
  see its `retroarch.cfg` and playlists as the user's own and do not overwrite them) and before UpdateRoms.
- The phase: read `MANIFEST.json`; refuse a manifest with a path that has `..`, is absolute, or is outside the
  allowed top-level folders; extract to the scratch folder; check **every file's sha256 and size against the
  manifest**; then copy file by file (temporary name, then rename, as `installZip` does). It **never deletes** anything.
  `config.ini` of the image replaces the stick's, and the existing `theme` / `emulator` fix-ups run after it.
- Minimal change: one option, one phase function (~80 lines in `Run`), one `phasesFor` line, a window checkbox /
  `--image` flag. `package()`, `prepare()` and `unpack()` stay as they are.

A manifest-driven overlay was preferred over "just untar a second tarball": the manifest makes the check
before-writing possible and lets the installer skip paths it owns (next section).

**Important gap (phase 2 must decide):** the tool, as the brief asks, images the whole stick tree minus the
exclusions - so it also holds the owner's *old copy of what the package ships* (`Autobleem/bin/autobleem`, `VERSION`,
shipped themes, `Docs`...). Laid over a newer package it would downgrade the launcher. Either the installer skips
every path `prepare()` replaces (list in section 1; recommended, the tool then stays a dumb copy), or the tool
learns a `--user-layer-only` filter. The image then only wins for `config.ini`, `Apps/*` that the package does
not ship, user themes, `RetroArch/bin/playlists` and `retroarch.cfg`.

## 3. Exclusions (enforced in the tool, never copied)

Matched case-insensitively. A matching folder is left out with everything below it and recorded in the manifest
(`excluded`: path, rule, file count, bytes).

| Rule | What |
|---|---|
| `ssh-key` | `System/ssh/` (a temporary key), at any depth (e.g. `Autobleem/System/ssh`); `Bios`, `Preferences`, `Region`, `UI` below stay root-only |
| `console-state` | `System/Bios`, `System/Preferences`, `System/Region`, `System/UI` (per-console) |
| `installer-scratch` | `System/Install/` (the installer's download folder; not asked for, added) |
| `logs` | any folder named `Logs` (e.g. `System/Logs`, `RetroArch/bin/logs`), any `*.log` |
| `found` | top-level `FOUND.*` (chkdsk) |
| `windows-system` | `$RECYCLE.BIN`, `System Volume Information` |
| `tmp` | `*.tmp` |
| `thumbs-db` | `Thumbs.db` |
| `owner-data` | see section 5; left out unless `--include-owner-data` |
| suspicious names | a file, folder or link whose **name** looks like a credential is **not copied** and is listed in the manifest (`credential_names_not_copied`: path, rule; stdout too) - see "Suspicious names" below |

### Suspicious names (best effort, names only)

The tool judges by **name only**. It does not open the files to look for secrets: a content scan can always be
fooled (encodings, compression, split values), so the tool makes no promise about contents. The policy is:

- An entry (file, folder or link, at any depth, case-insensitive) is **suspicious** when its name matches one of
  `CREDENTIAL_PATTERNS` (e.g. `.env`, `.env.*`, `.envrc`, `*token*`, `*credential*`, `*secret*`, `*passw*`, `shadow`,
  `*.pem`, `*.key`, `*.pfx`, `*.p12`, `*.p8`, `*.ppk`, `*.kdbx`, `*.jks`, `*.gpg`, `*.asc`, `*.keychain`, `*.ovpn`,
  `*.tfvars`, `terraform.tfstate*`, `keystore*`, `api_key*`, `apikey*`, `id_rsa*` / `id_ed25519*` / `id_ecdsa*` /
  `id_dsa*`, `authorized_keys*`, `known_hosts*`, `kubeconfig*`, `wallet.dat`, `.bash_history`, `.s3cfg`, `.dockercfg`,
  `.boto`, `.my.cnf`, `.htpasswd`, `.npmrc`, `.pypirc`, `.pgpass`, `.netrc`, `.git-credentials`, `wpa_supplicant*`,
  `service-account*.json`, `private_key*`, `privkey*`, `cookies*.sqlite`) or is one of `CREDENTIAL_DIR_NAMES`
  (`.ssh`, `.git`, `.aws`, `.gnupg`, `.config`, `.kube`, `.docker`, `secret`, `secrets`, `credentials`, `tokens`,
  `keys`, `private`, `certs`, `.password-store`). The extension does not rescue a name: `token.png` and
  `secret-garden.png` are suspicious too.
- A suspicious folder is left out whole, with everything below it.
- A suspicious entry is copied **only** if the owner passes `--allow-suspicious FILE` and the entry's relative path
  (as in the manifest, `/` separated, a trailing `/` ignored) is a line of FILE. The match is exact: no patterns, no
  case folding, one path per line, empty lines ignored, UTF-8, at most 1 MiB. Allowing a folder copies the folder, but
  what is inside it is judged again by name (a suspicious name inside needs its own line).
- The manifest lists every skipped suspicious name (`credential_names_not_copied`) so the owner can review them, and
  every one the allow-list let through (`suspicious_names_allowed`); the counts are in `counts`.

Limits: a secret in a file with an ordinary name (`notes.txt`, `config.ini`, a saved Wi-Fi password in a theme file)
**is copied**. The owner reads the manifest, reviews the image before sharing it and remains responsible for what is on
the stick. The tool lowers the risk of an accident; it is not a secret filter.

Refused outright (nothing written, exit code 2): a missing or empty tree; a tree with no `Autobleem/` at its top;
a symlink that resolves outside the tree; a path component `..`, `.` or empty; Windows reserved names (`CON PRN AUX
NUL CONIN$ CONOUT$ COM1-9 LPT1-9`, also the superscript digits `COM¹ COM² COM³ LPT¹ LPT² LPT³`, also with an extension), names with `<>:"|?*\` or control characters or a trailing dot/space,
non-UTF-8 names; two names in one folder that differ only by case or by Unicode normalization (NFC/NFD); a path over
1024 characters; a root file `MANIFEST.json`; an output folder inside the tree; an `--out` that is an existing file;
a label ending in `-owner` (that suffix marks owner-data images); an unreadable file, an unreadable or non-UTF-8 allow-list, or any other I/O error (the
temporary files and an output folder made by the run are removed); a symlink in `--out` named like the artifact, the
manifest or their temporary files; an existing output without `--force`; every file excluded.
Symlinks that stay inside the tree and special files are not followed and not copied; they are listed under `skipped`.
Entries inside an excluded folder are not checked (they are not copied).

## 4. The tool

    python3 tools/make_stick_image.py TREE --label LABEL --out DIR [--include-owner-data] [--allow-suspicious FILE] [--force]

stdlib only, read-only on `TREE`. Writes in `DIR`: `autobleem-stick-image-<label>[-owner].tar.gz` and
`autobleem-stick-image-<label>[-owner].MANIFEST.json` (identical to the tar's `MANIFEST.json`). Output is
deterministic: members sorted by path (folders kept, so empty ones survive), mtime fixed 2020-01-01, uid/gid 0,
modes 0644 / 0755, gzip without name or time, GNU tar format (the core's `TarArchive` reads GNU long names but skips pax
headers) - two runs on the same tree give the same sha256. The manifest has no absolute path and no timestamp.
Output is written to fresh temporary files made in `DIR` (`mkstemp`: exclusive create, unique name, so a planted
link is never opened) and renamed over the final names; a rename replaces the name itself and never follows a link.
`--force` replaces the pair as one step: the old tar is parked, the new tar and manifest are renamed in, and if the
second rename fails the old tar is put back, so the old pair stays intact (exit 2). Messages and the manifest show
names with every control character escaped (`\x1b`), never raw.
Tests: `python3 -m unittest discover -s tests/tools -p "test_*.py"` (on a fake tree); the tester's adversarial
scripts `python3 tests/tools/adv_stick_image.py` and `python3 tests/tools/adv_stick_image_r3.py` run the tool as a
subprocess.

## 5. The owner's games and BIOS files

The owner's games (`Games/`, with `!SaveStates` and `!MemCards`), RetroArch's `roms/`, `bios/`, `bin/saves` and
`bin/savestates` are **owner data**: they are copyrighted or personal and must not travel in anything shareable. The
tool leaves them out by default; `--include-owner-data` puts them in for the owner's own image only, and then the
artifact name ends in `-owner` (and the manifest says `owner_data_included: true`). Such an image restores onto
the same owner's stick (phase 2: only by an explicit option) and is never published or attached to anything.
Backups of the real stick are the owner's own step (to `E:\tmp\`), not part of this work.

## 6. Open questions for phase 2

- Skip list vs `--user-layer-only` (section 2 gap): which one?
- Is "Preferences / Region / UI / Bios" really under `System/`? The brief names them without a parent; the tool
  takes `System/` - check on the real stick copy.
- Does `config.ini` of the image win over the installer's `emulator = pcsx-abnxt` rule (currently: no, the rule runs after)?
- `MANIFEST.json` lands at the stick's root if the installer untars the whole image; phase 2 should extract it to
  the scratch folder instead.
- Should owner-data images apply only when the label/manifest matches the stick (a safety check), and may they
  overwrite files of games already on the stick?
- Credential-name list: confirm it; the Foundry rule text was not given in full.
