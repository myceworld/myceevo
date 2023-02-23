// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/masternodeutil.h>

node::NodeContext* g_rpc_node = nullptr;

int GetInputAge(const CTxIn& vin, Chainstate& chainstate)
{
    int height = chainstate.m_chain.Height() + 1;

    CCoinsView viewDummy;
    CCoinsViewCache view(&viewDummy);
    {
        const CTxMemPool& mempool = *g_rpc_node->mempool;
        LOCK(cs_main);
        LOCK(mempool.cs);
        CCoinsViewCache& viewChain = chainstate.CoinsTip();
        CCoinsViewMemPool viewMempool(&viewChain, mempool);
        view.SetBackend(viewMempool); // temporarily switch cache backend to db+mempool view

        COutPoint testInput(vin.prevout.hash, vin.prevout.n);
        const Coin& coin = view.AccessCoin(testInput);

        if (!coin.IsSpent()) {
            if (coin.nHeight < 0)
                return 0;
            return height - coin.nHeight;
        } else {
            return -1;
        }
    }
}

int GetIXConfirmations(uint256 nTXHash)
{
    return 0;
}

bool GetBudgetFinalizationCollateralTX(CTransactionRef& tx, uint256 hash)
{
    if (!stakeWallet.GetStakingWallet()) {
        LogPrint(BCLog::MNBUDGET, "%s: Wallet is not loaded\n", __func__);
        return false;
    }

    // make our change address
    wallet::ReserveDestination reserveDest(stakeWallet.GetStakingWallet(), OutputType::LEGACY);

    CScript scriptChange;
    scriptChange << OP_RETURN << ToByteVector(hash);

    CAmount nFeeRet = 0;
    std::string strFail;
    std::vector<CRecipient> vecSend;
    CRecipient entry{scriptChange, BUDGET_FEE_TX};
    vecSend.push_back(entry);

    constexpr int RANDOM_CHANGE_POSITION = -1;
    wallet::CCoinControl* coinControl = nullptr;
    auto res = CreateTransaction(*stakeWallet.GetStakingWallet(), vecSend, RANDOM_CHANGE_POSITION, *coinControl);
    if (!res) {
        LogPrint(BCLog::MNBUDGET, "%s: Error - %s\n", __func__, strFail);
        return false;
    }

    return true;
}

bool GetMasternodeVinAndKeys(CTxIn& txinRet, CPubKey& pubKeyRet, CKey& keyRet, std::string strTxHash, std::string strOutputIndex)
{
    if (!stakeWallet.GetStakingWallet()) {
        LogPrint(BCLog::MASTERNODE, "%s: Wallet is not loaded\n", __func__);
        return false;
    }

    LOCK(stakeWallet.GetStakingWallet()->cs_wallet);
    std::vector<COutput> vPossibleCoins;

    // Retrieve all possible outputs
    auto res = wallet::AvailableCoins(*stakeWallet.GetStakingWallet(), nullptr);
    for (auto entry : res.All()) {
        vPossibleCoins.push_back(entry);
    }

    if (vPossibleCoins.empty()) {
        LogPrint(BCLog::MASTERNODE, "%s: Could not locate any valid masternode vin\n", __func__);
        return false;
    }

    uint256 txHash = uint256S(strTxHash);
    int nOutputIndex = atoi(strOutputIndex.c_str());
    for (COutput& out : vPossibleCoins) {
        if (out.outpoint.hash == txHash && out.outpoint.n == nOutputIndex) {
            return GetVinAndKeysFromOutput(out, txinRet, pubKeyRet, keyRet);
        }
    }

    LogPrint(BCLog::MASTERNODE, "%s: Could not locate specified masternode vin\n", __func__);
    return false;
}

bool GetVinAndKeysFromOutput(COutput out, CTxIn& txinRet, CPubKey& pubkeyRet, CKey& keyRet)
{
    if (!stakeWallet.GetStakingWallet()) {
        LogPrint(BCLog::MASTERNODE, "GetVinAndKeysFromOutput: Wallet is not loaded\n");
        return false;
    }

    CKeyID keyID;
    CScript pubScript;
    txinRet = CTxIn(out.outpoint.hash, out.outpoint.n);
    pubScript = out.txout.scriptPubKey;

    CTxDestination address;
    ExtractDestination(pubScript, address);
    auto key_id = std::get_if<PKHash>(&address);
    keyID = ToKeyID(*key_id);
    if (!key_id) {
        LogPrint(BCLog::MASTERNODE, "%s: Address does not refer to a key\n", __func__);
        return false;
    }

    wallet::LegacyScriptPubKeyMan* spk_man = stakeWallet.GetStakingWallet()->GetLegacyScriptPubKeyMan();
    if (!spk_man) {
        LogPrint(BCLog::MASTERNODE, "%s: This type of wallet does not support this command\n", __func__);
        return false;
    }

    if (!spk_man->GetKey(keyID, keyRet)) {
        LogPrint(BCLog::MASTERNODE, "%s: Private key for address is not known\n", __func__);
        return false;
    }

    pubkeyRet = keyRet.GetPubKey();
    return true;
}
