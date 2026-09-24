AutoBleem 2 - the installer for the PlayStation Classic
=======================================================

What it does: puts AutoBleem onto a USB stick from this PC, and later updates it.

  1. Pick the channel: Release (the tested version), Testing (the next one, being tested) or Nightly
     (the newest development build - it may not work). The installer downloads that channel's
     AutoBleem from https://autobleem.retromenele.pl, so the PC needs the internet.
  2. Plug a USB stick into the PC. Pick it in the stick box (only removable drives are offered).
     The console needs a FAT32 stick (exFAT works with the AutoBleem kernel) - "Format..." makes one,
     erasing everything on it. Windows formats FAT32 up to 32 GB; for a bigger stick put fat32format.exe
     (Ridgecrop's free tool) next to this program, or use exFAT.
  3. Choose: the cover databases (Japan, USA, PAL - all three by default), RetroArch for the other
     systems' games (with its cores, apps and libraries; off by default), the BIOS files those cores
     need (needs RetroArch; about 300 MB, fetched from RetroBIOS), the sample games.
  4. Install. Then put the stick into the console's second controller port and boot it.

Run it again over a stick that has AutoBleem to update it: the launcher, the scripts, the themes and
the console tools are replaced; your games, save states, memory cards, settings, RetroArch's saves and
playlists, and anything else on the stick stay as they are.

Everything comes from https://autobleem.retromenele.pl: the chosen channel's AutoBleem and UpdateRoms,
and whatever else is ticked. The window says what is on the stick and what the channel would put there.

For scripts: AutoBleemInstaller.exe --quiet --drive F: [--channel release|testing|nightly] [--covers JUP]
             [--retroarch] [--bios] [--samples] [--package autobleem-psc-<version>.tar.gz]

If the console no longer starts at all - and Sony's recovery (the stick with LBOOT.EPB, the power cord
pulled and put back) does not bring it back - LastResortRecovery\LastResortRecovery.exe in this folder
writes the console's own backup back to it over USB. Its README.txt says what it takes.
