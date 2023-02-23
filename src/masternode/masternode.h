// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODE_H
#define MASTERNODE_H

#include <base58.h>
#include <key.h>
#include <masternode/masternodeutil.h>
#include <netbase.h>
#include <sync.h>
#include <timedata.h>
#include <util/system.h>

#define MASTERNODE_MIN_CONFIRMATIONS 15
#define MASTERNODE_MIN_MNP_SECONDS (10 * 60)
#define MASTERNODE_MIN_MNB_SECONDS (5 * 60)
#define MASTERNODE_PING_SECONDS (5 * 60)
#define MASTERNODE_EXPIRATION_SECONDS (120 * 60)
#define MASTERNODE_REMOVAL_SECONDS (130 * 60)
#define MASTERNODE_CHECK_SECONDS 5

class CConnman;
class CMasternode;
class CMasternodeBroadcast;
class CMasternodePing;
extern std::map<int64_t, uint256> mapCacheBlockHashes;

bool GetBlockHash(uint256& hash, int nBlockHeight, CBlockIndex* pindex);

//
// The Masternode Ping Class : Contains a different serialize method for sending pings from masternodes throughout the network
//

class CMasternodePing {
public:
    CTxIn vin;
    uint256 blockHash;
    int64_t sigTime; // mnb message times
    std::vector<unsigned char> vchSig;
    // removed stop

    CMasternodePing();
    CMasternodePing(CTxIn& newVin);

    SERIALIZE_METHODS(CMasternodePing, obj)
    {
        READWRITE(obj.vin);
        READWRITE(obj.blockHash);
        READWRITE(obj.sigTime);
        READWRITE(obj.vchSig);
    }

    bool CheckAndUpdate(int& nDos, CConnman* connman, bool fRequireEnabled = true, bool fCheckSigTimeOnly = false);
    bool Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode);
    bool VerifySignature(CPubKey& pubKeyMasternode, int& nDos);
    void Relay(CConnman* connman);

    uint256 GetHash()
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << vin;
        ss << sigTime;
        return ss.GetHash();
    }

    void swap(CMasternodePing& first, CMasternodePing& second) // nothrow
    {
        // enable ADL (not necessary in our case, but good practice)
        using std::swap;

        // by swapping the members of two classes,
        // the two classes are effectively swapped
        swap(first.vin, second.vin);
        swap(first.blockHash, second.blockHash);
        swap(first.sigTime, second.sigTime);
        swap(first.vchSig, second.vchSig);
    }

    CMasternodePing& operator=(CMasternodePing from)
    {
        swap(*this, from);
        return *this;
    }

    friend bool operator==(const CMasternodePing& a, const CMasternodePing& b)
    {
        return a.vin == b.vin && a.blockHash == b.blockHash;
    }

    friend bool operator!=(const CMasternodePing& a, const CMasternodePing& b)
    {
        return !(a == b);
    }
};

//
// The Masternode Class. For managing the Obfuscation process. It contains the input of the 10000 YCE, signature to prove
// it's the one who own that ip address and code for calculating the payment election.
//
class CMasternode {
private:
    // critical section to protect the inner data structures
    mutable RecursiveMutex cs;
    int64_t lastTimeChecked;

public:
    enum state {
        MASTERNODE_PRE_ENABLED,
        MASTERNODE_ENABLED,
        MASTERNODE_EXPIRED,
        MASTERNODE_OUTPOINT_SPENT,
        MASTERNODE_REMOVE,
        MASTERNODE_WATCHDOG_EXPIRED,
        MASTERNODE_POSE_BAN,
        MASTERNODE_VIN_SPENT,
        MASTERNODE_POS_ERROR
    };

    enum CollateralStatus {
        COLLATERAL_OK,
        COLLATERAL_UTXO_NOT_FOUND,
        COLLATERAL_INVALID_AMOUNT
    };

    CTxIn vin;
    CService addr;
    CPubKey pubKeyCollateralAddress;
    CPubKey pubKeyMasternode;
    CPubKey pubKeyCollateralAddress1;
    CPubKey pubKeyMasternode1;
    std::vector<unsigned char> sig;
    int activeState;
    int64_t sigTime; // mnb message time
    int cacheInputAge;
    int cacheInputAgeBlock;
    bool unitTest;
    bool allowFreeTx;
    int protocolVersion;
    int nActiveState;
    int64_t nLastDsq; // the dsq count from the last dsq broadcast of this node
    int nScanningErrorCount;
    int nLastScanningErrorBlockHeight;
    CMasternodePing lastPing;

    CMasternode();
    CMasternode(const CMasternode& other);
    CMasternode(const CMasternodeBroadcast& mnb);

    void swap(CMasternode& first, CMasternode& second) // nothrow
    {
        // enable ADL (not necessary in our case, but good practice)
        using std::swap;

        // by swapping the members of two classes,
        // the two classes are effectively swapped
        swap(first.vin, second.vin);
        swap(first.addr, second.addr);
        swap(first.pubKeyCollateralAddress, second.pubKeyCollateralAddress);
        swap(first.pubKeyMasternode, second.pubKeyMasternode);
        swap(first.sig, second.sig);
        swap(first.activeState, second.activeState);
        swap(first.sigTime, second.sigTime);
        swap(first.lastPing, second.lastPing);
        swap(first.cacheInputAge, second.cacheInputAge);
        swap(first.cacheInputAgeBlock, second.cacheInputAgeBlock);
        swap(first.unitTest, second.unitTest);
        swap(first.allowFreeTx, second.allowFreeTx);
        swap(first.protocolVersion, second.protocolVersion);
        swap(first.nLastDsq, second.nLastDsq);
        swap(first.nScanningErrorCount, second.nScanningErrorCount);
        swap(first.nLastScanningErrorBlockHeight, second.nLastScanningErrorBlockHeight);
    }

