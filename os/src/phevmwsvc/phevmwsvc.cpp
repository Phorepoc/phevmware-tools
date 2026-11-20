/*
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * phevmwsvc -- the VMware tools' user-side service for Phorepoc: GuestRPC over the PheBackdoor channel (vmw_backdoor.cpp),
 * the TCLO side the host talks to (this file), and on it copy and paste (vmw_dndcp.cpp).
 *
 * TCLO: the host's way to call into the guest. The guest opens a TCLO channel and polls it: each round sends the result of the
 * previous command ("OK <text>" or "ERROR <text>"; empty the first time) and receives the next command, or nothing:
 *   reset                    -> "OK ATR toolbox-dnd" (the host learns which guest application this is), then we register our
 *                               capabilities (the copy and paste version, by RPCI) and ping the copy and paste transport
 *   ping                     -> "OK "
 *   Capabilities_Register    -> "OK ", then the same registration
 *   Set_Option <o> <v>       -> "OK " (copypaste / enableDnD: noted; the rest is not ours: "ERROR Unknown Command")
 *   copypaste.transport <p>  -> the packet goes to vmw_dndcp.cpp
 *   dnd.transport <p>        -> likewise
 * (open-vm-tools lib/rpcIn/rpcin.c and lib/rpcChannel/rpcChannel.c, LGPL-2.1 only, are the reference for this loop.)
 *
 * One thread, one timer (100 ms). With no VMware backdoor (QEMU, VirtualBox, real hardware) the service says so in the debug log and
 * ends.
 */
#include <e32base.h>
#include <e32std.h>
#include <e32debug.h>
#include <f32file.h>
#include <baclipb.h>
#include <txtetext.h>
#include "vmw_backdoor.h"
#include "vmw_dndcp.h"

_LIT(KSingleton, "PheVmwSvcSingleton");
const TInt KTickUs = 100 * 1000;
const TInt KClipPollTicks = 10;		// the Symbian clipboard, once a second
const TInt KHostPollTicks = 20;		// ask the host for its clipboard every 2 s (until the host pushes by itself)

_LIT8(KToolsApp, "toolbox-dnd");
_LIT8(KCapDndVersion, "tools.capability.dnd_version 4");
_LIT8(KQueryDndVersion, "vmx.capability.dnd_version");
_LIT8(KCapCpVersion, "tools.capability.copypaste_version 4");
_LIT8(KQueryCpVersion, "vmx.capability.copypaste_version");

static void Log(const char* aMsg, TInt a = 0, TInt b = 0)
	{
	RDebug::Printf("[phevmw] %s %d %d", aMsg, a, b);
	}

static void LogText(const char* aMsg, const TDesC8& aText)
	{
	// RDebug::Printf takes an 8-bit format: %s is a char* (RDebug::Print, 16-bit, would read it as UTF-16 and run off the string)
	TBuf8<121> t(aText.Left(120));
	for (TInt i = 0; i < t.Length(); ++i)
		if (t[i] < 0x20 || t[i] > 0x7E)
			t[i] = '.';
	RDebug::Printf("[phevmw] %s: %s", aMsg, t.PtrZ());
	}

// ---- the Symbian clipboard ---------------------------------------------------------------------------------------

static HBufC* ReadClipTextL(RFs& aFs)
	{
	CClipboard* clip = NULL;
	TRAPD(r, clip = CClipboard::NewForReadingL(aFs));
	if (r != KErrNone || !clip)
		return HBufC::NewL(0);
	CleanupStack::PushL(clip);
	CPlainText* text = CPlainText::NewL();
	CleanupStack::PushL(text);
	TRAP(r, text->PasteFromStoreL(clip->Store(), clip->StreamDictionary(), 0));
	HBufC* out = HBufC::NewL(r == KErrNone ? text->DocumentLength() : 0);
	if (r == KErrNone && text->DocumentLength() > 0)
		{
		TPtr p = out->Des();
		text->Extract(p, 0, text->DocumentLength());
		}
	CleanupStack::PopAndDestroy(2);
	return out;
	}

