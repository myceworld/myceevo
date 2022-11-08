// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ACTIVEMASTERNODE_H
#define ACTIVEMASTERNODE_H

#include <init.h>
#include <key.h>
#include <masternode/masternode.h>
#include <net.h>
#include <node/blockstorage.h>
#include <sync.h>
#include <wallet/coinselection.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#define ACTIVE_MASTERNODE_INITIAL 0 // initial state
#define ACTIVE_MASTERNODE_SYNC_IN_PROCESS 1
#define ACTIVE_MASTERNODE_INPUT_TOO_NEW 2
#define ACTIVE_MASTERNODE_NOT_CAPABLE 3
#define ACTIVE_MASTERNODE_STARTED 4

using wallet::COutput;

// Responsible for activating the Masternode and pinging the network
class CActiveMasternode {
private:
    // critical section to protect the inner data structures
    mutable RecursiveMutex cs;

    /// Ping Masternode
    bool SendMasternodePing(std::string& errorMessage, CConnman* connman);

    /// Create Masternode broadcast, needs to be relayed manually after that
    bool CreateBroadcast(CTxIn vin, CService addr, CKey key, CPubKey pubKey, CKey keyMasternode, CPubKey pubKeyMasternode, std::string& errorMessage, CMasternodeBroadcast& mnb, CConnman* connman);

    /// Get 10000 YCE input that can be used for the Masternode
    bool GetMasterNodeVin(CTxIn& vin, CPubKey& pubkey, CKey& secretKey, std::string strTxHash, std::string strOutputIndex);
    bool GetVinFromOutput(COutput out, CTxIn& vin, CPubKey& pubkey, CKey& secretKey);

    ChainstateManager* chainman{nullptr};

public:
    // Initialized by init.cpp
    // Keys for the main Masternode
    CPubKey pubKeyMasternode;

    // Initialized while registering Masternode
    CTxIn vin;
    CService service;

    int status;
    std::string notCapableReason;

    CActiveMasternode()
    {
        status = ACTIVE_MASTERNODE_INITIAL;
    }

    /// Attach chainman pointer to class
    void Attach(ChainstateManager* other) {
        chainman = other;
    }

    /// Manage status of main Masternode
    void ManageStatus(CConnman* connman);
    std::string GetStatus();

    /// Create Masternode broadcast, needs to be relayed manually after that
    bool CreateBroadcast(std::string strService, std::string strKey, std::string strTxHash, std::string strOutputIndex, std::string& errorMessage, CMasternodeBroadcast& mnb, CConnman* connman);

    /// Get 10000 YCE input that can be used for the Masternode
    bool GetMasterNodeVin(CTxIn& vin, CPubKey& pubkey, CKey& secretKey);
    std::vector<COutput> SelectCoinsMasternode();

    /// Enable cold wallet mode (run a Masternode with no funds)
    bool EnableHotColdMasterNode(CTxIn& vin, CService& newService);
};

#endif
