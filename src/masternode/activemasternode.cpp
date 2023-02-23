// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/activemasternode.h>

#include <addrman.h>
#include <masternode/init.h>
#include <masternode/masternode-sync.h>
#include <masternode/masternode.h>
#include <masternode/masternodeconfig.h>
#include <masternode/masternodeman.h>
#include <masternode/masternodesigner.h>
#include <masternode/spork.h>
#include <pos/wallet.h>
#include <protocol.h>
#include <util/system.h>
#include <wallet/receive.h>

//
// Bootup the Masternode, look for a 100000 Myce input and register on the network
//
void CActiveMasternode::ManageStatus(CConnman* connman)
{
    std::string errorMessage;

    if (!fMasterNode)
        return;

    LogPrintf("CActiveMasternode::ManageStatus() - Begin\n");

    // need correct blocks to send ping
    if (!masternodeSync.IsBlockchainSynced()) {
        status = ACTIVE_MASTERNODE_SYNC_IN_PROCESS;
        LogPrintf("CActiveMasternode::ManageStatus() - %s\n", GetStatus());
        return;
    }

    if (status == ACTIVE_MASTERNODE_SYNC_IN_PROCESS)
        status = ACTIVE_MASTERNODE_INITIAL;

    if (status == ACTIVE_MASTERNODE_INITIAL) {
        CMasternode* pmn;
        pmn = mnodeman.Find(pubKeyMasternode);
        if (pmn) {
            pmn->Check();
            if (pmn->IsEnabled() && pmn->protocolVersion == PROTOCOL_VERSION)
                EnableHotColdMasterNode(pmn->vin, pmn->addr);
        }
    }

    if (status != ACTIVE_MASTERNODE_STARTED) {
        // Set defaults
        status = ACTIVE_MASTERNODE_NOT_CAPABLE;
        notCapableReason = "";

        if (!stakeWallet.GetStakingWallet()) {
            notCapableReason = "Wallet not loaded.";
            LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason);
            return;
        }

        if (stakeWallet.GetStakingWallet()->IsLocked()) {
            notCapableReason = "Wallet is locked.";
            LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason);
            return;
        }

        const auto bal = GetBalance(*stakeWallet.GetStakingWallet());
        if (bal.m_mine_trusted == 0 * COIN) {
            notCapableReason = "Hot node, waiting for remote activation.";
            LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason);
            return;
        }

        if (strMasterNodeAddr.empty()) {
            if (!GetLocal(service)) {
                notCapableReason = "Can't detect external address. Please use the masternodeaddr configuration option.";
                LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason);
                return;
            }
        }

        LogPrintf("CActiveMasternode::ManageStatus() - Checking inbound connection to '%s'\n", service.ToString());

        CAddress addr;
        int fCountFailure;
        CNode* pnode = connman->ConnectNode(addr, strMasterNodeAddr.c_str(), fCountFailure, ConnectionType::MANUAL);
        if (!pnode) {
            notCapableReason = "Could not connect to " + service.ToString();
            LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason);
            return;
        }
        pnode->Release();

        // Choose coins to use
        CPubKey pubKeyCollateralAddress;
        CKey keyCollateralAddress;

        if (GetMasterNodeVin(vin, pubKeyCollateralAddress, keyCollateralAddress)) {
            if (GetInputAge(vin, chainman->ActiveChainstate()) < MASTERNODE_MIN_CONFIRMATIONS) {
                status = ACTIVE_MASTERNODE_INPUT_TOO_NEW;
                notCapableReason = strprintf("%s - %d confirmations", GetStatus(), GetInputAge(vin, chainman->ActiveChainstate()));
                LogPrintf("CActiveMasternode::ManageStatus() - %s\n", notCapableReason);
                return;
            }

            LOCK(stakeWallet.GetStakingWallet()->cs_wallet);
            stakeWallet.GetStakingWallet()->LockCoin(vin.prevout);

            // send to all nodes
            CPubKey pubKeyMasternode;
            CKey keyMasternode;

            if (!legacySigner.GetKeysFromSecret(strMasterNodePrivKey, keyMasternode, pubKeyMasternode)) {
                LogPrintf("%s : Invalid masternode key", __func__);
                return;
            }

            CMasternodeBroadcast mnb;
            if (!CreateBroadcast(vin, service, keyCollateralAddress, pubKeyCollateralAddress, keyMasternode, pubKeyMasternode, notCapableReason, mnb, connman)) {
                notCapableReason = "Error on Register: " + errorMessage;
                LogPrintf("CActiveMasternode::ManageStatus() - %s\n", notCapableReason);
                return;
            }

            // send to all peers
            LogPrintf("CActiveMasternode::ManageStatus() - Relay broadcast vin = %s\n", vin.ToString());
            mnb.Relay(connman);

            LogPrintf("CActiveMasternode::ManageStatus() - Is capable master node!\n");
            status = ACTIVE_MASTERNODE_STARTED;

            return;
        } else {
            notCapableReason = "Could not find suitable coins!";
            LogPrintf("CActiveMasternode::ManageStatus() - %s\n", notCapableReason);
            return;
        }
    }

    // send to all peers
    if (!SendMasternodePing(errorMessage, connman)) {
        LogPrintf("CActiveMasternode::ManageStatus() - Error on Ping: %s\n", errorMessage);
    }
}

