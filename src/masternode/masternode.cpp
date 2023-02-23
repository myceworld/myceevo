// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/init.h>
#include <masternode/activemasternode.h>
#include <masternode/masternode.h>
#include <masternode/masternodeman.h>
#include <masternode/masternodesigner.h>
#include <masternode/masternode-payments.h>
#include <masternode/masternode-sync.h>
#include <netmessagemaker.h>
#include <pos/wallet.h>
#include <shutdown.h>
#include <sync.h>
#include <util/system.h>
#include <validation.h>

// keep track of the scanning errors I've seen
std::map<uint256, int> mapSeenMasternodeScanningErrors;
// cache block hashes as we calculate them
std::map<int64_t, uint256> mapCacheBlockHashes;

// Get the last hash that matches the modulus given. Processed in reverse order
bool GetBlockHash(uint256& hash, int nBlockHeight, CBlockIndex* pindex)
{
    if (!pindex)
        return false;

    if (!nBlockHeight)
        nBlockHeight = pindex->nHeight;

    if (mapCacheBlockHashes.count(nBlockHeight)) {
        hash = mapCacheBlockHashes[nBlockHeight];
        return true;
    }

    const CBlockIndex* BlockLastSolved = pindex;
    const CBlockIndex* BlockReading = pindex;

    if (!BlockLastSolved || BlockLastSolved->nHeight == 0 || pindex->nHeight + 1 < nBlockHeight)
        return false;

    int nBlocksAgo = 0;
    if (nBlockHeight > 0)
        nBlocksAgo = (pindex->nHeight + 1) - nBlockHeight;
    assert(nBlocksAgo >= 0);

    int n = 0;
    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if (n >= nBlocksAgo) {
            hash = BlockReading->GetBlockHash();
            mapCacheBlockHashes[nBlockHeight] = hash;
            return true;
        }
        n++;

        if (BlockReading->pprev == NULL) {
            assert(BlockReading);
            break;
        }
        BlockReading = BlockReading->pprev;
    }

    return false;
}

CMasternode::CMasternode()
{
    LOCK(cs);
    vin = CTxIn();
    addr = CService();
    pubKeyCollateralAddress = CPubKey();
    pubKeyMasternode = CPubKey();
    sig = std::vector<unsigned char>();
    activeState = MASTERNODE_ENABLED;
    sigTime = TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime());
    lastPing = CMasternodePing();
    cacheInputAge = 0;
    cacheInputAgeBlock = 0;
    unitTest = false;
    allowFreeTx = true;
    nActiveState = MASTERNODE_ENABLED,
    protocolVersion = PROTOCOL_VERSION;
    nLastDsq = 0;
    nScanningErrorCount = 0;
    nLastScanningErrorBlockHeight = 0;
    lastTimeChecked = 0;
}

CMasternode::CMasternode(const CMasternode& other)
{
    LOCK(cs);
    vin = other.vin;
    addr = other.addr;
    pubKeyCollateralAddress = other.pubKeyCollateralAddress;
    pubKeyMasternode = other.pubKeyMasternode;
    sig = other.sig;
    activeState = other.activeState;
    sigTime = other.sigTime;
    lastPing = other.lastPing;
    cacheInputAge = other.cacheInputAge;
    cacheInputAgeBlock = other.cacheInputAgeBlock;
    unitTest = other.unitTest;
    allowFreeTx = other.allowFreeTx;
    nActiveState = MASTERNODE_ENABLED,
    protocolVersion = other.protocolVersion;
    nLastDsq = other.nLastDsq;
    nScanningErrorCount = other.nScanningErrorCount;
    nLastScanningErrorBlockHeight = other.nLastScanningErrorBlockHeight;
    lastTimeChecked = 0;
}

CMasternode::CMasternode(const CMasternodeBroadcast& mnb)
{
    LOCK(cs);
    vin = mnb.vin;
    addr = mnb.addr;
    pubKeyCollateralAddress = mnb.pubKeyCollateralAddress;
    pubKeyMasternode = mnb.pubKeyMasternode;
    sig = mnb.sig;
    activeState = MASTERNODE_ENABLED;
    sigTime = mnb.sigTime;
    lastPing = mnb.lastPing;
    cacheInputAge = 0;
    cacheInputAgeBlock = 0;
    unitTest = false;
    allowFreeTx = true;
    nActiveState = MASTERNODE_ENABLED,
    protocolVersion = mnb.protocolVersion;
    nLastDsq = mnb.nLastDsq;
    nScanningErrorCount = 0;
    nLastScanningErrorBlockHeight = 0;
    lastTimeChecked = 0;
}

