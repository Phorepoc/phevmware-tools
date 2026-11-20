/*
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * vmw_hgfs.cpp -- a minimal HGFS server for Symbian. The packet layouts and operation numbers are those of open-vm-tools'
 * lib/include/hgfsProto.h and hgfs.h (LGPL-2.1 only, "Copyright (C) 1998-2021 VMware, Inc."); the code is ours. Version-3
 * operations only (open, write, read, close, create-directory, get/set attributes, delete, query-volume), after either the legacy
 * 8-byte header (id, op) or the version-4 header (op 0xFF marker at offset 4, session create/destroy); everything else is answered
 * "operation not supported". The host reaches a path below the root the owner set (the staging directory of a drag and drop) and nothing
 * else: a name that leaves the root is refused.
 */
#include <e32std.h>
#include <e32debug.h>
#include "vmw_hgfs.h"

// hgfs.h, HgfsStatus (the order of the enum)
enum
	{
	KStOk = 0, KStNoSuchFile, KStInvalidHandle, KStNotPermitted, KStFileExists, KStNotDirectory, KStDirNotEmpty, KStProtocol,
	KStAccessDenied, KStInvalidName, KStGeneric, KStSharing, KStNoSpace, KStNotSupported, KStNameTooLong, KStInvalidParam
	};
// hgfsProto.h, HgfsOp
enum
	{
	KOpOpenV3 = 24, KOpReadV3, KOpWriteV3, KOpCloseV3, KOpSearchOpenV3, KOpSearchReadV3, KOpSearchCloseV3, KOpGetattrV3, KOpSetattrV3,
	KOpCreateDirV3, KOpDeleteFileV3, KOpDeleteDirV3, KOpRenameV3, KOpQueryVolumeV3,
	KOpCreateSessionV4 = 41, KOpDestroySessionV4 = 42, KOpNewHeader = 0xFF
	};
const TInt KV4HeaderBytes = 52;
const TInt KNameV3Prefix = 16;		// HgfsFileNameV3: length, flags, caseType, fid
const TInt KAttrV2Bytes = 96;
const TInt KMaxIo = 60 * 1024;

static TUint32 Get32(const TUint8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((TUint32)p[3] << 24); }
static TUint64 Get64(const TUint8* p) { return Get32(p) | ((TUint64)Get32(p + 4) << 32); }
static void Put32(RBuf8& b, TUint32 v) { for (TInt i = 0; i < 4; ++i) b.Append((TUint8)(v >> (8 * i))); }
static void Put64(RBuf8& b, TUint64 v) { Put32(b, (TUint32)v); Put32(b, (TUint32)(v >> 32)); }
static void PutZero(RBuf8& b, TInt n) { for (TInt i = 0; i < n; ++i) b.Append((TUint8)0); }

CVmwHgfs* CVmwHgfs::NewL(RFs& aFs)
	{
	return new (ELeave) CVmwHgfs(aFs);
	}

CVmwHgfs::~CVmwHgfs()
	{
	ClearRoot();
	iHandles.Close();
	iFiles.Close();
	}

void CVmwHgfs::SetRoot(const TDesC& aRoot)
	{
	ClearRoot();
	iRoot.Copy(aRoot);
	if (iRoot.Length() && iRoot[iRoot.Length() - 1] != '\\')
		iRoot.Append('\\');
	}

void CVmwHgfs::ClearRoot()
	{
	for (TInt i = 0; i < iFiles.Count(); ++i)
		{
		iFiles[i]->Close();
		delete iFiles[i];
		}
	iFiles.Reset();
	iHandles.Reset();
	iRoot.Zero();
	}

