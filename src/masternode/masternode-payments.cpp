// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/masternode-payments.h>

#include <addrman.h>
#include <key_io.h>
#include <masternode/init.h>
#include <masternode/activemasternode.h>
#include <masternode/masternode-budget.h>
#include <masternode/masternode-sync.h>
#include <masternode/masternodeman.h>
#include <masternode/masternodesigner.h>
#include <masternode/spork.h>
#include <sync.h>
#include <util/moneystr.h>
#include <util/system.h>

#include <boost/filesystem.hpp>

RecursiveMutex cs_vecPayments;
RecursiveMutex cs_mapMasternodeBlocks;
RecursiveMutex cs_mapMasternodePayeeVotes;

//
// CMasternodePaymentDB
//

CMasternodePaymentDB::CMasternodePaymentDB()
{
    pathDB = gArgs.GetDataDirNet() / "mnpayments.dat";
    strMagicMessage = "MasternodePayments";
}

bool CMasternodePaymentDB::Write(const CMasternodePayments& objToSave)
{
    int64_t nStart = GetTimeMillis();

    // serialize, checksum data up to that point, then append checksum
    CDataStream ssObj(SER_DISK, CLIENT_VERSION);
    ssObj << strMagicMessage; // masternode cache file specific magic message
    ssObj << Params().MessageStart(); // network specific magic number
    ssObj << objToSave;
    uint256 hash = Hash(ssObj);
    ssObj << hash;

    // open output file, and associate with CAutoFile
    FILE* file = fopen(pathDB.string().c_str(), "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file %s", __func__, pathDB.string());

    // Write and commit header, data
    try {
        fileout << ssObj;
    } catch (std::exception& e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    fileout.fclose();

    LogPrint(BCLog::MASTERNODE, "Written info to mnpayments.dat  %dms\n", GetTimeMillis() - nStart);

    return true;
}

CMasternodePaymentDB::ReadResult CMasternodePaymentDB::Read(CMasternodePayments& objToLoad, bool fDryRun)
{
    int64_t nStart = GetTimeMillis();
    // open input file, and associate with CAutoFile
    FILE* file = fopen(pathDB.string().c_str(), "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        error("%s : Failed to open file %s", __func__, pathDB.string());
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = std::filesystem::file_size(pathDB);
    int dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
        dataSize = 0;
    std::vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read(MakeWritableByteSpan(vchData));
        filein >> hashIn;
    } catch (std::exception& e) {
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return HashReadError;
    }
    filein.fclose();

    CDataStream ssObj(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssObj);
    if (hashIn != hashTmp) {
        error("%s : Checksum mismatch, data corrupted", __func__);
        return IncorrectHash;
    }

    unsigned char pchMsgTmp[4];
    std::string strMagicMessageTmp;
    try {
        // de-serialize file header (masternode cache file specific magic message) and ..
        ssObj >> strMagicMessageTmp;

        // ... verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp) {
            error("%s : Invalid masternode payement cache magic message", __func__);
            return IncorrectMagicMessage;
        }

        // de-serialize file header (network specific magic number) and ..
        ssObj >> pchMsgTmp;

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp))) {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }

        // de-serialize data into CMasternodePayments object
        ssObj >> objToLoad;
    } catch (std::exception& e) {
        objToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    LogPrint(BCLog::MASTERNODE, "Loaded info from mnpayments.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrint(BCLog::MASTERNODE, "  %s\n", objToLoad.ToString());
    if (!fDryRun) {
        LogPrint(BCLog::MASTERNODE, "Masternode payments manager - cleaning....\n");
        objToLoad.CleanPaymentList();
        LogPrint(BCLog::MASTERNODE, "Masternode payments manager - result:\n");
        LogPrint(BCLog::MASTERNODE, "  %s\n", objToLoad.ToString());
    }

    return Ok;
}

void DumpMasternodePayments()
{
    int64_t nStart = GetTimeMillis();

    CMasternodePaymentDB paymentdb;
    CMasternodePayments tempPayments;

    LogPrint(BCLog::MASTERNODE, "Verifying mnpayments.dat format...\n");
    CMasternodePaymentDB::ReadResult readResult = paymentdb.Read(tempPayments, true);
    // there was an error and it was not an error on file opening => do not proceed
    if (readResult == CMasternodePaymentDB::FileError)
        LogPrint(BCLog::MASTERNODE, "Missing budgets file - mnpayments.dat, will try to recreate\n");
    else if (readResult != CMasternodePaymentDB::Ok) {
        LogPrint(BCLog::MASTERNODE, "Error reading mnpayments.dat: ");
        if (readResult == CMasternodePaymentDB::IncorrectFormat)
            LogPrint(BCLog::MASTERNODE, "magic is ok but data has invalid format, will try to recreate\n");
        else {
            LogPrint(BCLog::MASTERNODE, "file format is unknown or invalid, please fix it manually\n");
            return;
        }
    }
    LogPrint(BCLog::MASTERNODE, "Writting info to mnpayments.dat...\n");
    paymentdb.Write(masternodePayments);

    LogPrint(BCLog::MASTERNODE, "Budget dump finished  %dms\n", GetTimeMillis() - nStart);
}

bool IsBlockValueValid(const CBlock& block, CAmount nExpectedValue, CAmount nMinted, int nHeight)
{
    if (!masternodeSync.IsSynced()) { // there is no budget data to use to check anything
        // super blocks will always be on these blocks, max 100 per budgeting
        if (nHeight % GetBudgetPaymentCycleBlocks() < 100) {
            return true;
        } else {
            if (nMinted > nExpectedValue) {
                return false;
            }
        }
    } else { // we're synced and have data so check the budget schedule

        // are these blocks even enabled
        if (!IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS)) {
            return nMinted <= nExpectedValue;
        }

        if (budget.IsBudgetPaymentBlock(nHeight)) {
            // the value of the block is evaluated in CheckBlock
            return true;
        } else {
            if (nMinted > nExpectedValue) {
                return false;
            }
        }
    }

    return true;
}

