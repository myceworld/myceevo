// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODEMAN_H
#define MASTERNODEMAN_H

#include <base58.h>
#include <key.h>
#include <masternode/masternode.h>
#include <net.h>
#include <sync.h>
#include <util/system.h>
#include <validation.h>

#define MASTERNODES_DUMP_SECONDS (15 * 60)
#define MASTERNODES_DSEG_SECONDS (3 * 60 * 60)

class CMasternodeMan;
void DumpMasternodes();

/** Access to the MN database (mncache.dat)
 */
class CMasternodeDB {
private:
    std::filesystem::path pathMN;
    std::string strMagicMessage;

public:
    enum ReadResult {
        Ok,
        FileError,
        HashReadError,
        IncorrectHash,
        IncorrectMagicMessage,
        IncorrectMagicNumber,
        IncorrectFormat
    };

    CMasternodeDB();
    bool Write(const CMasternodeMan& mnodemanToSave);
    ReadResult Read(CMasternodeMan& mnodemanToLoad, bool fDryRun = false);
};

class CMasternodeMan {
private:
    // critical section to protect the inner data structures
    mutable RecursiveMutex cs;

    // critical section to protect the inner data structures specifically on messaging
    mutable RecursiveMutex cs_process_message;

    // map to hold all MNs
    std::vector<CMasternode> vMasternodes;
    // who's asked for the Masternode list and the last time
    std::map<CNetAddr, int64_t> mAskedUsForMasternodeList;
    // who we asked for the Masternode list and the last time
    std::map<CNetAddr, int64_t> mWeAskedForMasternodeList;
    // which Masternodes we've asked for
    std::map<COutPoint, int64_t> mWeAskedForMasternodeListEntry;

    ChainstateManager* chainman;

public:
    // Keep track of all broadcasts I've seen
    std::map<uint256, CMasternodeBroadcast> mapSeenMasternodeBroadcast;
    // Keep track of all pings I've seen
    std::map<uint256, CMasternodePing> mapSeenMasternodePing;

    // keep track of dsq count to prevent masternodes from gaming obfuscation queue
    int64_t nDsqCount;

    SERIALIZE_METHODS(CMasternodeMan, obj)
    {
        LOCK(obj.cs);

        READWRITE(obj.vMasternodes);
        READWRITE(obj.mAskedUsForMasternodeList);
        READWRITE(obj.mWeAskedForMasternodeList);
        READWRITE(obj.mWeAskedForMasternodeListEntry);
        READWRITE(obj.nDsqCount);
        READWRITE(obj.mapSeenMasternodeBroadcast);
        READWRITE(obj.mapSeenMasternodePing);
    }

    CMasternodeMan();
    CMasternodeMan(CMasternodeMan& other);

    /// Attach chainman pointer to class
    void Attach(ChainstateManager* other) {
        chainman = other;
    }

    ChainstateManager* getChainMan() { return chainman; }

    /// Add an entry
    bool Add(CMasternode& mn);

    /// Ask (source) node for mnb
    void AskForMN(CNode* pnode, CTxIn& vin, CConnman* connman);

    /// Check all Masternodes
    void Check();

    /// Check all Masternodes and remove inactive
    void CheckAndRemove(bool forceExpiredRemoval = false);

    /// Clear Masternode vector
    void Clear();

    int CountEnabled(int protocolVersion = -1);
    void CountNetworks(int protocolVersion, int& ipv4, int& ipv6, int& onion);

    void DsegUpdate(CNode* pnode, CConnman* connman);

    /// Find an entry
    CMasternode* Find(const CScript& payee);
    CMasternode* Find(const CTxIn& vin);
    CMasternode* Find(const CPubKey& pubKeyMasternode);

    /// Find an entry in the masternode list that is next to be paid
    CMasternode* GetNextMasternodeInQueueForPayment(CBlockIndex* pindex, int nBlockHeight, bool fFilterSigTime, int& nCount);

    /// Find a random entry
    CMasternode* FindRandomNotInVec(std::vector<CTxIn>& vecToExclude, int protocolVersion = -1);

    /// Get the current winner for this block
    CMasternode* GetCurrentMasterNode(int mod = 1, int64_t nBlockHeight = 0, int minProtocol = 0);

    std::vector<CMasternode> GetFullMasternodeVector()
    {
        Check();
        return vMasternodes;
    }

    std::vector<std::pair<int, CMasternode>> GetMasternodeRanks(CBlockIndex* pindex, int64_t nBlockHeight, int minProtocol = 0);
    int GetMasternodeRank(CBlockIndex* pindex, const CTxIn& vin, int64_t nBlockHeight, int minProtocol = 0, bool fOnlyActive = true);
    CMasternode* GetMasternodeByRank(int nRank, int64_t nBlockHeight, int minProtocol = 0, bool fOnlyActive = true);

    void ProcessMasternodeConnections(CConnman& connman);

    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman* connman);

    /// Return the number of (unique) Masternodes
    int size() { return vMasternodes.size(); }

    /// Return the number of Masternodes older than (default) 8000 seconds
    int stable_size();

    std::string ToString() const;

    void Remove(CTxIn vin);

    int GetEstimatedMasternodes(int nBlock);

    /// Update masternode list and maps using provided CMasternodeBroadcast
    void UpdateMasternodeList(CMasternodeBroadcast mnb, CConnman* connman);
};

#endif
