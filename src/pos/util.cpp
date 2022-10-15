#include <pos/util.h>

bool GetCoinAge(const CTransaction& tx, const ChainstateManager& chainman, const CCoinsViewCache &view, uint64_t& nCoinAge, unsigned int nTimeTx)
{
    arith_uint256 bnCentSecond = 0;
    nCoinAge = 0;

    if (tx.IsCoinBase())
        return true;

    if (!g_txindex)
        return false;

    for (const auto& txin : tx.vin)
    {
        // First try finding the previous transaction in database
        const COutPoint &prevout = txin.prevout;
        Coin coin;

        if (!view.GetCoin(prevout, coin))
            continue;  // previous transaction not in main chain

        if (nTimeTx < coin.nTime)
            return false;  // Transaction timestamp violation

        uint256 blockHash;
        CTransactionRef txPrev;
        if (g_txindex->FindTx(prevout.hash, blockHash, txPrev))
        {
            const CBlockIndex* pindex{chainman.m_blockman.LookupBlockIndex(blockHash)};
            if (!pindex) {
                continue;
            }
            CBlockHeader header = pindex->GetBlockHeader();

            if (txPrev->GetHash() != prevout.hash)
                return error("%s() : txid mismatch in GetCoinAge()", __PRETTY_FUNCTION__);

            if (header.GetBlockTime() + Params().GetConsensus().nStakeMinAge > nTimeTx)
                continue; // only count coins meeting min age requirement

            int64_t nValueIn = txPrev->vout[txin.prevout.n].nValue;
            int nEffectiveAge = nTimeTx-(txPrev->nTime ? txPrev->nTime : header.GetBlockTime());

            bnCentSecond += arith_uint256(nValueIn) * nEffectiveAge / CENT;

            if (gArgs.GetBoolArg("-printcoinage", false))
                LogPrintf("coin age nValueIn=%-12lld nTimeDiff=%d bnCentSecond=%s\n", nValueIn, nEffectiveAge, bnCentSecond.ToString());
        }
        else
            return error("%s() : tx missing in tx index in GetCoinAge()", __PRETTY_FUNCTION__);
    }

    arith_uint256 bnCoinDay = bnCentSecond * CENT / COIN / (24 * 60 * 60);
    if (gArgs.GetBoolArg("-printcoinage", false))
        LogPrintf("coin age bnCoinDay=%s\n", bnCoinDay.ToString());
    nCoinAge = bnCoinDay.GetLow64();
    return true;
}

bool SignBlock(CBlock& block, CStakeWallet& keystore)
{
    std::vector<valtype> vSolutions;
    const CTxOut& txout = block.IsProofOfStake()? block.vtx[1]->vout[1] : block.vtx[0]->vout[0];

    if (Solver(txout.scriptPubKey, vSolutions) != TxoutType::PUBKEY)
        return false;

    const valtype& vchPubKey = vSolutions[0];

    CKey key;
    if (!keystore.GetStakingWallet()->GetLegacyScriptPubKeyMan()->GetKey(CKeyID(Hash160(vchPubKey)), key)) {
        return false;
    }

    if (key.GetPubKey() != CPubKey(vchPubKey))
        return false;

    return key.Sign(block.GetHash(), block.vchBlockSig, 0);
}

bool CheckBlockSignature(const CBlock& block)
{
    if (block.GetHash() == Params().GetConsensus().hashGenesisBlock)
        return block.vchBlockSig.empty();

    std::vector<valtype> vSolutions;
    const CTxOut& txout = block.IsProofOfStake()? block.vtx[1]->vout[1] : block.vtx[0]->vout[0];

    if (Solver(txout.scriptPubKey, vSolutions) != TxoutType::PUBKEY)
        return false;

    const valtype& vchPubKey = vSolutions[0];
    CPubKey key(vchPubKey);
    if (block.vchBlockSig.empty())
        return false;

    return key.Verify(block.GetHash(), block.vchBlockSig);
}
