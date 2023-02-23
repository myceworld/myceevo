// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/masternode-budget.h>

#include <addrman.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <init.h>
#include <key_io.h>
#include <masternode/init.h>
#include <masternode/activemasternode.h>
#include <masternode/budgetdb.h>
#include <masternode/masternode-sync.h>
#include <masternode/masternode.h>
#include <masternode/masternodeman.h>
#include <masternode/masternodesigner.h>
#include <pos/wallet.h>
#include <util/system.h>
#include <validation.h>

#include <boost/filesystem.hpp>

RecursiveMutex cs_budget;

std::map<uint256, int64_t> askedForSourceProposalOrBudget;
std::vector<CBudgetProposalBroadcast> vecImmatureBudgetProposals;
std::vector<CFinalizedBudgetBroadcast> vecImmatureFinalizedBudgets;

int nSubmittedFinalBudget;

int GetBudgetPaymentCycleBlocks()
{
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        return 14 * 24 * 60 * 60 / Params().GetConsensus().nPowTargetSpacing; // number of blocks in 14 days
    } else {
        return 24 * 6 * 60 / Params().GetConsensus().nPowTargetSpacing; // ten times per day
    }
}

bool IsBudgetCollateralValid(uint256 nTxCollateralHash, uint256 nExpectedHash, std::string& strError, int64_t& nTime, int& nConf, Chainstate& chainstate, bool fBudgetFinalization)
{
    const Consensus::Params& params = Params().GetConsensus();

    uint256 nBlockHash;
    CTransactionRef txCollateral = node::GetTransaction(nullptr, nullptr, nTxCollateralHash, params, nBlockHash);
    if (!txCollateral) {
        strError = strprintf("Can't find collateral tx %s", txCollateral->ToString());
        LogPrint(BCLog::MNBUDGET, "CBudgetProposalBroadcast::IsBudgetCollateralValid - %s\n", strError);
        return false;
    }

    if (txCollateral->vout.size() < 1)
        return false;
    if (txCollateral->nLockTime != 0)
        return false;

    CScript findScript;
    findScript << OP_RETURN << ToByteVector(nExpectedHash);

    bool foundOpReturn = false;
    for (const CTxOut& o : txCollateral->vout) {
        if (!o.scriptPubKey.IsNormalPaymentScript() && !o.scriptPubKey.IsUnspendable()) {
            strError = strprintf("Invalid Script %s", txCollateral->ToString());
            LogPrint(BCLog::MNBUDGET, "CBudgetProposalBroadcast::IsBudgetCollateralValid - %s\n", strError);
            return false;
        }
        if (fBudgetFinalization) {
            // Collateral for budget finalization
            // Note: there are still old valid budgets out there, but the check for the new 5 YCE finalization collateral
            //       will also cover the old 50 YCE finalization collateral.
            LogPrint(BCLog::MNBUDGET, "Final Budget: o.scriptPubKey(%s) == findScript(%s) ?\n", o.scriptPubKey.ToString(), findScript.ToString());
            if (o.scriptPubKey == findScript) {
                LogPrint(BCLog::MNBUDGET, "Final Budget: o.nValue(%ld) >= BUDGET_FEE_TX(%ld) ?\n", o.nValue, BUDGET_FEE_TX);
                if (o.nValue >= BUDGET_FEE_TX) {
                    foundOpReturn = true;
                }
            }
        } else {
            // Collateral for normal budget proposal
            LogPrint(BCLog::MNBUDGET, "Normal Budget: o.scriptPubKey(%s) == findScript(%s) ?\n", o.scriptPubKey.ToString(), findScript.ToString());
            if (o.scriptPubKey == findScript) {
                LogPrint(BCLog::MNBUDGET, "Normal Budget: o.nValue(%ld) >= PROPOSAL_FEE_TX(%ld) ?\n", o.nValue, PROPOSAL_FEE_TX);
                if (o.nValue >= PROPOSAL_FEE_TX) {
                    foundOpReturn = true;
                }
            }
        }
    }
    if (!foundOpReturn) {
        strError = strprintf("Couldn't find opReturn %s in %s", nExpectedHash.ToString(), txCollateral->ToString());
        LogPrint(BCLog::MNBUDGET, "CBudgetProposalBroadcast::IsBudgetCollateralValid - %s\n", strError);
        return false;
    }

    // RETRIEVE CONFIRMATIONS AND NTIME
    /*
        - nTime starts as zero and is passed-by-reference out of this function and stored in the external proposal
        - nTime is never validated via the hashing mechanism and comes from a full-validated source (the blockchain)
    */

    int conf = GetIXConfirmations(nTxCollateralHash);
    if (nBlockHash != uint256()) {
        node::BlockMap::iterator mi = chainstate.m_chainman.m_blockman.m_block_index.find(nBlockHash);
        if (mi != chainstate.m_chainman.m_blockman.m_block_index.end() && &(*mi).second) {
            CBlockIndex* pindex = &(*mi).second;
            if (chainstate.m_chainman.ActiveChain().Contains(pindex)) {
                conf += chainstate.m_chainman.ActiveChain().Height() - pindex->nHeight + 1;
                nTime = pindex->nTime;
            }
        }
    }

    nConf = conf;

    // if we're syncing we won't have swiftTX information, so accept 1 confirmation
    if (conf >= params.nBudgetFeeConfirmations) {
        return true;
    } else {
        strError = strprintf("Collateral requires at least %d confirmations - %d confirmations", params.nBudgetFeeConfirmations, conf);
        LogPrint(BCLog::MNBUDGET, "CBudgetProposalBroadcast::IsBudgetCollateralValid - %s - %d confirmations\n", strError, conf);
        return false;
    }
}

void CBudgetManager::CheckOrphanVotes(CConnman& connman)
{
    LOCK(cs);

    std::string strError;
    std::map<uint256, CBudgetVote>::iterator it1 = mapOrphanMasternodeBudgetVotes.begin();
    while (it1 != mapOrphanMasternodeBudgetVotes.end()) {
        if (budget.UpdateProposal(((*it1).second), NULL, connman, strError)) {
            LogPrint(BCLog::MNBUDGET, "CBudgetManager::CheckOrphanVotes - Proposal/Budget is known, activating and removing orphan vote\n");
            mapOrphanMasternodeBudgetVotes.erase(it1++);
        } else {
            ++it1;
        }
    }
    std::map<uint256, CFinalizedBudgetVote>::iterator it2 = mapOrphanFinalizedBudgetVotes.begin();
    while (it2 != mapOrphanFinalizedBudgetVotes.end()) {
        if (budget.UpdateFinalizedBudget(((*it2).second), NULL, connman, strError)) {
            LogPrint(BCLog::MNBUDGET, "CBudgetManager::CheckOrphanVotes - Proposal/Budget is known, activating and removing orphan vote\n");
            mapOrphanFinalizedBudgetVotes.erase(it2++);
        } else {
            ++it2;
        }
    }
    LogPrint(BCLog::MNBUDGET, "CBudgetManager::CheckOrphanVotes - Done\n");
}

void CBudgetManager::SubmitFinalBudget(CBlockIndex* pindex, CConnman& connman, Chainstate& chainstate)
{
    const Consensus::Params& params = Params().GetConsensus();

    static int nSubmittedHeight = 0; // height at which final budget was submitted last time
    int nCurrentHeight;

    {
        LOCK(cs_main);
        nCurrentHeight = pindex->nHeight;
    }

    int nBlockStart = nCurrentHeight - nCurrentHeight % GetBudgetPaymentCycleBlocks() + GetBudgetPaymentCycleBlocks();
    if (nSubmittedHeight >= nBlockStart) {
        LogPrint(BCLog::MNBUDGET, "CBudgetManager::SubmitFinalBudget - nSubmittedHeight(=%ld) < nBlockStart(=%ld) condition not fulfilled.\n", nSubmittedHeight, nBlockStart);
        return;
    }

    // Submit final budget during the last 2 days before payment for Mainnet, about 9 minutes (9 blocks) for Testnet
    int finalizationWindow = ((GetBudgetPaymentCycleBlocks() / 14) * 2);

    if (Params().NetworkIDString() == CBaseChainParams::TESTNET) {
        // NOTE: 9 blocks for testnet is way to short to have any masternode submit an automatic vote on the finalized(!) budget,
        //       because those votes are only submitted/relayed once every 56 blocks in CFinalizedBudget::AutoCheck()

        finalizationWindow = 64; // 56 + 4 finalization confirmations + 4 minutes buffer for propagation
    }

    int nFinalizationStart = nBlockStart - finalizationWindow;

    int nOffsetToStart = nFinalizationStart - nCurrentHeight;

    if (nBlockStart - nCurrentHeight > finalizationWindow) {
        LogPrint(BCLog::MNBUDGET, "CBudgetManager::SubmitFinalBudget - Too early for finalization. Current block is %ld, next Superblock is %ld.\n", nCurrentHeight, nBlockStart);
        LogPrint(BCLog::MNBUDGET, "CBudgetManager::SubmitFinalBudget - First possible block for finalization: %ld. Last possible block for finalization: %ld. You have to wait for %ld block(s) until Budget finalization will be possible\n", nFinalizationStart, nBlockStart, nOffsetToStart);

        return;
    }

    std::vector<CBudgetProposal*> vBudgetProposals = budget.GetBudget(pindex);
    std::string strBudgetName = "main";
    std::vector<CTxBudgetPayment> vecTxBudgetPayments;

    for (unsigned int i = 0; i < vBudgetProposals.size(); i++) {
        CTxBudgetPayment txBudgetPayment;
        txBudgetPayment.nProposalHash = vBudgetProposals[i]->GetHash();
        txBudgetPayment.payee = vBudgetProposals[i]->GetPayee();
        txBudgetPayment.nAmount = vBudgetProposals[i]->GetAllotted();
        vecTxBudgetPayments.push_back(txBudgetPayment);
    }

    if (vecTxBudgetPayments.size() < 1) {
        LogPrint(BCLog::MNBUDGET, "CBudgetManager::SubmitFinalBudget - Found No Proposals For Period\n");
        return;
    }

    CFinalizedBudgetBroadcast tempBudget(strBudgetName, nBlockStart, vecTxBudgetPayments, uint256());
    if (mapSeenFinalizedBudgets.count(tempBudget.GetHash())) {
        LogPrint(BCLog::MNBUDGET, "CBudgetManager::SubmitFinalBudget - Budget already exists - %s\n", tempBudget.GetHash().ToString());
        nSubmittedHeight = nCurrentHeight;
        return; // already exists
    }

    // create fee tx
    CTransactionRef tx;
    uint256 txidCollateral;

    if (!mapCollateralTxids.count(tempBudget.GetHash())) {

        if (!stakeWallet.GetStakingWallet()) {
            LogPrint(BCLog::MNBUDGET, "CBudgetManager::SubmitFinalBudget - Wallet is not loaded\n");
            return;
        }

        CTransactionRef wtx;
        if (!GetBudgetFinalizationCollateralTX(wtx, tempBudget.GetHash())) {
            LogPrint(BCLog::MNBUDGET, "CBudgetManager::SubmitFinalBudget - Can't make collateral transaction\n");
            return;
        }

        // Send the tx to the network. Do NOT use SwiftTx, locking might need too much time to propagate, especially for testnet
        stakeWallet.GetStakingWallet()->CommitTransaction(wtx, {}, {});
        tx = wtx;
        txidCollateral = tx->GetHash();
        mapCollateralTxids.insert(std::make_pair(tempBudget.GetHash(), txidCollateral));
    } else {
        txidCollateral = mapCollateralTxids[tempBudget.GetHash()];
    }

    uint256 nBlockHash{};
    int conf = GetIXConfirmations(txidCollateral);
    CTransactionRef txCollateral = node::GetTransaction(nullptr, nullptr, txidCollateral, params, nBlockHash);
    if (!txCollateral) {
        LogPrint(BCLog::MNBUDGET, "CBudgetManager::SubmitFinalBudget - Can't find collateral tx %s", txidCollateral.ToString());
        return;
    }

    if (nBlockHash != uint256()) {
        node::BlockMap::iterator mi = chainstate.m_blockman.m_block_index.find(nBlockHash);
        if (mi != chainstate.m_blockman.m_block_index.end() && &(*mi).second) {
            CBlockIndex* pindex = &(*mi).second;
            if (chainstate.m_chainman.ActiveChain().Contains(pindex)) {
                conf += chainstate.m_chainman.ActiveChain().Height() - pindex->nHeight + 1;
            }
        }
    }

    /*
        Wait will we have 1 extra confirmation, otherwise some clients might reject this feeTX
        -- This function is tied to NewBlock, so we will propagate this budget while the block is also propagating
    */
    if (conf < params.nBudgetFeeConfirmations + 1) {
        LogPrint(BCLog::MNBUDGET, "CBudgetManager::SubmitFinalBudget - Collateral requires at least %d confirmations - %s - %d confirmations\n", params.nBudgetFeeConfirmations + 1, txidCollateral.ToString(), conf);
        return;
    }

    // create the proposal incase we're the first to make it
    CFinalizedBudgetBroadcast finalizedBudgetBroadcast(strBudgetName, nBlockStart, vecTxBudgetPayments, txidCollateral);

    std::string strError;
    if (!finalizedBudgetBroadcast.IsValid(pindex, strError)) {
        LogPrint(BCLog::MNBUDGET, "CBudgetManager::SubmitFinalBudget - Invalid finalized budget - %s \n", strError);
        return;
    }

    LOCK(cs);
    mapSeenFinalizedBudgets.insert(std::make_pair(finalizedBudgetBroadcast.GetHash(), finalizedBudgetBroadcast));
    finalizedBudgetBroadcast.Relay(&connman);
    budget.AddFinalizedBudget(finalizedBudgetBroadcast, pindex);
    nSubmittedHeight = nCurrentHeight;
    LogPrint(BCLog::MNBUDGET, "CBudgetManager::SubmitFinalBudget - Done! %s\n", finalizedBudgetBroadcast.GetHash().ToString());
}