//
// When a new masternode broadcast is sent, update our information
//
bool CMasternode::UpdateFromNewBroadcast(CMasternodeBroadcast& mnb, CConnman* connman)
{
    if (mnb.sigTime > sigTime) {
        pubKeyMasternode = mnb.pubKeyMasternode;
        pubKeyCollateralAddress = mnb.pubKeyCollateralAddress;
        sigTime = mnb.sigTime;
        sig = mnb.sig;
        protocolVersion = mnb.protocolVersion;
        addr = mnb.addr;
        lastTimeChecked = 0;
        int nDoS = 0;
        if (mnb.lastPing == CMasternodePing() || (mnb.lastPing != CMasternodePing() && mnb.lastPing.CheckAndUpdate(nDoS, connman, false))) {
            lastPing = mnb.lastPing;
            mnodeman.mapSeenMasternodePing.insert(std::make_pair(lastPing.GetHash(), lastPing));
        }
        return true;
    }
    return false;
}

//
// Deterministically calculate a given "score" for a Masternode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
uint256 CMasternode::CalculateScore(int mod, int64_t nBlockHeight, CBlockIndex* pindex)
{
    if (!pindex)
        return uint256();

    uint256 hash = uint256();
    arith_uint256 aux = UintToArith256(vin.prevout.hash) + vin.prevout.n;

    if (!GetBlockHash(hash, nBlockHeight, pindex)) {
        LogPrint(BCLog::MASTERNODE, "CalculateScore ERROR - nHeight %d - Returned 0\n", nBlockHeight);
        return uint256();
    }

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << hash;
    uint256 hash2 = ss.GetHash();

    CHashWriter ss2(SER_GETHASH, PROTOCOL_VERSION);
    ss2 << hash;
    ss2 << ArithToUint256(aux);
    uint256 hash3 = ss2.GetHash();

    arith_uint256 r = (UintToArith256(hash3) > UintToArith256(hash2) ?
                       UintToArith256(hash3) - UintToArith256(hash2) :
                       UintToArith256(hash2) - UintToArith256(hash3));

    return ArithToUint256(r);
}

bool GetUTXOCoin(const COutPoint& outpoint, Coin& coin, Chainstate& chainstate)
{
    LOCK(cs_main);
    if (!chainstate.CoinsTip().GetCoin(outpoint, coin)) {
        return false;
    }
    if (coin.IsSpent()) {
        return false;
    }
    return true;
}

CMasternode::CollateralStatus CMasternode::CheckCollateral(const COutPoint& outpoint, int& nHeightRet, Chainstate& chainstate)
{
    AssertLockHeld(cs_main);

    Coin coin;
    if (!GetUTXOCoin(outpoint, coin, chainstate)) {
        return COLLATERAL_UTXO_NOT_FOUND;
    }

    if (coin.out.nValue != 100000 * COIN) {
        return COLLATERAL_INVALID_AMOUNT;
    }

    nHeightRet = coin.nHeight;
    return COLLATERAL_OK;
}

CMasternode::CollateralStatus CMasternode::CheckCollateral(const COutPoint& outpoint)
{
    int nHeight;
    return CheckCollateral(outpoint, nHeight, g_rpc_node->chainman->ActiveChainstate());
}

void CMasternode::Check(bool forceCheck)
{
    if (ShutdownRequested())
        return;

    if (!forceCheck && (GetTime() - lastTimeChecked < MASTERNODE_CHECK_SECONDS))
        return;
    lastTimeChecked = GetTime();

    // once spent, stop doing the checks
    if (activeState == MASTERNODE_VIN_SPENT)
        return;

    if (!IsPingedWithin(MASTERNODE_REMOVAL_SECONDS)) {
        activeState = MASTERNODE_REMOVE;
        return;
    }

    if (!IsPingedWithin(MASTERNODE_EXPIRATION_SECONDS)) {
        activeState = MASTERNODE_EXPIRED;
        return;
    }

    if (lastPing.sigTime - sigTime < MASTERNODE_MIN_MNP_SECONDS) {
        activeState = MASTERNODE_PRE_ENABLED;
        return;
    }

    if (!unitTest) {
        CollateralStatus err = CheckCollateral(vin.prevout);
        if (err == COLLATERAL_UTXO_NOT_FOUND) {
            nActiveState = MASTERNODE_OUTPOINT_SPENT;
            LogPrint(BCLog::MASTERNODE, "CMasternode::Check -- Failed to find Masternode UTXO, masternode=%s\n", vin.prevout.ToString());
            return;
        }
    }

    activeState = MASTERNODE_ENABLED; // OK
}