std::string CActiveMasternode::GetStatus()
{
    switch (status) {
    case ACTIVE_MASTERNODE_INITIAL:
        return "Node just started, not yet activated";
    case ACTIVE_MASTERNODE_SYNC_IN_PROCESS:
        return "Sync in progress. Must wait until sync is complete to start Masternode";
    case ACTIVE_MASTERNODE_INPUT_TOO_NEW:
        return strprintf("Masternode input must have at least %d confirmations", MASTERNODE_MIN_CONFIRMATIONS);
    case ACTIVE_MASTERNODE_NOT_CAPABLE:
        return "Not capable masternode: " + notCapableReason;
    case ACTIVE_MASTERNODE_STARTED:
        return "Masternode successfully started";
    default:
        return "unknown";
    }
}

bool CActiveMasternode::SendMasternodePing(std::string& errorMessage, CConnman* connman)
{
    if (status != ACTIVE_MASTERNODE_STARTED) {
        errorMessage = "Masternode is not in a running status";
        return false;
    }

    CPubKey pubKeyMasternode;
    CKey keyMasternode;

    if (!legacySigner.SetKey(strMasterNodePrivKey, keyMasternode, pubKeyMasternode)) {
        errorMessage = strprintf("Error upon calling SetKey: %s\n", errorMessage);
        return false;
    }

    LogPrintf("CActiveMasternode::SendMasternodePing() - Relay Masternode Ping vin = %s\n", vin.ToString());

    CMasternodePing mnp(vin);
    if (!mnp.Sign(keyMasternode, pubKeyMasternode)) {
        errorMessage = "Couldn't sign Masternode Ping";
        return false;
    }

    // Update lastPing for our masternode in Masternode list
    CMasternode* pmn = mnodeman.Find(vin);
    if (pmn) {
        if (pmn->IsPingedWithin(MASTERNODE_PING_SECONDS, mnp.sigTime)) {
            errorMessage = "Too early to send Masternode Ping";
            return false;
        }

        pmn->lastPing = mnp;
        mnodeman.mapSeenMasternodePing.insert(std::make_pair(mnp.GetHash(), mnp));

        // mnodeman.mapSeenMasternodeBroadcast.lastPing is probably outdated, so we'll update it
        CMasternodeBroadcast mnb(*pmn);
        uint256 hash = mnb.GetHash();
        if (mnodeman.mapSeenMasternodeBroadcast.count(hash))
            mnodeman.mapSeenMasternodeBroadcast[hash].lastPing = mnp;

        mnp.Relay(connman);

        return true;
    } else {
        // Seems like we are trying to send a ping while the Masternode is not registered in the network
        errorMessage = "Obfuscation Masternode List doesn't include our Masternode, shutting down Masternode pinging service! " + vin.ToString();
        status = ACTIVE_MASTERNODE_NOT_CAPABLE;
        notCapableReason = errorMessage;
        return false;
    }
}