bool CBudgetManager::AddFinalizedBudget(CFinalizedBudget& finalizedBudget, CBlockIndex* pindex)
{
    std::string strError;
    if (!finalizedBudget.IsValid(pindex, strError))
        return false;

    if (mapFinalizedBudgets.count(finalizedBudget.GetHash())) {
        return false;
    }

    mapFinalizedBudgets.insert(std::make_pair(finalizedBudget.GetHash(), finalizedBudget));
    return true;
}

bool CBudgetManager::AddProposal(CBudgetProposal& budgetProposal, CBlockIndex* pindex)
{
    LOCK(cs);
    std::string strError;
    if (!budgetProposal.IsValid(pindex, strError)) {
        LogPrint(BCLog::MNBUDGET, "CBudgetManager::AddProposal - invalid budget proposal - %s\n", strError);
        return false;
    }

    if (mapProposals.count(budgetProposal.GetHash())) {
        return false;
    }

    mapProposals.insert(std::make_pair(budgetProposal.GetHash(), budgetProposal));
    LogPrint(BCLog::MNBUDGET, "CBudgetManager::AddProposal - proposal %s added\n", budgetProposal.GetName().c_str());
    return true;
}

void CBudgetManager::CheckAndRemove(CBlockIndex* pindex, CConnman* connman)
{
    int nHeight = 0;

    {
        if (pindex) {
            nHeight = pindex->nHeight;
        }
    }

    LogPrint(BCLog::MNBUDGET, "CBudgetManager::CheckAndRemove at Height=%d\n", nHeight);

    std::map<uint256, CFinalizedBudget> tmpMapFinalizedBudgets;
    std::map<uint256, CBudgetProposal> tmpMapProposals;

    std::string strError;

    LogPrint(BCLog::MNBUDGET, "CBudgetManager::CheckAndRemove - mapFinalizedBudgets cleanup - size before: %d\n", mapFinalizedBudgets.size());
    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while (it != mapFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);

        pfinalizedBudget->fValid = pfinalizedBudget->IsValid(pindex, strError);
        if (!strError.empty()) {
            LogPrint(BCLog::MNBUDGET, "CBudgetManager::CheckAndRemove - Invalid finalized budget: %s\n", strError);
        } else {
            LogPrint(BCLog::MNBUDGET, "CBudgetManager::CheckAndRemove - Found valid finalized budget: %s %s\n",
                pfinalizedBudget->strBudgetName, pfinalizedBudget->nFeeTXHash.ToString());
        }

        if (pfinalizedBudget->fValid) {
            pfinalizedBudget->CheckAndVote(pindex, connman);
            tmpMapFinalizedBudgets.insert(std::make_pair(pfinalizedBudget->GetHash(), *pfinalizedBudget));
        }

        ++it;
    }

    LogPrint(BCLog::MNBUDGET, "CBudgetManager::CheckAndRemove - mapProposals cleanup - size before: %d\n", mapProposals.size());
    std::map<uint256, CBudgetProposal>::iterator it2 = mapProposals.begin();
    while (it2 != mapProposals.end()) {
        CBudgetProposal* pbudgetProposal = &((*it2).second);
        pbudgetProposal->fValid = pbudgetProposal->IsValid(pindex, strError);
        if (!strError.empty()) {
            LogPrint(BCLog::MNBUDGET, "CBudgetManager::CheckAndRemove - Invalid budget proposal - %s\n", strError);
            strError = "";
        } else {
            LogPrint(BCLog::MNBUDGET, "CBudgetManager::CheckAndRemove - Found valid budget proposal: %s %s\n",
                pbudgetProposal->strProposalName, pbudgetProposal->nFeeTXHash.ToString());
        }
        if (pbudgetProposal->fValid) {
            tmpMapProposals.insert(std::make_pair(pbudgetProposal->GetHash(), *pbudgetProposal));
        }

        ++it2;
    }
    // Remove invalid entries by overwriting complete map
    mapFinalizedBudgets.swap(tmpMapFinalizedBudgets);
    mapProposals.swap(tmpMapProposals);

    // clang doesn't accept copy assignemnts :-/
    // mapFinalizedBudgets = tmpMapFinalizedBudgets;
    // mapProposals = tmpMapProposals;

    LogPrint(BCLog::MNBUDGET, "CBudgetManager::CheckAndRemove - mapFinalizedBudgets cleanup - size after: %d\n", mapFinalizedBudgets.size());
    LogPrint(BCLog::MNBUDGET, "CBudgetManager::CheckAndRemove - mapProposals cleanup - size after: %d\n", mapProposals.size());
    LogPrint(BCLog::MNBUDGET, "CBudgetManager::CheckAndRemove - PASSED\n");
}

void CBudgetManager::FillBlockPayee(int nBlockHeight, CMutableTransaction& txNew, CAmount nFees, bool fProofOfStake)
{
    LOCK(cs);

    int nHighestCount = 0;
    CScript payee;
    CAmount nAmount = 0;

    // ------- Grab The Highest Count

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while (it != mapFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);
        if (pfinalizedBudget->GetVoteCount() > nHighestCount && nBlockHeight + 1 >= pfinalizedBudget->GetBlockStart() && nBlockHeight + 1 <= pfinalizedBudget->GetBlockEnd() && pfinalizedBudget->GetPayeeAndAmount(nBlockHeight + 1, payee, nAmount)) {
            nHighestCount = pfinalizedBudget->GetVoteCount();
        }

        ++it;
    }

    const CChainParams& params = Params();
    CAmount blockValue = GetBlockSubsidy(nBlockHeight, params, fProofOfStake);

    if (fProofOfStake) {
        if (nHighestCount > 0) {
            unsigned int i = txNew.vout.size();
            txNew.vout.resize(i + 1);
            txNew.vout[i].scriptPubKey = payee;
            txNew.vout[i].nValue = nAmount;

            CTxDestination address1;
            ExtractDestination(payee, address1);
            CTxDestination address2(address1);
            LogPrint(BCLog::MNBUDGET, "CBudgetManager::FillBlockPayee - Budget payment to %s for %lld, nHighestCount = %d\n", EncodeDestination(address2), nAmount, nHighestCount);
        } else {
            LogPrint(BCLog::MNBUDGET, "CBudgetManager::FillBlockPayee - No Budget payment, nHighestCount = %d\n", nHighestCount);
        }
    } else {
        // miners get the full amount on these blocks
        txNew.vout[0].nValue = blockValue;

        if (nHighestCount > 0) {
            txNew.vout.resize(2);

            // these are super blocks, so their value can be much larger than normal
            txNew.vout[1].scriptPubKey = payee;
            txNew.vout[1].nValue = nAmount;

            CTxDestination address1;
            ExtractDestination(payee, address1);
            CTxDestination address2(address1);

            LogPrint(BCLog::MNBUDGET, "CBudgetManager::FillBlockPayee - Budget payment to %s for %lld\n", EncodeDestination(address2), nAmount);
        }
    }
}

CFinalizedBudget* CBudgetManager::FindFinalizedBudget(uint256 nHash)
{
    if (mapFinalizedBudgets.count(nHash))
        return &mapFinalizedBudgets[nHash];

    return NULL;
}

CBudgetProposal* CBudgetManager::FindProposal(const std::string& strProposalName)
{
    // find the prop with the highest yes count

    int nYesCount = -99999;
    CBudgetProposal* pbudgetProposal = NULL;

    std::map<uint256, CBudgetProposal>::iterator it = mapProposals.begin();
    while (it != mapProposals.end()) {
        if ((*it).second.strProposalName == strProposalName && (*it).second.GetYeas() > nYesCount) {
            pbudgetProposal = &((*it).second);
            nYesCount = pbudgetProposal->GetYeas();
        }
        ++it;
    }

    if (nYesCount == -99999)
        return NULL;

    return pbudgetProposal;
}

CBudgetProposal* CBudgetManager::FindProposal(uint256 nHash)
{
    LOCK(cs);

    if (mapProposals.count(nHash))
        return &mapProposals[nHash];

    return NULL;
}

bool CBudgetManager::IsBudgetPaymentBlock(int nBlockHeight)
{
    int nHighestCount = -1;
    int nFivePercent = mnodeman.CountEnabled(PROTOCOL_VERSION - 1) / 20;

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while (it != mapFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);
        if (pfinalizedBudget->GetVoteCount() > nHighestCount && nBlockHeight >= pfinalizedBudget->GetBlockStart() && nBlockHeight <= pfinalizedBudget->GetBlockEnd()) {
            nHighestCount = pfinalizedBudget->GetVoteCount();
        }

        ++it;
    }

    LogPrint(BCLog::MNBUDGET, "CBudgetManager::IsBudgetPaymentBlock() - nHighestCount: %lli, 5%% of Masternodes: %lli. Number of finalized budgets: %lli\n",
        nHighestCount, nFivePercent, mapFinalizedBudgets.size());

    // If budget doesn't have 5% of the network votes, then we should pay a masternode instead
    if (nHighestCount > nFivePercent)
        return true;

    return false;
}

