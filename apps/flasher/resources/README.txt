AutoBleem 2 - the flasher for the PC USB stick
==============================================

What it does: writes the AutoBleem PC stick image onto a USB stick from this PC. Boot a PC from that
stick and it comes up as a PlayStation Classic-style console; the stick's first boot sets AutoBleem up
and turns the rest of the stick into the games partition.

  1. Run AutoBleemFlasher.exe. Windows asks for administrator rights: writing a whole disk needs them.
  2. Pick the channel: Release (the tested version), Testing (the next one, being tested) or Nightly
     (the newest development build - it may not work). The flasher downloads that channel's image from
     https://autobleem.retromenele.pl (about 650 MB), so the PC needs the internet. "An image file on this
     PC..." writes an .img.xz you downloaded yourself instead.
  3. Plug in a USB stick of 8 GB or more and pick it. Only USB and SD disks are offered, never the disk
     Windows runs from. EVERYTHING ON THE STICK IS ERASED - the flasher asks twice.
  4. Write. The image is checked against its published checksum, written, and read back and compared.

The downloaded image is kept in %TEMP%\AutoBleemFlasher, so a second stick does not download it again.

If Windows says the stick is in use, close every Explorer window and program that has a file on it open.

For scripts: AutoBleemFlasher.exe --list
             AutoBleemFlasher.exe --quiet --disk N --yes [--channel release|testing|nightly | --image FILE]
                                  [--no-verify]

On Linux or macOS, without the flasher:
             xzcat autobleem-*-pcusb-i386.img.xz | sudo dd of=/dev/sdX bs=4M status=progress