bool CActiveMasternode::CreateBroadcast(std::string strService, std::string strKeyMasternode, std::string strTxHash, std::string strOutputIndex, std::string& errorMessage, CMasternodeBroadcast& mnb, CConnman* connman)
{
    CTxIn vin;
    CPubKey pubKeyCollateralAddress;
    CKey keyCollateralAddress;
    CPubKey pubKeyMasternode;
    CKey keyMasternode;

    // need correct blocks to send ping
    if (!masternodeSync.IsBlockchainSynced()) {
        errorMessage = "Sync in progress. Must wait until sync is complete to start Masternode";
        LogPrintf("CActiveMasternode::CreateBroadcast() - %s\n", errorMessage);
        return false;
    }

    if (!legacySigner.SetKey(strKeyMasternode, keyMasternode, pubKeyMasternode)) {
        errorMessage = strprintf("Can't find keys for masternode %s - %s", strService, errorMessage);
        LogPrintf("CActiveMasternode::CreateBroadcast() - %s\n", errorMessage);
        return false;
    }

    if (!GetMasterNodeVin(vin, pubKeyCollateralAddress, keyCollateralAddress, strTxHash, strOutputIndex)) {
        errorMessage = strprintf("Could not allocate vin %s:%s for masternode %s", strTxHash, strOutputIndex, strService);
        LogPrintf("CActiveMasternode::CreateBroadcast() - %s\n", errorMessage);
        return false;
    }

    CAddress addr;
    int fCountFailure;
    connman->ConnectNode(addr, strMasterNodeAddr.c_str(), fCountFailure, ConnectionType::MANUAL);

    return CreateBroadcast(vin, addr, keyCollateralAddress, pubKeyCollateralAddress, keyMasternode, pubKeyMasternode, errorMessage, mnb, connman);
}

bool CActiveMasternode::CreateBroadcast(CTxIn vin, CService addr, CKey keyCollateralAddress, CPubKey pubKeyCollateralAddress, CKey keyMasternode, CPubKey pubKeyMasternode, std::string& errorMessage, CMasternodeBroadcast& mnb, CConnman* connman)
{
    // wait for reindex and/or import to finish
    if (node::fImporting || node::fReindex)
        return false;

    CMasternodePing mnp(vin);
    if (!mnp.Sign(keyMasternode, pubKeyMasternode)) {
        errorMessage = strprintf("Failed to sign ping, vin: %s", vin.ToString());
        LogPrintf("CActiveMasternode::CreateBroadcast() -  %s\n", errorMessage);
        mnb = CMasternodeBroadcast();
        return false;
    }

    mnb = CMasternodeBroadcast(addr, vin, pubKeyCollateralAddress, pubKeyMasternode, PROTOCOL_VERSION);
    mnb.lastPing = mnp;
    if (!mnb.Sign(keyCollateralAddress)) {
        errorMessage = strprintf("Failed to sign broadcast, vin: %s", vin.ToString());
        LogPrintf("CActiveMasternode::CreateBroadcast() - %s\n", errorMessage);
        mnb = CMasternodeBroadcast();
        return false;
    }

    return true;
}

bool CActiveMasternode::GetMasterNodeVin(CTxIn& vin, CPubKey& pubkey, CKey& secretKey)
{
    return GetMasterNodeVin(vin, pubkey, secretKey, "", "");
}

bool CActiveMasternode::GetMasterNodeVin(CTxIn& vin, CPubKey& pubkey, CKey& secretKey, std::string strTxHash, std::string strOutputIndex)
{
    // wait for reindex and/or import to finish
    if (node::fImporting || node::fReindex)
        return false;

    // Find possible candidates
    if (!stakeWallet.GetStakingWallet())
        return false;
    TRY_LOCK(stakeWallet.GetStakingWallet()->cs_wallet, fWallet);

    COutput* selectedOutput;
    std::vector<COutput> possibleCoins = SelectCoinsMasternode();

    // Find the vin
    if (!strTxHash.empty()) {
        // Let's find it
        uint256 txHash = uint256S(strTxHash);
        int outputIndex;
        try {
            outputIndex = std::stoi(strOutputIndex);
        } catch (const std::exception& e) {
            LogPrintf("%s: %s on strOutputIndex\n", __func__, e.what());
            return false;
        }

        bool found = false;
        for (COutput& out : possibleCoins) {
            if (out.outpoint.hash == txHash && out.outpoint.n == outputIndex) {
                selectedOutput = &out;
                found = true;
                break;
            }
        }
        if (!found) {
            LogPrintf("CActiveMasternode::GetMasterNodeVin - Could not locate valid vin\n");
            return false;
        }
    } else {
        // No output specified,  Select the first one
        if (possibleCoins.size() > 0) {
            selectedOutput = &possibleCoins[0];
        } else {
            LogPrintf("CActiveMasternode::GetMasterNodeVin - Could not locate specified vin from possible list\n");
            return false;
        }
    }

    // At this point we have a selected output, retrieve the associated info
    return GetVinFromOutput(*selectedOutput, vin, pubkey, secretKey);
}