TrxValidationStatus CBudgetManager::IsTransactionValid(const CTransactionRef& txNew, int nBlockHeight)
{
    LOCK(cs);

    TrxValidationStatus transactionStatus = TrxValidationStatus::InValid;
    int nHighestCount = 0;
    int nFivePercent = mnodeman.CountEnabled(PROTOCOL_VERSION - 1) / 20;
    std::vector<CFinalizedBudget*> ret;

    LogPrint(BCLog::MNBUDGET, "CBudgetManager::IsTransactionValid - checking %lli finalized budgets\n", mapFinalizedBudgets.size());

    // ------- Grab The Highest Count

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while (it != mapFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);

        if (pfinalizedBudget->GetVoteCount() > nHighestCount && nBlockHeight >= pfinalizedBudget->GetBlockStart() && nBlockHeight <= pfinalizedBudget->GetBlockEnd()) {
            nHighestCount = pfinalizedBudget->GetVoteCount();
        }

        ++it;
    }

    LogPrint(BCLog::MNBUDGET, "CBudgetManager::IsTransactionValid() - nHighestCount: %lli, 5%% of Masternodes: %lli mapFinalizedBudgets.size(): %ld\n",
        nHighestCount, nFivePercent, mapFinalizedBudgets.size());
    /*
        If budget doesn't have 5% of the network votes, then we should pay a masternode instead
    */
    if (nHighestCount < nFivePercent)
        return TrxValidationStatus::InValid;

    // check the highest finalized budgets (+/- 10% to assist in consensus)

    std::string strProposals = "";
    int nCountThreshold = nHighestCount - mnodeman.CountEnabled(PROTOCOL_VERSION - 1) / 10;
    bool fThreshold = false;
    it = mapFinalizedBudgets.begin();
    while (it != mapFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);
        strProposals = pfinalizedBudget->GetProposals();

        LogPrint(BCLog::MNBUDGET, "CBudgetManager::IsTransactionValid - checking budget (%s) with blockstart %lli, blockend %lli, nBlockHeight %lli, votes %lli, nCountThreshold %lli\n",
            strProposals, pfinalizedBudget->GetBlockStart(), pfinalizedBudget->GetBlockEnd(),
            nBlockHeight, pfinalizedBudget->GetVoteCount(), nCountThreshold);

        if (pfinalizedBudget->GetVoteCount() > nCountThreshold) {
            fThreshold = true;
            LogPrint(BCLog::MNBUDGET, "CBudgetManager::IsTransactionValid - GetVoteCount() > nCountThreshold passed\n");
            if (nBlockHeight >= pfinalizedBudget->GetBlockStart() && nBlockHeight <= pfinalizedBudget->GetBlockEnd()) {
                LogPrint(BCLog::MNBUDGET, "CBudgetManager::IsTransactionValid - GetBlockStart() passed\n");
                transactionStatus = pfinalizedBudget->IsTransactionValid(txNew, nBlockHeight);
                if (transactionStatus == TrxValidationStatus::Valid) {
                    LogPrint(BCLog::MNBUDGET, "CBudgetManager::IsTransactionValid - pfinalizedBudget->IsTransactionValid() passed\n");
                    return TrxValidationStatus::Valid;
                } else {
                    LogPrint(BCLog::MNBUDGET, "CBudgetManager::IsTransactionValid - pfinalizedBudget->IsTransactionValid() error\n");
                }
            } else {
                LogPrint(BCLog::MNBUDGET, "CBudgetManager::IsTransactionValid - GetBlockStart() failed, budget is outside current payment cycle and will be ignored.\n");
            }
        }

        ++it;
    }

    // If not enough masternodes autovoted for any of the finalized budgets pay a masternode instead
    if (!fThreshold) {
        transactionStatus = TrxValidationStatus::VoteThreshold;
    }

    // We looked through all of the known budgets
    return transactionStatus;
}

std::vector<CBudgetProposal*> CBudgetManager::GetAllProposals()
{
    LOCK(cs);

    std::vector<CBudgetProposal*> vBudgetProposalRet;

    std::map<uint256, CBudgetProposal>::iterator it = mapProposals.begin();
    while (it != mapProposals.end()) {
        (*it).second.CleanAndRemove(false);

        CBudgetProposal* pbudgetProposal = &((*it).second);
        vBudgetProposalRet.push_back(pbudgetProposal);

        ++it;
    }

    return vBudgetProposalRet;
}

//
// Sort by votes, if there's a tie sort by their feeHash TX
//
struct sortProposalsByVotes {
    bool operator()(const std::pair<CBudgetProposal*, int>& left, const std::pair<CBudgetProposal*, int>& right)
    {
        if (left.second != right.second) {
            return (left.second > right.second);
        }
        return (UintToArith256(left.first->nFeeTXHash) > UintToArith256(right.first->nFeeTXHash));
    }
};

// Need to review this function
std::vector<CBudgetProposal*> CBudgetManager::GetBudget(CBlockIndex* pindex)
{
    LOCK(cs);

    // ------- Sort budgets by Yes Count

    std::vector<std::pair<CBudgetProposal*, int>> vBudgetPorposalsSort;

    std::map<uint256, CBudgetProposal>::iterator it = mapProposals.begin();
    while (it != mapProposals.end()) {
        (*it).second.CleanAndRemove(false);
        vBudgetPorposalsSort.push_back(std::make_pair(&((*it).second), (*it).second.GetYeas() - (*it).second.GetNays()));
        ++it;
    }

    std::sort(vBudgetPorposalsSort.begin(), vBudgetPorposalsSort.end(), sortProposalsByVotes());

    // ------- Grab The Budgets In Order

    std::vector<CBudgetProposal*> vBudgetProposalsRet;

    CAmount nBudgetAllocated = 0;

    if (!pindex) {
        return vBudgetProposalsRet;
    }

    int nBlockStart = pindex->nHeight - pindex->nHeight % GetBudgetPaymentCycleBlocks() + GetBudgetPaymentCycleBlocks();
    int nBlockEnd = nBlockStart + GetBudgetPaymentCycleBlocks() - 1;
    int mnCount = mnodeman.CountEnabled(PROTOCOL_VERSION - 1);
    CAmount nTotalBudget = GetTotalBudget(nBlockStart);

    std::vector<std::pair<CBudgetProposal*, int>>::iterator it2 = vBudgetPorposalsSort.begin();
    while (it2 != vBudgetPorposalsSort.end()) {
        CBudgetProposal* pbudgetProposal = (*it2).first;

        LogPrint(BCLog::MNBUDGET, "CBudgetManager::GetBudget() - Processing Budget %s\n", pbudgetProposal->strProposalName);
        // prop start/end should be inside this period
        if (pbudgetProposal->IsPassing(pindex, nBlockStart, nBlockEnd, mnCount)) {
            LogPrint(BCLog::MNBUDGET, "CBudgetManager::GetBudget() -   Check 1 passed: valid=%d | %ld <= %ld | %ld >= %ld | Yeas=%d Nays=%d Count=%d | established=%d\n",
                pbudgetProposal->fValid, pbudgetProposal->nBlockStart, nBlockStart, pbudgetProposal->nBlockEnd,
                nBlockEnd, pbudgetProposal->GetYeas(), pbudgetProposal->GetNays(), mnCount / 10,
                pbudgetProposal->IsEstablished());

            if (pbudgetProposal->GetAmount() + nBudgetAllocated <= nTotalBudget) {
                pbudgetProposal->SetAllotted(pbudgetProposal->GetAmount());
                nBudgetAllocated += pbudgetProposal->GetAmount();
                vBudgetProposalsRet.push_back(pbudgetProposal);
                LogPrint(BCLog::MNBUDGET, "CBudgetManager::GetBudget() -     Check 2 passed: Budget added\n");
            } else {
                pbudgetProposal->SetAllotted(0);
                LogPrint(BCLog::MNBUDGET, "CBudgetManager::GetBudget() -     Check 2 failed: no amount allotted\n");
            }
        } else {
            LogPrint(BCLog::MNBUDGET, "CBudgetManager::GetBudget() -   Check 1 failed: valid=%d | %ld <= %ld | %ld >= %ld | Yeas=%d Nays=%d Count=%d | established=%d\n",
                pbudgetProposal->fValid, pbudgetProposal->nBlockStart, nBlockStart, pbudgetProposal->nBlockEnd,
                nBlockEnd, pbudgetProposal->GetYeas(), pbudgetProposal->GetNays(), mnodeman.CountEnabled(PROTOCOL_VERSION - 1) / 10,
                pbudgetProposal->IsEstablished());
        }

        ++it2;
    }

    return vBudgetProposalsRet;
}

// Sort by votes, if there's a tie sort by their feeHash TX
struct sortFinalizedBudgetsByVotes {
    bool operator()(const std::pair<CFinalizedBudget*, int>& left, const std::pair<CFinalizedBudget*, int>& right)
    {
        if (left.second != right.second) {
            return (left.second > right.second);
        }
        return (UintToArith256(left.first->nFeeTXHash) > UintToArith256(right.first->nFeeTXHash));
    }
};

std::vector<CFinalizedBudget*> CBudgetManager::GetFinalizedBudgets()
{
    LOCK(cs);

    std::vector<CFinalizedBudget*> vFinalizedBudgetsRet;
    std::vector<std::pair<CFinalizedBudget*, int>> vFinalizedBudgetsSort;

    // ------- Grab The Budgets In Order

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while (it != mapFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);

        vFinalizedBudgetsSort.push_back(std::make_pair(pfinalizedBudget, pfinalizedBudget->GetVoteCount()));
        ++it;
    }
    std::sort(vFinalizedBudgetsSort.begin(), vFinalizedBudgetsSort.end(), sortFinalizedBudgetsByVotes());

    std::vector<std::pair<CFinalizedBudget*, int>>::iterator it2 = vFinalizedBudgetsSort.begin();
    while (it2 != vFinalizedBudgetsSort.end()) {
        vFinalizedBudgetsRet.push_back((*it2).first);
        ++it2;
    }

    return vFinalizedBudgetsRet;
}

std::string CBudgetManager::GetRequiredPaymentsString(int nBlockHeight)
{
    LOCK(cs);

    std::string ret = "unknown-budget";

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while (it != mapFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);
        if (nBlockHeight >= pfinalizedBudget->GetBlockStart() && nBlockHeight <= pfinalizedBudget->GetBlockEnd()) {
            CTxBudgetPayment payment;
            if (pfinalizedBudget->GetBudgetPaymentByBlock(nBlockHeight, payment)) {
                if (ret == "unknown-budget") {
                    ret = payment.nProposalHash.ToString();
                } else {
                    ret += ",";
                    ret += payment.nProposalHash.ToString();
                }
            } else {
                LogPrint(BCLog::MNBUDGET, "CBudgetManager::GetRequiredPaymentsString - Couldn't find budget payment for block %d\n", nBlockHeight);
            }
        }

        ++it;
    }

    return ret;
}

CAmount CBudgetManager::GetTotalBudget(int nHeight)
{
    const CChainParams& params = Params();

    CAmount nSubsidy = 0;
    int endHeight = nHeight + GetBudgetPaymentCycleBlocks();
    for (int height = nHeight; height < endHeight; height++) {
        nSubsidy += GetBlockSubsidy(height, params, height > params.GetConsensus().nPoWBlock);
    }

    return nSubsidy / 10; // 10% of block reward
}

