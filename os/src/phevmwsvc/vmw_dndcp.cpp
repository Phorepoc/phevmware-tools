/*
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * vmw_dndcp.cpp -- the VMware copy and paste (and, next, drag and drop) transport, version 4, for Symbian. Reimplemented from
 * open-vm-tools' services/plugins/dndcp (LGPL-2.1 only, "Copyright (C) 2005-2021 VMware, Inc."): the message header and its
 * packetising (dnd/dndCPMsgV4.[ch], dndGuest/rpcV4Util.cpp), the command numbers (dndCPMsgV4.h), the clipboard serialisation
 * (dnd/dndClipboard.c) and the guest's copy and paste behaviour (dndGuest/copyPasteRpcV4.cc, guestCopyPasteMgr.cc,
 * guestCopyPasteDest.cc, guestCopyPasteSrc.cc). The numbers and layouts are theirs; the code is ours.
 *
 * Over GuestRPC: the host sends the TCLO command "copypaste.transport <packet>"; the guest sends the same command by RPCI.
 * A packet is a 56-byte header (14 words) and a payload; a message whose binary is longer than 65380 bytes is split into
 * packets, the receiver asking for each next one (DNDCP_CMD_REQUEST_NEXT).
 */
#include <e32std.h>
#include <e32debug.h>
#include <charconv.h>
#include <utf.h>
#include "vmw_dndcp.h"

// dndCPMsgV4.h
enum
	{
	KCmdPing = 1, KCmdPingReply = 2, KCmdRequestNext = 3, KCmdReply = 4,
	KCpRequestClipboard = 2000, KCpRequestFiles, KCpRecvClipboard, KCpSendClipboard, KCpGetFilesDone, KCpSendFilesDone
	};
enum { KTypeDnd = 1, KTypeCp = 2, KTypeFt = 3 };
enum { KSrcHost = 1, KSrcController = 2, KSrcGuest = 3 };
enum { KStatusSuccess = 0, KStatusError = 1, KStatusInvalidPacket = 5 };
enum { KCapValid = 1 << 0, KCapDnd = 1 << 1, KCapCp = 1 << 2, KCapPlainTextDnd = 1 << 3, KCapPlainTextCp = 1 << 4 };
const TUint32 KMaxBinary = 1 << 22;

// what this guest can do: copy and paste of plain text
const TUint32 KGuestCpCaps = KCapValid | KCapCp | KCapPlainTextCp;

// dnd.h: CPFORMAT_MAX; the text format is 1 (NUL terminated UTF-8)
const TUint32 KCpFormatMax = 11, KCpFormatText = 1;

_LIT8(KCmdCp, "copypaste.transport ");
_LIT8(KCmdDnd, "dnd.transport ");

static TUint32 Get32(const TUint8* aP) { return aP[0] | (aP[1] << 8) | (aP[2] << 16) | ((TUint32)aP[3] << 24); }
static void Put32(RBuf8& aB, TUint32 aV)
	{
	aB.Append((TUint8)aV);
	aB.Append((TUint8)(aV >> 8));
	aB.Append((TUint8)(aV >> 16));
	aB.Append((TUint8)(aV >> 24));
	}

static void Log(const char* aMsg, TInt a = 0, TInt b = 0)
	{
	RDebug::Printf("[phevmw] %s %d %d", aMsg, a, b);
	}

CVmwDndCp* CVmwDndCp::NewL(CVmwBackdoor& aBd, MVmwClip& aClip)
	{
	return new (ELeave) CVmwDndCp(aBd, aClip);
	}

CVmwDndCp::~CVmwDndCp()
	{
	iIn[0].Close();
	iIn[1].Close();
	iOut.Close();
	}

static TUint32 TypeOf(TVmwTransport aT) { return aT == EVmwCopyPaste ? KTypeCp : KTypeDnd; }

static void PutHeader(RBuf8& aReq, const TVmwMsg& aH, TUint32 aTotal, TUint32 aOffset, TUint32 aPayload)
	{
	Put32(aReq, aH.iCmd); Put32(aReq, aH.iType); Put32(aReq, aH.iSrc); Put32(aReq, aH.iSession); Put32(aReq, aH.iStatus);
	for (TInt i = 0; i < 6; ++i)
		Put32(aReq, aH.iP[i]);
	Put32(aReq, aTotal); Put32(aReq, aOffset); Put32(aReq, aPayload);
	}