int64_t CMasternode::SecondsSincePayment(CBlockIndex* pindex)
{
    CScript pubkeyScript;
    pubkeyScript = GetScriptForDestination(PKHash(pubKeyCollateralAddress));

    int64_t sec = (TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime()) - GetLastPaid(pindex));
    int64_t month = 60 * 60 * 24 * 30;
    if (sec < month)
        return sec; // if it's less than 30 days, give seconds

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << vin;
    ss << sigTime;
    uint256 hash = ss.GetHash();

    int64_t temp = month + UintToArith256(hash).GetCompact(false);

    // return some deterministic value for unknown/unpaid but force it to be more than 30 days old
    return temp;
}

int64_t CMasternode::GetLastPaid(CBlockIndex* pindex)
{
    if (!pindex)
        return false;

    CScript mnpayee;
    mnpayee = GetScriptForDestination(PKHash(pubKeyCollateralAddress));

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << vin;
    ss << sigTime;
    uint256 hash = ss.GetHash();

    // use a deterministic offset to break a tie -- 2.5 minutes
    int64_t nOffset = UintToArith256(hash).GetCompact(false) % 150;

    if (!pindex)
        return false;

    const CBlockIndex* BlockReading = pindex;

    int nMnCount = mnodeman.CountEnabled() * 1.25;
    int n = 0;
    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if (n >= nMnCount) {
            return 0;
        }
        n++;

        if (masternodePayments.mapMasternodeBlocks.count(BlockReading->nHeight)) {
            /*
                Search for this payee, with at least 2 votes. This will aid in consensus allowing the network
                to converge on the same payees quickly, then keep the same schedule.
            */
            if (masternodePayments.mapMasternodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(mnpayee, 2)) {
                return BlockReading->nTime + nOffset;
            }
        }

        if (BlockReading->pprev == NULL) {
            assert(BlockReading);
            break;
        }
        BlockReading = BlockReading->pprev;
    }

    return 0;
}

std::string CMasternode::GetStatus()
{
    switch (nActiveState) {
    case CMasternode::MASTERNODE_PRE_ENABLED:
        return "PRE_ENABLED";
    case CMasternode::MASTERNODE_ENABLED:
        return "ENABLED";
    case CMasternode::MASTERNODE_EXPIRED:
        return "EXPIRED";
    case CMasternode::MASTERNODE_OUTPOINT_SPENT:
        return "OUTPOINT_SPENT";
    case CMasternode::MASTERNODE_REMOVE:
        return "REMOVE";
    case CMasternode::MASTERNODE_WATCHDOG_EXPIRED:
        return "WATCHDOG_EXPIRED";
    case CMasternode::MASTERNODE_POSE_BAN:
        return "POSE_BAN";
    default:
        return "UNKNOWN";
    }
}

bool CMasternode::IsValidNetAddr()
{
    // TODO: regtest is fine with any addresses for now,
    // should probably be a bit smarter if one day we start to implement tests for this
    return Params().NetworkIDString() == CBaseChainParams::REGTEST || (IsReachable(addr) && addr.IsRoutable());
}

CMasternodeBroadcast::CMasternodeBroadcast()
{
    vin = CTxIn();
    addr = CService();
    pubKeyCollateralAddress = CPubKey();
    pubKeyMasternode1 = CPubKey();
    sig = std::vector<unsigned char>();
    activeState = MASTERNODE_ENABLED;
    sigTime = TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime());
    lastPing = CMasternodePing();
    cacheInputAge = 0;
    cacheInputAgeBlock = 0;
    unitTest = false;
    allowFreeTx = true;
    protocolVersion = PROTOCOL_VERSION;
    nLastDsq = 0;
    nScanningErrorCount = 0;
    nLastScanningErrorBlockHeight = 0;
}