void CBudgetManager::NewBlock(CBlockIndex* pindex, CConnman* connman, Chainstate& chainstate)
{
    TRY_LOCK(cs, fBudgetNewBlock);
    if (!fBudgetNewBlock)
        return;

    if (masternodeSync.RequestedMasternodeAssets <= MASTERNODE_SYNC_BUDGET)
        return;

    if (strBudgetMode == "suggest") { // suggest the budget we see
        SubmitFinalBudget(pindex, *connman, chainstate);
    }

    // this function should be called 1/14 blocks, allowing up to 100 votes per day on all proposals
    if (pindex->nHeight % 14 != 0)
        return;

    // incremental sync with our peers
    if (masternodeSync.IsSynced()) {
        LogPrint(BCLog::MNBUDGET, "CBudgetManager::NewBlock - incremental sync started\n");
        if (pindex->nHeight % 1440 == rand() % 1440) {
            ClearSeen();
            ResetSync();
        }

        std::vector<CNode*> vNodesCopy;
        connman->CopyNodeVector(vNodesCopy);
        for (CNode* pnode : vNodesCopy)
            if (pnode->nVersion >= PROTOCOL_VERSION - 1)
                Sync(pnode, uint256(), *connman, true);

        MarkSynced();
    }

    CheckAndRemove(pindex, connman);

    // remove invalid votes once in a while (we have to check the signatures and validity of every vote, somewhat CPU intensive)

    LogPrint(BCLog::MNBUDGET, "CBudgetManager::NewBlock - askedForSourceProposalOrBudget cleanup - size: %d\n", askedForSourceProposalOrBudget.size());
    std::map<uint256, int64_t>::iterator it = askedForSourceProposalOrBudget.begin();
    while (it != askedForSourceProposalOrBudget.end()) {
        if ((*it).second > GetTime() - (60 * 60 * 24)) {
            ++it;
        } else {
            askedForSourceProposalOrBudget.erase(it++);
        }
    }

    LogPrint(BCLog::MNBUDGET, "CBudgetManager::NewBlock - mapProposals cleanup - size: %d\n", mapProposals.size());
    std::map<uint256, CBudgetProposal>::iterator it2 = mapProposals.begin();
    while (it2 != mapProposals.end()) {
        (*it2).second.CleanAndRemove(false);
        ++it2;
    }

    LogPrint(BCLog::MNBUDGET, "CBudgetManager::NewBlock - mapFinalizedBudgets cleanup - size: %d\n", mapFinalizedBudgets.size());
    std::map<uint256, CFinalizedBudget>::iterator it3 = mapFinalizedBudgets.begin();
    while (it3 != mapFinalizedBudgets.end()) {
        (*it3).second.CleanAndRemove(false);
        ++it3;
    }

    LogPrint(BCLog::MNBUDGET, "CBudgetManager::NewBlock - vecImmatureBudgetProposals cleanup - size: %d\n", vecImmatureBudgetProposals.size());
    std::vector<CBudgetProposalBroadcast>::iterator it4 = vecImmatureBudgetProposals.begin();
    while (it4 != vecImmatureBudgetProposals.end()) {
        std::string strError;
        int nConf = 0;
        if (!IsBudgetCollateralValid((*it4).nFeeTXHash, (*it4).GetHash(), strError, (*it4).nTime, nConf, chainstate)) {
            ++it4;
            continue;
        }

        if (!(*it4).IsValid(pindex, strError)) {
            LogPrint(BCLog::MNBUDGET, "mprop (immature) - invalid budget proposal - %s\n", strError);
            it4 = vecImmatureBudgetProposals.erase(it4);
            continue;
        }

        CBudgetProposal budgetProposal((*it4));
        if (AddProposal(budgetProposal, pindex)) {
            (*it4).Relay(connman);
        }

        LogPrint(BCLog::MNBUDGET, "mprop (immature) - new budget - %s\n", (*it4).GetHash().ToString());
        it4 = vecImmatureBudgetProposals.erase(it4);
    }

    LogPrint(BCLog::MNBUDGET, "CBudgetManager::NewBlock - vecImmatureFinalizedBudgets cleanup - size: %d\n", vecImmatureFinalizedBudgets.size());
    std::vector<CFinalizedBudgetBroadcast>::iterator it5 = vecImmatureFinalizedBudgets.begin();
    while (it5 != vecImmatureFinalizedBudgets.end()) {
        std::string strError;
        int nConf = 0;
        if (!IsBudgetCollateralValid((*it5).nFeeTXHash, (*it5).GetHash(), strError, (*it5).nTime, nConf, chainstate, true)) {
            ++it5;
            continue;
        }

        if (!(*it5).IsValid(pindex, strError)) {
            LogPrint(BCLog::MNBUDGET, "fbs (immature) - invalid finalized budget - %s\n", strError);
            it5 = vecImmatureFinalizedBudgets.erase(it5);
            continue;
        }

        LogPrint(BCLog::MNBUDGET, "fbs (immature) - new finalized budget - %s\n", (*it5).GetHash().ToString());

        CFinalizedBudget finalizedBudget((*it5));
        if (AddFinalizedBudget(finalizedBudget, pindex)) {
            (*it5).Relay(connman);
        }

        it5 = vecImmatureFinalizedBudgets.erase(it5);
    }
    LogPrint(BCLog::MNBUDGET, "CBudgetManager::NewBlock - PASSED\n");
}

void CBudgetManager::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman* connman)
{
    if (!masternodeSync.IsBlockchainSynced())
        return;

    LOCK(cs_budget);
    CBlockIndex* pindex = chainman->ActiveChain().Tip();

    if (strCommand == NetMsgType::BUDGETVOTESYNC) {

        uint256 nProp;
        vRecv >> nProp;

        if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
            if (nProp.IsNull()) {
                if (netfulfilledman.HasFulfilledRequest(pfrom->addr, NetMsgType::BUDGETVOTESYNC)) {
                    LogPrint(BCLog::MNBUDGET, "mnvs - peer already asked me for the list\n");
                    //Misbehaving(pfrom->GetId(), 20);
                    return;
                }
                netfulfilledman.AddFulfilledRequest(pfrom->addr, NetMsgType::BUDGETVOTESYNC);
            }
        }

        Sync(pfrom, nProp, *connman);
        LogPrint(BCLog::MNBUDGET, "mnvs - Sent Masternode votes to peer %i\n", pfrom->GetId());
    }

    if (strCommand == NetMsgType::BUDGETPROPOSAL) {

        CBudgetProposalBroadcast budgetProposalBroadcast;
        vRecv >> budgetProposalBroadcast;

        if (mapSeenMasternodeBudgetProposals.count(budgetProposalBroadcast.GetHash())) {
            masternodeSync.AddedBudgetItem(budgetProposalBroadcast.GetHash());
            return;
        }

        std::string strError;
        int nConf = 0;
        if (!IsBudgetCollateralValid(budgetProposalBroadcast.nFeeTXHash, budgetProposalBroadcast.GetHash(), strError, budgetProposalBroadcast.nTime, nConf, chainman->ActiveChainstate())) {
            LogPrint(BCLog::MNBUDGET, "Proposal FeeTX is not valid - %s - %s\n", budgetProposalBroadcast.nFeeTXHash.ToString(), strError);
            if (nConf >= 1)
                vecImmatureBudgetProposals.push_back(budgetProposalBroadcast);
            return;
        }

        mapSeenMasternodeBudgetProposals.insert(std::make_pair(budgetProposalBroadcast.GetHash(), budgetProposalBroadcast));

        if (!budgetProposalBroadcast.IsValid(pindex, strError)) {
            LogPrint(BCLog::MNBUDGET, "mprop - invalid budget proposal - %s\n", strError);
            return;
        }

        CBudgetProposal budgetProposal(budgetProposalBroadcast);
        if (AddProposal(budgetProposal, pindex)) {
            budgetProposalBroadcast.Relay(connman);
        }
        masternodeSync.AddedBudgetItem(budgetProposalBroadcast.GetHash());

        LogPrint(BCLog::MNBUDGET, "mprop - new budget - %s\n", budgetProposalBroadcast.GetHash().ToString());

        // We might have active votes for this proposal that are valid now
        CheckOrphanVotes(*connman);
    }

    if (strCommand == NetMsgType::BUDGETVOTE) {

        CBudgetVote vote;
        vRecv >> vote;
        vote.fValid = true;

        if (mapSeenMasternodeBudgetVotes.count(vote.GetHash())) {
            masternodeSync.AddedBudgetItem(vote.GetHash());
            return;
        }

        CMasternode* pmn = mnodeman.Find(vote.vin);
        if (!pmn) {
            LogPrint(BCLog::MNBUDGET, "mvote - unknown masternode - vin: %s\n", vote.vin.prevout.hash.ToString());
            mnodeman.AskForMN(pfrom, vote.vin, connman);
            return;
        }

        mapSeenMasternodeBudgetVotes.insert(std::make_pair(vote.GetHash(), vote));
        if (!vote.SignatureValid(true)) {
            if (masternodeSync.IsSynced()) {
                LogPrintf("CBudgetManager::ProcessMessage() : mvote - signature invalid\n");
                //Misbehaving(pfrom->GetId(), 20);
            }
            // it could just be a non-synced masternode
            mnodeman.AskForMN(pfrom, vote.vin, connman);
            return;
        }

        std::string strError;
        if (UpdateProposal(vote, pfrom, *connman, strError)) {
            vote.Relay(connman);
            masternodeSync.AddedBudgetItem(vote.GetHash());
        }

        LogPrint(BCLog::MNBUDGET, "mvote - new budget vote for budget %s - %s\n", vote.nProposalHash.ToString(), vote.GetHash().ToString());
    }

    if (strCommand == NetMsgType::FINALBUDGET) {

        CFinalizedBudgetBroadcast finalizedBudgetBroadcast;
        vRecv >> finalizedBudgetBroadcast;

        if (mapSeenFinalizedBudgets.count(finalizedBudgetBroadcast.GetHash())) {
            masternodeSync.AddedBudgetItem(finalizedBudgetBroadcast.GetHash());
            return;
        }

        std::string strError;
        int nConf = 0;
        if (!IsBudgetCollateralValid(finalizedBudgetBroadcast.nFeeTXHash, finalizedBudgetBroadcast.GetHash(), strError, finalizedBudgetBroadcast.nTime, nConf, chainman->ActiveChainstate(), true)) {
            LogPrint(BCLog::MNBUDGET, "fbs - Finalized Budget FeeTX is not valid - %s - %s\n", finalizedBudgetBroadcast.nFeeTXHash.ToString(), strError);

            if (nConf >= 1)
                vecImmatureFinalizedBudgets.push_back(finalizedBudgetBroadcast);
            return;
        }

        mapSeenFinalizedBudgets.insert(std::make_pair(finalizedBudgetBroadcast.GetHash(), finalizedBudgetBroadcast));

        if (!finalizedBudgetBroadcast.IsValid(pindex, strError)) {
            LogPrint(BCLog::MNBUDGET, "fbs - invalid finalized budget - %s\n", strError);
            return;
        }

        LogPrint(BCLog::MNBUDGET, "fbs - new finalized budget - %s\n", finalizedBudgetBroadcast.GetHash().ToString());

        CFinalizedBudget finalizedBudget(finalizedBudgetBroadcast);
        if (AddFinalizedBudget(finalizedBudget, pindex)) {
            finalizedBudgetBroadcast.Relay(connman);
        }
        masternodeSync.AddedBudgetItem(finalizedBudgetBroadcast.GetHash());

        // we might have active votes for this budget that are now valid
        CheckOrphanVotes(*connman);
    }

    if (strCommand == NetMsgType::FINALBUDGETVOTE) {

        CFinalizedBudgetVote vote;
        vRecv >> vote;
        vote.fValid = true;

        if (mapSeenFinalizedBudgetVotes.count(vote.GetHash())) {
            masternodeSync.AddedBudgetItem(vote.GetHash());
            return;
        }

        CMasternode* pmn = mnodeman.Find(vote.vin);
        if (!pmn) {
            LogPrint(BCLog::MNBUDGET, "fbvote - unknown masternode - vin: %s\n", vote.vin.prevout.hash.ToString());
            mnodeman.AskForMN(pfrom, vote.vin, connman);
            return;
        }

        mapSeenFinalizedBudgetVotes.insert(std::make_pair(vote.GetHash(), vote));
        if (!vote.SignatureValid(true)) {
            if (masternodeSync.IsSynced()) {
                LogPrintf("CBudgetManager::ProcessMessage() : fbvote - signature from masternode %s invalid\n", HexStr(pmn->pubKeyMasternode));
                //Misbehaving(pfrom->GetId(), 20);
            }
            // it could just be a non-synced masternode
            mnodeman.AskForMN(pfrom, vote.vin, connman);
            return;
        }

        std::string strError;
        if (UpdateFinalizedBudget(vote, pfrom, *connman, strError)) {
            vote.Relay(connman);
            masternodeSync.AddedBudgetItem(vote.GetHash());

            LogPrint(BCLog::MNBUDGET, "fbvote - new finalized budget vote - %s from masternode %s\n", vote.GetHash().ToString(), HexStr(pmn->pubKeyMasternode));
        } else {
            LogPrint(BCLog::MNBUDGET, "fbvote - rejected finalized budget vote - %s from masternode %s - %s\n", vote.GetHash().ToString(), HexStr(pmn->pubKeyMasternode), strError);
        }
    }
}