static void WriteClipTextL(RFs& aFs, const TDesC& aText)
	{
	CPlainText* text = CPlainText::NewL();
	CleanupStack::PushL(text);
	if (aText.Length())
		text->InsertL(0, aText);
	CClipboard* clip = CClipboard::NewForWritingLC(aFs);
	text->CopyToStoreL(clip->Store(), clip->StreamDictionary(), 0, text->DocumentLength());
	clip->CommitL();
	CleanupStack::PopAndDestroy(2);
	}

// ---- the service -------------------------------------------------------------------------------------------------

class CVmwSvc : public CTimer, public MVmwClip
	{
public:
	static CVmwSvc* NewL()
		{
		CVmwSvc* s = new (ELeave) CVmwSvc;
		CleanupStack::PushL(s);
		s->ConstructL();
		CleanupStack::Pop(s);
		return s;
		}
	~CVmwSvc()
		{
		Cancel();
		iTclo.Close();
		delete iCp;
		delete iBd;
		iIn.Close();
		delete iLast;
		iFs.Close();
		}
	void StartL();
	// MVmwClip
	virtual void HostTextL(const TDesC& aText)
		{
		WriteClipTextL(iFs, aText);
		delete iLast;
		iLast = NULL;
		iLast = ReadClipTextL(iFs);			// our own write is not a change to report (the text as the clipboard now holds it)
		}
	virtual HBufC* GuestTextL() { return ReadClipTextL(iFs); }
private:
	CVmwSvc() : CTimer(EPriorityStandard), iResetDone(EFalse), iCapsDue(EFalse), iTicks(0), iNoSend(0) { CActiveScheduler::Add(this); }
	void RunL();
	void RegisterCaps();
	void Rpci(const TDesC8& aReq, const char* aWhat);
	void Dispatch(const TDesC8& aCmd);
	void SetResult(const TDesC8& aStatus, const TDesC8& aText = KNullDesC8());
	void CheckGuestClipboard();
	CVmwBackdoor* iBd;
	CVmwDndCp* iCp;
	TVmwChannel iTclo;
	RBuf8 iIn;
	RBuf8 iResult;
	RFs iFs;
	HBufC* iLast;
	TBool iResetDone, iCapsDue;
	TInt iTicks, iNoSend;
	TInt iDndVersion, iCpVersion;
	};

void CVmwSvc::StartL()
	{
	iBd = CVmwBackdoor::NewL();					// KErrNotSupported: no VMware backdoor here
	User::LeaveIfError(iFs.Connect());
	iCp = CVmwDndCp::NewL(*iBd, *this, iFs);
	iLast = HBufC::NewL(0);
	iIn.CreateL(256);
	iResult.CreateL(256);
	// is the host answering GuestRPC? (a QEMU vmport has the backdoor but no RPCI)
	RBuf8 reply;
	reply.CreateL(256);
	CleanupClosePushL(reply);
	TInt r = VmwRpci(*iBd, _L8("log phevmw: guest tools service starting"), reply);
	Log("rpci log: status", r);
	User::LeaveIfError(r == KErrNotFound ? KErrNotSupported : KErrNone);
	r = VmwRpci(*iBd, _L8("info-set guestinfo.phevmw.version 0.1.0"), reply);
	Log("rpci info-set: status", r);
	CleanupStack::PopAndDestroy(&reply);
	User::LeaveIfError(iTclo.Open(*iBd, KVmwProtoTclo));
	Log("TCLO channel opened");
	After(KTickUs);
	}

void CVmwSvc::Rpci(const TDesC8& aReq, const char* aWhat)
	{
	RBuf8 reply;
	if (reply.Create(256) != KErrNone)
		return;
	TInt r = VmwRpci(*iBd, aReq, reply);
	LogText(aWhat, reply);
	Log(aWhat, r);
	reply.Close();
	}

