// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/budgetdb.h>

#include <chainparams.h>
#include <clientversion.h>

//
// CBudgetDB
//

CBudgetDB::CBudgetDB()
{
    pathDB = gArgs.GetDataDirNet() / "budget.dat";
    strMagicMessage = "MasternodeBudget";
}

bool CBudgetDB::Write(const CBudgetManager& objToSave)
{
    int64_t nStart = GetTimeMillis();

    // serialize, checksum data up to that point, then append checksum
    CDataStream ssObj(SER_DISK, CLIENT_VERSION);
    ssObj << strMagicMessage;                   // masternode cache file specific magic message
    ssObj << Params().MessageStart();           // network specific magic number
    ssObj << objToSave;
    uint256 hash = Hash(ssObj);
    ssObj << hash;

    // open output file, and associate with CAutoFile
    FILE* file = fsbridge::fopen(pathDB, "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file", __func__);

    // Write and commit header, data
    try {
        fileout << ssObj;
    } catch (const std::exception& e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    fileout.fclose();

    LogPrint(BCLog::MNBUDGET,"Written info to budget.dat  %dms\n", GetTimeMillis() - nStart);

    return true;
}

CBudgetDB::ReadResult CBudgetDB::Read(CBudgetManager& objToLoad, bool fDryRun)
{
    int64_t nStart = GetTimeMillis();
    // open input file, and associate with CAutoFile
    FILE* file = fsbridge::fopen(pathDB, "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        error("%s : Failed to open file", __func__);
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = fs::file_size(pathDB);
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
    } catch (const std::exception& e) {
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
            error("%s : Invalid masternode cache magic message", __func__);
            return IncorrectMagicMessage;
        }


        // de-serialize file header (network specific magic number) and ..
        ssObj >> pchMsgTmp;

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp))) {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }

        // de-serialize data into CBudgetManager object
        ssObj >> objToLoad;
    } catch (const std::exception& e) {
        objToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    LogPrint(BCLog::MNBUDGET,"Loaded info from budget.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrint(BCLog::MNBUDGET,"%s\n", objToLoad.ToString());
    if (!fDryRun) {
        LogPrint(BCLog::MNBUDGET,"Budget manager - cleaning....\n");
        objToLoad.CheckAndRemove(nullptr, nullptr);
        LogPrint(BCLog::MNBUDGET,"Budget manager - result: %s\n", objToLoad.ToString());
    }

    return Ok;
}

void DumpBudgets(CBudgetManager& budgetman)
{
    int64_t nStart = GetTimeMillis();

    CBudgetDB budgetdb;
    CBudgetManager tempBudget;

    LogPrint(BCLog::MNBUDGET,"Verifying budget.dat format...\n");
    CBudgetDB::ReadResult readResult = budgetdb.Read(tempBudget, true);
    // there was an error and it was not an error on file opening => do not proceed
    if (readResult == CBudgetDB::FileError)
        LogPrint(BCLog::MNBUDGET,"Missing budgets file - budget.dat, will try to recreate\n");
    else if (readResult != CBudgetDB::Ok) {
        LogPrint(BCLog::MNBUDGET,"Error reading budget.dat: ");
        if (readResult == CBudgetDB::IncorrectFormat)
            LogPrint(BCLog::MNBUDGET,"magic is ok but data has invalid format, will try to recreate\n");
        else {
            LogPrint(BCLog::MNBUDGET,"file format is unknown or invalid, please fix it manually\n");
            return;
        }
    }
    LogPrint(BCLog::MNBUDGET,"Writing info to budget.dat...\n");
    budgetdb.Write(budgetman);

    LogPrint(BCLog::MNBUDGET,"Budget dump finished  %dms\n", GetTimeMillis() - nStart);
}

