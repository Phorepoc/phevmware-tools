/*
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * vmw_dndcp.h -- the VMware copy and paste / drag and drop transport, version 4 (messages over GuestRPC).
 */
#ifndef VMW_DNDCP_H
#define VMW_DNDCP_H

#include <e32base.h>
#include <f32file.h>
#include "vmw_backdoor.h"
#include "vmw_hgfs.h"

// the Symbian side of the clipboard
class MVmwClip
	{
public:
	virtual void HostTextL(const TDesC& aText) = 0;			// the host's text arrived: put it on the Symbian clipboard
	virtual HBufC* GuestTextL() = 0;						// the Symbian clipboard's text ("" if none)
	};

// which transport of the two a message travelled on
enum TVmwTransport { EVmwCopyPaste, EVmwDnd };

const TInt KVmwHdrBytes = 56;								// DnDCPMsgHdrV4: 14 little-endian words
const TUint32 KVmwMaxPayload = 65436 - KVmwHdrBytes;		// DND_CP_PACKET_MAX_PAYLOAD_SIZE_V4

// header words of a V4 message
struct TVmwMsg
	{
	TUint32 iCmd, iType, iSrc, iSession, iStatus, iP[6], iBinarySize, iOffset, iPayload;
	};

class CVmwDndCp : public CBase
	{
public:
	static CVmwDndCp* NewL(CVmwBackdoor& aBd, MVmwClip& aClip, RFs& aFs);
	~CVmwDndCp();
	// the packet of a copypaste.transport or dnd.transport command from the host (everything after the command and its space)
	void OnPacket(TVmwTransport aT, const TDesC8& aPacket);
	void SendPing(TVmwTransport aT);
	void RequestHostClip();									// ask the host for its clipboard
	void PushGuestClip();									// send the guest's text unasked (a change)
	TUint32 HostCaps() const { return iHostCaps; }
	// the host's HGFS requests (TCLO command "f"): the reply, empty if there is no drop in progress
	TInt OnHgfs(const TDesC8& aRequest, RBuf8& aReply);
private:
	CVmwDndCp(CVmwBackdoor& aBd, MVmwClip& aClip, RFs& aFs) : iBd(aBd), iClip(aClip), iFs(aFs), iHostCaps(0), iOutOffset(0), iOutT(EVmwCopyPaste), iDropping(EFalse), iDropSession(0), iCount(0) {}
	void ConstructL();
	void OnDnd(const TVmwMsg& aH, const TDesC8& aBin);
	void FinishDrop(TBool aOk);
	void CleanStaging();
	TInt Send(TVmwTransport aT, const TVmwMsg& aHdr, const TDesC8& aBinary);
	void SendChunk(TVmwTransport aT);
	void Deliver(TVmwTransport aT, const TVmwMsg& aH, const TDesC8& aBinary);
	void OnClipboardL(const TDesC8& aBinary);
	void SendGuestClipL(TUint32 aSession, TUint32 aActive);
	TInt SendCmdReply(TVmwTransport aT, TUint32 aCmd, TUint32 aStatus);
	CVmwBackdoor& iBd;
	MVmwClip& iClip;
	RFs& iFs;
	CVmwHgfs* iHgfs;
	TFileName iStaging;
	TBool iDropping;
	TUint32 iDropSession;
	TInt iCount;
	TUint32 iHostCaps;
	// a multi-packet message coming in (one per transport) and one going out
	RBuf8 iIn[2];
	TVmwMsg iInHdr[2];
	RBuf8 iOut;
	TVmwMsg iOutHdr;
	TUint32 iOutOffset;
	TVmwTransport iOutT;
	};

#endif
