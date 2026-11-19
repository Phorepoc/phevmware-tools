/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: LGPL-2.1-only
 * (a copy of the PC image's interface header of the same name, which is EPL-2.0 OR GPL-2.0-or-later there; its authors license this copy
 * under LGPL-2.1-only as well, so the package has one licence) */

// rphebackdoor.h -- OURS: the user-side API of the VMware backdoor channel (xk_bdchannel.cpp, built into ekern, device
// "PheBackdoor"). A thin, generic channel: the backdoor is reached with an IN instruction on port 0x5658, which only ring 0 may
// execute, so the kernel executes it for a client that holds CommDD -- and nothing more. Everything that speaks a protocol over it
// (GuestRPC: RPCI and TCLO, copy and paste, drag and drop) lives in a user-side program that installs from Catalogue.
//   Call  -> one backdoor IN: the client's registers in, the host's out (eax is set to the backdoor magic and the low half
//            of edx to the port by the kernel).
// What Call accepts (anything else is KErrNotSupported / KErrArgument):
//   * command 10, GETVERSION (is a VMware host there?)
//   * command 30, MESSAGE (GuestRPC), types OPEN .. CLOSE (0..6). OPEN only for the RPCI and TCLO protocols; every other type
//     only on a channel this client opened. The kernel closes the channels a client leaves open when its handle goes away.
// No high-bandwidth port, no other command (clipboard, mouse, time, power ...): those are not reachable through this channel.
// Open fails with KErrNotSupported where no backdoor answers (VirtualBox, real hardware).
#ifndef RPHEBACKDOOR_H
#define RPHEBACKDOOR_H

#include <e32cmn.h>
#ifndef __KERNEL_MODE__
#include <e32std.h>
#endif

_LIT(KPheBackdoorName, "PheBackdoor");
const TInt KPheBackdoorMajorVersion = 1;

struct TPheBdRegs
	{
	TUint32 iEax;
	TUint32 iEbx;
	TUint32 iEcx;
	TUint32 iEdx;
	TUint32 iEsi;
	TUint32 iEdi;
	};

enum TPheBdControl { EPheBdCtrlCall };

#ifndef __KERNEL_MODE__
class RPheBackdoor : public RBusLogicalChannel
	{
public:
	inline static TVersion VersionRequired() { return TVersion(KPheBackdoorMajorVersion, 0, 0); }
	inline TInt Open() { return DoCreate(KPheBackdoorName, VersionRequired(), KNullUnit, NULL, NULL, EOwnerProcess); }
	inline TInt Call(TPheBdRegs& aRegs) { TPckg<TPheBdRegs> p(aRegs); return DoControl(EPheBdCtrlCall, &p); }
	};
#endif

#endif
