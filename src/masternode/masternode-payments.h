// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODE_PAYMENTS_H
#define MASTERNODE_PAYMENTS_H

#include <key.h>
#include <masternode/masternode.h>
#include <validation.h>

extern RecursiveMutex cs_vecPayments;
extern RecursiveMutex cs_mapMasternodeBlocks;
extern RecursiveMutex cs_mapMasternodePayeeVotes;

class CMasternodePayments;
class CMasternodePaymentWinner;
class CMasternodeBlockPayees;

#define MNPAYMENTS_SIGNATURES_REQUIRED 6
#define MNPAYMENTS_SIGNATURES_TOTAL 10

void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman* connman);
bool IsBlockPayeeValid(const CBlock& block, int nBlockHeight);
std::string GetRequiredPaymentsString(int nBlockHeight);
bool IsBlockValueValid(const CBlock& block, CAmount nExpectedValue, CAmount nMinted, int nHeight);
void FillBlockPayee(int nBlockHeight, CMutableTransaction& txNew, CAmount nFees, bool fProofOfStake, bool fZYCEStake);

void DumpMasternodePayments();

/** Save Masternode Payment Data (mnpayments.dat)
 */
class CMasternodePaymentDB {
private:
    std::filesystem::path pathDB;
    std::string strMagicMessage;

public:
    enum ReadResult {
        Ok,
        FileError,
        HashReadError,
        IncorrectHash,
        IncorrectMagicMessage,
        IncorrectMagicNumber,
        IncorrectFormat
    };

    CMasternodePaymentDB();
    bool Write(const CMasternodePayments& objToSave);
    ReadResult Read(CMasternodePayments& objToLoad, bool fDryRun = false);
};

class CMasternodePayee {
public:
    CScript scriptPubKey;
    int nVotes;

    CMasternodePayee()
    {
        scriptPubKey = CScript();
        nVotes = 0;
    }

    CMasternodePayee(CScript payee, int nVotesIn)
    {
        scriptPubKey = payee;
        nVotes = nVotesIn;
    }

    SERIALIZE_METHODS(CMasternodePayee, obj)
    {
        READWRITE(*(CScriptBase*)(&obj.scriptPubKey));
        READWRITE(obj.nVotes);
    }
};

// Keep track of votes for payees from masternodes
class CMasternodeBlockPayees {
public:
    int nBlockHeight;
    std::vector<CMasternodePayee> vecPayments;

    CMasternodeBlockPayees()
    {
        nBlockHeight = 0;
        vecPayments.clear();
    }
    CMasternodeBlockPayees(int nBlockHeightIn)
    {
        nBlockHeight = nBlockHeightIn;
        vecPayments.clear();
    }

    void AddPayee(CScript payeeIn, int nIncrement)
    {
        LOCK(cs_vecPayments);

        for (CMasternodePayee& payee : vecPayments) {
            if (payee.scriptPubKey == payeeIn) {
                payee.nVotes += nIncrement;
                return;
            }
        }

        CMasternodePayee c(payeeIn, nIncrement);
        vecPayments.push_back(c);
    }

    bool GetPayee(CScript& payee)
    {
        LOCK(cs_vecPayments);

        int nVotes = -1;
        for (CMasternodePayee& p : vecPayments) {
            if (p.nVotes > nVotes) {
                payee = p.scriptPubKey;
                nVotes = p.nVotes;
            }
        }

        return (nVotes > -1);
    }

    bool HasPayeeWithVotes(CScript payee, int nVotesReq)
    {
        LOCK(cs_vecPayments);

        for (CMasternodePayee& p : vecPayments) {
            if (p.nVotes >= nVotesReq && p.scriptPubKey == payee)
                return true;
        }

        return false;
    }

    bool IsTransactionValid(const CTransactionRef& txNew);
    std::string GetRequiredPaymentsString();

    SERIALIZE_METHODS(CMasternodeBlockPayees, obj)
    {
        READWRITE(obj.nBlockHeight);
        READWRITE(obj.vecPayments);
    }
};

// for storing the winning payments
class CMasternodePaymentWinner {
public:
    CTxIn vinMasternode;