bool CBudgetManager::PropExists(uint256 nHash)
{
    if (mapProposals.count(nHash))
        return true;
    return false;
}

// mark that a full sync is needed
void CBudgetManager::ResetSync()
{
    LOCK(cs);

    std::map<uint256, CBudgetProposalBroadcast>::iterator it1 = mapSeenMasternodeBudgetProposals.begin();
    while (it1 != mapSeenMasternodeBudgetProposals.end()) {
        CBudgetProposal* pbudgetProposal = FindProposal((*it1).first);
        if (pbudgetProposal && pbudgetProposal->fValid) {
            // mark votes
            std::map<uint256, CBudgetVote>::iterator it2 = pbudgetProposal->mapVotes.begin();
            while (it2 != pbudgetProposal->mapVotes.end()) {
                (*it2).second.fSynced = false;
                ++it2;
            }
        }
        ++it1;
    }

    std::map<uint256, CFinalizedBudgetBroadcast>::iterator it3 = mapSeenFinalizedBudgets.begin();
    while (it3 != mapSeenFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = FindFinalizedBudget((*it3).first);
        if (pfinalizedBudget && pfinalizedBudget->fValid) {
            // send votes
            std::map<uint256, CFinalizedBudgetVote>::iterator it4 = pfinalizedBudget->mapVotes.begin();
            while (it4 != pfinalizedBudget->mapVotes.end()) {
                (*it4).second.fSynced = false;
                ++it4;
            }
        }
        ++it3;
    }
}

void CBudgetManager::MarkSynced()
{
    LOCK(cs);

    /*
        Mark that we've sent all valid items
    */

    std::map<uint256, CBudgetProposalBroadcast>::iterator it1 = mapSeenMasternodeBudgetProposals.begin();
    while (it1 != mapSeenMasternodeBudgetProposals.end()) {
        CBudgetProposal* pbudgetProposal = FindProposal((*it1).first);
        if (pbudgetProposal && pbudgetProposal->fValid) {
            // mark votes
            std::map<uint256, CBudgetVote>::iterator it2 = pbudgetProposal->mapVotes.begin();
            while (it2 != pbudgetProposal->mapVotes.end()) {
                if ((*it2).second.fValid)
                    (*it2).second.fSynced = true;
                ++it2;
            }
        }
        ++it1;
    }

    std::map<uint256, CFinalizedBudgetBroadcast>::iterator it3 = mapSeenFinalizedBudgets.begin();
    while (it3 != mapSeenFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = FindFinalizedBudget((*it3).first);
        if (pfinalizedBudget && pfinalizedBudget->fValid) {
            // mark votes
            std::map<uint256, CFinalizedBudgetVote>::iterator it4 = pfinalizedBudget->mapVotes.begin();
            while (it4 != pfinalizedBudget->mapVotes.end()) {
                if ((*it4).second.fValid)
                    (*it4).second.fSynced = true;
                ++it4;
            }
        }
        ++it3;
    }
}

void CBudgetManager::Sync(CNode* pfrom, uint256 nProp, CConnman& connman, bool fPartial)
{
    LOCK(cs);

    /*
        Sync with a client on the network

        --

        This code checks each of the hash maps for all known budget proposals and finalized budget proposals, then checks them against the
        budget object to see if they're OK. If all checks pass, we'll send it to the peer.

    */

    int nInvCount = 0;
    const CNetMsgMaker msgMaker(PROTOCOL_VERSION);
    std::map<uint256, CBudgetProposalBroadcast>::iterator it1 = mapSeenMasternodeBudgetProposals.begin();
    while (it1 != mapSeenMasternodeBudgetProposals.end()) {
        CBudgetProposal* pbudgetProposal = FindProposal((*it1).first);
        if (pbudgetProposal && pbudgetProposal->fValid && (nProp.IsNull() || (*it1).first == nProp)) {
            connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::INV, CInv(MSG_BUDGET_PROPOSAL, (*it1).second.GetHash())));
            nInvCount++;

            // send votes
            std::map<uint256, CBudgetVote>::iterator it2 = pbudgetProposal->mapVotes.begin();
            while (it2 != pbudgetProposal->mapVotes.end()) {
                if ((*it2).second.fValid) {
                    if ((fPartial && !(*it2).second.fSynced) || !fPartial) {
                        connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::INV, CInv(MSG_BUDGET_VOTE, (*it2).second.GetHash())));
                        nInvCount++;
                    }
                }
                ++it2;
            }
        }
        ++it1;
    }

    connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_BUDGET_PROP, nInvCount));

    LogPrint(BCLog::MNBUDGET, "CBudgetManager::Sync - sent %d items\n", nInvCount);

    nInvCount = 0;
    std::map<uint256, CFinalizedBudgetBroadcast>::iterator it3 = mapSeenFinalizedBudgets.begin();
    while (it3 != mapSeenFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = FindFinalizedBudget((*it3).first);
        if (pfinalizedBudget && pfinalizedBudget->fValid && (nProp.IsNull() || (*it3).first == nProp)) {
            connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::INV, CInv(MSG_BUDGET_FINALIZED, (*it3).second.GetHash())));
            nInvCount++;

            // send votes
            std::map<uint256, CFinalizedBudgetVote>::iterator it4 = pfinalizedBudget->mapVotes.begin();
            while (it4 != pfinalizedBudget->mapVotes.end()) {
                if ((*it4).second.fValid) {
                    if ((fPartial && !(*it4).second.fSynced) || !fPartial) {
                        connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::INV, CInv(MSG_BUDGET_FINALIZED_VOTE, (*it4).second.GetHash())));
                        nInvCount++;
                    }
                }
                ++it4;
            }
        }
        ++it3;
    }

    connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_BUDGET_FIN, nInvCount));
    LogPrint(BCLog::MNBUDGET, "CBudgetManager::Sync - sent %d items\n", nInvCount);
}

bool CBudgetManager::UpdateProposal(CBudgetVote& vote, CNode* pfrom, CConnman& connman, std::string& strError)
{
    LOCK(cs);

    if (!mapProposals.count(vote.nProposalHash)) {
        if (pfrom) {
            // only ask for missing items after our syncing process is complete --
            //   otherwise we'll think a full sync succeeded when they return a result
            if (!masternodeSync.IsSynced())
                return false;

            LogPrint(BCLog::MNBUDGET, "CBudgetManager::UpdateProposal - Unknown proposal %d, asking for source proposal\n", vote.nProposalHash.ToString());
            mapOrphanMasternodeBudgetVotes[vote.nProposalHash] = vote;

            if (!askedForSourceProposalOrBudget.count(vote.nProposalHash)) {
                const CNetMsgMaker msgMaker(PROTOCOL_VERSION);
                connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::BUDGETVOTESYNC, vote.nProposalHash));
                askedForSourceProposalOrBudget[vote.nProposalHash] = GetTime();
            }
        }

        strError = "Proposal not found!";
        return false;
    }

    return mapProposals[vote.nProposalHash].AddOrUpdateVote(vote, strError);
}

bool CBudgetManager::UpdateFinalizedBudget(CFinalizedBudgetVote& vote, CNode* pfrom, CConnman& connman, std::string& strError)
{
    LOCK(cs);

    if (!mapFinalizedBudgets.count(vote.nBudgetHash)) {
        if (pfrom) {
            // only ask for missing items after our syncing process is complete --
            //   otherwise we'll think a full sync succeeded when they return a result
            if (!masternodeSync.IsSynced())
                return false;

            LogPrint(BCLog::MNBUDGET, "CBudgetManager::UpdateFinalizedBudget - Unknown Finalized Proposal %s, asking for source budget\n", vote.nBudgetHash.ToString());
            mapOrphanFinalizedBudgetVotes[vote.nBudgetHash] = vote;

            if (!askedForSourceProposalOrBudget.count(vote.nBudgetHash)) {
                const CNetMsgMaker msgMaker(PROTOCOL_VERSION);
                connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::BUDGETVOTESYNC, vote.nBudgetHash));
                askedForSourceProposalOrBudget[vote.nBudgetHash] = GetTime();
            }
        }

        strError = "Finalized Budget " + vote.nBudgetHash.ToString() + " not found!";
        return false;
    }
    LogPrint(BCLog::MNBUDGET, "CBudgetManager::UpdateFinalizedBudget - Finalized Proposal %s added\n", vote.nBudgetHash.ToString());
    return mapFinalizedBudgets[vote.nBudgetHash].AddOrUpdateVote(vote, strError);
}

CBudgetProposal::CBudgetProposal()
{
    strProposalName = "unknown";
    nBlockStart = 0;
    nBlockEnd = 0;
    nAmount = 0;
    nTime = 0;
    fValid = true;
}

CBudgetProposal::CBudgetProposal(std::string strProposalNameIn, std::string strURLIn, int nBlockStartIn, int nBlockEndIn, CScript addressIn, CAmount nAmountIn, uint256 nFeeTXHashIn)
{
    strProposalName = strProposalNameIn;
    strURL = strURLIn;
    nBlockStart = nBlockStartIn;
    nBlockEnd = nBlockEndIn;
    address = addressIn;
    nAmount = nAmountIn;
    nFeeTXHash = nFeeTXHashIn;
    fValid = true;
}

CBudgetProposal::CBudgetProposal(const CBudgetProposal& other)
{
    strProposalName = other.strProposalName;
    strURL = other.strURL;
    nBlockStart = other.nBlockStart;
    nBlockEnd = other.nBlockEnd;
    address = other.address;
    nAmount = other.nAmount;
    nTime = other.nTime;
    nFeeTXHash = other.nFeeTXHash;
    mapVotes = other.mapVotes;
    fValid = true;
}

