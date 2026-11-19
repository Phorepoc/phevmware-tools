/*
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * vmw_backdoor.cpp -- GuestRPC over the VMware backdoor: the message protocol of open-vm-tools' lib/message/message.c (LGPL-2.1
 * only, "Copyright (C) 1999-2021 VMware, Inc.") reimplemented for Symbian over the kernel's PheBackdoor channel. The steps, the
 * register layout and the status bits are that file's; the code is ours. Low-bandwidth port only: every word crosses the channel
 * in a register, so a message of n bytes costs n/4 calls (clipboard text is small).
 */
#include <e32std.h>
#include <e32debug.h>
#include "vmw_backdoor.h"

// guest_msg_def.h
enum { EMsgOpen, EMsgSendSize, EMsgSendPayload, EMsgRecvSize, EMsgRecvPayload, EMsgRecvStatus, EMsgClose };
const TUint32 KStSuccess = 0x0001, KStDoRecv = 0x0002, KStCpt = 0x0010;
const TUint32 KCookieFlag = 0x80000000;
const TUint32 KCmdMessage = 30;
const TInt KMaxRetries = 4;

CVmwBackdoor* CVmwBackdoor::NewL()
	{
	CVmwBackdoor* b = new (ELeave) CVmwBackdoor;
	TInt r = b->iDev.Open();
	if (r != KErrNone)
		{
		delete b;
		User::Leave(r);
		}
	return b;
	}

CVmwBackdoor::~CVmwBackdoor()
	{
	iDev.Close();
	}

TInt TVmwChannel::Msg(TUint32 aType, TUint32 aEbx, TUint32& aEcxOut, TUint32& aEbxOut, TUint32& aEdxOut)
	{
	TPheBdRegs r;
	r.iEax = 0;
	r.iEbx = aEbx;
	r.iEcx = (aType << 16) | KCmdMessage;
	r.iEdx = iId << 16;
	r.iEsi = iCookieHi;
	r.iEdi = iCookieLo;
	TInt e = iBd->Call(r);
	aEcxOut = r.iEcx;
	aEbxOut = r.iEbx;
	aEdxOut = r.iEdx;
	return e;
	}

TInt TVmwChannel::Open(CVmwBackdoor& aBd, TUint32 aProto)
	{
	iBd = &aBd;
	iId = iCookieHi = iCookieLo = 0;
	for (TUint32 flags = KCookieFlag, pass = 0; pass < 2; flags = 0, ++pass)	// cookies first, then without
		{
		TPheBdRegs r;
		r.iEax = 0;
		r.iEbx = aProto | flags;
		r.iEcx = (EMsgOpen << 16) | KCmdMessage;
		r.iEdx = 0;
		r.iEsi = r.iEdi = 0;
		TInt e = iBd->Call(r);
		if (e != KErrNone)
			return e;
		if ((r.iEcx >> 16) & KStSuccess)
			{
			iId = r.iEdx >> 16;
			iCookieHi = r.iEsi;
			iCookieLo = r.iEdi;
			iOpen = ETrue;
			return KErrNone;
			}
		}
	return KErrNotFound;			// the host does not answer GuestRPC (a QEMU vmport: the backdoor is there, the messages are not)
	}

void TVmwChannel::Close()
	{
	if (iOpen)
		{
		TUint32 c, b, d;
		Msg(EMsgClose, 0, c, b, d);
		iOpen = EFalse;
		}
	}

TInt TVmwChannel::Send(const TDesC8& aMsg)
	{
	if (!iOpen)
		return KErrNotReady;
	for (TInt attempt = 0; attempt < KMaxRetries; ++attempt)
		{
		TUint32 c, b, d;
		TInt e = Msg(EMsgSendSize, aMsg.Length(), c, b, d);
		if (e != KErrNone)
			return e;
		if (!((c >> 16) & KStSuccess))
			return KErrGeneral;
		TBool retry = EFalse;
		for (TInt i = 0; i < aMsg.Length() && !retry; i += 4)
			{
			TUint32 w = 0;
			for (TInt k = 0; k < 4 && i + k < aMsg.Length(); ++k)
				w |= (TUint32)aMsg[i + k] << (8 * k);
			e = Msg(EMsgSendPayload, w, c, b, d);
			if (e != KErrNone)
				return e;
			if (!((c >> 16) & KStSuccess))
				{
				if ((c >> 16) & KStCpt)
					retry = ETrue;			// a checkpoint happened: send the message again
				else
					return KErrGeneral;
				}
			}
		if (!retry)
			return KErrNone;
		}
	return KErrGeneral;
	}

TInt TVmwChannel::Receive(RBuf8& aOut)
	{
	if (!iOpen)
		return KErrNotReady;
	aOut.Zero();
	for (TInt attempt = 0; attempt < KMaxRetries; ++attempt)
		{
		TUint32 c, size, d;
		TInt e = Msg(EMsgRecvSize, 0, c, size, d);
		if (e != KErrNone)
			return e;
		if (!((c >> 16) & KStSuccess))
			return KErrGeneral;
		if (!((c >> 16) & KStDoRecv))
			return KErrNone;				// nothing to receive
		if ((d >> 16) != EMsgSendSize || size > (TUint32)KVmwMaxMessage)
			{
			Msg(EMsgRecvStatus, 0, c, size, d);	// tell the host the message was not taken
			return KErrCorrupt;
			}
		if (aOut.MaxLength() < (TInt)size + 1)
			{
			e = aOut.ReAlloc(size + 1);
			if (e != KErrNone)
				{
				Msg(EMsgRecvStatus, 0, c, size, d);
				return e;
				}
			}
		TBool retry = EFalse;
		TUint32 left = size;
		while (left && !retry)
			{
			TUint32 w;
			e = Msg(EMsgRecvPayload, KStSuccess, c, w, d);
			if (e != KErrNone)
				return e;
			if (!((c >> 16) & KStSuccess))
				{
				if ((c >> 16) & KStCpt)
					{
					retry = ETrue;
					break;
					}
				Msg(EMsgRecvPayload, 0, c, w, d);
				return KErrGeneral;
				}
			if ((d >> 16) != EMsgSendPayload)
				{
				Msg(EMsgRecvPayload, 0, c, w, d);
				return KErrCorrupt;
				}
			TUint32 n = left < 4 ? left : 4;
			for (TUint32 k = 0; k < n; ++k)
				aOut.Append((TUint8)(w >> (8 * k)));
			left -= n;
			}
		if (retry)
			{
			aOut.Zero();
			continue;
			}
		TUint32 w;
		e = Msg(EMsgRecvStatus, KStSuccess, c, w, d);
		if (e != KErrNone)
			return e;
		if (!((c >> 16) & KStSuccess))
			{
			if ((c >> 16) & KStCpt)
				{
				aOut.Zero();
				continue;
				}
			return KErrGeneral;
			}
		return KErrNone;
		}
	return KErrGeneral;
	}

TInt VmwRpci(CVmwBackdoor& aBd, const TDesC8& aRequest, RBuf8& aReply)
	{
	TVmwChannel ch;
	TInt r = ch.Open(aBd, KVmwProtoRpci);
	if (r != KErrNone)
		return r;
	r = ch.Send(aRequest);
	if (r == KErrNone)
		r = ch.Receive(aReply);
	ch.Close();
	if (r != KErrNone)
		return r;
	if (aReply.Length() < 2 || aReply[1] != ' ' || (aReply[0] != '1' && aReply[0] != '0'))
		return KErrCorrupt;
	const TBool ok = aReply[0] == '1';
	aReply.Delete(0, 2);
	return ok ? KErrNone : KErrGeneral;
	}