    CMasternode& operator=(CMasternode from)
    {
        swap(*this, from);
        return *this;
    }
    friend bool operator==(const CMasternode& a, const CMasternode& b)
    {
        return a.vin == b.vin;
    }
    friend bool operator!=(const CMasternode& a, const CMasternode& b)
    {
        return !(a.vin == b.vin);
    }

    uint256 CalculateScore(int mod = 1, int64_t nBlockHeight = 0, CBlockIndex* pindex = nullptr);

    SERIALIZE_METHODS(CMasternode, obj)
    {
        READWRITE(obj.vin);
        READWRITE(obj.addr);
        READWRITE(obj.pubKeyCollateralAddress);
        READWRITE(obj.pubKeyMasternode);
        READWRITE(obj.sig);
        READWRITE(obj.sigTime);
        READWRITE(obj.protocolVersion);
        READWRITE(obj.activeState);
        READWRITE(obj.lastPing);
        READWRITE(obj.cacheInputAge);
        READWRITE(obj.cacheInputAgeBlock);
        READWRITE(obj.unitTest);
        READWRITE(obj.allowFreeTx);
        READWRITE(obj.nLastDsq);
        READWRITE(obj.nScanningErrorCount);
        READWRITE(obj.nLastScanningErrorBlockHeight);
    }

    int64_t SecondsSincePayment(CBlockIndex* pindex);

    bool UpdateFromNewBroadcast(CMasternodeBroadcast& mnb, CConnman* connman);

    inline uint64_t SliceHash(uint256& hash, int slice)
    {
        uint64_t n = 0;
        memcpy(&n, &hash + slice * 64, 64);
        return n;
    }

    void Check(bool forceCheck = false);

    bool IsBroadcastedWithin(int seconds)
    {
        return (TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime()) - sigTime) < seconds;
    }

    bool IsPingedWithin(int seconds, int64_t now = -1)
    {
        now == -1 ? now = TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime()) : now;

        return (lastPing == CMasternodePing()) ? false : now - lastPing.sigTime < seconds;
    }

    void Disable()
    {
        sigTime = 0;
        lastPing = CMasternodePing();
    }

    bool IsEnabled()
    {
        return activeState == MASTERNODE_ENABLED;
    }

    std::string GetStatus();

    std::string Status()
    {
        std::string strStatus = "ACTIVE";

        if (activeState == CMasternode::MASTERNODE_ENABLED)
            strStatus = "ENABLED";
        if (activeState == CMasternode::MASTERNODE_EXPIRED)
            strStatus = "EXPIRED";
        if (activeState == CMasternode::MASTERNODE_VIN_SPENT)
            strStatus = "VIN_SPENT";
        if (activeState == CMasternode::MASTERNODE_REMOVE)
            strStatus = "REMOVE";
        if (activeState == CMasternode::MASTERNODE_POS_ERROR)
            strStatus = "POS_ERROR";

        return strStatus;
    }

    CollateralStatus CheckCollateral(const COutPoint& outpoint);
    CollateralStatus CheckCollateral(const COutPoint& outpoint, int& nHeightRet, Chainstate& chainstate);
    int64_t GetLastPaid(CBlockIndex* pindex);
    bool IsValidNetAddr();
};

//
// The Masternode Broadcast Class : Contains a different serialize method for sending masternodes through the network
//

class CMasternodeBroadcast : public CMasternode {
public:
    CMasternodeBroadcast();
    CMasternodeBroadcast(CService newAddr, CTxIn newVin, CPubKey newPubkey, CPubKey newPubkey2, int protocolVersionIn);
    CMasternodeBroadcast(const CMasternode& mn);

    bool CheckAndUpdate(int& nDoS, CConnman* connman);
    bool CheckInputsAndAdd(int& nDos, Chainstate& chainstate, CConnman* connman);
    bool Sign(CKey& keyCollateralAddress);
    bool VerifySignature();
    void Relay(CConnman* connman);
    std::string GetOldStrMessage();
    std::string GetNewStrMessage();

    SERIALIZE_METHODS(CMasternodeBroadcast, obj)
    {
        READWRITE(obj.vin);
        READWRITE(obj.addr);
        READWRITE(obj.pubKeyCollateralAddress);
        READWRITE(obj.pubKeyMasternode);
        READWRITE(obj.sig);
        READWRITE(obj.sigTime);
        READWRITE(obj.protocolVersion);
        READWRITE(obj.lastPing);
        READWRITE(obj.nLastDsq);
    }

    uint256 GetHash() const;

    /// Create Masternode broadcast, needs to be relayed manually after that
    static bool Create(CTxIn vin, CService service, CKey keyCollateralAddressNew, CPubKey pubKeyCollateralAddressNew, CKey keyMasternodeNew, CPubKey pubKeyMasternodeNew, std::string& strErrorRet, CMasternodeBroadcast& mnbRet);
    static bool Create(std::string strService, std::string strKey, std::string strTxHash, std::string strOutputIndex, std::string& strErrorRet, CMasternodeBroadcast& mnbRet, bool fOffline = false);
    static bool CheckDefaultPort(std::string strService, std::string& strErrorRet, std::string strContext);
};

#endif
