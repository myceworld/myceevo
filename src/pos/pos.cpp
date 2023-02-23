// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/pos.h>

#include <index/disktxpos.h>
#include <index/txindex.h>
#include <node/transaction.h>
#include <pos/stakeinput.h>
#include <timedata.h>
#include <util/system.h>
#include <validation.h>

static bool GetLastStakeModifier(const CBlockIndex* pindex, uint64_t& nStakeModifier, int64_t& nModifierTime)
{
    if (!pindex) {
        LogPrint(BCLog::POS, "%s: null pindex", __func__);
        return false;
    }
    while (pindex && pindex->pprev && !pindex->GeneratedStakeModifier()) {
        pindex = pindex->pprev;
    }
    if (!pindex->GeneratedStakeModifier()) {
        LogPrint(BCLog::POS, "%s: no generation at genesis block", __func__);
        return false;
    }
    nStakeModifier = pindex->nStakeModifier;
    nModifierTime = pindex->GetBlockTime();
    return true;
}

static int64_t GetStakeModifierSelectionIntervalSection(int nSection)
{
    assert(nSection >= 0 && nSection < 64);
    return (int64_t) (Params().GetConsensus().nModifierInterval * 63 / (63 + ((63 - nSection) * (MODIFIER_INTERVAL_RATIO - 1))));
}

static int64_t GetStakeModifierSelectionInterval()
{
    int64_t nSelectionInterval = 0;
    for (int nSection = 0; nSection < 64; nSection++)
        nSelectionInterval += GetStakeModifierSelectionIntervalSection(nSection);
    return nSelectionInterval;
}

static bool SelectBlockFromCandidates(
    std::vector<std::pair<int64_t, uint256> >& vSortedByTimestamp,
    std::map<uint256, const CBlockIndex*>& mapSelectedBlocks,
    int64_t nSelectionIntervalStop, uint64_t nStakeModifierPrev,
    const CBlockIndex** pindexSelected,
    const Consensus::Params& params,
    Chainstate& chainstate)
{
    bool fModifierV2 = false;
    bool fFirstRun = true;
    bool fSelected = false;
    arith_uint256 hashBest = arith_uint256();
    *pindexSelected = nullptr;
    for (const auto& item : vSortedByTimestamp) {
        const CBlockIndex* pindex = chainstate.m_blockman.LookupBlockIndex(item.second);
        if (!pindex) {
            LogPrint(BCLog::POS, "%s: failed to find block index for candidate block %s\n", __func__, item.second.ToString());
            return false;
        }
        if (fSelected && pindex->GetBlockTime() > nSelectionIntervalStop) {
            break;
        }
        if (fFirstRun) {
            fModifierV2 = pindex->nHeight >= params.nModifierUpgrade;
            fFirstRun = false;
        }
        if (mapSelectedBlocks.count(pindex->GetBlockHash()) > 0) {
            continue;
        }
        uint256 hashProof;
        if (fModifierV2)
            hashProof = pindex->GetBlockHash();
        else
            hashProof = pindex->IsProofOfStake() ? uint256() : pindex->GetBlockHash();

        // compute the selection hash by hashing its proof-hash and the
        // previous proof-of-stake modifier
        CDataStream ss(SER_GETHASH, 0);
        ss << hashProof << nStakeModifierPrev;
        arith_uint256 hashSelection = UintToArith256(Hash(ss));

        // the selection hash is divided by 2**32 so that proof-of-stake block
        // is always favored over proof-of-work block. this is to preserve
        // the energy efficiency property
        if (pindex->IsProofOfStake()) {
            hashSelection >>= 32;
        }

        if (fSelected && hashSelection < hashBest) {
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*)pindex;
        } else if (!fSelected) {
            fSelected = true;
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*)pindex;
        }
    }

    if (gArgs.GetBoolArg("-printstakemodifier", DEFAULT_PRINTSTAKEMODIFIER)) {
        LogPrint(BCLog::POS, "%s: selection hash=%s\n", __func__, hashBest.ToString());
    }

    return fSelected;
}

