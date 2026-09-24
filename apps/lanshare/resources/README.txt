AutoBleem LAN Share
===================

Puts your own PS1 games on the AutoBleem Store's LAN server - an abstored running on a Raspberry Pi, a NAS or
another PC on your home network - and reads a PS1 disc from this PC's CD or DVD drive for it. The Store on
the console, the Pi or the PC then installs them from there.

Start LanShare.exe. Nothing to install; the settings are kept in %LOCALAPPDATA%\AutoBleem LAN Share\.

The server
----------
- Address: the server's, as the Store has it - http://<its address>:<port> (the store.tsv address is fine too).
  Press Connect: its games and any problems its scan found are listed on the left.
- To put games on it, one of:
  - Share: the server's games folder as this PC reaches it on the network, e.g. \\raspberrypi\games (Samba).
    LAN Share copies there and asks the server to scan. The server itself stays read only.
  - Token: when the server was started with --allow-uploads, its upload token (the server prints it at start
    and keeps it in its state folder, <state>/upload-token). LAN Share uploads over HTTP; a stopped upload goes
    on where it stopped.
- Remove from the server...: takes the selected games off it. Nothing is deleted - each is moved into the
  .removed folder next to the server's games, and moving it back puts it back.

Publishing
----------
- Games on this PC: a folder with one folder per game (a .cue with its .bin files, a .chd, a .pbp). Tick games
  and press Publish the ticked games. "On the server" says whether the server has a game already (by serial,
  else by title); such a game is skipped, never sent twice.
- Read a disc and publish it: a PS1 disc in the drive is read whole into a .bin + .cue (+ .sbi for a LibCrypt
  game, when the drive gives the subchannel), named after its title, checked against the known good dump
  when the databases are chosen, and published. Tick "The game has more than one disc" to be asked for each
  next disc; they go up as one game. A disc the server has already is not sent.
- Databases: AutoBleem's covers folder (coversU/P/J.db) and RetroArch's "Sony - PlayStation.rdb" give the
  titles and the check of a read disc. Both are optional.

This PC
-------
"Also share the games on this PC with the Store" serves that folder from this PC as well (off by default). The
first time, Windows asks whether to let it through the firewall: allow private networks only.

LAN Share, the server and the Store speak plain HTTP, meant for a home network. Share only games you may share.
