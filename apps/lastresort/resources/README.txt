AutoBleem 2 - LastResortRecovery
================================

For a PlayStation Classic that no longer starts at all, and that Sony's own recovery (the stick with
LBOOT.EPB in the console, the power cord pulled and put back) cannot bring back.

What it does: writes the console's own backup - LBOOT.EPB, which AutoBleem made on the stick before it
installed its kernel - back to the console over USB, the way Android devices are repaired: with
Android's fastboot client, with the console in its fastboot mode. Then it turns Sony's recovery off
(so the console does not start into it again) and restarts the console.

You need:
  - the LBOOT.EPB from the stick AutoBleem was installed from (it is at the stick's root), or a copy;
  - a screwdriver and tweezers (or a piece of wire), to open the console and touch two pads on its board;
  - a USB data cable for the console's micro-USB port (where its power goes).

  1. Run LastResortRecovery.exe. It finds LBOOT.EPB on a USB stick by itself; or pick "A file on this
     PC..." if you copied it somewhere.
  2. Open the console and put it into fastboot mode: hold the two FASTBOOT pads on its board together
     (the window shows where) while you connect the console to this PC. The window sees the console
     arrive. The first time, Windows has no driver for it: press "Install driver" (Windows asks for
     permission once).
  3. Start the recovery. Every step is shown as it happens; it takes a few minutes. Do not disconnect
     the console until it says it is done.
  4. Disconnect the console, close it, connect its own power supply and switch it on.

About the driver: fastboot talks to the console through Windows' own WinUSB driver. LastResortRecovery
writes a driver package for exactly this device (USB 0BB4:0C01, "PlayStation Classic (fastboot)") and
signs it on the spot with a certificate made for that one package, which Windows is told to trust; the
certificate's private key is deleted straight away, so nothing else can ever be signed with it. It is
the method Zadig uses (libwdi). The driver can be removed in Device Manager like any other.

The folder platform-tools\ is Google's Android SDK Platform-Tools (fastboot.exe and the two DLLs it
needs), unchanged; its licences are in platform-tools\NOTICE.txt.

Everything the recovery does is logged to %TEMP%\LastResortRecovery\LastResortRecovery.log - keep it if
you ask for help.

For scripts: LastResortRecovery.exe --probe
             LastResortRecovery.exe --quiet --backup LBOOT.EPB --yes [--no-reboot]

By hand, anywhere fastboot runs (console in fastboot mode, the backup unzipped):
             fastboot flash BOOTIMG1 boot.img
             fastboot flash TEE1 tz.img
             fastboot flash ROOTFS1 rootfs.ext4      (only a full backup has it)
             fastboot flash USRDATA userdata.ext4
             fastboot flash MISC recovery-off.img    (a file of sixteen zero bytes)
             fastboot reboot