bool IsBlockPayeeValid(const CBlock& block, int nBlockHeight)
{
    TrxValidationStatus transactionStatus = TrxValidationStatus::InValid;

    if (!masternodeSync.IsSynced()) { // there is no budget data to use to check anything -- find the longest chain
        LogPrint(BCLog::MNPAYMENTS, "Client not synced, skipping block payee checks\n");
        return true;
    }

    const CTransactionRef& txNew = (block.IsProofOfStake() ? block.vtx[1] : block.vtx[0]);

    // check if it's a budget block
    if (IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS)) {
        if (budget.IsBudgetPaymentBlock(nBlockHeight)) {
            transactionStatus = budget.IsTransactionValid(txNew, nBlockHeight);
            if (transactionStatus == TrxValidationStatus::Valid) {
                return true;
            }

            if (transactionStatus == TrxValidationStatus::InValid) {
                LogPrint(BCLog::MASTERNODE, "Invalid budget payment detected %s\n", txNew->ToString());
                if (IsSporkActive(SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT))
                    return false;

                LogPrint(BCLog::MASTERNODE, "Budget enforcement is disabled, accepting block\n");
            }
        }
    }

    // If we end here the transaction was either TrxValidationStatus::InValid and Budget enforcement is disabled, or
    // a double budget payment (status = TrxValidationStatus::DoublePayment) was detected, or no/not enough masternode
    // votes (status = TrxValidationStatus::VoteThreshold) for a finalized budget were found
    // In all cases a masternode will get the payment for this block

    // check for masternode payee
    if (masternodePayments.IsTransactionValid(txNew, nBlockHeight))
        return true;
    LogPrint(BCLog::MASTERNODE, "Invalid mn payment detected %s\n", txNew->ToString());

    if (IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT))
        return false;
    LogPrint(BCLog::MASTERNODE, "Masternode payment enforcement is disabled, accepting block\n");

    return true;
}

void FillBlockPayee(int nBlockHeight, CMutableTransaction& txNew, CAmount nFees, bool fProofOfStake, bool fZYCEStake)
{
    if (IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS) && budget.IsBudgetPaymentBlock(nBlockHeight)) {
        budget.FillBlockPayee(nBlockHeight, txNew, nFees, fProofOfStake);
    } else {
        masternodePayments.FillBlockPayee(txNew, nFees, fProofOfStake, fZYCEStake);
    }
}

std::string GetRequiredPaymentsString(int nBlockHeight)
{
    if (IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS) && budget.IsBudgetPaymentBlock(nBlockHeight)) {
        return budget.GetRequiredPaymentsString(nBlockHeight);
    } else {
        return masternodePayments.GetRequiredPaymentsString(nBlockHeight);
    }
}

