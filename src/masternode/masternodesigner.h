// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODE_MASTERNODESIGNER_H
#define MASTERNODE_MASTERNODESIGNER_H

#include <key.h>
#include <key_io.h>
#include <masternode/masternodesigner.h>
#include <node/transaction.h>
#include <util/system.h>
#include <timedata.h>
#include <validation.h>

using node::GetTransaction;

class CLegacySigner;
extern CLegacySigner legacySigner;

class CLegacySigner {
public:
    bool GetSignatureVersion();
    bool GetKeysFromSecret(std::string strSecret, CKey& keyRet, CPubKey& pubkeyRet);
    bool SetKey(std::string strSecret, CKey& key, CPubKey& pubkey);
    bool SignMessage(std::string strMessage, std::string& errorMessage, std::vector<unsigned char>& vchSig, CKey key);
    bool SignMessage(std::string strMessage, std::vector<unsigned char>& vchSig, CKey key);
    bool VerifyMessage(CPubKey pubkey, std::vector<unsigned char>& vchSig, std::string strMessage, std::string& errorMessage);
    bool VerifyMessage(CPubKey pubkey, std::vector<unsigned char>& vchSig, std::string strMessage);
    bool IsVinAssociatedWithPubkey(CTxIn& vin, CPubKey& pubkey);
};

#endif // MASTERNODE_MASTERNODESIGNER_H
