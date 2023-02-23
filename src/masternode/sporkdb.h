// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MYCE_CSPORKDB_H
#define MYCE_CSPORKDB_H

#include <boost/filesystem/path.hpp>
#include <dbwrapper.h>
#include <masternode/spork.h>

class CSporkDB;
extern CSporkDB* pSporkDB;

class CSporkDB : public CDBWrapper {
public:
    CSporkDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);

private:
    CSporkDB(const CSporkDB&);
    void operator=(const CSporkDB&);

public:
    bool WriteSpork(const int nSporkId, const CSporkMessage& spork);
    bool ReadSpork(const int nSporkId, CSporkMessage& spork);
    bool SporkExists(const int nSporkId);
};

#endif // MYCE_CSPORKDB_H