void CMasternodePayments::FillBlockPayee(CMutableTransaction& txNew, int64_t nFees, bool fProofOfStake, bool fZYCEStake)
{
    CBlockIndex* pindexPrev = chainman->ActiveChain().Tip();
    if (!pindexPrev)
        return;

    bool hasPayment = true;
    CScript payee;

    // spork
    if (!masternodePayments.GetBlockPayee(pindexPrev->nHeight + 1, payee)) {
        // no masternode detected
        CMasternode* winningNode = mnodeman.GetCurrentMasterNode(1);
        if (winningNode) {
            payee = GetScriptForDestination(PKHash(winningNode->pubKeyCollateralAddress));
        } else {
            LogPrint(BCLog::MASTERNODE, "CreateNewBlock: Failed to detect masternode to pay\n");
            hasPayment = false;
        }
    }

    const CChainParams& params = Params();
    CAmount blockValue = GetBlockSubsidy(pindexPrev->nHeight + 1, params, fProofOfStake);
    CAmount masternodePayment = GetMasternodePayment(pindexPrev->nHeight + 1, blockValue);

    if (hasPayment) {
        if (fProofOfStake) {
            /**For Proof Of Stake vout[0] must be null
             * Stake reward can be split into many different outputs, so we must
             * use vout.size() to align with several different cases.
             * An additional output is appended as the masternode payment
             */
            unsigned int i = txNew.vout.size();
            txNew.vout.resize(i + 1);
            txNew.vout[i].scriptPubKey = payee;
            txNew.vout[i].nValue = masternodePayment;

            // subtract mn payment from the stake reward
            if (i == 2) {
                // Majority of cases; do it quick and move on
                txNew.vout[i - 1].nValue -= masternodePayment;
            } else if (i > 2) {
                // special case, stake is split between (i-1) outputs
                unsigned int outputs = i - 1;
                CAmount mnPaymentSplit = masternodePayment / outputs;
                CAmount mnPaymentRemainder = masternodePayment - (mnPaymentSplit * outputs);
                for (unsigned int j = 1; j <= outputs; j++) {
                    txNew.vout[j].nValue -= mnPaymentSplit;
                }
                // in case it's not an even division, take the last bit of dust from the last one
                txNew.vout[outputs].nValue -= mnPaymentRemainder;
            }
        } else {
            txNew.vout.resize(2);
            txNew.vout[1].scriptPubKey = payee;
            txNew.vout[1].nValue = masternodePayment;
            txNew.vout[0].nValue = blockValue - masternodePayment;
        }

        CTxDestination address;
        ExtractDestination(payee, address);

        LogPrint(BCLog::MASTERNODE, "Masternode payment of %s to %s\n", FormatMoney(masternodePayment), EncodeDestination(address));
    } else {
        if (!fProofOfStake) {
            txNew.vout[0].nValue = blockValue;
        }
    }
}

int CMasternodePayments::GetMinMasternodePaymentsProto()
{
    return PROTOCOL_VERSION - 1;
}

