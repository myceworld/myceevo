// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/masternodesigner.h>

const std::string strMessageMagic = "Myce Signed Message:\n";

class CLegacySigner;
CLegacySigner legacySigner;

bool CLegacySigner::GetSignatureVersion()
{
    return true;
}

bool CLegacySigner::GetKeysFromSecret(std::string strSecret, CKey& keyRet, CPubKey& pubkeyRet)
{
    CTxDestination destination = DecodeDestination(strSecret);
    if (!IsValidDestination(destination)) {
        return false;
    }

    keyRet = DecodeSecret(strSecret);
    if (!keyRet.IsValid()) {
        return false;
    }

    pubkeyRet = keyRet.GetPubKey();
    if (!pubkeyRet.IsValid()) {
        return false;
    }

    return true;
}

bool CLegacySigner::SetKey(std::string strSecret, CKey& key, CPubKey& pubkey)
{
    return GetKeysFromSecret(strSecret, key, pubkey);
}

bool CLegacySigner::SignMessage(std::string strMessage, std::string& errorMessage, std::vector<unsigned char>& vchSig, CKey key)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    if (!key.SignCompact(ss.GetHash(), vchSig)) {
        errorMessage = "Signing failed";
        return false;
    }

    return true;
}

bool CLegacySigner::SignMessage(std::string strMessage, std::vector<unsigned char>& vchSig, CKey key)
{
    std::string errorMessage;
    return SignMessage(strMessage, errorMessage, vchSig, key);
}

bool CLegacySigner::VerifyMessage(CPubKey pubkey, std::vector<unsigned char>& vchSig, std::string strMessage, std::string& errorMessage)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    CPubKey pubkey2;
    if (!pubkey2.RecoverCompact(ss.GetHash(), vchSig)) {
        errorMessage = "Error recovering public key";
        return false;
    }

    return PKHash(pubkey2) == PKHash(pubkey);
}

bool CLegacySigner::VerifyMessage(CPubKey pubkey, std::vector<unsigned char>& vchSig, std::string strMessage)
{
    std::string errorMessage;
    return VerifyMessage(pubkey, vchSig, strMessage, errorMessage);
}

bool CLegacySigner::IsVinAssociatedWithPubkey(CTxIn& vin, CPubKey& pubkey)
{
    CScript payee2 = GetScriptForDestination(PKHash(pubkey));

    uint256 hashBlock{};
    CTransactionRef txVin = GetTransaction(nullptr, nullptr, vin.prevout.hash, Params().GetConsensus(), hashBlock);
    if (txVin) {
        for (CTxOut out : txVin->vout) {
            if (out.nValue == 100000 * COIN) {
                if (out.scriptPubKey == payee2) {
                    return true;
                }
            }
        }
    }

    return false;
}