// header + binary as the command string the host takes by RPCI (a binary longer than one packet: the first packet now, the rest as asked)
TInt CVmwDndCp::Send(TVmwTransport aT, const TVmwMsg& aH, const TDesC8& aBinary)
	{
	const TUint32 total = aBinary.Length();
	const TUint32 n = total > KVmwMaxPayload ? KVmwMaxPayload : total;
	const TDesC8& cmd = aT == EVmwCopyPaste ? KCmdCp() : KCmdDnd();
	RBuf8 req;
	TInt r = req.Create(cmd.Length() + KVmwHdrBytes + n);
	if (r != KErrNone)
		return r;
	req.Append(cmd);
	PutHeader(req, aH, total, 0, n);
	req.Append(aBinary.Left(n));
	RBuf8 reply;
	reply.Create(64);
	r = VmwRpci(iBd, req, reply);
	reply.Close();
	req.Close();
	if (r == KErrNone && total > n)
		{
		iOut.Close();
		if (iOut.Create(aBinary) == KErrNone)
			{
			iOutHdr = aH;
			iOutOffset = n;
			iOutT = aT;
			}
		}
	return r;
	}

void CVmwDndCp::SendChunk(TVmwTransport aT)
	{
	if (!iOut.Length() || iOutT != aT)
		return;
	const TUint32 total = iOut.Length();
	const TUint32 n = total - iOutOffset > KVmwMaxPayload ? KVmwMaxPayload : total - iOutOffset;
	const TDesC8& cmd = aT == EVmwCopyPaste ? KCmdCp() : KCmdDnd();
	RBuf8 req;
	if (req.Create(cmd.Length() + KVmwHdrBytes + n) != KErrNone)
		return;
	req.Append(cmd);
	PutHeader(req, iOutHdr, total, iOutOffset, n);
	req.Append(iOut.Mid(iOutOffset, n));
	RBuf8 reply;
	reply.Create(64);
	TInt r = VmwRpci(iBd, req, reply);
	reply.Close();
	req.Close();
	iOutOffset += n;
	if (r != KErrNone || iOutOffset >= total)
		iOut.Close();
	}

static void InitMsg(TVmwMsg& aM, TVmwTransport aT, TUint32 aCmd, TUint32 aSession)
	{
	Mem::FillZ(&aM, sizeof(aM));
	aM.iCmd = aCmd;
	aM.iType = TypeOf(aT);
	aM.iSrc = KSrcGuest;
	aM.iSession = aSession;
	}

TInt CVmwDndCp::SendCmdReply(TVmwTransport aT, TUint32 aCmd, TUint32 aStatus)
	{
	TVmwMsg m;
	InitMsg(m, aT, KCmdReply, 0);
	m.iStatus = aStatus;
	m.iP[0] = aCmd;
	return Send(aT, m, KNullDesC8());
	}

void CVmwDndCp::SendPing(TVmwTransport aT)
	{
	TVmwMsg m;
	InitMsg(m, aT, KCmdPing, 0);
	m.iP[0] = 4;										// version 4.0
	m.iP[1] = 0;
	m.iP[2] = aT == EVmwCopyPaste ? KGuestCpCaps : KCapValid;
	TInt r = Send(aT, m, KNullDesC8());
	Log("ping sent: transport, status", aT, r);
	}

// ---- the clipboard binary and the text ------------------------------------------------------------------------------

// Symbian text (paragraph U+2029, line break U+2028) -> NUL terminated UTF-8 with "\n" line ends
static HBufC8* TextToUtf8L(const TDesC& aText)
	{
	HBufC* t = HBufC::NewLC(aText.Length());
	TPtr tp = t->Des();
	for (TInt i = 0; i < aText.Length(); ++i)
		tp.Append(aText[i] == 0x2029 || aText[i] == 0x2028 ? TChar('\n') : TChar(aText[i]));
	HBufC8* u = HBufC8::NewL(aText.Length() * 3 + 1);
	CleanupStack::PushL(u);
	TPtr8 up = u->Des();
	CnvUtfConverter::ConvertFromUnicodeToUtf8(up, *t);
	up.Append(TChar(0));
	CleanupStack::Pop(u);
	CleanupStack::PopAndDestroy(t);
	return u;
	}

