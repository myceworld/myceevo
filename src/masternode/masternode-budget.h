// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODE_BUDGET_H
#define MASTERNODE_BUDGET_H

#include <base58.h>
#include <init.h>
#include <key.h>
#include <masternode/masternode.h>
#include <masternode/netfulfilledman.h>
#include <net.h>
#include <netmessagemaker.h>
#include <sync.h>
#include <util/system.h>
#include <validation.h>

extern RecursiveMutex cs_budget;

class CFinalizedBudgetBroadcast;
class CFinalizedBudget;
class CBudgetProposal;
class CBudgetProposalBroadcast;
class CTxBudgetPayment;

#define VOTE_ABSTAIN 0
#define VOTE_YES 1
#define VOTE_NO 2

enum class TrxValidationStatus {
    InValid, /** Transaction verification failed */
    Valid, /** Transaction successfully verified */
    DoublePayment, /** Transaction successfully verified, but includes a double-budget-payment */
    VoteThreshold /** If not enough masternodes have voted on a finalized budget */
};

static const CAmount PROPOSAL_FEE_TX = (20 * COIN);
static const CAmount BUDGET_FEE_TX_OLD = (5 * COIN);
static const CAmount BUDGET_FEE_TX = (5 * COIN);
static const int64_t BUDGET_VOTE_UPDATE_MIN = 60 * 60;
static std::map<uint256, int> mapPayment_History;

extern std::vector<CBudgetProposalBroadcast> vecImmatureBudgetProposals;
extern std::vector<CFinalizedBudgetBroadcast> vecImmatureFinalizedBudgets;

void DumpBudgets();

// Define amount of blocks in budget payment cycle
int GetBudgetPaymentCycleBlocks();

// Check the collateral transaction for the budget proposal/finalized budget
bool IsBudgetCollateralValid(uint256 nTxCollateralHash, uint256 nExpectedHash, std::string& strError, int64_t& nTime, int& nConf, Chainstate& chainstate, bool fBudgetFinalization = false);

//
// CBudgetVote - Allow a masternode node to vote and broadcast throughout the network
//

class CBudgetVote {
public:
    bool fValid; // if the vote is currently valid / counted
    bool fSynced; // if we've sent this to our peers
    CTxIn vin;
    uint256 nProposalHash;
    int nVote;
    int64_t nTime;
    std::vector<unsigned char> vchSig;

    CBudgetVote();
    CBudgetVote(CTxIn vin, uint256 nProposalHash, int nVoteIn);

    bool Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode);
    bool SignatureValid(bool fSignatureCheck);
    void Relay(CConnman* connman);

    std::string GetVoteString()
    {
        std::string ret = "ABSTAIN";
        if (nVote == VOTE_YES)
            ret = "YES";
        if (nVote == VOTE_NO)
            ret = "NO";
        return ret;
    }

    uint256 GetHash()
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << vin;
        ss << nProposalHash;
        ss << nVote;
        ss << nTime;
        return ss.GetHash();
    }

    SERIALIZE_METHODS(CBudgetVote, obj)
    {
        READWRITE(obj.vin);
        READWRITE(obj.nProposalHash);
        READWRITE(obj.nVote);
        READWRITE(obj.nTime);
        READWRITE(obj.vchSig);
    }
};

//
// CFinalizedBudgetVote - Allow a masternode node to vote and broadcast throughout the network
//

class CFinalizedBudgetVote {
public:
    bool fValid; // if the vote is currently valid / counted
    bool fSynced; // if we've sent this to our peers
    CTxIn vin;
    uint256 nBudgetHash;
    int64_t nTime;
    std::vector<unsigned char> vchSig;

    CFinalizedBudgetVote();
    CFinalizedBudgetVote(CTxIn vinIn, uint256 nBudgetHashIn);

    bool Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode);
    bool SignatureValid(bool fSignatureCheck);
    void Relay(CConnman* connman);

    uint256 GetHash()
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << vin;
        ss << nBudgetHash;
        ss << nTime;
        return ss.GetHash();
    }

    SERIALIZE_METHODS(CFinalizedBudgetVote, obj)
    {
        READWRITE(obj.vin);
        READWRITE(obj.nBudgetHash);
        READWRITE(obj.nTime);
        READWRITE(obj.vchSig);
    }
};