bool CBudgetProposal::IsValid(CBlockIndex* pindex, std::string& strError, bool fCheckCollateral)
{
    if (GetNays() - GetYeas() > mnodeman.CountEnabled(PROTOCOL_VERSION - 1) / 10) {
        strError = "Proposal " + strProposalName + ": Active removal";
        return false;
    }

    if (nBlockStart < 0) {
        strError = "Invalid Proposal";
        return false;
    }

    if (nBlockEnd < nBlockStart) {
        strError = "Proposal " + strProposalName + ": Invalid nBlockEnd (end before start)";
        return false;
    }

    if (nAmount < 10 * COIN) {
        strError = "Proposal " + strProposalName + ": Invalid nAmount";
        return false;
    }

    if (address == CScript()) {
        strError = "Proposal " + strProposalName + ": Invalid Payment Address";
        return false;
    }

    if (fCheckCollateral) {
        int nConf = 0;
        auto chainman = budget.getChainMan();
        if (!IsBudgetCollateralValid(nFeeTXHash, GetHash(), strError, nTime, nConf, chainman->ActiveChainstate())) {
            strError = "Proposal " + strProposalName + ": Invalid collateral";
            return false;
        }
    }

    /*
        TODO: There might be an issue with multisig in the coinbase on mainnet, we will add support for it in a future release.
    */
    if (address.IsPayToScriptHash()) {
        strError = "Proposal " + strProposalName + ": Multisig is not currently supported.";
        return false;
    }

    // if proposal doesn't gain traction within 2 weeks, remove it
    //  nTime not being saved correctly
    //  -- TODO: We should keep track of the last time the proposal was valid, if it's invalid for 2 weeks, erase it
    //  if(nTime + (60*60*24*2) < TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime())) {
    //      if(GetYeas()-GetNays() < (mnodeman.CountEnabled(PROTOCOL_VERSION-1)/10)) {
    //          strError = "Not enough support";
    //          return false;
    //      }
    //  }

    // can only pay out 10% of the possible coins (min value of coins)
    if (nAmount > budget.GetTotalBudget(nBlockStart)) {
        strError = "Proposal " + strProposalName + ": Payment more than max";
        return false;
    }

    if (!pindex) {
        strError = "Proposal " + strProposalName + ": Tip is NULL";
        return true;
    }

    // Calculate maximum block this proposal will be valid, which is start of proposal + (number of payments * cycle)
    int nProposalEnd = GetBlockStart() + (GetBudgetPaymentCycleBlocks() * GetTotalPaymentCount());

    // if (GetBlockEnd() < pindex->nHeight - GetBudgetPaymentCycleBlocks() / 2) {
    if (nProposalEnd < pindex->nHeight) {
        strError = "Proposal " + strProposalName + ": Invalid nBlockEnd (" + std::to_string(nProposalEnd) + ") < current height (" + std::to_string(pindex->nHeight) + ")";
        return false;
    }

    return true;
}

bool CBudgetProposal::IsPassing(const CBlockIndex* pindexPrev, int nBlockStartBudget, int nBlockEndBudget, int mnCount)
{
    if (!fValid)
        return false;

    if (!pindexPrev)
        return false;

    if (this->nBlockStart > nBlockStartBudget)
        return false;

    if (this->nBlockEnd < nBlockEndBudget)
        return false;

    if (GetYeas() - GetNays() <= mnCount / 10)
        return false;

    if (!IsEstablished())
        return false;

    return true;
}

bool CBudgetProposal::AddOrUpdateVote(CBudgetVote& vote, std::string& strError)
{
    std::string strAction = "New vote inserted:";
    LOCK(cs);

    uint256 hash = vote.vin.prevout.hash;

    if (mapVotes.count(hash)) {
        if (mapVotes[hash].nTime > vote.nTime) {
            strError = strprintf("new vote older than existing vote - %s\n", vote.GetHash().ToString());
            LogPrint(BCLog::MNBUDGET, "CBudgetProposal::AddOrUpdateVote - %s\n", strError);
            return false;
        }
        if (vote.nTime - mapVotes[hash].nTime < BUDGET_VOTE_UPDATE_MIN) {
            strError = strprintf("time between votes is too soon - %s - %lli sec < %lli sec\n", vote.GetHash().ToString(), vote.nTime - mapVotes[hash].nTime, BUDGET_VOTE_UPDATE_MIN);
            LogPrint(BCLog::MNBUDGET, "CBudgetProposal::AddOrUpdateVote - %s\n", strError);
            return false;
        }
        strAction = "Existing vote updated:";
    }

    if (vote.nTime > GetTime() + (60 * 60)) {
        strError = strprintf("new vote is too far ahead of current time - %s - nTime %lli - Max Time %lli\n", vote.GetHash().ToString(), vote.nTime, GetTime() + (60 * 60));
        LogPrint(BCLog::MNBUDGET, "CBudgetProposal::AddOrUpdateVote - %s\n", strError);
        return false;
    }

    mapVotes[hash] = vote;
    LogPrint(BCLog::MNBUDGET, "CBudgetProposal::AddOrUpdateVote - %s %s\n", strAction, vote.GetHash().ToString());

    return true;
}

// If masternode voted for a proposal, but is now invalid -- remove the vote
void CBudgetProposal::CleanAndRemove(bool fSignatureCheck)
{
    std::map<uint256, CBudgetVote>::iterator it = mapVotes.begin();

    while (it != mapVotes.end()) {
        (*it).second.fValid = (*it).second.SignatureValid(fSignatureCheck);
        ++it;
    }
}

double CBudgetProposal::GetRatio()
{
    int yeas = 0;
    int nays = 0;

    std::map<uint256, CBudgetVote>::iterator it = mapVotes.begin();

    while (it != mapVotes.end()) {
        if ((*it).second.nVote == VOTE_YES)
            yeas++;
        if ((*it).second.nVote == VOTE_NO)
            nays++;
        ++it;
    }

    if (yeas + nays == 0)
        return 0.0f;

    return ((double)(yeas) / (double)(yeas + nays));
}

int CBudgetProposal::GetYeas() const
{
    int ret = 0;

    std::map<uint256, CBudgetVote>::const_iterator it = mapVotes.begin();
    while (it != mapVotes.end()) {
        if ((*it).second.nVote == VOTE_YES && (*it).second.fValid)
            ret++;
        ++it;
    }

    return ret;
}

int CBudgetProposal::GetNays() const
{
    int ret = 0;

    std::map<uint256, CBudgetVote>::const_iterator it = mapVotes.begin();
    while (it != mapVotes.end()) {
        if ((*it).second.nVote == VOTE_NO && (*it).second.fValid)
            ret++;
        ++it;
    }

    return ret;
}

int CBudgetProposal::GetAbstains() const
{
    int ret = 0;

    std::map<uint256, CBudgetVote>::const_iterator it = mapVotes.begin();
    while (it != mapVotes.end()) {
        if ((*it).second.nVote == VOTE_ABSTAIN && (*it).second.fValid)
            ret++;
        ++it;
    }

    return ret;
}

int CBudgetProposal::GetBlockStartCycle()
{
    // end block is half way through the next cycle (so the proposal will be removed much after the payment is sent)

    return nBlockStart - nBlockStart % GetBudgetPaymentCycleBlocks();
}

int CBudgetProposal::GetBlockCurrentCycle(CBlockIndex* pindex)
{
    if (!pindex)
        return -1;

    if (pindex->nHeight >= GetBlockEndCycle())
        return -1;

    return pindex->nHeight - pindex->nHeight % GetBudgetPaymentCycleBlocks();
}

int CBudgetProposal::GetBlockEndCycle()
{
    // Right now single payment proposals have nBlockEnd have a cycle too early!
    // switch back if it break something else
    // end block is half way through the next cycle (so the proposal will be removed much after the payment is sent)
    // return nBlockEnd - GetBudgetPaymentCycleBlocks() / 2;

    // End block is half way through the next cycle (so the proposal will be removed much after the payment is sent)
    return nBlockEnd;
}

int CBudgetProposal::GetTotalPaymentCount()
{
    return (GetBlockEndCycle() - GetBlockStartCycle()) / GetBudgetPaymentCycleBlocks();
}

int CBudgetProposal::GetRemainingPaymentCount(CBlockIndex* pindex)
{
    // If this budget starts in the future, this value will be wrong
    int nPayments = (GetBlockEndCycle() - GetBlockCurrentCycle(pindex)) / GetBudgetPaymentCycleBlocks() - 1;
    // Take the lowest value
    return std::min(nPayments, GetTotalPaymentCount());
}

CBudgetProposalBroadcast::CBudgetProposalBroadcast(std::string strProposalNameIn, std::string strURLIn, int nPaymentCount, CScript addressIn, CAmount nAmountIn, int nBlockStartIn, uint256 nFeeTXHashIn)
{
    strProposalName = strProposalNameIn;
    strURL = strURLIn;

    nBlockStart = nBlockStartIn;

    int nCycleStart = nBlockStart - nBlockStart % GetBudgetPaymentCycleBlocks();

    // Right now single payment proposals have nBlockEnd have a cycle too early!
    // switch back if it break something else
    // calculate the end of the cycle for this vote, add half a cycle (vote will be deleted after that block)
    // nBlockEnd = nCycleStart + GetBudgetPaymentCycleBlocks() * nPaymentCount + GetBudgetPaymentCycleBlocks() / 2;

    // Calculate the end of the cycle for this vote, vote will be deleted after next cycle
    nBlockEnd = nCycleStart + (GetBudgetPaymentCycleBlocks() + 1) * nPaymentCount;

    address = addressIn;
    nAmount = nAmountIn;

    nFeeTXHash = nFeeTXHashIn;
}

void CBudgetProposalBroadcast::Relay(CConnman* connman)
{
    CInv inv(MSG_BUDGET_PROPOSAL, GetHash());
    connman->RelayInv(inv);
}

CBudgetVote::CBudgetVote()
{
    vin = CTxIn();
    nProposalHash = uint256();
    nVote = VOTE_ABSTAIN;
    nTime = 0;
    fValid = true;
    fSynced = false;
}

CBudgetVote::CBudgetVote(CTxIn vinIn, uint256 nProposalHashIn, int nVoteIn)
{
    vin = vinIn;
    nProposalHash = nProposalHashIn;
    nVote = nVoteIn;
    nTime = TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime());
    fValid = true;
    fSynced = false;
}

void CBudgetVote::Relay(CConnman* connman)
{
    CInv inv(MSG_BUDGET_VOTE, GetHash());
    connman->RelayInv(inv);
}

bool CBudgetVote::Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode)
{
    // Choose coins to use
    CPubKey pubKeyCollateralAddress;
    CKey keyCollateralAddress;

    std::string errorMessage;
    std::string strMessage = vin.prevout.ToStringShort() + nProposalHash.ToString() + std::to_string(nVote) + std::to_string(nTime);

    if (!legacySigner.SignMessage(strMessage, vchSig, keyMasternode)) {
        LogPrint(BCLog::MNBUDGET, "CBudgetVote::Sign - Error upon calling SignMessage");
        return false;
    }

    if (!legacySigner.VerifyMessage(pubKeyMasternode, vchSig, strMessage, errorMessage)) {
        LogPrint(BCLog::MNBUDGET, "CBudgetVote::Sign - Error upon calling VerifyMessage");
        return false;
    }

    return true;
}

bool CBudgetVote::SignatureValid(bool fSignatureCheck)
{
    std::string errorMessage;
    std::string strMessage = vin.prevout.ToStringShort() + nProposalHash.ToString() + std::to_string(nVote) + std::to_string(nTime);

    CMasternode* pmn = mnodeman.Find(vin);

    if (!pmn) {
        LogPrint(BCLog::MNBUDGET, "CBudgetVote::SignatureValid() - Unknown Masternode - %s\n", vin.prevout.hash.ToString());
        return false;
    }

    if (!fSignatureCheck)
        return true;

    if (!legacySigner.VerifyMessage(pmn->pubKeyMasternode, vchSig, strMessage, errorMessage)) {
        LogPrint(BCLog::MNBUDGET, "CBudgetVote::SignatureValid() - Verify message failed\n");
        return false;
    }

    return true;
}