static HBufC* Utf8ToTextL(const TDesC8& aUtf8)
	{
	TInt len = aUtf8.Length();
	while (len > 0 && aUtf8[len - 1] == 0)
		--len;
	HBufC* w = HBufC::NewLC(len + 1);
	TPtr wp = w->Des();
	CnvUtfConverter::ConvertToUnicodeFromUtf8(wp, aUtf8.Left(len));
	HBufC* out = HBufC::NewL(wp.Length());
	TPtr op = out->Des();
	for (TInt i = 0; i < wp.Length(); ++i)
		{
		TText c = wp[i];
		if (c == '\r')
			{
			if (i + 1 < wp.Length() && wp[i + 1] == '\n')
				++i;
			c = 0x2029;
			}
		else if (c == '\n')
			c = 0x2029;
		op.Append(TChar(c));
		}
	CleanupStack::PopAndDestroy(w);
	return out;
	}

void CVmwDndCp::OnClipboardL(const TDesC8& aBin)
	{
	// CPClipboard_Serialize: maxFmt, then per format (1 .. maxFmt-1): exists, size, data; then "changed"
	if (aBin.Length() < 4)
		User::Leave(KErrCorrupt);
	const TUint8* p = aBin.Ptr();
	TInt left = aBin.Length();
	TUint32 maxFmt = Get32(p);
	p += 4;
	left -= 4;
	if (maxFmt > KCpFormatMax)
		maxFmt = KCpFormatMax;
	TPtrC8 text;
	TBool have = EFalse;
	for (TUint32 fmt = 1; fmt < maxFmt; ++fmt)
		{
		if (left < 8)
			User::Leave(KErrCorrupt);
		TUint32 exists = Get32(p), size = Get32(p + 4);
		p += 8;
		left -= 8;
		if (exists && size)
			{
			if ((TInt)size > left)
				User::Leave(KErrCorrupt);
			if (fmt == KCpFormatText)
				{
				text.Set(p, size);
				have = ETrue;
				}
			p += size;
			left -= size;
			}
		}
	if (!have)
		{
		Log("host clipboard has no text");
		return;
		}
	HBufC* t = Utf8ToTextL(text);
	CleanupStack::PushL(t);
	iClip.HostTextL(*t);
	Log("host text -> Symbian clipboard: chars", t->Length());
	CleanupStack::PopAndDestroy(t);
	}

void CVmwDndCp::SendGuestClipL(TUint32 aSession, TUint32 aActive)
	{
	HBufC* t = iClip.GuestTextL();
	CleanupStack::PushL(t);
	HBufC8* u = t->Length() ? TextToUtf8L(*t) : NULL;
	CleanupStack::PopAndDestroy(t);
	CleanupStack::PushL(u);
	// the clipboard binary: our one format, the rest "not there"
	RBuf8 bin;
	bin.CreateL(8 + (u ? u->Length() : 0) + 12 * KCpFormatMax);
	CleanupClosePushL(bin);
	Put32(bin, KCpFormatMax);
	for (TUint32 fmt = 1; fmt < KCpFormatMax; ++fmt)
		{
		if (fmt == KCpFormatText && u)
			{
			Put32(bin, 1);
			Put32(bin, u->Length());
			bin.Append(*u);
			}
		else
			{
			Put32(bin, 0);
			Put32(bin, 0);
			}
		}
	Put32(bin, 1);										// changed
	TVmwMsg m;
	InitMsg(m, EVmwCopyPaste, KCpSendClipboard, aSession);
	m.iP[0] = 4;										// cpInfo: major, minor, (capability), isActive
	m.iP[1] = 0;
	m.iP[3] = aActive;
	TInt r = Send(EVmwCopyPaste, m, bin);
	Log("guest text -> host: bytes, status", bin.Length(), r);
	CleanupStack::PopAndDestroy(2);						// bin, u
	}

void CVmwDndCp::RequestHostClip()
	{
	TVmwMsg m;
	InitMsg(m, EVmwCopyPaste, KCpRequestClipboard, 0);
	m.iP[0] = 4;
	m.iP[1] = 0;
	Send(EVmwCopyPaste, m, KNullDesC8());
	}

void CVmwDndCp::PushGuestClip()
	{
	TRAPD(r, SendGuestClipL(0, 1));
	if (r != KErrNone)
		Log("push guest clipboard failed", r);
	}