//
// Budget Manager : Contains all proposals for the budget
//
class CBudgetManager {
private:
    // hold txes until they mature enough to use
    std::map<uint256, uint256> mapCollateralTxids;

    ChainstateManager* chainman{nullptr};

public:
    // critical section to protect the inner data structures
    mutable RecursiveMutex cs;

    // keep track of the scanning errors I've seen
    std::map<uint256, CBudgetProposal> mapProposals;
    std::map<uint256, CFinalizedBudget> mapFinalizedBudgets;

    std::map<uint256, CBudgetProposalBroadcast> mapSeenMasternodeBudgetProposals;
    std::map<uint256, CBudgetVote> mapSeenMasternodeBudgetVotes;
    std::map<uint256, CBudgetVote> mapOrphanMasternodeBudgetVotes;
    std::map<uint256, CFinalizedBudgetBroadcast> mapSeenFinalizedBudgets;
    std::map<uint256, CFinalizedBudgetVote> mapSeenFinalizedBudgetVotes;
    std::map<uint256, CFinalizedBudgetVote> mapOrphanFinalizedBudgetVotes;

    CBudgetManager()
    {
        mapProposals.clear();
        mapFinalizedBudgets.clear();
    }

    /// Attach chainman pointer to class
    void Attach(ChainstateManager* other) {
        chainman = other;
    }

    void ClearSeen()
    {
        mapSeenMasternodeBudgetProposals.clear();
        mapSeenMasternodeBudgetVotes.clear();
        mapSeenFinalizedBudgets.clear();
        mapSeenFinalizedBudgetVotes.clear();
    }

    int sizeFinalized() { return (int)mapFinalizedBudgets.size(); }
    int sizeProposals() { return (int)mapProposals.size(); }
    ChainstateManager* getChainMan() { return chainman; }

    void ResetSync();
    void MarkSynced();
    void Sync(CNode* node, uint256 nProp, CConnman& connman, bool fPartial = false);

    void Calculate();
    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman* connman);
    void NewBlock(CBlockIndex* pindex, CConnman* connman, Chainstate& chainstate);
    CBudgetProposal* FindProposal(const std::string& strProposalName);
    CBudgetProposal* FindProposal(uint256 nHash);
    CFinalizedBudget* FindFinalizedBudget(uint256 nHash);
    std::pair<std::string, std::string> GetVotes(std::string strProposalName);

    CAmount GetTotalBudget(int nHeight);
    std::vector<CBudgetProposal*> GetBudget(CBlockIndex* pindex);
    std::vector<CBudgetProposal*> GetAllProposals();
    std::vector<CFinalizedBudget*> GetFinalizedBudgets();
    bool IsBudgetPaymentBlock(int nBlockHeight);
    bool AddProposal(CBudgetProposal& budgetProposal, CBlockIndex* pindex);
    bool AddFinalizedBudget(CFinalizedBudget& finalizedBudget, CBlockIndex* pindex);
    void SubmitFinalBudget(CBlockIndex* pindex, CConnman& connman, Chainstate& chainstate);

    bool UpdateProposal(CBudgetVote& vote, CNode* pfrom, CConnman& connman, std::string& strError);
    bool UpdateFinalizedBudget(CFinalizedBudgetVote& vote, CNode* pfrom, CConnman& connman, std::string& strError);
    bool PropExists(uint256 nHash);
    TrxValidationStatus IsTransactionValid(const CTransactionRef& txNew, int nBlockHeight);
    std::string GetRequiredPaymentsString(int nBlockHeight);
    void FillBlockPayee(int nBlockHeight, CMutableTransaction& txNew, CAmount nFees, bool fProofOfStake);

    void CheckOrphanVotes(CConnman& connman);
    void Clear()
    {
        LOCK(cs);

        LogPrintf("Budget object cleared\n");
        mapProposals.clear();
        mapFinalizedBudgets.clear();
        mapSeenMasternodeBudgetProposals.clear();
        mapSeenMasternodeBudgetVotes.clear();
        mapSeenFinalizedBudgets.clear();
        mapSeenFinalizedBudgetVotes.clear();
        mapOrphanMasternodeBudgetVotes.clear();
        mapOrphanFinalizedBudgetVotes.clear();
    }
    void CheckAndRemove(CBlockIndex* pindex, CConnman* connman);
    std::string ToString() const;

    SERIALIZE_METHODS(CBudgetManager, obj)
    {
        READWRITE(obj.mapSeenMasternodeBudgetProposals);
        READWRITE(obj.mapSeenMasternodeBudgetVotes);
        READWRITE(obj.mapSeenFinalizedBudgets);
        READWRITE(obj.mapSeenFinalizedBudgetVotes);
        READWRITE(obj.mapOrphanMasternodeBudgetVotes);
        READWRITE(obj.mapOrphanFinalizedBudgetVotes);
        READWRITE(obj.mapProposals);
        READWRITE(obj.mapFinalizedBudgets);
    }
};

