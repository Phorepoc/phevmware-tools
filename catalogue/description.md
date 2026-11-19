# VMware tools

The guest side of VMware's host integration for a Phorepoc PC image running in VMware Workstation or Player. This first version
shares the **clipboard** (text) in both directions: copy on the host, paste in the device, and the other way round.

- Needs *Enable copy and paste* in the VM's settings (Options > Guest Isolation), which is on by default.
- Starts at boot where the device runs boot services; otherwise start **VMware Tools** from the menu once after installing.
- Text only, as much as fits in one message (64 KB). Pictures and files are not shared yet; dropping files from the host comes next.
- Needs the PC image's kernel channel for guest-tools programs (the image says so in its release notes); on other hardware, or
  under another hypervisor, it finds no VMware and ends.

Free software, LGPL-2.1. The licence and the written source offer are installed with the package in `\resource\phevmw\licences\`.