bool ComputeNextStakeModifier(Chainstate& chainstate, const CBlockIndex* pindexCurrent, uint64_t& nStakeModifier, bool& fGeneratedStakeModifier)
{
    const Consensus::Params& params = Params().GetConsensus();
    const CBlockIndex* pindexPrev = pindexCurrent->pprev;

    nStakeModifier = 0;
    fGeneratedStakeModifier = false;

    if (!pindexPrev) {
        fGeneratedStakeModifier = true;
        return true;
    } else if (pindexPrev->nHeight == 0) {
        fGeneratedStakeModifier = true;
        nStakeModifier = 0x7374616b656d6f64; // stakemod
        return true;
    }

    // First find current stake modifier and its generation block time
    // if it's not old enough, return the same stake modifier
    int64_t nModifierTime = 0;
    if (!GetLastStakeModifier(pindexPrev, nStakeModifier, nModifierTime)) {
        LogPrint(BCLog::POS, "%s: unable to get last modifier", __func__);
        return false;
    }

    // LogPrintf("ComputeNextStakeModifier: prev modifier=0x%016x time=%s epoch=%u\n", nStakeModifier, FormatISO8601DateTime(nModifierTime), (unsigned int)nModifierTime);

    if (nModifierTime / params.nModifierInterval >= pindexPrev->GetBlockTime() / params.nModifierInterval) {
        return true;
    }

    // Sort candidate blocks by timestamp
    std::vector<std::pair<int64_t, uint256>> vSortedByTimestamp;
    vSortedByTimestamp.reserve(64 * params.nModifierInterval / ((pindexPrev->nHeight + 1 >= params.nModifierUpgrade) ? (int) params.nPowTargetSpacing : 60));
    int64_t nSelectionInterval = GetStakeModifierSelectionInterval();
    int64_t nSelectionIntervalStart = (pindexPrev->GetBlockTime() / params.nModifierInterval) * params.nModifierInterval - nSelectionInterval;
    const CBlockIndex* pindex = pindexPrev;

    while (pindex && pindex->GetBlockTime() >= nSelectionIntervalStart) {
        vSortedByTimestamp.push_back(std::make_pair(pindex->GetBlockTime(), pindex->GetBlockHash()));
        pindex = pindex->pprev;
    }
    int nHeightFirstCandidate = pindex ? (pindex->nHeight + 1) : 0;

    // Shuffle before sort
    for(int i = vSortedByTimestamp.size() - 1; i > 1; --i)
    std::swap(vSortedByTimestamp[i], vSortedByTimestamp[GetRand(i)]);

    std::sort(vSortedByTimestamp.begin(), vSortedByTimestamp.end(), [] (const std::pair<int64_t, uint256> &a, const std::pair<int64_t, uint256> &b)
    {
        if (a.first != b.first)
            return a.first < b.first;
        // Timestamp equals - compare block hashes
        const uint32_t *pa = a.second.GetDataPtr();
        const uint32_t *pb = b.second.GetDataPtr();
        int cnt = 256 / 32;
        do {
            --cnt;
            if (pa[cnt] != pb[cnt])
                return pa[cnt] < pb[cnt];
        } while(cnt);
            return false; // Elements are equal
    });

    // Select 64 blocks from candidate blocks to generate stake modifier
    uint64_t nStakeModifierNew = 0;
    int64_t nSelectionIntervalStop = nSelectionIntervalStart;
    std::map<uint256, const CBlockIndex*> mapSelectedBlocks;
    for (int nRound = 0; nRound < std::min(64, (int)vSortedByTimestamp.size()); nRound++) {
        // add an interval section to the current selection round
        nSelectionIntervalStop += GetStakeModifierSelectionIntervalSection(nRound);
        // select a block from the candidates of current round
        if (!SelectBlockFromCandidates(vSortedByTimestamp, mapSelectedBlocks, nSelectionIntervalStop, nStakeModifier, &pindex, params, chainstate)) {
            LogPrint(BCLog::POS, "%s: unable to select block at round %d\n", __func__, nRound);
            return false;
        }
        // write the entropy bit of the selected block
        nStakeModifierNew |= (((uint64_t)pindex->GetStakeEntropyBit()) << nRound);
        // add the selected block from candidates to selected list
        mapSelectedBlocks.insert(std::make_pair(pindex->GetBlockHash(), pindex));
        if (gArgs.GetBoolArg("-printstakemodifier", DEFAULT_PRINTSTAKEMODIFIER)) {
            LogPrint(BCLog::POS, "%s: selected round %d stop=%s height=%d bit=%d\n", __func__, nRound, FormatISO8601DateTime(nSelectionIntervalStop), pindex->nHeight, pindex->GetStakeEntropyBit());
        }
    }

    // Print selection map for visualization of the selected blocks
    if (gArgs.GetBoolArg("-printstakemodifier", DEFAULT_PRINTSTAKEMODIFIER)) {
        std::string strSelectionMap = "";
        // '-' indicates proof-of-work blocks not selected
        strSelectionMap.insert(0, pindexPrev->nHeight - nHeightFirstCandidate + 1, '-');
        pindex = pindexPrev;
        while (pindex && pindex->nHeight >= nHeightFirstCandidate) {
            // '=' indicates proof-of-stake blocks not selected
            if (pindex->IsProofOfStake())
                strSelectionMap.replace(pindex->nHeight - nHeightFirstCandidate, 1, "=");
            pindex = pindex->pprev;
        }
        for (const auto& item : mapSelectedBlocks) {
            // 'S' indicates selected proof-of-stake blocks
            // 'W' indicates selected proof-of-work blocks
            strSelectionMap.replace(item.second->nHeight - nHeightFirstCandidate, 1, item.second->IsProofOfStake() ? "S" : "W");
        }
        LogPrintf("%s: selection height [%d, %d] map %s\n", __func__, nHeightFirstCandidate, pindexPrev->nHeight, strSelectionMap);
    }

    LogPrintf("%s: new modifier=0x%016x time=%s\n", __func__, nStakeModifierNew, FormatISO8601DateTime(pindexPrev->GetBlockTime()));

    nStakeModifier = nStakeModifierNew;
    fGeneratedStakeModifier = true;
    return true;
}