// the root as a CP name: "C:" "Data" "Received" ... separated by NULs (no leading or trailing separator)
TInt CVmwHgfs::RootAsCpName(RBuf8& aOut) const
	{
	aOut.Zero();
	TInt start = 0;
	for (TInt i = 0; i <= iRoot.Length(); ++i)
		{
		if (i == iRoot.Length() || iRoot[i] == '\\')
			{
			if (i > start)
				{
				if (aOut.Length())
					aOut.Append((TUint8)0);
				for (TInt k = start; k < i; ++k)
					aOut.Append((TUint8)iRoot[k]);
				}
			start = i + 1;
			}
		}
	return KErrNone;
	}

// a CP name (UTF-8 components separated by NUL) -> a Symbian path below the root; KErrAccessDenied if it leaves the root
TInt CVmwHgfs::MapName(const TDesC8& aCp, TFileName& aPath) const
	{
	if (!iRoot.Length())
		return KErrAccessDenied;
	TBuf<KMaxFileName> uni;
	TInt len = aCp.Length();
	while (len > 0 && aCp[len - 1] == 0)
		--len;
	TPtrC8 cp(aCp.Left(len));
	TFileName path;
	TInt start = 0;
	TInt comp = 0;
	for (TInt i = 0; i <= cp.Length(); ++i)
		{
		if (i == cp.Length() || cp[i] == 0)
			{
			TPtrC8 c(cp.Mid(start, i - start));
			start = i + 1;
			if (!c.Length())
				continue;
			if (c == _L8(".") )
				continue;
			if (c == _L8("..") || c.Locate('\\') >= 0 || c.Locate('/') >= 0)
				return KErrAccessDenied;
			TBuf<KMaxFileName> w;
			if (c.Length() > w.MaxLength())
				return KErrBadName;
			w.Copy(c);					// the names of a drop are ASCII or UTF-8 text: the high bytes pass through Copy as Latin-1
			if (comp == 0 && (w.Length() == 1 || (w.Length() == 2 && w[1] == ':')) && w.Length() <= 2 && TChar(w[0]).IsAlpha())
				{
				path.Zero();
				path.Append(w.Left(1));
				path.Append(':');
				path.Append('\\');
				}
			else
				{
				if (path.Length() + w.Length() + 1 > path.MaxLength())
					return KErrBadName;
				if (path.Length() && path[path.Length() - 1] != '\\')
					path.Append('\\');
				path.Append(w);
				}
			++comp;
			}
		}
	TFileName rootNoSlash(iRoot);
	rootNoSlash.Delete(rootNoSlash.Length() - 1, 1);
	if (path.CompareF(rootNoSlash) != 0 && path.Left(iRoot.Length()).CompareF(iRoot) != 0)
		return KErrAccessDenied;
	aPath = path;
	return KErrNone;
	}

static TUint32 ToStatus(TInt aErr)
	{
	switch (aErr)
		{
	case KErrNone: return KStOk;
	case KErrNotFound: case KErrPathNotFound: return KStNoSuchFile;
	case KErrAlreadyExists: return KStFileExists;
	case KErrAccessDenied: case KErrPermissionDenied: return KStAccessDenied;
	case KErrInUse: return KStSharing;
	case KErrBadName: return KStInvalidName;
	case KErrDiskFull: return KStNoSpace;
	case KErrArgument: return KStInvalidParam;
	default: return KStGeneric;
		}
	}

