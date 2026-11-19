/*
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * vmw_backdoor.h -- GuestRPC over the VMware backdoor, user side: message channels (RPCI to the host, TCLO from it).
 */
#ifndef VMW_BACKDOOR_H
#define VMW_BACKDOOR_H

#include <e32base.h>
#include "rphebackdoor.h"

const TUint32 KVmwProtoRpci = 0x49435052;	// "RPCI"
const TUint32 KVmwProtoTclo = 0x4F4C4354;	// "TCLO"
const TInt KVmwMaxMessage = 64 * 1024;		// what the host takes in one message

// the backdoor device, shared by the channels
class CVmwBackdoor : public CBase
	{
public:
	static CVmwBackdoor* NewL();				// leaves KErrNotSupported where no VMware backdoor answers
	~CVmwBackdoor();
	TInt Call(TPheBdRegs& aRegs) { return iDev.Call(aRegs); }
private:
	RPheBackdoor iDev;
	};

// one GuestRPC message channel
class TVmwChannel
	{
public:
	TVmwChannel() : iBd(NULL), iId(0), iOpen(EFalse) {}
	TInt Open(CVmwBackdoor& aBd, TUint32 aProto);
	void Close();
	TBool IsOpen() const { return iOpen; }
	TInt Send(const TDesC8& aMsg);
	// the next message from the host: KErrNone with aOut empty = none waiting; the message is in aOut (RBuf8, grown as needed)
	TInt Receive(RBuf8& aOut);
private:
	TInt Msg(TUint32 aType, TUint32 aEbx, TUint32& aEcxOut, TUint32& aEbxOut, TUint32& aEdxOut);
	CVmwBackdoor* iBd;
	TUint32 iId, iCookieHi, iCookieLo;
	TBool iOpen;
	};

// RPCI: one request to the host (open, send, receive, close). aReply gets the text after the "1 " / "0 " status; KErrNone if the host
// answered "1 ", KErrGeneral if "0 ".
TInt VmwRpci(CVmwBackdoor& aBd, const TDesC8& aRequest, RBuf8& aReply);

#endif
