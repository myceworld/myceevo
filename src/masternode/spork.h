// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SPORK_H
#define SPORK_H

#include <base58.h>
#include <hash.h>
#include <key.h>
#include <net.h>
#include <sync.h>
#include <util/system.h>
#include <validation.h>

#include <boost/lexical_cast.hpp>
#include <protocol.h>

using namespace std;
using namespace boost;

/*
    Don't ever reuse these IDs for other sporks
    - This would result in old clients getting confused about which spork is for what

    Sporks 11,12, and 16 to be removed with 1st zerocoin release
*/
#define SPORK_START 10001
#define SPORK_END 10015

#define SPORK_2_SWIFTTX 10001
#define SPORK_3_SWIFTTX_BLOCK_FILTERING 10002
#define SPORK_5_MAX_VALUE 10004
#define SPORK_7_MASTERNODE_SCANNING 10006
#define SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT 10007
#define SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT 10008
#define SPORK_10_MASTERNODE_PAY_UPDATED_NODES 10009
#define SPORK_13_ENABLE_SUPERBLOCKS 10012
#define SPORK_14_NEW_PROTOCOL_ENFORCEMENT 10013
#define SPORK_15_NEW_PROTOCOL_ENFORCEMENT_2 10014
#define SPORK_16_ZEROCOIN_MAINTENANCE_MODE 10015

#define SPORK_2_SWIFTTX_DEFAULT 978307200 // 2001-1-1
#define SPORK_3_SWIFTTX_BLOCK_FILTERING_DEFAULT 1424217600 // 2015-2-18
#define SPORK_5_MAX_VALUE_DEFAULT 1000 // 1000 Myce
#define SPORK_7_MASTERNODE_SCANNING_DEFAULT 978307200 // 2001-1-1
#define SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT_DEFAULT 4070908800 // OFF
#define SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT_DEFAULT 4070908800 // OFF
#define SPORK_10_MASTERNODE_PAY_UPDATED_NODES_DEFAULT 4070908800 // OFF
#define SPORK_13_ENABLE_SUPERBLOCKS_DEFAULT 4070908800 // OFF
#define SPORK_14_NEW_PROTOCOL_ENFORCEMENT_DEFAULT 4070908800 // OFF
#define SPORK_15_NEW_PROTOCOL_ENFORCEMENT_2_DEFAULT 4070908800 // OFF
#define SPORK_16_ZEROCOIN_MAINTENANCE_MODE_DEFAULT 4070908800 // OFF

class CSporkMessage;
extern std::map<uint256, CSporkMessage> mapSporks;
extern std::map<int, CSporkMessage> mapSporksActive;

void LoadSporksFromDB();
void ProcessSpork(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman* connman);
int64_t GetSporkValue(int nSporkID);
bool IsSporkActive(int nSporkID);

//
// Spork Class
// Keeps track of all of the network spork settings
//

class CSporkMessage {
public:
    std::vector<unsigned char> vchSig;
    int nSporkID;
    int64_t nValue;
    int64_t nTimeSigned;

    uint256 GetHash()
    {
        CHashWriter s(SER_GETHASH, 0);
        s << nSporkID;
        s << nValue;
        s << nTimeSigned;
        return s.GetHash();
    }

    SERIALIZE_METHODS(CSporkMessage, obj)
    {
        READWRITE(obj.nSporkID);
        READWRITE(obj.nValue);
        READWRITE(obj.nTimeSigned);
        READWRITE(obj.vchSig);
    }
};

class CSporkManager {
private:
    std::vector<unsigned char> vchSig;
    std::string strMasterPrivKey;

    ChainstateManager* chainman{nullptr};

public:
    CSporkManager()
    {
    }

    /// Attach chainman pointer to class
    void Attach(ChainstateManager* other) {
        chainman = other;
    }

    std::string GetSporkNameByID(int id);
    int GetSporkIDByName(std::string strName);
    bool UpdateSpork(int nSporkID, int64_t nValue, CConnman* connman);
    bool SetPrivKey(std::string strPrivKey);
    bool CheckSignature(CSporkMessage& spork, bool fCheckSigner = false);
    bool Sign(CSporkMessage& spork);
    void Relay(CSporkMessage& msg, CConnman* connman);
};

#endif