CMasternodeBroadcast::CMasternodeBroadcast(CService newAddr, CTxIn newVin, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyMasternodeNew, int protocolVersionIn)
{
    vin = newVin;
    addr = newAddr;
    pubKeyCollateralAddress = pubKeyCollateralAddressNew;
    pubKeyMasternode = pubKeyMasternodeNew;
    sig = std::vector<unsigned char>();
    activeState = MASTERNODE_ENABLED;
    sigTime = TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime());
    lastPing = CMasternodePing();
    cacheInputAge = 0;
    cacheInputAgeBlock = 0;
    unitTest = false;
    allowFreeTx = true;
    protocolVersion = protocolVersionIn;
    nLastDsq = 0;
    nScanningErrorCount = 0;
    nLastScanningErrorBlockHeight = 0;
}

CMasternodeBroadcast::CMasternodeBroadcast(const CMasternode& mn)
{
    vin = mn.vin;
    addr = mn.addr;
    pubKeyCollateralAddress = mn.pubKeyCollateralAddress;
    pubKeyMasternode = mn.pubKeyMasternode;
    sig = mn.sig;
    activeState = mn.activeState;
    sigTime = mn.sigTime;
    lastPing = mn.lastPing;
    cacheInputAge = mn.cacheInputAge;
    cacheInputAgeBlock = mn.cacheInputAgeBlock;
    unitTest = mn.unitTest;
    allowFreeTx = mn.allowFreeTx;
    protocolVersion = mn.protocolVersion;
    nLastDsq = mn.nLastDsq;
    nScanningErrorCount = mn.nScanningErrorCount;
    nLastScanningErrorBlockHeight = mn.nLastScanningErrorBlockHeight;
}