// ---- incoming ----------------------------------------------------------------------------------------------------------

void CVmwDndCp::OnPacket(TVmwTransport aT, const TDesC8& aPacket)
	{
	if (aPacket.Length() < KVmwHdrBytes)
		{
		Log("short packet", aPacket.Length());
		return;
		}
	const TUint8* p = aPacket.Ptr();
	TVmwMsg h;
	h.iCmd = Get32(p); h.iType = Get32(p + 4); h.iSrc = Get32(p + 8); h.iSession = Get32(p + 12); h.iStatus = Get32(p + 16);
	for (TInt i = 0; i < 6; ++i)
		h.iP[i] = Get32(p + 20 + 4 * i);
	h.iBinarySize = Get32(p + 44); h.iOffset = Get32(p + 48); h.iPayload = Get32(p + 52);
	if (h.iPayload + KVmwHdrBytes != (TUint32)aPacket.Length() || h.iBinarySize > KMaxBinary
		|| h.iOffset > h.iBinarySize || h.iPayload > h.iBinarySize || h.iOffset + h.iPayload > h.iBinarySize)
		{
		Log("invalid packet: size, payload", aPacket.Length(), h.iPayload);
		SendCmdReply(aT, 0, KStatusInvalidPacket);
		return;
		}
	TPtrC8 payload(p + KVmwHdrBytes, h.iPayload);
	if (h.iBinarySize <= KVmwMaxPayload)
		{
		if (h.iOffset != 0)
			{
			SendCmdReply(aT, 0, KStatusInvalidPacket);
			return;
			}
		Deliver(aT, h, payload);
		return;
		}
	// a message in several packets
	RBuf8& in = iIn[aT];
	TVmwMsg& ih = iInHdr[aT];
	if (h.iOffset == 0)
		{
		in.Close();
		if (in.Create(h.iBinarySize) != KErrNone)
			return;
		ih = h;
		}
	else if (in.Length() != (TInt)h.iOffset || ih.iBinarySize != h.iBinarySize || ih.iSession != h.iSession)
		{
		in.Close();
		SendCmdReply(aT, 0, KStatusInvalidPacket);
		return;
		}
	in.Append(payload);
	if (h.iOffset + h.iPayload < h.iBinarySize)
		{
		TVmwMsg m;
		InitMsg(m, aT, KCmdRequestNext, h.iSession);
		m.iP[0] = h.iCmd;
		m.iP[1] = h.iBinarySize;
		m.iP[2] = in.Length();
		Send(aT, m, KNullDesC8());
		return;
		}
	Deliver(aT, ih, in);
	in.Close();
	}

void CVmwDndCp::Deliver(TVmwTransport aT, const TVmwMsg& aH, const TDesC8& aBin)
	{
	Log("message: cmd, bytes", aH.iCmd, aBin.Length());
	switch (aH.iCmd)
		{
	case KCmdPing:
		{
		iHostCaps = aH.iP[2];
		TVmwMsg m;
		InitMsg(m, aT, KCmdPingReply, aH.iSession);
		m.iP[0] = 4;
		m.iP[1] = 0;
		m.iP[2] = aT == EVmwCopyPaste ? KGuestCpCaps : KCapValid;
		Send(aT, m, KNullDesC8());
		break;
		}
	case KCmdPingReply:
		iHostCaps = aH.iP[2];
		Log("host capabilities", iHostCaps);
		break;
	case KCmdRequestNext:
		SendChunk(aT);
		break;
	case KCmdReply:
		Log("host reply: command, status", aH.iP[0], aH.iStatus);
		break;
	default:
		if (aT == EVmwCopyPaste)
			{
			if (aH.iCmd == KCpRecvClipboard)
				{
				TRAPD(r, OnClipboardL(aBin));
				if (r != KErrNone)
					Log("host clipboard unreadable", r);
				break;
				}
			if (aH.iCmd == KCpRequestClipboard)
				{
				// the host pastes: give it the Symbian clipboard's text (cpInfo: param4 = active)
				TRAPD(r, SendGuestClipL(aH.iSession, aH.iP[3]));
				if (r != KErrNone)
					Log("guest clipboard send failed", r);
				break;
				}
			}
		Log("unhandled command", aH.iCmd, aT);
		break;
		}
	}