bool GetKernelStakeModifier(Chainstate& chainstate, CBlockIndex* pindexPrev, uint256 hashBlockFrom, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake)
{
    const Consensus::Params& params = Params().GetConsensus();
    nStakeModifier = 0;
    const CBlockIndex* pindexFrom = chainstate.m_blockman.LookupBlockIndex(hashBlockFrom);
    if (!pindexFrom) {
        LogPrint(BCLog::POS, "%s: block not indexed\n");
        return false;
    }

    nStakeModifierHeight = pindexFrom->nHeight;
    nStakeModifierTime = pindexFrom->GetBlockTime();
    int64_t nStakeModifierSelectionInterval = GetStakeModifierSelectionInterval();

    const CBlockIndex* pindex = pindexFrom;
    CBlockIndex* pindexNext = chainstate.m_chain[pindexFrom->nHeight + 1];

    // loop to find the stake modifier later by a selection interval
    while (nStakeModifierTime < pindexFrom->GetBlockTime() + nStakeModifierSelectionInterval) {
        if (!pindexNext) {
            return false;
        }

        pindex = pindexNext;
        pindexNext = chainstate.m_chain[pindexNext->nHeight + 1];

        if (pindex->GeneratedStakeModifier()) {
            nStakeModifierHeight = pindex->nHeight;
            nStakeModifierTime = pindex->GetBlockTime();
        }
    }
    nStakeModifier = pindex->nStakeModifier;
    return true;
}

bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx)
{
    return (nTimeBlock == nTimeTx);
}

bool stakeTargetHit(const uint256& hashProofOfStake, const int64_t& nValueIn, arith_uint256& bnTargetPerCoinDay)
{
    arith_uint256 bnCoinDayWeight = nValueIn / 100;
    arith_uint256 bnTargetWeight = bnCoinDayWeight * bnTargetPerCoinDay;
    return UintToArith256(hashProofOfStake) < bnTargetWeight;
}

bool checkStake(const CDataStream& ssUniqueID, CAmount nValueIn, const uint64_t nStakeModifier, arith_uint256& bnTarget, unsigned int nTimeBlockFrom, unsigned int& nTimeTx, uint256& hashProofOfStake)
{
    CDataStream ss(SER_GETHASH, 0);
    ss << nStakeModifier << nTimeBlockFrom << ssUniqueID << nTimeTx;
    hashProofOfStake = Hash(ss);
    LogPrintf("%s: modifier:%d nTimeBlockFrom:%d nTimeTx:%d hash:%s\n", __func__, nStakeModifier, nTimeBlockFrom, nTimeTx, hashProofOfStake.ToString());
    return stakeTargetHit(hashProofOfStake, nValueIn, bnTarget);
}