bool CMasternodeBroadcast::Create(std::string strService, std::string strKeyMasternode, std::string strTxHash, std::string strOutputIndex, std::string& strErrorRet, CMasternodeBroadcast& mnbRet, bool fOffline)
{
    CTxIn txin;
    CPubKey pubKeyCollateralAddressNew;
    CKey keyCollateralAddressNew;
    CPubKey pubKeyMasternodeNew;
    CKey keyMasternodeNew;

    // need correct blocks to send ping
    if (!masternodeSync.IsBlockchainSynced()) {
        strErrorRet = "Sync in progress. Must wait until sync is complete to start Masternode";
        LogPrint(BCLog::MASTERNODE, "CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if (!legacySigner.GetKeysFromSecret(strKeyMasternode, keyMasternodeNew, pubKeyMasternodeNew)) {
        strErrorRet = strprintf("Invalid masternode key %s", strKeyMasternode);
        LogPrint(BCLog::MASTERNODE, "CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if (!GetMasternodeVinAndKeys(txin, pubKeyCollateralAddressNew, keyCollateralAddressNew, strTxHash, strOutputIndex)) {
        strErrorRet = strprintf("Could not allocate txin %s:%s for masternode %s", strTxHash, strOutputIndex, strService);
        LogPrint(BCLog::MASTERNODE, "CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    CService service;
    service.SetSpecial(strService);
    return Create(txin, service, keyCollateralAddressNew, pubKeyCollateralAddressNew, keyMasternodeNew, pubKeyMasternodeNew, strErrorRet, mnbRet);
}

bool CMasternodeBroadcast::Create(CTxIn txin, CService service, CKey keyCollateralAddressNew, CPubKey pubKeyCollateralAddressNew, CKey keyMasternodeNew, CPubKey pubKeyMasternodeNew, std::string& strErrorRet, CMasternodeBroadcast& mnbRet)
{
    // wait for reindex and/or import to finish
    if (node::fImporting || node::fReindex)
        return false;

    CMasternodePing mnp(txin);
    if (!mnp.Sign(keyMasternodeNew, pubKeyMasternodeNew)) {
        strErrorRet = strprintf("Failed to sign ping, masternode=%s", txin.prevout.hash.ToString());
        LogPrint(BCLog::MASTERNODE, "CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CMasternodeBroadcast();
        return false;
    }

    mnbRet = CMasternodeBroadcast(service, txin, pubKeyCollateralAddressNew, pubKeyMasternodeNew, PROTOCOL_VERSION);

    if (!mnbRet.IsValidNetAddr()) {
        strErrorRet = strprintf("Invalid IP address %s, masternode=%s", mnbRet.addr.ToStringIP(), txin.prevout.hash.ToString());
        LogPrint(BCLog::MASTERNODE, "CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CMasternodeBroadcast();
        return false;
    }

    mnbRet.lastPing = mnp;
    if (!mnbRet.Sign(keyCollateralAddressNew)) {
        strErrorRet = strprintf("Failed to sign broadcast, masternode=%s", txin.prevout.hash.ToString());
        LogPrint(BCLog::MASTERNODE, "CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CMasternodeBroadcast();
        return false;
    }

    return true;
}

bool CMasternodeBroadcast::CheckDefaultPort(std::string strService, std::string& strErrorRet, std::string strContext)
{
    int nDefaultPort = Params().GetDefaultPort();

    CService service;
    Lookup(strService, service, nDefaultPort, true);
    if (!service.IsValid()) {
        strErrorRet = strprintf("Invalid address for masternode");
        return false;
    }

    if (service.GetPort() != nDefaultPort) {
        strErrorRet = strprintf("Invalid port %u for masternode %s, only %d is supported on %s-net.",
            service.GetPort(), strService, nDefaultPort, Params().NetworkIDString());
        LogPrint(BCLog::MASTERNODE, "%s - %s\n", strContext, strErrorRet);
        return false;
    }

    return true;
}

bool CMasternodeBroadcast::CheckAndUpdate(int& nDos, CConnman* connman)
{
    // make sure signature isn't in the future (past is OK)
    if (sigTime > TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime()) + 60 * 60) {
        LogPrint(BCLog::MASTERNODE, "mnb - Signature rejected, too far into the future %s\n", vin.prevout.hash.ToString());
        nDos = 1;
        return false;
    }

    // incorrect ping or its sigTime
    if (lastPing == CMasternodePing() || !lastPing.CheckAndUpdate(nDos, connman, false, true))
        return false;

    if (protocolVersion < masternodePayments.GetMinMasternodePaymentsProto()) {
        LogPrint(BCLog::MASTERNODE, "mnb - ignoring outdated Masternode %s protocol version %d\n", vin.prevout.hash.ToString(), protocolVersion);
        return false;
    }

    CScript pubkeyScript;
    pubkeyScript = GetScriptForDestination(PKHash(pubKeyCollateralAddress));

    if (pubkeyScript.size() != 25) {
        LogPrint(BCLog::MASTERNODE, "mnb - pubkey the wrong size\n");
        nDos = 100;
        return false;
    }

    CScript pubkeyScript2;
    pubkeyScript2 = GetScriptForDestination(PKHash(pubKeyMasternode));

    if (pubkeyScript2.size() != 25) {
        LogPrint(BCLog::MASTERNODE, "mnb - pubkey2 the wrong size\n");
        nDos = 100;
        return false;
    }

    if (!vin.scriptSig.empty()) {
        LogPrint(BCLog::MASTERNODE, "mnb - Ignore Not Empty ScriptSig %s\n", vin.prevout.hash.ToString());
        return false;
    }

    std::string errorMessage;
    if (!legacySigner.VerifyMessage(pubKeyCollateralAddress, sig, GetNewStrMessage(), errorMessage)
        && !legacySigner.VerifyMessage(pubKeyCollateralAddress, sig, GetOldStrMessage(), errorMessage)) {
        // don't ban for old masternodes, their sigs could be broken because of the bug
        nDos = protocolVersion < MIN_PEER_MNANNOUNCE ? 0 : 100;
        return error("CMasternodeBroadcast::CheckAndUpdate - Got bad Masternode address signature : %s", errorMessage);
    }

    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (addr.GetPort() != 23511)
            return false;
    } else if (addr.GetPort() == 23511)
        return false;

    // search existing Masternode list, this is where we update existing Masternodes with new mnb broadcasts
    CMasternode* pmn = mnodeman.Find(vin);

    // no such masternode, nothing to update
    if (!pmn)
        return true;

    // this broadcast is older or equal than the one that we already have - it's bad and should never happen
    // unless someone is doing something fishy
    // (mapSeenMasternodeBroadcast in CMasternodeMan::ProcessMessage should filter legit duplicates)
    if (pmn->sigTime >= sigTime) {
        return error("CMasternodeBroadcast::CheckAndUpdate - Bad sigTime %d for Masternode %20s %105s (existing broadcast is at %d)",
            sigTime, addr.ToString(), vin.ToString(), pmn->sigTime);
    }

    // masternode is not enabled yet/already, nothing to update
    if (!pmn->IsEnabled())
        return true;

    // mn.pubkey = pubkey, IsVinAssociatedWithPubkey is validated once below,
    //   after that they just need to match
    if (pmn->pubKeyCollateralAddress == pubKeyCollateralAddress && !pmn->IsBroadcastedWithin(MASTERNODE_MIN_MNB_SECONDS)) {
        // take the newest entry
        LogPrint(BCLog::MASTERNODE, "mnb - Got updated entry for %s\n", vin.prevout.hash.ToString());
        if (pmn->UpdateFromNewBroadcast((*this), connman)) {
            pmn->Check();
            if (pmn->IsEnabled())
                Relay(connman);
        }
        masternodeSync.AddedMasternodeList(GetHash());
    }

    return true;
}

bool CMasternodeBroadcast::CheckInputsAndAdd(int& nDoS, Chainstate& chainstate, CConnman* connman)
{
    // we are a masternode with the same vin (i.e. already activated) and this mnb is ours (matches our Masternode privkey)
    // so nothing to do here for us
    if (fMasterNode && vin.prevout == activeMasternode.vin.prevout && pubKeyMasternode == activeMasternode.pubKeyMasternode)
        return true;

    // incorrect ping or its sigTime
    if (lastPing == CMasternodePing() || !lastPing.CheckAndUpdate(nDoS, connman, false, true))
        return false;

    // search existing Masternode list
    CMasternode* pmn = mnodeman.Find(vin);

    if (pmn) {
        // nothing to do here if we already know about this masternode and it's enabled
        if (pmn->IsEnabled())
            return true;
        // if it's not enabled, remove old MN first and continue
        else
            mnodeman.Remove(pmn->vin);
    }

    //! note we normally check collateral here, however we now check this in .check() like dash

    LogPrint(BCLog::MASTERNODE, "mnb - Accepted Masternode entry\n");

    if (GetInputAge(vin, chainstate) < MASTERNODE_MIN_CONFIRMATIONS) {
        LogPrint(BCLog::MASTERNODE, "mnb - Input must have at least %d confirmations\n", MASTERNODE_MIN_CONFIRMATIONS);
        // maybe we miss few blocks, let this mnb to be checked again later
        mnodeman.mapSeenMasternodeBroadcast.erase(GetHash());
        masternodeSync.mapSeenSyncMNB.erase(GetHash());
        return false;
    }

    // verify that sig time is legit in past
    // should be at least not earlier than block when 100000 YCE tx got MASTERNODE_MIN_CONFIRMATIONS
    uint256 hashBlock{};
    CTransactionRef tx2 = node::GetTransaction(nullptr, nullptr, vin.prevout.hash, Params().GetConsensus(), hashBlock);

    node::BlockMap::iterator mi = chainstate.m_blockman.m_block_index.find(hashBlock);
    if (mi != chainstate.m_blockman.m_block_index.end() && &(*mi).second) {
        // block for 100000 Myce tx -> 1 confirmation
        CBlockIndex* pMNIndex = &(*mi).second;
        // block where tx got MASTERNODE_MIN_CONFIRMATIONS
        CBlockIndex* pConfIndex = chainstate.m_chainman.ActiveChain()[pMNIndex->nHeight + MASTERNODE_MIN_CONFIRMATIONS - 1];
        if (pConfIndex->GetBlockTime() > sigTime) {
            LogPrint(BCLog::MASTERNODE, "mnb - Bad sigTime %d for Masternode %s (%i conf block is at %d)\n",
                     sigTime, vin.prevout.hash.ToString(), MASTERNODE_MIN_CONFIRMATIONS, pConfIndex->GetBlockTime());
            return false;
        }
    }

    LogPrint(BCLog::MASTERNODE, "mnb - Got NEW Masternode entry - %s - %lli \n", vin.prevout.hash.ToString(), sigTime);
    CMasternode mn(*this);
    mnodeman.Add(mn);

    // if it matches our Masternode privkey, then we've been remotely activated
    if (pubKeyMasternode == activeMasternode.pubKeyMasternode && protocolVersion == PROTOCOL_VERSION) {
        activeMasternode.EnableHotColdMasterNode(vin, addr);
    }

    bool isLocal = addr.IsRFC1918() || addr.IsLocal();
    if (Params().NetworkIDString() == CBaseChainParams::REGTEST)
        isLocal = false;

    if (!isLocal)
        Relay(connman);

    return true;
}

void CMasternodeBroadcast::Relay(CConnman* connman)
{
    CInv inv(MSG_MASTERNODE_ANNOUNCE, GetHash());
    connman->RelayInv(inv);
}

uint256 CMasternodeBroadcast::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << sigTime;
    ss << pubKeyCollateralAddress;
    return ss.GetHash();
}

bool CMasternodeBroadcast::Sign(CKey& keyCollateralAddress)
{
    std::string errorMessage;
    sigTime = TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime());

    std::string strMessage = GetNewStrMessage();

    if (!legacySigner.SignMessage(strMessage, sig, keyCollateralAddress))
        return error("CMasternodeBroadcast::Sign() - Error: %s", errorMessage);

    if (!legacySigner.VerifyMessage(pubKeyCollateralAddress, sig, strMessage, errorMessage))
        return error("CMasternodeBroadcast::Sign() - Error: %s", errorMessage);

    return true;
}

bool CMasternodeBroadcast::VerifySignature()
{
    std::string errorMessage;

    if (!legacySigner.VerifyMessage(pubKeyCollateralAddress, sig, GetNewStrMessage(), errorMessage)
        && !legacySigner.VerifyMessage(pubKeyCollateralAddress, sig, GetOldStrMessage(), errorMessage))
        return error("CMasternodeBroadcast::VerifySignature() - Error: %s", errorMessage);

    return true;
}

std::string CMasternodeBroadcast::GetOldStrMessage()
{
    std::string strMessage;

    std::string vchPubKey(pubKeyCollateralAddress.begin(), pubKeyCollateralAddress.end());
    std::string vchPubKey2(pubKeyMasternode.begin(), pubKeyMasternode.end());
    strMessage = addr.ToString() + std::to_string(sigTime) + vchPubKey + vchPubKey2 + std::to_string(protocolVersion);

    return strMessage;
}

std::string CMasternodeBroadcast::GetNewStrMessage()
{
    std::string strMessage;

    strMessage = addr.ToString() + std::to_string(sigTime) + PKHash(pubKeyCollateralAddress).ToString() + PKHash(pubKeyMasternode).ToString() + std::to_string(protocolVersion);

    return strMessage;
}

CMasternodePing::CMasternodePing()
{
    vin = CTxIn();
    blockHash = uint256();
    sigTime = 0;
    vchSig = std::vector<unsigned char>();
}

CMasternodePing::CMasternodePing(CTxIn& newVin)
{
    auto chainman = mnodeman.getChainMan();
    vin = newVin;
    int height = chainman->ActiveChain().Height();
    if (height > 12)
        blockHash = chainman->ActiveChain()[height - 12]->GetBlockHash();
    sigTime = TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime());
    vchSig = std::vector<unsigned char>();
}

bool CMasternodePing::Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode)
{
    std::string errorMessage;
    std::string strMasterNodeSignMessage;

    sigTime = TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime());
    std::string strMessage = vin.ToString() + blockHash.ToString() + std::to_string(sigTime);

    if (!legacySigner.SignMessage(strMessage, vchSig, keyMasternode)) {
        LogPrint(BCLog::MASTERNODE, "CMasternodePing::Sign() - Error: %s\n", errorMessage);
        return false;
    }

    if (!legacySigner.VerifyMessage(pubKeyMasternode, vchSig, strMessage, errorMessage)) {
        LogPrint(BCLog::MASTERNODE, "CMasternodePing::Sign() - Error: %s\n", errorMessage);
        return false;
    }

    return true;
}

