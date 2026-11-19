# phevmware-tools

The VMware tools for Phorepoc OS, as a Catalogue package. Phorepoc runs as a VMware (Workstation or Player) guest on a PC (x86-64
CPU, x32 ABI); this is what lets the host and the guest share things the way VMware Tools do. The VMware counterpart of
[phevbox-additions](https://github.com/Phorepoc/phevbox-additions). LGPL-2.1.

| Step | What | State |
|---|---|---|
| 1 | the GuestRPC client (RPCI to the host, TCLO from it) over the image's backdoor channel | this repository, `phevmwsvc.exe` |
| 2 | copy and paste of text, both ways (copy and paste protocol version 4) | this repository |
| 3 | drag and drop of files from the host into `C:\Data\Received\` | designed, not built |

## How it fits together

    host (VMware Workstation / Player)  GuestRPC: RPCI (guest asks), TCLO (host calls the guest), copypaste.transport
         |  the backdoor: IN eax,dx on port 0x5658 (a privileged instruction), registers as the message
    kernel (the PC image, not here)  logical device PheBackdoor: does the IN for a CommDD client; GETVERSION and GuestRPC only
         |  rphebackdoor.h
    phevmwsvc.exe (this package)  message channels, RPCI, the TCLO loop, then copy and paste on it

The kernel side stays tiny and generic and is part of the PC image: it carries no LGPL or GPL code and speaks no protocol.
Everything that is VMware's protocol is here, so it installs and updates through Catalogue and is under this package's licence. A
kernel cannot load a driver from a package on Phorepoc (the PC kernel links its drivers in), which is why the channel is in the
image and the service is not.

### Copy and paste

`vmw_dndcp.cpp` speaks version 4 of VMware's copy and paste messages: the host sends the TCLO command `copypaste.transport <packet>`
and the guest answers by RPCI with the same command. A packet is a 56-byte header and a payload; the clipboard travels as a
serialised `CPClipboard` whose text item is NUL-terminated UTF-8. On the Symbian side the text is `CPlainText` on the
`CClipboard` (paragraph end = a new line on the host). The Symbian clipboard has no change notification: the service compares its text once a second.

## Install and use

Install `phevmware-tools-x32.sis` from Catalogue on a Phorepoc PC image that has the PheBackdoor channel. The package installs a boot
services entry (`\resource\phe\services\phevmwsvc.txt`) that core's shell selector runs before the shell, and a menu entry **VMware Tools**
for devices that do not run it. The service needs the CommDD capability to open the channel: it installs where the installer trusts the
package (Phorepoc's developer mode, the default). With no VMware under it the service says so in the debug log and ends.

Build: the pdk's `package.yml` (see `.github/workflows/release.yml`); `package.conf` names the component, `registry/components_phevmw.py`
its mmp. The test component `phevmwtest` (`SSM_TEST=phevmwtest` in a board image build) runs the same service with a self-test.