// the version handshake of the copy and paste / drag and drop plug-in: say which version we speak, ask which the host speaks
void CVmwSvc::RegisterCaps()
	{
	RBuf8 reply;
	if (reply.Create(256) != KErrNone)
		return;
	TInt r = VmwRpci(*iBd, KCapCpVersion(), reply);
	Log("tools.capability.copypaste_version: status", r);
	reply.Zero();
	r = VmwRpci(*iBd, KQueryCpVersion(), reply);
	Log("vmx.capability.copypaste_version: status", r);
	LogText("  host copy and paste version", reply);
	TLex8 lx(reply);
	iCpVersion = 0;
	lx.Val(iCpVersion);
	reply.Close();
	if (iCpVersion == 4)
		iCp->SendPing(EVmwCopyPaste);
	else
		Log("the host does not speak copy and paste version 4", iCpVersion);
	// drag and drop: the same handshake
	reply.Close();
	if (reply.Create(256) != KErrNone)
		return;
	r = VmwRpci(*iBd, KCapDndVersion(), reply);
	Log("tools.capability.dnd_version: status", r);
	reply.Zero();
	r = VmwRpci(*iBd, KQueryDndVersion(), reply);
	Log("vmx.capability.dnd_version: status", r);
	LogText("  host drag and drop version", reply);
	TLex8 lx2(reply);
	iDndVersion = 0;
	lx2.Val(iDndVersion);
	reply.Close();
	if (iDndVersion == 4)
		iCp->SendPing(EVmwDnd);
	else
		Log("the host does not speak drag and drop version 4", iDndVersion);
	}

void CVmwSvc::SetResult(const TDesC8& aStatus, const TDesC8& aText)
	{
	iResult.Zero();
	if (iResult.MaxLength() < aStatus.Length() + aText.Length())
		iResult.ReAlloc(aStatus.Length() + aText.Length());
	iResult.Append(aStatus);
	iResult.Append(aText);
	}

void CVmwSvc::Dispatch(const TDesC8& aCmd)
	{
	TInt sp = aCmd.Locate(' ');
	// (both branches TPtrC8: a ?: between a TDesC8& and a TPtrC8 would slice the TPtrC8 into a TDesC8 with no pointer)
	TPtrC8 name(aCmd);
	TPtrC8 args(KNullDesC8);
	if (sp >= 0)
		{
		name.Set(aCmd.Left(sp));
		args.Set(aCmd.Mid(sp + 1));
		}
	_LIT8(KOk, "OK ");
	_LIT8(KErr, "ERROR ");
	if (name == _L8("reset"))
		{
		TBuf8<32> atr;
		atr.Copy(_L8("ATR "));
		atr.Append(KToolsApp);
		SetResult(KOk, atr);
		iResetDone = ETrue;
		iCapsDue = ETrue;				// registered after this reply has been sent
		Log("TCLO: reset");
		}
	else if (name == _L8("ping"))
		SetResult(KOk);
	else if (name == _L8("Capabilities_Register"))
		{
		SetResult(KOk);
		iCapsDue = ETrue;
		Log("TCLO: Capabilities_Register");
		}
	else if (name == _L8("Set_Option"))
		{
		LogText("TCLO: Set_Option", args);
		if (args.Left(9) == _L8("copypaste") || args.Left(9) == _L8("enableDnD"))
			SetResult(KOk);
		else
			SetResult(KErr, _L8("Unknown Command"));
		}
	else if (name == _L8("copypaste.transport"))
		{
		SetResult(KOk);
		iCp->OnPacket(EVmwCopyPaste, args);
		}
	else if (name == _L8("f"))
		{
		// an HGFS request (a drag and drop writing its files): the reply is the result text
		RBuf8 rep;
		TInt hr = iCp->OnHgfs(args, rep);
		SetResult(KOk, hr == KErrNone ? (const TDesC8&)rep : (const TDesC8&)KNullDesC8());
		rep.Close();
		}
	else if (name == _L8("dnd.transport"))
		{
		SetResult(KOk);
		iCp->OnPacket(EVmwDnd, args);
		}
	else
		{
		LogText("TCLO: unknown command", name);
		SetResult(KErr, _L8("Unknown Command"));
		}
	}

