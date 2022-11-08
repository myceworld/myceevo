// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODE_INIT_H
#define MASTERNODE_INIT_H

#include <validation.h>

class CConnman;

class CActiveMasternode;
class CBudgetManager;
class CMasternodeMan;
class CMasternodePayments;
class CMasternodeSync;
class CSporkManager;

extern CActiveMasternode activeMasternode;
extern CBudgetManager budget;
extern CMasternodeMan mnodeman;
extern CMasternodePayments masternodePayments;
extern CMasternodeSync masternodeSync;
extern CSporkManager sporkManager;

extern std::thread masternodeThread;

void InitObjects(ChainstateManager* chainman);
void MasternodeThread(CConnman* connman);

#endif // MASTERNODE_INIT_H
