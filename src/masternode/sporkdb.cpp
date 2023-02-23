// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/init.h>
#include <masternode/spork.h>
#include <masternode/sporkdb.h>

class CSporkDB;
CSporkDB* pSporkDB = NULL;

CSporkDB::CSporkDB(size_t nCacheSize, bool fMemory, bool fWipe)
    : CDBWrapper(gArgs.GetDataDirNet() / "sporks", nCacheSize, fMemory, fWipe)
{
}

bool CSporkDB::WriteSpork(const int nSporkId, const CSporkMessage& spork)
{
    LogPrintf("Wrote spork %s to database\n", sporkManager.GetSporkNameByID(nSporkId));
    return Write(nSporkId, spork);
}

bool CSporkDB::ReadSpork(const int nSporkId, CSporkMessage& spork)
{
    return Read(nSporkId, spork);
}

bool CSporkDB::SporkExists(const int nSporkId)
{
    return Exists(nSporkId);
}