void CVmwSvc::RunL()
	{
	// 1. the previous result out, the next command in
	TInt r = iTclo.Send(iResult);
	iResult.Zero();
	if (r == KErrNone)
		{
		if (iCapsDue)
			{
			iCapsDue = EFalse;
			RegisterCaps();
			}
		r = iTclo.Receive(iIn);
		if (r == KErrNone && iIn.Length())
			Dispatch(iIn);
		}
	if (r != KErrNone)
		{
		// the host closed the channel (a reset, a snapshot): open it again
		Log("TCLO channel error", r);
		iTclo.Close();
		iResult.Zero();
		TInt o = iTclo.Open(*iBd, KVmwProtoTclo);
		Log("TCLO channel reopened", o);
		}
	// 2. the Symbian clipboard, once a second; the host's, every 2 s
	++iTicks;
	if (iResetDone)
		{
		if (iTicks % KClipPollTicks == 0)
			CheckGuestClipboard();
		if (iTicks % KHostPollTicks == 0 && iCpVersion == 4)
			iCp->RequestHostClip();
		}
	After(KTickUs);
	}

// a change on the Symbian clipboard -> push it to the host
void CVmwSvc::CheckGuestClipboard()
	{
	HBufC* now = NULL;
	TRAPD(r, now = ReadClipTextL(iFs));
	if (r == KErrNone && now)
		{
		if (iLast->Compare(*now) != 0)
			{
			delete iLast;
			iLast = now;
			now = NULL;
			Log("guest clipboard changed: chars", iLast->Length());
			if (iLast->Length() && iCpVersion == 4)
				iCp->PushGuestClip();
			}
		}
	delete now;
#ifdef PHEVMW_SELFTEST
	static TBool done = EFalse;
	if (!done && iTicks >= 15 * 10)
		{
		done = ETrue;
		TRAPD(w, WriteClipTextL(iFs, _L("phevmw: guest to host clipboard, from the Symbian clipboard")));
		Log("selftest: text set on the Symbian clipboard", w);
		// the RPCI round trip: the host sets guestinfo.phevmw.in (vmrun writeVariable guestVar), we read it and echo it back
		RBuf8 reply;
		if (reply.Create(256) == KErrNone)
			{
			TInt g = VmwRpci(*iBd, _L8("info-get guestinfo.phevmw.in"), reply);
			Log("selftest: info-get guestinfo.phevmw.in: status", g);
			LogText("selftest: guestinfo.phevmw.in", reply);
			RBuf8 req;
			if (req.Create(300) == KErrNone)
				{
				req.Append(_L8("info-set guestinfo.phevmw.echo "));
				req.Append(reply);
				RBuf8 rr;
				if (rr.Create(64) == KErrNone)
					{
					VmwRpci(*iBd, req, rr);
					rr.Close();
					}
				req.Close();
				}
			reply.Close();
			}
		}
#endif
	}

static TInt Run()
	{
	RSemaphore single;
	if (single.CreateGlobal(KSingleton, 0) != KErrNone)
		{
		Log("already running");
		return KErrNone;
		}
	CTrapCleanup* cleanup = CTrapCleanup::New();
	CActiveScheduler* sched = new CActiveScheduler;
	if (!cleanup || !sched)
		return KErrNoMemory;
	CActiveScheduler::Install(sched);
	CVmwSvc* svc = NULL;
	TRAPD(r, svc = CVmwSvc::NewL());
	if (r == KErrNone)
		TRAP(r, svc->StartL());
	if (r == KErrNotSupported)
		Log("no VMware: the backdoor channel is absent or the host does not answer GuestRPC; ending", r);
	else
		Log("started", r);
	if (r == KErrNone)
		CActiveScheduler::Start();
	delete svc;
	delete sched;
	delete cleanup;
	single.Close();
	return r == KErrNotSupported ? KErrNone : r;
	}

TInt E32Main()
	{
	return Run();
	}