class CTxBudgetPayment {
public:
    uint256 nProposalHash;
    CScript payee;
    CAmount nAmount;

    CTxBudgetPayment()
    {
        payee = CScript();
        nAmount = 0;
        nProposalHash = uint256();
    }

    SERIALIZE_METHODS(CTxBudgetPayment, obj)
    {
        READWRITE(*(CScriptBase*)(&obj.payee));
        READWRITE(obj.nAmount);
        READWRITE(obj.nProposalHash);
    }
};

//
// Finalized Budget : Contains the suggested proposals to pay on a given block
//

class CFinalizedBudget {
private:
    // critical section to protect the inner data structures
    mutable RecursiveMutex cs;
    bool fAutoChecked; // If it matches what we see, we'll auto vote for it (masternode only)

public:
    bool fValid;
    std::string strBudgetName;
    int nBlockStart;
    std::vector<CTxBudgetPayment> vecBudgetPayments;
    std::map<uint256, CFinalizedBudgetVote> mapVotes;
    uint256 nFeeTXHash;
    int64_t nTime;

    CFinalizedBudget();
    CFinalizedBudget(const CFinalizedBudget& other);

    void CleanAndRemove(bool fSignatureCheck);
    bool AddOrUpdateVote(CFinalizedBudgetVote& vote, std::string& strError);
    double GetScore();
    bool HasMinimumRequiredSupport();

    bool IsValid(CBlockIndex* pindex, std::string& strError, bool fCheckCollateral = true);

    std::string GetName() { return strBudgetName; }
    std::string GetProposals();
    int GetBlockStart() { return nBlockStart; }
    int GetBlockEnd() { return nBlockStart + (int)(vecBudgetPayments.size() - 1); }
    int GetVoteCount() { return (int)mapVotes.size(); }
    bool IsPaidAlready(uint256 nProposalHash, int nBlockHeight);
    TrxValidationStatus IsTransactionValid(const CTransactionRef& txNew, int nBlockHeight);
    bool GetBudgetPaymentByBlock(int64_t nBlockHeight, CTxBudgetPayment& payment)
    {
        LOCK(cs);

        int i = nBlockHeight - GetBlockStart();
        if (i < 0)
            return false;
        if (i > (int)vecBudgetPayments.size() - 1)
            return false;
        payment = vecBudgetPayments[i];
        return true;
    }
    bool GetPayeeAndAmount(int64_t nBlockHeight, CScript& payee, CAmount& nAmount)
    {
        LOCK(cs);

        int i = nBlockHeight - GetBlockStart();
        if (i < 0)
            return false;
        if (i > (int)vecBudgetPayments.size() - 1)
            return false;
        payee = vecBudgetPayments[i].payee;
        nAmount = vecBudgetPayments[i].nAmount;
        return true;
    }

    // Verify and vote on finalized budget
    void CheckAndVote(CBlockIndex* pindex, CConnman* connman);
    // total myce paid out by this budget
    CAmount GetTotalPayout();
    // vote on this finalized budget as a masternode
    void SubmitVote(CConnman* connman);