CFinalizedBudget::CFinalizedBudget()
{
    strBudgetName = "";
    nBlockStart = 0;
    vecBudgetPayments.clear();
    mapVotes.clear();
    nFeeTXHash = uint256();
    nTime = 0;
    fValid = true;
    fAutoChecked = false;
}

CFinalizedBudget::CFinalizedBudget(const CFinalizedBudget& other)
{
    strBudgetName = other.strBudgetName;
    nBlockStart = other.nBlockStart;
    vecBudgetPayments = other.vecBudgetPayments;
    mapVotes = other.mapVotes;
    nFeeTXHash = other.nFeeTXHash;
    nTime = other.nTime;
    fValid = true;
    fAutoChecked = false;
}

bool CFinalizedBudget::AddOrUpdateVote(CFinalizedBudgetVote& vote, std::string& strError)
{
    LOCK(cs);

    uint256 hash = vote.vin.prevout.hash;
    std::string strAction = "New vote inserted:";

    if (mapVotes.count(hash)) {
        if (mapVotes[hash].nTime > vote.nTime) {
            strError = strprintf("new vote older than existing vote - %s\n", vote.GetHash().ToString());
            LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AddOrUpdateVote - %s\n", strError);
            return false;
        }
        if (vote.nTime - mapVotes[hash].nTime < BUDGET_VOTE_UPDATE_MIN) {
            strError = strprintf("time between votes is too soon - %s - %lli sec < %lli sec\n", vote.GetHash().ToString(), vote.nTime - mapVotes[hash].nTime, BUDGET_VOTE_UPDATE_MIN);
            LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AddOrUpdateVote - %s\n", strError);
            return false;
        }
        strAction = "Existing vote updated:";
    }

    if (vote.nTime > GetTime() + (60 * 60)) {
        strError = strprintf("new vote is too far ahead of current time - %s - nTime %lli - Max Time %lli\n", vote.GetHash().ToString(), vote.nTime, GetTime() + (60 * 60));
        LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AddOrUpdateVote - %s\n", strError);
        return false;
    }

    mapVotes[hash] = vote;
    LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AddOrUpdateVote - %s %s\n", strAction, vote.GetHash().ToString());
    return true;
}

// Sort budget proposals by hash
struct sortProposalsByHash {
    bool operator()(const CBudgetProposal* left, const CBudgetProposal* right)
    {
        return (left->GetHash() < right->GetHash());
    }
};

// Sort budget payments by hash
struct sortPaymentsByHash {
    bool operator()(const CTxBudgetPayment& left, const CTxBudgetPayment& right)
    {
        return (left.nProposalHash < right.nProposalHash);
    }
};

// Check finalized budget and vote on it if correct. Masternodes only
void CFinalizedBudget::CheckAndVote(CBlockIndex* pindex, CConnman* connman)
{
    LOCK(cs);

    if (!pindex)
        return;

    LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AutoCheck - %lli - %d\n", pindex->nHeight, fAutoChecked);

    if (!fMasterNode || fAutoChecked) {
        LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AutoCheck fMasterNode=%d fAutoChecked=%d\n", fMasterNode, fAutoChecked);
        return;
    }

    // Do this 1 in 4 blocks -- spread out the voting activity
    // -- this function is only called every fourteenth block, so this is really 1 in 56 blocks
    if (rand() % 4 != 0) {
        LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AutoCheck - waiting\n");
        return;
    }

    fAutoChecked = true; // we only need to check this once

    if (strBudgetMode == "auto") // only vote for exact matches
    {
        std::vector<CBudgetProposal*> vBudgetProposals = budget.GetBudget(pindex);

        // We have to resort the proposals by hash (they are sorted by votes here) and sort the payments
        // by hash (they are not sorted at all) to make the following tests deterministic
        // We're working on copies to avoid any side-effects by the possibly changed sorting order

        // Sort copy of proposals by hash
        std::vector<CBudgetProposal*> vBudgetProposalsSortedByHash(vBudgetProposals);
        std::sort(vBudgetProposalsSortedByHash.begin(), vBudgetProposalsSortedByHash.end(), sortProposalsByHash());

        // Sort copy payments by hash
        std::vector<CTxBudgetPayment> vecBudgetPaymentsSortedByHash(vecBudgetPayments);
        std::sort(vecBudgetPaymentsSortedByHash.begin(), vecBudgetPaymentsSortedByHash.end(), sortPaymentsByHash());

        for (unsigned int i = 0; i < vecBudgetPaymentsSortedByHash.size(); i++) {
            LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AutoCheck Budget-Payments - nProp %d %s\n", i, vecBudgetPaymentsSortedByHash[i].nProposalHash.ToString());
            LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AutoCheck Budget-Payments - Payee %d %s\n", i, vecBudgetPaymentsSortedByHash[i].payee.ToString());
            LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AutoCheck Budget-Payments - nAmount %d %lli\n", i, vecBudgetPaymentsSortedByHash[i].nAmount);
        }

        for (unsigned int i = 0; i < vBudgetProposalsSortedByHash.size(); i++) {
            LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AutoCheck Budget-Proposals - nProp %d %s\n", i, vBudgetProposalsSortedByHash[i]->GetHash().ToString());
            LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AutoCheck Budget-Proposals - Payee %d %s\n", i, vBudgetProposalsSortedByHash[i]->GetPayee().ToString());
            LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AutoCheck Budget-Proposals - nAmount %d %lli\n", i, vBudgetProposalsSortedByHash[i]->GetAmount());
        }

        if (vBudgetProposalsSortedByHash.size() == 0) {
            LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AutoCheck - No Budget-Proposals found, aborting\n");
            return;
        }

        if (vBudgetProposalsSortedByHash.size() != vecBudgetPaymentsSortedByHash.size()) {
            LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AutoCheck - Budget-Proposal length (%ld) doesn't match Budget-Payment length (%ld).\n",
                vBudgetProposalsSortedByHash.size(), vecBudgetPaymentsSortedByHash.size());
            return;
        }

        for (unsigned int i = 0; i < vecBudgetPaymentsSortedByHash.size(); i++) {
            if (i > vBudgetProposalsSortedByHash.size() - 1) {
                LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AutoCheck - Proposal size mismatch, i=%d > (vBudgetProposals.size() - 1)=%d\n", i, vBudgetProposalsSortedByHash.size() - 1);
                return;
            }

            if (vecBudgetPaymentsSortedByHash[i].nProposalHash != vBudgetProposalsSortedByHash[i]->GetHash()) {
                LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AutoCheck - item #%d doesn't match %s %s\n", i, vecBudgetPaymentsSortedByHash[i].nProposalHash.ToString(), vBudgetProposalsSortedByHash[i]->GetHash().ToString());
                return;
            }

            // if(vecBudgetPayments[i].payee != vBudgetProposals[i]->GetPayee()){ -- triggered with false positive
            if (vecBudgetPaymentsSortedByHash[i].payee.ToString() != vBudgetProposalsSortedByHash[i]->GetPayee().ToString()) {
                LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AutoCheck - item #%d payee doesn't match %s %s\n", i, vecBudgetPaymentsSortedByHash[i].payee.ToString(), vBudgetProposalsSortedByHash[i]->GetPayee().ToString());
                return;
            }

            if (vecBudgetPaymentsSortedByHash[i].nAmount != vBudgetProposalsSortedByHash[i]->GetAmount()) {
                LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AutoCheck - item #%d payee doesn't match %lli %lli\n", i, vecBudgetPaymentsSortedByHash[i].nAmount, vBudgetProposalsSortedByHash[i]->GetAmount());
                return;
            }
        }

        LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::AutoCheck - Finalized Budget Matches! Submitting Vote.\n");
        SubmitVote(connman);
    }
}

// Remove votes from masternodes which are not valid/existent anymore
void CFinalizedBudget::CleanAndRemove(bool fSignatureCheck)
{
    std::map<uint256, CFinalizedBudgetVote>::iterator it = mapVotes.begin();

    while (it != mapVotes.end()) {
        (*it).second.fValid = (*it).second.SignatureValid(fSignatureCheck);
        ++it;
    }
}

CAmount CFinalizedBudget::GetTotalPayout()
{
    CAmount ret = 0;

    for (unsigned int i = 0; i < vecBudgetPayments.size(); i++) {
        ret += vecBudgetPayments[i].nAmount;
    }

    return ret;
}

std::string CFinalizedBudget::GetProposals()
{
    LOCK(cs);
    std::string ret = "";

    for (CTxBudgetPayment& budgetPayment : vecBudgetPayments) {
        CBudgetProposal* pbudgetProposal = budget.FindProposal(budgetPayment.nProposalHash);

        std::string token = budgetPayment.nProposalHash.ToString();

        if (pbudgetProposal)
            token = pbudgetProposal->GetName();
        if (ret == "") {
            ret = token;
        } else {
            ret += "," + token;
        }
    }
    return ret;
}

std::string CFinalizedBudget::GetStatus()
{
    std::string retBadHashes = "";
    std::string retBadPayeeOrAmount = "";

    for (int nBlockHeight = GetBlockStart(); nBlockHeight <= GetBlockEnd(); nBlockHeight++) {
        CTxBudgetPayment budgetPayment;
        if (!GetBudgetPaymentByBlock(nBlockHeight, budgetPayment)) {
            LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::GetStatus - Couldn't find budget payment for block %lld\n", nBlockHeight);
            continue;
        }

        CBudgetProposal* pbudgetProposal = budget.FindProposal(budgetPayment.nProposalHash);
        if (!pbudgetProposal) {
            if (retBadHashes == "") {
                retBadHashes = "Unknown proposal hash! Check this proposal before voting: " + budgetPayment.nProposalHash.ToString();
            } else {
                retBadHashes += "," + budgetPayment.nProposalHash.ToString();
            }
        } else {
            if (pbudgetProposal->GetPayee() != budgetPayment.payee || pbudgetProposal->GetAmount() != budgetPayment.nAmount) {
                if (retBadPayeeOrAmount == "") {
                    retBadPayeeOrAmount = "Budget payee/nAmount doesn't match our proposal! " + budgetPayment.nProposalHash.ToString();
                } else {
                    retBadPayeeOrAmount += "," + budgetPayment.nProposalHash.ToString();
                }
            }
        }
    }

    if (retBadHashes == "" && retBadPayeeOrAmount == "")
        return "OK";

    return retBadHashes + retBadPayeeOrAmount;
}

