// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <pos/pos.h>
#include <pos/stakeinput.h>
#include <pos/wallet.h>
#include <validation.h>

bool CMyceStake::SetInput(CTransactionRef txPrev, unsigned int n)
{
    this->txFrom = txPrev;
    this->nPosition = n;
    return true;
}

bool CMyceStake::GetTxFrom(CTransactionRef& tx)
{
    tx = txFrom;
    return true;
}

bool CMyceStake::CreateTxIn(CStakeWallet* wallet, CTxIn& txIn, uint256 hashTxOut)
{
    txIn = CTxIn(txFrom->GetHash(), nPosition);
    return true;
}

CAmount CMyceStake::GetValue()
{
    return txFrom->vout[nPosition].nValue;
}

bool CMyceStake::CreateTxOuts(CStakeWallet* wallet, std::vector<CTxOut>& vout, CAmount nTotal)
{
    const wallet::CWallet* pwallet = wallet->GetStakingWallet();
    if (!pwallet) {
        LogPrint(BCLog::POS, "%s: could not obtain wallet pointer\n", __func__);
        return false;
    }

    std::vector<valtype> vSolutions;
    CScript scriptPubKeyKernel = txFrom->vout[nPosition].scriptPubKey;
    TxoutType whichType = Solver(scriptPubKeyKernel, vSolutions);
    if (whichType == TxoutType::NONSTANDARD) {
        LogPrint(BCLog::POS, "%s: failed to parse kernel\n", __func__);
        return false;
    }

    if (whichType != TxoutType::PUBKEY && whichType != TxoutType::PUBKEYHASH) {
        LogPrint(BCLog::POS, "%s: unsupported key type\n", __func__);
        return false;
    }

    CScript scriptPubKey;
    if (whichType == TxoutType::PUBKEYHASH)
    {
        // convert to pay to public key type
        CPubKey pubKeyStake;
        uint160 hash160(vSolutions[0]);

        auto spk_man = pwallet->GetLegacyScriptPubKeyMan();
        if (!spk_man) {
            LogPrint(BCLog::POS, "%s: failed to get legacyscriptpubkeyman\n", __func__);
            return false;
        }

        CKey key;
        if (!spk_man->GetKey(CKeyID(hash160), key)) {
            LogPrint(BCLog::POS, "%s: failed to get key for kernel type=%d\n", __func__, GetTxnOutputType(whichType));
            return false;
        }
        scriptPubKey << ToByteVector(key.GetPubKey()) << OP_CHECKSIG;
    } else {
        // TODO: extra checks here?
        scriptPubKey = scriptPubKeyKernel;
    }

    vout.emplace_back(CTxOut(0, scriptPubKey));

    // Calculate if we need to split the output
    if (nStakeSplitThreshold > 0) {
        int nSplit = nTotal / (static_cast<CAmount>(nStakeSplitThreshold * COIN));
        if (nSplit > 1) {
            // if nTotal is twice or more of the threshold; create more outputs
            int txSizeMax = MAX_STANDARD_TX_WEIGHT >> 11; // limit splits to <10% of the max TX size (/2048)
            if (nSplit > txSizeMax)
                nSplit = txSizeMax;
            for (int i = nSplit; i > 1; i--) {
                LogPrintf("%s: StakeSplit: nTotal = %d; adding output %d of %d\n", __func__, nTotal, (nSplit-i)+2, nSplit);
                vout.emplace_back(CTxOut(0, scriptPubKey));
            }
        }
    }

    return true;
}

bool CMyceStake::GetModifier(uint64_t& nStakeModifier, Chainstate& chainstate)
{
    int nStakeModifierHeight = 0;
    int64_t nStakeModifierTime = 0;
    GetIndexFrom(chainstate);
    if (!pindexFrom) {
        LogPrint(BCLog::POS, "%s: failed to get index from\n", __func__);
        return false;
    }

    uint256 blockHash = pindexFrom->GetBlockHash();
    if (!GetKernelStakeModifier(chainstate, pindexFrom, blockHash, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, false)) {
        LogPrint(BCLog::POS, "%s: failed to get kernel stake modifier\n", __func__);
        return false;
    }

    return true;
}

CDataStream CMyceStake::GetUniqueness()
{
    CDataStream ss(SER_NETWORK, 0);
    ss << nPosition << txFrom->GetHash();
    return ss;
}

CBlockIndex* CMyceStake::GetIndexFrom(Chainstate& chainstate)
{
    const Consensus::Params& params = Params().GetConsensus();

    uint256 hashBlock{};
    CTransactionRef tx = node::GetTransaction(nullptr, nullptr, txFrom->GetHash(), params, hashBlock);
    if (tx) {
        CBlockIndex* pindex = chainstate.m_blockman.LookupBlockIndex(hashBlock);
        if (pindex) {
            if (chainstate.m_chain.Contains(pindex)) {
                pindexFrom = pindex;
            }
        } else {
            LogPrint(BCLog::POS, "%s: failed to find blockindex entry for block %s\n", __func__, hashBlock.ToString());
            pindexFrom = nullptr;
        }
    } else {
        LogPrint(BCLog::POS, "%s: failed to find tx %s\n", __func__, txFrom->GetHash().ToString());
        pindexFrom = nullptr;
    }

    return pindexFrom;
}