    // checks the hashes to make sure we know about them
    std::string GetStatus();

    uint256 GetHash()
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << strBudgetName;
        ss << nBlockStart;
        ss << vecBudgetPayments;

        uint256 h1 = ss.GetHash();
        return h1;
    }

    SERIALIZE_METHODS(CFinalizedBudget, obj)
    {
        READWRITE(LIMITED_STRING(obj.strBudgetName, 20));
        READWRITE(obj.nFeeTXHash);
        READWRITE(obj.nTime);
        READWRITE(obj.nBlockStart);
        READWRITE(obj.vecBudgetPayments);
        READWRITE(obj.fAutoChecked);
        READWRITE(obj.mapVotes);
    }
};

// FinalizedBudget are cast then sent to peers with this object, which leaves the votes out
class CFinalizedBudgetBroadcast : public CFinalizedBudget {
private:
    std::vector<unsigned char> vchSig;

public:
    CFinalizedBudgetBroadcast();
    CFinalizedBudgetBroadcast(const CFinalizedBudget& other);
    CFinalizedBudgetBroadcast(std::string strBudgetNameIn, int nBlockStartIn, std::vector<CTxBudgetPayment> vecBudgetPaymentsIn, uint256 nFeeTXHashIn);

    void swap(CFinalizedBudgetBroadcast& first, CFinalizedBudgetBroadcast& second) // nothrow
    {
        // enable ADL (not necessary in our case, but good practice)
        using std::swap;

        // by swapping the members of two classes,
        // the two classes are effectively swapped
        swap(first.strBudgetName, second.strBudgetName);
        swap(first.nBlockStart, second.nBlockStart);
        first.mapVotes.swap(second.mapVotes);
        first.vecBudgetPayments.swap(second.vecBudgetPayments);
        swap(first.nFeeTXHash, second.nFeeTXHash);
        swap(first.nTime, second.nTime);
    }

    CFinalizedBudgetBroadcast& operator=(CFinalizedBudgetBroadcast from)
    {
        swap(*this, from);
        return *this;
    }

    void Relay(CConnman* connman);

    SERIALIZE_METHODS(CFinalizedBudgetBroadcast, obj)
    {
        READWRITE(LIMITED_STRING(obj.strBudgetName, 20));
        READWRITE(obj.nBlockStart);
        READWRITE(obj.vecBudgetPayments);
        READWRITE(obj.nFeeTXHash);
    }
};

//
// Budget Proposal : Contains the masternode votes for each budget
//

class CBudgetProposal {
private:
    // critical section to protect the inner data structures
    mutable RecursiveMutex cs;
    CAmount nAlloted;

public:
    bool fValid;
    std::string strProposalName;

    /*
        json object with name, short-description, long-description, pdf-url and any other info
        This allows the proposal website to stay 100% decentralized
    */
    std::string strURL;
    int nBlockStart;
    int nBlockEnd;
    CAmount nAmount;
    CScript address;
    int64_t nTime;
    uint256 nFeeTXHash;

    std::map<uint256, CBudgetVote> mapVotes;
    // cache object

    CBudgetProposal();
    CBudgetProposal(const CBudgetProposal& other);
    CBudgetProposal(std::string strProposalNameIn, std::string strURLIn, int nBlockStartIn, int nBlockEndIn, CScript addressIn, CAmount nAmountIn, uint256 nFeeTXHashIn);

    void Calculate();
    bool AddOrUpdateVote(CBudgetVote& vote, std::string& strError);
    bool HasMinimumRequiredSupport();
    std::pair<std::string, std::string> GetVotes();

    bool IsValid(CBlockIndex* pindex, std::string& strError, bool fCheckCollateral = true);

    bool IsEstablished()
    {
        // Proposals must be at least a day old to make it into a budget
        if (Params().NetworkIDString() == CBaseChainParams::MAIN)
            return (nTime < GetTime() - (60 * 60 * 24));

        // For testing purposes - 5 minutes
        return (nTime < GetTime() - (60 * 5));
    }
    bool IsPassing(const CBlockIndex* pindexPrev, int nBlockStartBudget, int nBlockEndBudget, int mnCount);