void CMasternodePayments::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman* connman)
{
    if (!masternodeSync.IsBlockchainSynced())
        return;

    if (strCommand == NetMsgType::GETMNWINNERS) {

        int nCountNeeded;
        vRecv >> nCountNeeded;

        if (netfulfilledman.HasFulfilledRequest(pfrom->addr, NetMsgType::GETMNWINNERS)) {
           LogPrint(BCLog::MNPAYMENTS, "%s: mnget - peer already asked me for the list\n", __func__);
           return;
        }

        netfulfilledman.AddFulfilledRequest(pfrom->addr, NetMsgType::GETMNWINNERS);
        masternodePayments.Sync(pfrom, nCountNeeded, connman);
        LogPrint(BCLog::MNPAYMENTS, "%s: mnget - Sent Masternode winners to peer %i\n", __func__, pfrom->GetId());

    } else if (strCommand == NetMsgType::MNWINNER) {

        CMasternodePaymentWinner winner;
        vRecv >> winner;

        if (pfrom->nVersion < PROTOCOL_VERSION - 1)
            return;

        int nHeight;
        CBlockIndex* pindex;
        {
            TRY_LOCK(cs_main, locked);
            if (!locked || chainman->ActiveChain().Tip() == NULL)
                return;
            pindex = chainman->ActiveChain().Tip();
            nHeight = pindex->nHeight;
        }

        if (masternodePayments.mapMasternodePayeeVotes.count(winner.GetHash())) {
            LogPrint(BCLog::MNPAYMENTS, "mnw - Already seen - %s bestHeight %d\n", winner.GetHash().ToString(), nHeight);
            masternodeSync.AddedMasternodeWinner(winner.GetHash());
            return;
        }

        int nFirstBlock = nHeight - (mnodeman.CountEnabled() * 1.25);
        if (winner.nBlockHeight < nFirstBlock || winner.nBlockHeight > nHeight + 20) {
            LogPrint(BCLog::MNPAYMENTS, "mnw - winner out of range - FirstBlock %d Height %d bestHeight %d\n", nFirstBlock, winner.nBlockHeight, nHeight);
            return;
        }

        std::string strError = "";
        if (!winner.IsValid(pindex, pfrom, strError, connman)) {
            if (strError != "")
                LogPrint(BCLog::MASTERNODE, "mnw - invalid message - %s\n", strError);
            return;
        }

        if (!masternodePayments.CanVote(winner.vinMasternode.prevout, winner.nBlockHeight)) {
            LogPrint(BCLog::MASTERNODE, "mnw - masternode already voted - %s\n", winner.vinMasternode.prevout.ToStringShort());
            return;
        }

        CTxDestination address;
        ExtractDestination(winner.payee, address);
        LogPrint(BCLog::MNPAYMENTS,  "mnw - winning vote - Addr %s Height %d bestHeight %d - %s\n", EncodeDestination(address), winner.nBlockHeight, nHeight, winner.vinMasternode.prevout.ToStringShort());

        if (masternodePayments.AddWinningMasternode(winner)) {
            winner.Relay(connman);
            masternodeSync.AddedMasternodeWinner(winner.GetHash());
        }
    }
}

bool CMasternodePaymentWinner::Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode)
{
    std::string errorMessage;
    std::string strMasterNodeSignMessage;

    std::string strMessage = vinMasternode.prevout.ToStringShort() + std::to_string(nBlockHeight) + payee.ToString();

    if (!legacySigner.SignMessage(strMessage, vchSig, keyMasternode)) {
        LogPrint(BCLog::MASTERNODE, "CMasternodePing::Sign() - Error: %s\n", errorMessage);
        return false;
    }

    if (!legacySigner.VerifyMessage(pubKeyMasternode, vchSig, strMessage, errorMessage)) {
        LogPrint(BCLog::MASTERNODE, "CMasternodePing::Sign() - Error: %s\n", errorMessage);
        return false;
    }

    return true;
}

bool CMasternodePayments::GetBlockPayee(int nBlockHeight, CScript& payee)
{
    if (mapMasternodeBlocks.count(nBlockHeight)) {
        return mapMasternodeBlocks[nBlockHeight].GetPayee(payee);
    }

    return false;
}