TInt CVmwHgfs::Process(const TDesC8& aReq, RBuf8& aReply)
	{
	aReply.Zero();
	const TUint8* p = aReq.Ptr();
	const TInt n = aReq.Length();
	if (n < 8)
		return KErrCorrupt;
	const TBool v4 = n >= KV4HeaderBytes && Get32(p + 4) == KOpNewHeader;
	TUint32 id, op;
	const TUint8* body;
	TInt blen;
	TUint32 flagsIn = 0;
	if (v4)
		{
		id = Get32(p + 16);
		op = Get32(p + 20);
		body = p + Get32(p + 12);			// headerSize
		blen = n - Get32(p + 12);
		flagsIn = Get32(p + 28);
		if (Get32(p + 12) > (TUint32)n)
			return KErrCorrupt;
		}
	else
		{
		id = Get32(p);
		op = Get32(p + 4);
		body = p + 8;
		blen = n - 8;
		}
	RDebug::Printf("[phevmw] hgfs: op %d, %d bytes%s", (TInt)op, n, v4 ? " (v4 header)" : "");
	RBuf8 out;
	if (out.Create(256) != KErrNone)
		return KErrNoMemory;
	TUint32 status = KStOk;
	TInt r = Dispatch(op, body, blen, out, status);
	if (r != KErrNone)
		{
		out.Close();
		return r;
		}
	// the reply: the same header kind, status, the operation's reply body
	if (aReply.Create(out.Length() + (v4 ? KV4HeaderBytes : 8)) != KErrNone)
		{
		out.Close();
		return KErrNoMemory;
		}
	if (v4)
		{
		aReply.Append(p, KV4HeaderBytes);
		TUint8* q = (TUint8*)aReply.Ptr();
		const TUint32 total = KV4HeaderBytes + out.Length();
		for (TInt i = 0; i < 4; ++i)
			{
			q[8 + i] = (TUint8)(total >> (8 * i));			// packetSize
			q[24 + i] = (TUint8)(status >> (8 * i));		// status
			q[28 + i] = (TUint8)(2 >> (8 * i));				// flags = reply
			}
		(void)flagsIn;
		}
	else
		{
		Put32(aReply, id);
		Put32(aReply, status);
		}
	if (status == KStOk || op == KOpCreateSessionV4)
		aReply.Append(out);
	else
		{
		// an error reply of a version-3 operation still carries the reserved word
		}
	out.Close();
	return KErrNone;
	}