bool CMasternodePing::VerifySignature(CPubKey& pubKeyMasternode, int& nDos)
{
    std::string errorMessage;
    std::string strMessage = vin.ToString() + blockHash.ToString() + std::to_string(sigTime);

    if (!legacySigner.VerifyMessage(pubKeyMasternode, vchSig, strMessage, errorMessage)) {
        nDos = 33;
        return error("CMasternodePing::VerifySignature - Got bad Masternode ping signature %s Error: %s", vin.ToString(), errorMessage);
    }
    return true;
}

bool CMasternodePing::CheckAndUpdate(int& nDos, CConnman* connman, bool fRequireEnabled, bool fCheckSigTimeOnly)
{
    if (sigTime > TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime()) + 60 * 60) {
        LogPrint(BCLog::MASTERNODE, "CMasternodePing::CheckAndUpdate - Signature rejected, too far into the future %s\n", vin.prevout.hash.ToString());
        nDos = 1;
        return false;
    }

    if (sigTime <= TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime()) - 60 * 60) {
        LogPrint(BCLog::MASTERNODE, "CMasternodePing::CheckAndUpdate - Signature rejected, too far into the past %s - %d %d \n", vin.prevout.hash.ToString(), sigTime, TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime()));
        nDos = 1;
        return false;
    }

    if (fCheckSigTimeOnly) {
        CMasternode* pmn = mnodeman.Find(vin);
        if (pmn)
            return VerifySignature(pmn->pubKeyMasternode, nDos);
        return true;
    }

    LogPrint(BCLog::MASTERNODE, "CMasternodePing::CheckAndUpdate - New Ping - %s - %s - %lli\n", GetHash().ToString(), blockHash.ToString(), sigTime);

    // see if we have this Masternode
    CMasternode* pmn = mnodeman.Find(vin);
    if (pmn && pmn->protocolVersion >= masternodePayments.GetMinMasternodePaymentsProto()) {
        if (fRequireEnabled && !pmn->IsEnabled())
            return false;

        // LogPrint(BCLog::MASTERNODE, "mnping - Found corresponding mn for vin: %s\n", vin.ToString());
        // update only if there is no known ping for this masternode or
        // last ping was more then MASTERNODE_MIN_MNP_SECONDS-60 ago comparing to this one
        if (!pmn->IsPingedWithin(MASTERNODE_MIN_MNP_SECONDS - 60, sigTime)) {
            if (!VerifySignature(pmn->pubKeyMasternode, nDos))
                return false;

            auto chainman = mnodeman.getChainMan();

            node::BlockMap::iterator mi = chainman->m_blockman.m_block_index.find(blockHash);
            if (mi != chainman->m_blockman.m_block_index.end() && &(*mi).second) {
                if ((*mi).second.nHeight < chainman->ActiveChain().Height() - 24) {
                    LogPrint(BCLog::MASTERNODE, "CMasternodePing::CheckAndUpdate - Masternode %s block hash %s is too old\n", vin.prevout.hash.ToString(), blockHash.ToString());
                    // Do nothing here (no Masternode update, no mnping relay)
                    // Let this node to be visible but fail to accept mnping
                    return false;
                }
            } else {
                LogPrint(BCLog::MASTERNODE, "CMasternodePing::CheckAndUpdate - Masternode %s block hash %s is unknown\n", vin.prevout.hash.ToString(), blockHash.ToString());
                // maybe we stuck so we shouldn't ban this node, just fail to accept it
                // TODO: or should we also request this block?
                return false;
            }

            pmn->lastPing = *this;

            // mnodeman.mapSeenMasternodeBroadcast.lastPing is probably outdated, so we'll update it
            CMasternodeBroadcast mnb(*pmn);
            uint256 hash = mnb.GetHash();
            if (mnodeman.mapSeenMasternodeBroadcast.count(hash)) {
                mnodeman.mapSeenMasternodeBroadcast[hash].lastPing = *this;
            }

            pmn->Check(true);
            if (!pmn->IsEnabled())
                return false;

            LogPrint(BCLog::MASTERNODE, "CMasternodePing::CheckAndUpdate - Masternode ping accepted, vin: %s\n", vin.prevout.hash.ToString());

            Relay(connman);
            return true;
        }
        LogPrint(BCLog::MASTERNODE, "CMasternodePing::CheckAndUpdate - Masternode ping arrived too early, vin: %s\n", vin.prevout.hash.ToString());
        // nDos = 1; //disable, this is happening frequently and causing banned peers
        return false;
    }
    LogPrint(BCLog::MASTERNODE, "CMasternodePing::CheckAndUpdate - Couldn't find compatible Masternode entry, vin: %s\n", vin.prevout.hash.ToString());

    return false;
}

void CMasternodePing::Relay(CConnman* connman)
{
    CInv inv(MSG_MASTERNODE_PING, GetHash());
    connman->RelayInv(inv);
}