    int nBlockHeight;
    CScript payee;
    std::vector<unsigned char> vchSig;

    CMasternodePaymentWinner()
    {
        nBlockHeight = 0;
        vinMasternode = CTxIn();
        payee = CScript();
    }

    CMasternodePaymentWinner(CTxIn vinIn)
    {
        nBlockHeight = 0;
        vinMasternode = vinIn;
        payee = CScript();
    }

    uint256 GetHash()
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << payee;
        ss << nBlockHeight;
        ss << vinMasternode.prevout;

        return ss.GetHash();
    }

    bool Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode);
    bool IsValid(CBlockIndex* pindex, CNode* pnode, std::string& strError, CConnman* connman);
    bool SignatureValid();
    void Relay(CConnman* connman);

    void AddPayee(CScript payeeIn)
    {
        payee = payeeIn;
    }

    SERIALIZE_METHODS(CMasternodePaymentWinner, obj)
    {
        READWRITE(obj.vinMasternode);
        READWRITE(obj.nBlockHeight);
        READWRITE(*(CScriptBase*)(&obj.payee));
        READWRITE(obj.vchSig);
    }

    std::string ToString()
    {
        std::string ret = "";
        ret += vinMasternode.ToString();
        ret += ", " + std::to_string(nBlockHeight);
        ret += ", " + payee.ToString();
        ret += ", " + std::to_string((int)vchSig.size());
        return ret;
    }
};

//
// Masternode Payments Class
// Keeps track of who should get paid for which blocks
//

class CMasternodePayments {
private:
    int nSyncedFromPeer;
    int nLastBlockHeight;

    ChainstateManager* chainman{nullptr};

public:
    std::map<uint256, CMasternodePaymentWinner> mapMasternodePayeeVotes;
    std::map<int, CMasternodeBlockPayees> mapMasternodeBlocks;
    std::map<uint256, int> mapMasternodesLastVote; // prevout.hash + prevout.n, nBlockHeight

    CMasternodePayments()
    {
        nSyncedFromPeer = 0;
        nLastBlockHeight = 0;
    }

    void Clear()
    {
        LOCK2(cs_mapMasternodeBlocks, cs_mapMasternodePayeeVotes);
        mapMasternodeBlocks.clear();
        mapMasternodePayeeVotes.clear();
    }

    /// Attach chainman pointer to class
    void Attach(ChainstateManager* other) {
        chainman = other;
    }

    bool AddWinningMasternode(CMasternodePaymentWinner& winner);
    bool ProcessBlock(CBlockIndex* pindex, int nBlockHeight, CConnman* connman);

    void Sync(CNode* node, int nCountNeeded, CConnman* connman);
    void CleanPaymentList();
    int LastPayment(CMasternode& mn);

    bool GetBlockPayee(int nBlockHeight, CScript& payee);
    bool IsTransactionValid(const CTransactionRef& txNew, int nBlockHeight);
    bool IsScheduled(CMasternode& mn, int nNotBlockHeight);

    bool CanVote(COutPoint outMasternode, int nBlockHeight)
    {
        LOCK(cs_mapMasternodePayeeVotes);

	arith_uint256 voteHash = UintToArith256(outMasternode.hash) + outMasternode.n;
        if (mapMasternodesLastVote.count(ArithToUint256(voteHash))) {
            if (mapMasternodesLastVote[ArithToUint256(voteHash)] == nBlockHeight) {
                return false;
            }
        }

        // record this masternode voted
        mapMasternodesLastVote[ArithToUint256(voteHash)] = nBlockHeight;
        return true;
    }

    int GetMinMasternodePaymentsProto();
    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman* connman);
    std::string GetRequiredPaymentsString(int nBlockHeight);
    void FillBlockPayee(CMutableTransaction& txNew, int64_t nFees, bool fProofOfStake, bool fZYCEStake);
    std::string ToString() const;
    int GetOldestBlock();
    int GetNewestBlock();

    SERIALIZE_METHODS(CMasternodePayments, obj)
    {
        READWRITE(obj.mapMasternodePayeeVotes);
        READWRITE(obj.mapMasternodeBlocks);
    }
};

#endif
