// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODE_MASTERNODEUTIL_H
#define MASTERNODE_MASTERNODEUTIL_H

#include <index/txindex.h>
#include <masternode/masternode-budget.h>
#include <node/context.h>
#include <pos/wallet.h>
#include <util/result.h>
#include <validation.h>
#include <wallet/coinselection.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

using wallet::COutput;
using wallet::CRecipient;

extern node::NodeContext* g_rpc_node;

int GetInputAge(const CTxIn& vin, Chainstate& chainstate);
int GetIXConfirmations(uint256 nTXHash);
bool GetBudgetFinalizationCollateralTX(CTransactionRef& tx, uint256 hash);
bool GetMasternodeVinAndKeys(CTxIn& txinRet, CPubKey& pubKeyRet, CKey& keyRet, std::string strTxHash, std::string strOutputIndex);
bool GetVinAndKeysFromOutput(COutput out, CTxIn& txinRet, CPubKey& pubkeyRet, CKey& keyRet);

#endif // MASTERNODE_MASTERNODEUTIL_H
