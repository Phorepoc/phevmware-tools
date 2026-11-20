/*
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * vmw_hgfs.h -- a minimal HGFS server: just what the host needs to write the files of a drag and drop into a directory of this device.
 */
#ifndef VMW_HGFS_H
#define VMW_HGFS_H

#include <e32base.h>
#include <f32file.h>

class CVmwHgfs : public CBase
	{
public:
	static CVmwHgfs* NewL(RFs& aFs);
	~CVmwHgfs();
	// the files the host may touch: everything at or below this directory (a Symbian path with a drive, ending in a backslash)
	void SetRoot(const TDesC& aRoot);
	void ClearRoot();
	// one HGFS request (legacy or version-4 header, version-3 operations) -> its reply (aReply is emptied first)
	TInt Process(const TDesC8& aRequest, RBuf8& aReply);
	// the root directory as the CP name the host uses (components separated by NULs)
	TInt RootAsCpName(RBuf8& aOut) const;
	TInt Written() const { return iBytesWritten; }
private:
	CVmwHgfs(RFs& aFs) : iFs(aFs), iNextHandle(1), iBytesWritten(0), iSession(0) {}
	TInt MapName(const TDesC8& aCp, TFileName& aPath) const;
	TInt Dispatch(TUint32 aOp, const TUint8* aBody, TInt aLen, RBuf8& aOut, TUint32& aStatus);
	RFs& iFs;
	TFileName iRoot;
	RArray<TUint32> iHandles;
	RPointerArray<RFile> iFiles;
	TUint32 iNextHandle;
	TInt iBytesWritten;
	TUint64 iSession;
	};

#endif
