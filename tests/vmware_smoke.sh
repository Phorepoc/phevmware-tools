#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: LGPL-2.1-only
#
# vmware_smoke.sh -- a scripted test of the VMware tools against a real VMware Workstation (Git Bash on Windows; vmrun on the PATH or
# VMRUN=...). It needs a PC image ISO built with SSM_TEST=phevmwtest (the pc variant workflow) and checks, from the serial log, the
# host's vmware.log and vmrun:
#   1. the kernel channel installs and opens ("[bdch] ... installed", "[phevmw] TCLO channel opened")
#   2. an RPCI round trip: the guest logs to vmware.log, sets guestinfo.phevmw.version; the host writes guestinfo.phevmw.in, the guest
#      reads it and writes guestinfo.phevmw.echo (vmrun readVariable)
#   3. the TCLO side: the host's "reset" is answered, the guest application registers ("toolbox-dnd" in vmware.log) and the copy and
#      paste version handshake ran
# usage: vmware_smoke.sh <dir with phorepoc-pc.iso> [seconds]
set -u
DIR=${1:?directory with phorepoc-pc.iso}; WAIT=${2:-240}
VMRUN=${VMRUN:-"/c/Program Files/VMware/VMware Workstation/vmrun.exe"}
VM=$DIR/PheVmwTest/PheVmwTest.vmx
mkdir -p "$DIR/PheVmwTest"; cp "$DIR/phorepoc-pc.iso" "$DIR/PheVmwTest/phorepoc-pc.iso" 2>/dev/null
[ -f "$VM" ] || { echo "no $VM (copy the vmx template next to the ISO)"; exit 2; }
rm -f "$DIR/PheVmwTest/serial.log"
"$VMRUN" -T ws start "$(cygpath -w "$VM")" nogui || exit 1
ok=0; i=0
while [ $i -lt "$WAIT" ]; do
  sleep 5; i=$((i+5))
  grep -q "phevmw\] started\|phevmw\] no VMware" "$DIR/PheVmwTest/serial.log" 2>/dev/null && { ok=1; break; }
done
echo "== serial log markers ($i s)"; grep -a "bdch\|phevmw" "$DIR/PheVmwTest/serial.log" | head -40
"$VMRUN" -T ws writeVariable "$(cygpath -w "$VM")" guestVar phevmw.in "hello-from-host"
sleep 40
echo "== guestinfo as the host sees it"
for v in phevmw.version phevmw.echo; do echo "$v = $("$VMRUN" -T ws readVariable "$(cygpath -w "$VM")" guestVar $v)"; done
echo "== vmware.log"; grep -a -i "phevmw\|GuestRpc\|toolbox\|capabilit" "$DIR/PheVmwTest/vmware.log" | head -40
echo "== serial log after"; grep -a "phevmw" "$DIR/PheVmwTest/serial.log" | tail -40
"$VMRUN" -T ws stop "$(cygpath -w "$VM")" hard