bool CFinalizedBudget::IsValid(CBlockIndex* pindex, std::string& strError, bool fCheckCollateral)
{
    // All(!) finalized budgets have the name "main", so get some additional information about them
    std::string strProposals = GetProposals();

    // Must be the correct block for payment to happen (once a month)
    if (nBlockStart % GetBudgetPaymentCycleBlocks() != 0) {
        strError = "Invalid BlockStart";
        return false;
    }

    // The following 2 checks check the same (basically if vecBudgetPayments.size() > 100)
    if (GetBlockEnd() - nBlockStart > 100) {
        strError = "Invalid BlockEnd";
        return false;
    }
    if ((int)vecBudgetPayments.size() > 100) {
        strError = "Invalid budget payments count (too many)";
        return false;
    }
    if (strBudgetName == "") {
        strError = "Invalid Budget Name";
        return false;
    }
    if (nBlockStart == 0) {
        strError = "Budget " + strBudgetName + " (" + strProposals + ") Invalid BlockStart == 0";
        return false;
    }
    if (nFeeTXHash.IsNull()) {
        strError = "Budget " + strBudgetName + " (" + strProposals + ") Invalid FeeTx == 0";
        return false;
    }

    // Can only pay out 10% of the possible coins (min value of coins)
    if (GetTotalPayout() > budget.GetTotalBudget(nBlockStart)) {
        strError = "Budget " + strBudgetName + " (" + strProposals + ") Invalid Payout (more than max)";
        return false;
    }

    std::string strError2 = "";
    if (fCheckCollateral) {
        int nConf = 0;
        auto chainman = budget.getChainMan();
        if (!IsBudgetCollateralValid(nFeeTXHash, GetHash(), strError2, nTime, nConf, chainman->ActiveChainstate(), true)) {
            {
                strError = "Budget " + strBudgetName + " (" + strProposals + ") Invalid Collateral : " + strError2;
                return false;
            }
        }
    }

    // Remove obsolete finalized budgets after some time
    if (!pindex)
        return true;

    // Get start of current budget-cycle
    int nCurrentHeight = pindex->nHeight;
    int nBlockStart = nCurrentHeight - nCurrentHeight % GetBudgetPaymentCycleBlocks() + GetBudgetPaymentCycleBlocks();

    // Remove budgets where the last payment (from max. 100) ends before 2 budget-cycles before the current one
    int nMaxAge = nBlockStart - (2 * GetBudgetPaymentCycleBlocks());

    if (GetBlockEnd() < nMaxAge) {
        strError = strprintf("Budget " + strBudgetName + " (" + strProposals + ") (ends at block %ld) too old and obsolete", GetBlockEnd());
        return false;
    }

    return true;
}

bool CFinalizedBudget::IsPaidAlready(uint256 nProposalHash, int nBlockHeight)
{
    // Remove budget-payments from former/future payment cycles
    std::map<uint256, int>::iterator it = mapPayment_History.begin();
    int nPaidBlockHeight = 0;
    uint256 nOldProposalHash;

    for (it = mapPayment_History.begin(); it != mapPayment_History.end(); /* No incrementation needed */) {
        nPaidBlockHeight = (*it).second;
        if ((nPaidBlockHeight < GetBlockStart()) || (nPaidBlockHeight > GetBlockEnd())) {
            nOldProposalHash = (*it).first;
            LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::IsPaidAlready - Budget Proposal %s, Block %d from old cycle deleted\n",
                nOldProposalHash.ToString(), nPaidBlockHeight);
            mapPayment_History.erase(it++);
        } else {
            ++it;
        }
    }

    // Now that we only have payments from the current payment cycle check if this budget was paid already
    if (mapPayment_History.count(nProposalHash) == 0) {
        // New proposal payment, insert into map for checks with later blocks from this cycle
        mapPayment_History.insert(std::pair<uint256, int>(nProposalHash, nBlockHeight));
        LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::IsPaidAlready - Budget Proposal %s, Block %d added to payment history\n",
            nProposalHash.ToString(), nBlockHeight);
        return false;
    }
    // This budget was paid already -> reject transaction so it gets paid to a masternode instead
    return true;
}

TrxValidationStatus CFinalizedBudget::IsTransactionValid(const CTransactionRef& txNew, int nBlockHeight)
{
    TrxValidationStatus transactionStatus = TrxValidationStatus::InValid;
    int nCurrentBudgetPayment = nBlockHeight - GetBlockStart();
    if (nCurrentBudgetPayment < 0) {
        LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::IsTransactionValid - Invalid block - height: %d start: %d\n", nBlockHeight, GetBlockStart());
        return TrxValidationStatus::InValid;
    }

    if (nCurrentBudgetPayment > (int)vecBudgetPayments.size() - 1) {
        LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::IsTransactionValid - Invalid last block - current budget payment: %d of %d\n", nCurrentBudgetPayment + 1, (int)vecBudgetPayments.size());
        return TrxValidationStatus::InValid;
    }

    bool paid = false;

    for (CTxOut out : txNew->vout) {
        LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::IsTransactionValid - nCurrentBudgetPayment=%d, payee=%s == out.scriptPubKey=%s, amount=%ld == out.nValue=%ld\n",
            nCurrentBudgetPayment, vecBudgetPayments[nCurrentBudgetPayment].payee.ToString(), out.scriptPubKey.ToString(),
            vecBudgetPayments[nCurrentBudgetPayment].nAmount, out.nValue);

        if (vecBudgetPayments[nCurrentBudgetPayment].payee == out.scriptPubKey && vecBudgetPayments[nCurrentBudgetPayment].nAmount == out.nValue) {
            // Check if this proposal was paid already. If so, pay a masternode instead
            paid = IsPaidAlready(vecBudgetPayments[nCurrentBudgetPayment].nProposalHash, nBlockHeight);
            if (paid) {
                LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::IsTransactionValid - Double Budget Payment of %d for proposal %d detected. Paying a masternode instead.\n",
                    vecBudgetPayments[nCurrentBudgetPayment].nAmount, vecBudgetPayments[nCurrentBudgetPayment].nProposalHash.Get32());
                // No matter what we've found before, stop all checks here. In future releases there might be more than one budget payment
                // per block, so even if the first one was not paid yet this one disables all budget payments for this block.
                transactionStatus = TrxValidationStatus::DoublePayment;
                break;
            } else {
                transactionStatus = TrxValidationStatus::Valid;
                LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::IsTransactionValid - Found valid Budget Payment of %d for proposal %d\n",
                    vecBudgetPayments[nCurrentBudgetPayment].nAmount, vecBudgetPayments[nCurrentBudgetPayment].nProposalHash.Get32());
            }
        }
    }

    if (transactionStatus == TrxValidationStatus::InValid) {
        CTxDestination address1;
        ExtractDestination(vecBudgetPayments[nCurrentBudgetPayment].payee, address1);
        CTxDestination address2(address1);

        LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::IsTransactionValid - Missing required payment - %s: %d c: %d\n",
            EncodeDestination(address2), vecBudgetPayments[nCurrentBudgetPayment].nAmount, nCurrentBudgetPayment);
    }

    return transactionStatus;
}

void CFinalizedBudget::SubmitVote(CConnman* connman)
{
    CPubKey pubKeyMasternode;
    CKey keyMasternode;
    std::string errorMessage;

    if (!legacySigner.SetKey(strMasterNodePrivKey, keyMasternode, pubKeyMasternode)) {
        LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::SubmitVote - Error upon calling SetKey\n");
        return;
    }

    CFinalizedBudgetVote vote(activeMasternode.vin, GetHash());
    if (!vote.Sign(keyMasternode, pubKeyMasternode)) {
        LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::SubmitVote - Failure to sign.");
        return;
    }

    std::string strError;
    if (budget.UpdateFinalizedBudget(vote, NULL, *connman, strError)) {
        LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::SubmitVote  - new finalized budget vote - %s\n", vote.GetHash().ToString());

        budget.mapSeenFinalizedBudgetVotes.insert(std::make_pair(vote.GetHash(), vote));
        vote.Relay(connman);
    } else {
        LogPrint(BCLog::MNBUDGET, "CFinalizedBudget::SubmitVote : Error submitting vote - %s\n", strError);
    }
}

CFinalizedBudgetBroadcast::CFinalizedBudgetBroadcast()
{
    strBudgetName = "";
    nBlockStart = 0;
    vecBudgetPayments.clear();
    mapVotes.clear();
    vchSig.clear();
    nFeeTXHash = uint256();
}

CFinalizedBudgetBroadcast::CFinalizedBudgetBroadcast(const CFinalizedBudget& other)
{
    strBudgetName = other.strBudgetName;
    nBlockStart = other.nBlockStart;
    for (CTxBudgetPayment out : other.vecBudgetPayments)
        vecBudgetPayments.push_back(out);
    mapVotes = other.mapVotes;
    nFeeTXHash = other.nFeeTXHash;
}

CFinalizedBudgetBroadcast::CFinalizedBudgetBroadcast(std::string strBudgetNameIn, int nBlockStartIn, std::vector<CTxBudgetPayment> vecBudgetPaymentsIn, uint256 nFeeTXHashIn)
{
    strBudgetName = strBudgetNameIn;
    nBlockStart = nBlockStartIn;
    for (CTxBudgetPayment out : vecBudgetPaymentsIn)
        vecBudgetPayments.push_back(out);
    mapVotes.clear();
    nFeeTXHash = nFeeTXHashIn;
}

void CFinalizedBudgetBroadcast::Relay(CConnman* connman)
{
    CInv inv(MSG_BUDGET_FINALIZED, GetHash());
    connman->RelayInv(inv);
}

CFinalizedBudgetVote::CFinalizedBudgetVote()
{
    vin = CTxIn();
    nBudgetHash = uint256();
    nTime = 0;
    vchSig.clear();
    fValid = true;
    fSynced = false;
}

CFinalizedBudgetVote::CFinalizedBudgetVote(CTxIn vinIn, uint256 nBudgetHashIn)
{
    vin = vinIn;
    nBudgetHash = nBudgetHashIn;
    nTime = TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime());
    vchSig.clear();
    fValid = true;
    fSynced = false;
}

void CFinalizedBudgetVote::Relay(CConnman* connman)
{
    CInv inv(MSG_BUDGET_FINALIZED_VOTE, GetHash());
    connman->RelayInv(inv);
}

bool CFinalizedBudgetVote::Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode)
{
    // Choose coins to use
    CPubKey pubKeyCollateralAddress;
    CKey keyCollateralAddress;

    std::string errorMessage;
    std::string strMessage = vin.prevout.ToStringShort() + nBudgetHash.ToString() + std::to_string(nTime);

    if (!legacySigner.SignMessage(strMessage, vchSig, keyMasternode)) {
        LogPrint(BCLog::MNBUDGET, "CFinalizedBudgetVote::Sign - Error upon calling SignMessage");
        return false;
    }

    if (!legacySigner.VerifyMessage(pubKeyMasternode, vchSig, strMessage, errorMessage)) {
        LogPrint(BCLog::MNBUDGET, "CFinalizedBudgetVote::Sign - Error upon calling VerifyMessage");
        return false;
    }

    return true;
}

bool CFinalizedBudgetVote::SignatureValid(bool fSignatureCheck)
{
    std::string errorMessage;

    std::string strMessage = vin.prevout.ToStringShort() + nBudgetHash.ToString() + std::to_string(nTime);

    CMasternode* pmn = mnodeman.Find(vin);

    if (!pmn) {
        LogPrint(BCLog::MNBUDGET, "CFinalizedBudgetVote::SignatureValid() - Unknown Masternode %s\n", strMessage);
        return false;
    }

    if (!fSignatureCheck)
        return true;

    if (!legacySigner.VerifyMessage(pmn->pubKeyMasternode, vchSig, strMessage, errorMessage)) {
        LogPrint(BCLog::MNBUDGET, "CFinalizedBudgetVote::SignatureValid() - Verify message failed %s %s\n", strMessage, errorMessage);
        return false;
    }

    return true;
}

std::string CBudgetManager::ToString() const
{
    std::ostringstream info;

    info << "Proposals: " << (int)mapProposals.size() << ", Budgets: " << (int)mapFinalizedBudgets.size() << ", Seen Budgets: " << (int)mapSeenMasternodeBudgetProposals.size() << ", Seen Budget Votes: " << (int)mapSeenMasternodeBudgetVotes.size() << ", Seen Final Budgets: " << (int)mapSeenFinalizedBudgets.size() << ", Seen Final Budget Votes: " << (int)mapSeenFinalizedBudgetVotes.size();

    return info.str();
}