TInt CVmwHgfs::Dispatch(TUint32 aOp, const TUint8* b, TInt len, RBuf8& out, TUint32& st)
	{
	TFileName path;
	switch (aOp)
		{
	case KOpCreateSessionV4:
		{
		// reply: sessionId, numCapabilities 0, maxPacketSize, identityOffset 0, flags 0, reserved
		++iSession;
		Put64(out, iSession);
		Put32(out, 0);
		Put32(out, 60000);
		Put32(out, 0);
		Put32(out, 0);
		Put32(out, 0);
		return KErrNone;
		}
	case KOpDestroySessionV4:
		PutZero(out, 4);
		return KErrNone;
	case KOpOpenV3:
		{
		if (len < 56 + KNameV3Prefix)
			{ st = KStProtocol; return KErrNone; }
		const TUint32 mask = Get32(b), mode = Get32(b + 4), flags = Get32(b + 8);
		const TUint32 nameLen = Get32(b + 56);
		if (56 + KNameV3Prefix + (TInt)nameLen > len)
			{ st = KStProtocol; return KErrNone; }
		TInt e = MapName(TPtrC8(b + 56 + KNameV3Prefix, nameLen), path);
		if (e != KErrNone)
			{ st = ToStatus(e); return KErrNone; }
		TUint32 acc = (mask & 1) ? (mode & 3) : 2;
		TUint m = EFileShareAny | (acc ? EFileWrite : EFileRead);
		RFile* f = new RFile;
		if (!f)
			return KErrNoMemory;
		if (acc)
			iFs.MkDirAll(path);			// the parents of a file the host creates (a no-op when the path is a directory name: see below)
		switch (flags)
			{
		case 0: e = f->Open(iFs, path, m); break;										// open, must exist
		case 1: e = f->Replace(iFs, path, m); break;										// open and empty (host: must exist; we allow)
		case 2: e = f->Open(iFs, path, m); if (e == KErrNotFound) e = f->Create(iFs, path, m); break;		// create if missing
		case 3: e = f->Create(iFs, path, m); break;										// create, error if present
		default: e = f->Replace(iFs, path, m); break;										// create or empty
			}
		if (e != KErrNone)
			{
			delete f;
			st = ToStatus(e);
			return KErrNone;
			}
		if (iFiles.Append(f) != KErrNone || iHandles.Append(iNextHandle) != KErrNone)
			{
			f->Close();
			delete f;
			return KErrNoMemory;
			}
		Put32(out, iNextHandle++);
		Put32(out, 0);					// no lock
		Put32(out, 0);
		Put32(out, 0);
		return KErrNone;
		}
	case KOpWriteV3:
		{
		if (len < 25)
			{ st = KStProtocol; return KErrNone; }
		TInt i = iHandles.Find(Get32(b));
		if (i < 0)
			{ st = KStInvalidHandle; return KErrNone; }
		const TUint64 off = Get64(b + 5);
		const TUint32 want = Get32(b + 13);
		const TInt avail = len - 25;
		TInt cnt = (TInt)want < avail ? (TInt)want : avail;
		TInt pos = (TInt)off;
		TInt e = iFiles[i]->Write(pos, TPtrC8(b + 25, cnt));
		if (e != KErrNone)
			{ st = ToStatus(e); return KErrNone; }
		iBytesWritten += cnt;
		Put32(out, cnt);
		Put64(out, 0);
		return KErrNone;
		}
	case KOpReadV3:
		{
		if (len < 24)
			{ st = KStProtocol; return KErrNone; }
		TInt i = iHandles.Find(Get32(b));
		if (i < 0)
			{ st = KStInvalidHandle; return KErrNone; }
		TInt want = (TInt)Get32(b + 12);
		if (want > KMaxIo)
			want = KMaxIo;
		HBufC8* buf = HBufC8::New(want ? want : 1);
		if (!buf)
			return KErrNoMemory;
		TPtr8 bp = buf->Des();
		TInt e = iFiles[i]->Read((TInt)Get64(b + 4), bp, want);
		if (e != KErrNone)
			{ delete buf; st = ToStatus(e); return KErrNone; }
		out.ReAlloc(16 + bp.Length());
		Put32(out, bp.Length());
		Put64(out, 0);
		out.Append(bp);
		delete buf;
		return KErrNone;
		}
	case KOpCloseV3:
		{
		if (len < 4)
			{ st = KStProtocol; return KErrNone; }
		TInt i = iHandles.Find(Get32(b));
		if (i < 0)
			{ st = KStInvalidHandle; return KErrNone; }
		iFiles[i]->Close();
		delete iFiles[i];
		iFiles.Remove(i);
		iHandles.Remove(i);
		Put64(out, 0);
		return KErrNone;
		}
	case KOpCreateDirV3:
		{
		if (len < 12 + KNameV3Prefix)
			{ st = KStProtocol; return KErrNone; }
		const TUint32 nameLen = Get32(b + 12);
		if (12 + KNameV3Prefix + (TInt)nameLen > len)
			{ st = KStProtocol; return KErrNone; }
		TInt e = MapName(TPtrC8(b + 12 + KNameV3Prefix, nameLen), path);
		if (e == KErrNone)
			{
			path.Append('\\');
			e = iFs.MkDirAll(path);
			if (e == KErrAlreadyExists)
				e = KErrNone;
			}
		st = ToStatus(e);
		Put64(out, 0);
		return KErrNone;
		}
	case KOpGetattrV3:
		{
		if (len < 12 + KNameV3Prefix)
			{ st = KStProtocol; return KErrNone; }
		const TUint32 nameLen = Get32(b + 12);
		if (12 + KNameV3Prefix + (TInt)nameLen > len)
			{ st = KStProtocol; return KErrNone; }
		TInt e = MapName(TPtrC8(b + 12 + KNameV3Prefix, nameLen), path);
		TEntry ent;
		if (e == KErrNone)
			{
			e = iFs.Entry(path, ent);
			if (e == KErrNotFound || e == KErrPathNotFound)
				{
				TFileName dir(path);
				dir.Append('\\');
				if (iFs.Entry(dir, ent) == KErrNone)
					e = KErrNone;
				}
			}
		if (e != KErrNone)
			{ st = ToStatus(e); return KErrNone; }
		TTime base(TDateTime(1601, EJanuary, 0, 0, 0, 0, 0));
		TTimeIntervalMicroSeconds us = ent.iModified.MicroSecondsFrom(base);
		TUint64 ft = (TUint64)us.Int64() * 10;
		// HgfsAttrV2: mask (type, size, write time), type, size, create, access, write, change, 4 perms, flags, alloc, uid, gid, fileId, vol, eff, reserved
		Put32(out, (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 9));
		Put32(out, ent.IsDir() ? 1 : 0);
		Put64(out, ent.IsDir() ? 0 : ent.iSize);
		Put64(out, ft); Put64(out, ft); Put64(out, ft); Put64(out, ft);
		out.Append((TUint8)0); out.Append((TUint8)6); out.Append((TUint8)6); out.Append((TUint8)6);		// special, owner, group, other: rw-
		Put32(out, 0);
		Put64(out, ent.IsDir() ? 0 : ent.iSize);
		PutZero(out, 4 + 4 + 8 + 4 + 4 + 8);
		Put64(out, 0);					// reserved
		PutZero(out, KNameV3Prefix);	// no symlink target
		return KErrNone;
		}
	case KOpSetattrV3:
		{
		// hints, attr (96), reserved, name: only the write time matters (a copy keeps the host's); everything else is accepted
		const TInt nameOff = 4 + KAttrV2Bytes + 8;
		if (len < nameOff + KNameV3Prefix)
			{ st = KStProtocol; return KErrNone; }
		const TUint32 nameLen = Get32(b + nameOff);
		if (nameOff + KNameV3Prefix + (TInt)nameLen > len)
			{ st = KStProtocol; return KErrNone; }
		TInt e = MapName(TPtrC8(b + nameOff + KNameV3Prefix, nameLen), path);
		if (e == KErrNone)
			{
			const TUint32 mask = Get32(b + 4);
			if (mask & (1 << 4))
				{
				TUint64 ft = Get64(b + 4 + 8 + 8 + 8 + 8 - 8 + 8);		// writeTime: mask 4, type 4, size 8, create 8, access 8 -> offset 4+32
				TTime base(TDateTime(1601, EJanuary, 0, 0, 0, 0, 0));
				TTime t = base + TTimeIntervalMicroSeconds(TInt64(ft / 10));
				iFs.SetModified(path, t);
				}
			}
		st = ToStatus(e);
		Put64(out, 0);
		return KErrNone;
		}
	case KOpDeleteFileV3:
	case KOpDeleteDirV3:
		{
		if (len < 12 + KNameV3Prefix)
			{ st = KStProtocol; return KErrNone; }
		const TUint32 nameLen = Get32(b + 12);
		if (12 + KNameV3Prefix + (TInt)nameLen > len)
			{ st = KStProtocol; return KErrNone; }
		TInt e = MapName(TPtrC8(b + 12 + KNameV3Prefix, nameLen), path);
		if (e == KErrNone)
			{
			if (aOp == KOpDeleteFileV3)
				e = iFs.Delete(path);
			else
				{
				path.Append('\\');
				e = iFs.RmDir(path);
				}
			}
		st = ToStatus(e);
		Put64(out, 0);
		return KErrNone;
		}
	case KOpQueryVolumeV3:
		{
		TVolumeInfo vi;
		TInt drv = iRoot.Length() ? (TInt)(TChar(iRoot[0]).GetUpperCase()) - 'A' : EDriveC;
		TInt e = iFs.Volume(vi, drv);
		if (e != KErrNone)
			{ st = ToStatus(e); return KErrNone; }
		Put64(out, vi.iFree);
		Put64(out, vi.iSize);
		return KErrNone;
		}
	default:
		RDebug::Printf("[phevmw] hgfs: operation %d not supported", (TInt)aOp);
		st = KStNotSupported;
		return KErrNone;
		}
	}