// Extract Masternode vin information from output
bool CActiveMasternode::GetVinFromOutput(COutput out, CTxIn& vin, CPubKey& pubkey, CKey& secretKey)
{
    // wait for reindex and/or import to finish
    if (node::fImporting || node::fReindex)
        return false;

    CScript pubScript;

    vin = CTxIn(out.outpoint.hash, out.outpoint.n);
    pubScript = out.txout.scriptPubKey;

    CTxDestination address1;
    ExtractDestination(pubScript, address1);
    auto id = std::get_if<PKHash>(&address1);
    if (ToKeyID(*id).IsNull()) {
        LogPrintf("CActiveMasternode::GetMasterNodeVin - Address does not refer to a key\n");
        return false;
    }

    if (!stakeWallet.GetStakingWallet()) {
        LogPrintf("CActiveMasternode::GetMasterNodeVin - Wallet is not loaded\n");
        return false;
    }

    wallet::LegacyScriptPubKeyMan* spk_man = stakeWallet.GetStakingWallet()->GetLegacyScriptPubKeyMan();
    if (!spk_man) {
        LogPrintf ("GetVinFromOutput -- This type of wallet does not support this command\n");
        return false;
    }

    if (!spk_man->GetKey(ToKeyID(*id), secretKey)) {
        LogPrintf("CActiveMasternode::GetMasterNodeVin - Private key for address is not known\n");
        return false;
    }

    pubkey = secretKey.GetPubKey();
    return true;
}

// get all possible outputs for running Masternode
std::vector<COutput> CActiveMasternode::SelectCoinsMasternode()
{
    std::vector<COutput> vCoins;
    std::vector<COutput> filteredCoins;
    std::vector<COutPoint> confLockedCoins;

    if (!stakeWallet.GetStakingWallet()) {
        LogPrintf("CActiveMasternode::SelectCoinsMasternode - Wallet is not loaded\n");
        return filteredCoins;
    }

    // Temporary unlock MN coins from masternode.conf
    if (gArgs.GetBoolArg("-mnconflock", true)) {
        uint256 mnTxHash;
        for (CMasternodeConfig::CMasternodeEntry mne : masternodeConfig.getEntries()) {
            mnTxHash.SetHex(mne.getTxHash());

            int nIndex;
            if (!mne.castOutputIndex(nIndex))
                continue;

            COutPoint outpoint(mnTxHash, nIndex);
            confLockedCoins.push_back(outpoint);
            stakeWallet.GetStakingWallet()->UnlockCoin(outpoint);
        }
    }

    // Retrieve all possible outputs
    auto res = AvailableCoins(*stakeWallet.GetStakingWallet(), nullptr);
    for (auto entry : res.All()) {
        vCoins.push_back(entry);
    }

    // Lock MN coins from masternode.conf back if they where temporary unlocked
    if (!confLockedCoins.empty()) {
        for (COutPoint outpoint : confLockedCoins)
            stakeWallet.GetStakingWallet()->LockCoin(outpoint);
    }

    // Filter
    for (const COutput& out : vCoins) {
        if (out.txout.nValue == 100000 * COIN) { // exactly
            filteredCoins.push_back(out);
        }
    }
    return filteredCoins;
}

// when starting a Masternode, this can enable to run as a hot wallet with no funds
bool CActiveMasternode::EnableHotColdMasterNode(CTxIn& newVin, CService& newService)
{
    if (!fMasterNode)
        return false;

    status = ACTIVE_MASTERNODE_STARTED;

    // The values below are needed for signing mnping messages going forward
    vin = newVin;
    service = newService;

    LogPrintf("CActiveMasternode::EnableHotColdMasterNode() - Enabled! You may shut down the cold daemon.\n");

    return true;
}