// Is this masternode scheduled to get paid soon?
// -- Only look ahead up to 8 blocks to allow for propagation of the latest 2 winners
bool CMasternodePayments::IsScheduled(CMasternode& mn, int nNotBlockHeight)
{
    LOCK(cs_mapMasternodeBlocks);

    int nHeight;
    {
        TRY_LOCK(cs_main, locked);
        if (!locked || chainman->ActiveChain().Tip() == NULL)
            return false;
        nHeight = chainman->ActiveChain().Tip()->nHeight;
    }

    CScript mnpayee;
    mnpayee = GetScriptForDestination(PKHash(mn.pubKeyCollateralAddress));

    CScript payee;
    for (int64_t h = nHeight; h <= nHeight + 8; h++) {
        if (h == nNotBlockHeight)
            continue;
        if (mapMasternodeBlocks.count(h)) {
            if (mapMasternodeBlocks[h].GetPayee(payee)) {
                if (mnpayee == payee) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool CMasternodePayments::AddWinningMasternode(CMasternodePaymentWinner& winnerIn)
{
    uint256 blockHash{};
    if (!GetBlockHash(blockHash, winnerIn.nBlockHeight - 100, chainman->ActiveChain().Tip())) {
        return false;
    }

    {
        LOCK2(cs_mapMasternodePayeeVotes, cs_mapMasternodeBlocks);

        if (mapMasternodePayeeVotes.count(winnerIn.GetHash())) {
            return false;
        }

        mapMasternodePayeeVotes[winnerIn.GetHash()] = winnerIn;

        if (!mapMasternodeBlocks.count(winnerIn.nBlockHeight)) {
            CMasternodeBlockPayees blockPayees(winnerIn.nBlockHeight);
            mapMasternodeBlocks[winnerIn.nBlockHeight] = blockPayees;
        }
    }

    mapMasternodeBlocks[winnerIn.nBlockHeight].AddPayee(winnerIn.payee, 1);

    return true;
}

bool CMasternodeBlockPayees::IsTransactionValid(const CTransactionRef& txNew)
{
    LOCK(cs_vecPayments);

    //require at least 6 signatures
    int nMaxSignatures = 0;
    for (CMasternodePayee& payee : vecPayments)
        if (payee.nVotes >= nMaxSignatures && payee.nVotes >= MNPAYMENTS_SIGNATURES_REQUIRED)
            nMaxSignatures = payee.nVotes;

    // if we don't have at least 6 signatures on a payee, approve whichever is the longest chain
    if (nMaxSignatures < MNPAYMENTS_SIGNATURES_REQUIRED)
        return true;

    CBlockIndex *pindex = g_rpc_node->chainman->ActiveTip();
    CAmount blockValue = GetBlockSubsidy(pindex->pprev->nHeight + 1, Params().GetConsensus());

    std::string strPayeesPossible = "";
    CAmount requiredMasternodePayment = GetMasternodePayment(pindex->pprev->nHeight + 1, blockValue);

    for (CMasternodePayee& payee : vecPayments) {
        bool found = false;
        for (CTxOut out : txNew->vout) {
            if (payee.scriptPubKey == out.scriptPubKey) {
                if (out.nValue >= requiredMasternodePayment)
                    found = true;
                else
                    LogPrint(BCLog::MASTERNODE, "Masternode payment is out of drift range. Paid=%s Min=%s\n", FormatMoney(out.nValue), FormatMoney(requiredMasternodePayment));
            }
        }

        if (payee.nVotes >= MNPAYMENTS_SIGNATURES_REQUIRED) {
            if (found)
                return true;

            CTxDestination address1;
            ExtractDestination(payee.scriptPubKey, address1);
            CTxDestination address2(address1);

            if (strPayeesPossible == "") {
                strPayeesPossible += EncodeDestination(address2);
            } else {
                strPayeesPossible += "," + EncodeDestination(address2);
            }
        }
    }

    LogPrint(BCLog::MASTERNODE, "CMasternodePayments::IsTransactionValid - Missing required payment of %s to %s\n", FormatMoney(requiredMasternodePayment), strPayeesPossible);
    return false;
}

std::string CMasternodeBlockPayees::GetRequiredPaymentsString()
{
    LOCK(cs_vecPayments);

    std::string ret = "Unknown";

    for (CMasternodePayee& payee : vecPayments) {
        CTxDestination address;
        ExtractDestination(payee.scriptPubKey, address);
        if (ret != "Unknown") {
            ret += ", " + EncodeDestination(address) + ":" + std::to_string(payee.nVotes);
        } else {
            ret = EncodeDestination(address) + ":" + std::to_string(payee.nVotes);
        }
    }

    return ret;
}

std::string CMasternodePayments::GetRequiredPaymentsString(int nBlockHeight)
{
    LOCK(cs_mapMasternodeBlocks);

    if (mapMasternodeBlocks.count(nBlockHeight)) {
        return mapMasternodeBlocks[nBlockHeight].GetRequiredPaymentsString();
    }

    return "Unknown";
}

bool CMasternodePayments::IsTransactionValid(const CTransactionRef& txNew, int nBlockHeight)
{
    LOCK(cs_mapMasternodeBlocks);

    if (mapMasternodeBlocks.count(nBlockHeight)) {
        return mapMasternodeBlocks[nBlockHeight].IsTransactionValid(txNew);
    }

    return true;
}

void CMasternodePayments::CleanPaymentList()
{
    LOCK2(cs_mapMasternodePayeeVotes, cs_mapMasternodeBlocks);

    int nHeight;
    {
        TRY_LOCK(cs_main, locked);
        if (!locked || chainman->ActiveChain().Tip() == NULL)
            return;
        nHeight = chainman->ActiveChain().Tip()->nHeight;
    }

    // keep up to five cycles for historical sake
    int nLimit = std::max(int(mnodeman.size() * 1.25), 1000);

    std::map<uint256, CMasternodePaymentWinner>::iterator it = mapMasternodePayeeVotes.begin();
    while (it != mapMasternodePayeeVotes.end()) {
        CMasternodePaymentWinner winner = (*it).second;

        if (nHeight - winner.nBlockHeight > nLimit) {
            LogPrint(BCLog::MNPAYMENTS, "CMasternodePayments::CleanPaymentList - Removing old Masternode payment - block %d\n", winner.nBlockHeight);
            masternodeSync.mapSeenSyncMNW.erase((*it).first);
            mapMasternodePayeeVotes.erase(it++);
            mapMasternodeBlocks.erase(winner.nBlockHeight);
        } else {
            ++it;
        }
    }
}

bool CMasternodePaymentWinner::IsValid(CBlockIndex* pindex, CNode* pnode, std::string& strError, CConnman* connman)
{
    CMasternode* pmn = mnodeman.Find(vinMasternode);

    if (!pmn) {
        strError = strprintf("Unknown Masternode %s", vinMasternode.prevout.hash.ToString());
        LogPrint(BCLog::MASTERNODE, "CMasternodePaymentWinner::IsValid - %s\n", strError);
        mnodeman.AskForMN(pnode, vinMasternode, connman);
        return false;
    }

    if (pmn->protocolVersion < PROTOCOL_VERSION - 1) {
        strError = strprintf("Masternode protocol too old %d - req %d", pmn->protocolVersion, PROTOCOL_VERSION - 1);
        LogPrint(BCLog::MASTERNODE, "CMasternodePaymentWinner::IsValid - %s\n", strError);
        return false;
    }

    int n = mnodeman.GetMasternodeRank(pindex, vinMasternode, nBlockHeight - 100, PROTOCOL_VERSION - 1);

    if (n > MNPAYMENTS_SIGNATURES_TOTAL) {
        // It's common to have masternodes mistakenly think they are in the top 10
        //  We don't want to print all of these messages, or punish them unless they're way off
        if (n > MNPAYMENTS_SIGNATURES_TOTAL * 2) {
            strError = strprintf("Masternode not in the top %d (%d)", MNPAYMENTS_SIGNATURES_TOTAL * 2, n);
            LogPrint(BCLog::MASTERNODE, "CMasternodePaymentWinner::IsValid - %s\n", strError);
            // if (masternodeSync.IsSynced()) Misbehaving(pnode->GetId(), 20);
        }
        return false;
    }

    return true;
}

bool CMasternodePayments::ProcessBlock(CBlockIndex* pindex, int nBlockHeight, CConnman* connman)
{
    if (!fMasterNode)
        return false;

    // reference node - hybrid mode

    int n = mnodeman.GetMasternodeRank(pindex, activeMasternode.vin, nBlockHeight - 100, PROTOCOL_VERSION - 1);

    if (n == -1) {
        LogPrint(BCLog::MNPAYMENTS, "CMasternodePayments::ProcessBlock - Unknown Masternode\n");
        return false;
    }

    if (n > MNPAYMENTS_SIGNATURES_TOTAL) {
        LogPrint(BCLog::MNPAYMENTS, "CMasternodePayments::ProcessBlock - Masternode not in the top %d (%d)\n", MNPAYMENTS_SIGNATURES_TOTAL, n);
        return false;
    }

    if (nBlockHeight <= nLastBlockHeight)
        return false;

    CMasternodePaymentWinner newWinner(activeMasternode.vin);

    if (budget.IsBudgetPaymentBlock(nBlockHeight)) {
        // is budget payment block -- handled by the budgeting software
    } else {
        LogPrint(BCLog::MASTERNODE, "CMasternodePayments::ProcessBlock() Start nHeight %d - vin %s. \n", nBlockHeight, activeMasternode.vin.prevout.hash.ToString());

        // pay to the oldest MN that still had no payment but its input is old enough and it was active long enough
        int nCount = 0;
        CMasternode* pmn = mnodeman.GetNextMasternodeInQueueForPayment(pindex, nBlockHeight, true, nCount);

        if (pmn) {
            LogPrint(BCLog::MASTERNODE, "CMasternodePayments::ProcessBlock() Found by FindOldestNotInVec \n");

            newWinner.nBlockHeight = nBlockHeight;

            CScript payee = GetScriptForDestination(PKHash(pmn->pubKeyCollateralAddress));
            newWinner.AddPayee(payee);

            CTxDestination address;
            ExtractDestination(payee, address);

            LogPrint(BCLog::MASTERNODE, "CMasternodePayments::ProcessBlock() Winner payee %s nHeight %d. \n", EncodeDestination(address), newWinner.nBlockHeight);
        } else {
            LogPrint(BCLog::MASTERNODE, "CMasternodePayments::ProcessBlock() Failed to find masternode to pay\n");
        }
    }

    std::string errorMessage;
    CPubKey pubKeyMasternode;
    CKey keyMasternode;

    if (!legacySigner.GetKeysFromSecret(strMasterNodePrivKey, keyMasternode, pubKeyMasternode)) {
        LogPrint(BCLog::MASTERNODE, "CMasternodePayments::ProcessBlock() - Error upon calling GetKeysFromSecret.\n");
        return false;
    }

    LogPrint(BCLog::MASTERNODE, "CMasternodePayments::ProcessBlock() - AddWinningMasternode\n");

    if (AddWinningMasternode(newWinner)) {
        newWinner.Relay(connman);
        nLastBlockHeight = nBlockHeight;
        return true;
    }

    return false;
}

void CMasternodePaymentWinner::Relay(CConnman* connman)
{
    CInv inv(MSG_MASTERNODE_WINNER, GetHash());
    connman->RelayInv(inv);
}

bool CMasternodePaymentWinner::SignatureValid()
{
    CMasternode* pmn = mnodeman.Find(vinMasternode);

    if (pmn) {
        std::string strMessage = vinMasternode.prevout.ToStringShort() + std::to_string(nBlockHeight) + payee.ToString();

        std::string errorMessage = "";
        if (!legacySigner.VerifyMessage(pmn->pubKeyMasternode, vchSig, strMessage, errorMessage)) {
            return error("CMasternodePaymentWinner::SignatureValid() - Got bad Masternode address signature %s\n", vinMasternode.prevout.hash.ToString());
        }

        return true;
    }

    return false;
}

void CMasternodePayments::Sync(CNode* node, int nCountNeeded, CConnman* connman)
{
    LOCK(cs_mapMasternodePayeeVotes);

    int nHeight;
    CBlockIndex* pindex;
    {
        TRY_LOCK(cs_main, locked);
        if (!locked || chainman->ActiveChain().Tip() == NULL)
            return;
        pindex = chainman->ActiveChain().Tip();
        nHeight = pindex->nHeight;
    }

    int nCount = (mnodeman.CountEnabled() * 1.25);
    if (nCountNeeded > nCount)
        nCountNeeded = nCount;

    int nInvCount = 0;
    const CNetMsgMaker msgMaker(PROTOCOL_VERSION);
    std::map<uint256, CMasternodePaymentWinner>::iterator it = mapMasternodePayeeVotes.begin();
    while (it != mapMasternodePayeeVotes.end()) {
        CMasternodePaymentWinner winner = (*it).second;
        if (winner.nBlockHeight >= nHeight - nCountNeeded && winner.nBlockHeight <= nHeight + 20) {
            connman->PushMessage(node, msgMaker.Make(NetMsgType::INV, CInv(MSG_MASTERNODE_WINNER, winner.GetHash())));
            nInvCount++;
        }
        ++it;
    }
    connman->PushMessage(node, msgMaker.Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_MNW, nInvCount));
}

std::string CMasternodePayments::ToString() const
{
    std::ostringstream info;

    info << "Votes: " << (int)mapMasternodePayeeVotes.size() << ", Blocks: " << (int)mapMasternodeBlocks.size();

    return info.str();
}

int CMasternodePayments::GetOldestBlock()
{
    LOCK(cs_mapMasternodeBlocks);

    int nOldestBlock = std::numeric_limits<int>::max();

    std::map<int, CMasternodeBlockPayees>::iterator it = mapMasternodeBlocks.begin();
    while (it != mapMasternodeBlocks.end()) {
        if ((*it).first < nOldestBlock) {
            nOldestBlock = (*it).first;
        }
        it++;
    }

    return nOldestBlock;
}

int CMasternodePayments::GetNewestBlock()
{
    LOCK(cs_mapMasternodeBlocks);

    int nNewestBlock = 0;

    std::map<int, CMasternodeBlockPayees>::iterator it = mapMasternodeBlocks.begin();
    while (it != mapMasternodeBlocks.end()) {
        if ((*it).first > nNewestBlock) {
            nNewestBlock = (*it).first;
        }
        it++;
    }

    return nNewestBlock;
}