bool buildStake(CStakeInput* stakeInput, unsigned int nBits, unsigned int nTimeBlockFrom, unsigned int& nTimeTx, uint256& hashProofOfStake, Chainstate& chainstate)
{
    const Consensus::Params& params = Params().GetConsensus();

    if (nTimeTx < nTimeBlockFrom) {
        LogPrint(BCLog::POS, "%s: nTime violation: nTimeTx < txPrev.nTime\n", __func__);
        return false;
    }

    if (nTimeBlockFrom + params.nStakeMinAge > nTimeTx) {
        LogPrint(BCLog::POS, "%s: min age violation\n", __func__);
        return false;
    }

    //grab difficulty
    arith_uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);

    //grab stake modifier
    uint64_t nStakeModifier = 0;
    if (!stakeInput->GetModifier(nStakeModifier, chainstate)) {
        return error("failed to get kernel stake modifier");
    }

    bool fSuccess = false;
    unsigned int nTryTime = 0;
    const int nHashDrift = 60;
    CDataStream ssUniqueID = stakeInput->GetUniqueness();
    CAmount nValueIn = stakeInput->GetValue();
    for (int i = 0; i < nHashDrift; i++)
    {
        nTryTime = nTimeTx + nHashDrift - i;
        if (!checkStake(ssUniqueID, nValueIn, nStakeModifier, bnTargetPerCoinDay, nTimeBlockFrom, nTryTime, hashProofOfStake)) {
            continue;
        }

        fSuccess = true;
        nTimeTx = nTryTime;
        break;
    }

    return fSuccess;
}

bool CheckProofOfStake(const CBlock& block, uint256& hashProofOfStake, std::unique_ptr<CStakeInput>& stake, Chainstate& chainstate)
{
    const Consensus::Params& params = Params().GetConsensus();

    const CTransactionRef& tx = block.vtx[1];
    if (!tx->IsCoinStake()) {
        return error("CheckProofOfStake() : called on non-coinstake %s", tx->GetHash().ToString());
    }

    if (node::fImporting || node::fReindex) {
        return true;
    }

    const CTxIn& txin = tx->vin[0];

    // Get transaction index for the previous transaction
    uint256 blockhash{};
    CTransactionRef txPrev;
    if (!g_txindex->FindTx(txin.prevout.hash, blockhash, txPrev)) {
        return error("CheckProofOfStake() : tx index not found");  // tx index not found
    }

    // Recreate stake object
    CMyceStake* myceInput = new CMyceStake();
    myceInput->SetInput(txPrev, txin.prevout.n);
    stake = std::unique_ptr<CStakeInput>(myceInput);

    // Retrieve header via blockindex
    CBlockIndex* pindex = stake->GetIndexFrom(chainstate);
    if (!pindex) {
        return error("%s: Failed to find the block index", __func__);
    }

    CBlockHeader blockPrev = pindex->GetBlockHeader();

    if (block.nVersion < params.nWalletVersion && chainstate.m_chain.Height() + 1 >= params.nWalletUpgrade)
        return false;
    else if (block.nVersion >= params.nWalletVersion && chainstate.m_chain.Height() + 1 < params.nWalletUpgrade)
        return false;

    arith_uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(block.nBits);

    uint64_t nStakeModifier = 0;
    if (!stake->GetModifier(nStakeModifier, chainstate))
        return error("%s failed to get modifier for stake input\n", __func__);

    unsigned int nTxTime = block.nTime;
    unsigned int nBlockFromTime = blockPrev.nTime;

    if (block.nVersion >= params.nWalletVersion && !checkStake(stake->GetUniqueness(), stake->GetValue(), nStakeModifier, bnTargetPerCoinDay, nBlockFromTime, nTxTime, hashProofOfStake)) {
        return error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s\n", tx->GetHash().ToString(), hashProofOfStake.ToString());
    }

    return true;
}