    std::string GetName() { return strProposalName; }
    std::string GetURL() { return strURL; }
    int GetBlockStart() { return nBlockStart; }
    int GetBlockEnd() { return nBlockEnd; }
    CScript GetPayee() { return address; }
    int GetTotalPaymentCount();
    int GetRemainingPaymentCount(CBlockIndex* pindex);
    int GetBlockStartCycle();
    int GetBlockCurrentCycle(CBlockIndex* pindex);
    int GetBlockEndCycle();
    double GetRatio();
    int GetYeas() const;
    int GetNays() const;
    int GetAbstains() const;
    CAmount GetAmount() { return nAmount; }
    void SetAllotted(CAmount nAllotedIn) { nAlloted = nAllotedIn; }
    CAmount GetAllotted() { return nAlloted; }

    void CleanAndRemove(bool fSignatureCheck);

    uint256 GetHash() const
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << strProposalName;
        ss << strURL;
        ss << nBlockStart;
        ss << nBlockEnd;
        ss << nAmount;
        ss << address;
        uint256 h1 = ss.GetHash();

        return h1;
    }

    SERIALIZE_METHODS(CBudgetProposal, obj)
    {
        READWRITE(LIMITED_STRING(obj.strProposalName, 20));
        READWRITE(LIMITED_STRING(obj.strURL, 64));
        READWRITE(obj.nTime);
        READWRITE(obj.nBlockStart);
        READWRITE(obj.nBlockEnd);
        READWRITE(obj.nAmount);
        READWRITE(*(CScriptBase*)(&obj.address));
        READWRITE(obj.nTime);
        READWRITE(obj.nFeeTXHash);
        READWRITE(obj.mapVotes);
    }
};

// Proposals are cast then sent to peers with this object, which leaves the votes out
class CBudgetProposalBroadcast : public CBudgetProposal {
public:
    CBudgetProposalBroadcast()
        : CBudgetProposal()
    {
    }
    CBudgetProposalBroadcast(const CBudgetProposal& other)
        : CBudgetProposal(other)
    {
    }
    CBudgetProposalBroadcast(const CBudgetProposalBroadcast& other)
        : CBudgetProposal(other)
    {
    }
    CBudgetProposalBroadcast(std::string strProposalNameIn, std::string strURLIn, int nPaymentCount, CScript addressIn, CAmount nAmountIn, int nBlockStartIn, uint256 nFeeTXHashIn);

    void swap(CBudgetProposalBroadcast& first, CBudgetProposalBroadcast& second) // nothrow
    {
        // enable ADL (not necessary in our case, but good practice)
        using std::swap;

        // by swapping the members of two classes,
        // the two classes are effectively swapped
        swap(first.strProposalName, second.strProposalName);
        swap(first.nBlockStart, second.nBlockStart);
        swap(first.strURL, second.strURL);
        swap(first.nBlockEnd, second.nBlockEnd);
        swap(first.nAmount, second.nAmount);
        swap(first.address, second.address);
        swap(first.nTime, second.nTime);
        swap(first.nFeeTXHash, second.nFeeTXHash);
        first.mapVotes.swap(second.mapVotes);
    }

    CBudgetProposalBroadcast& operator=(CBudgetProposalBroadcast from)
    {
        swap(*this, from);
        return *this;
    }

    void Relay(CConnman* connman);

    SERIALIZE_METHODS(CBudgetProposalBroadcast, obj)
    {
        READWRITE(LIMITED_STRING(obj.strProposalName, 20));
        READWRITE(LIMITED_STRING(obj.strURL, 64));
        READWRITE(obj.nTime);
        READWRITE(obj.nBlockStart);
        READWRITE(obj.nBlockEnd);
        READWRITE(obj.nAmount);
        READWRITE(*(CScriptBase*)(&obj.address));
        READWRITE(obj.nFeeTXHash);
    }
};

#endif
