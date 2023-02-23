// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/init.h>

#include <masternode/activemasternode.h>
#include <masternode/masternodeman.h>
#include <masternode/masternode-budget.h>
#include <masternode/masternode-payments.h>
#include <masternode/masternode-sync.h>
#include <masternode/spork.h>
#include <shutdown.h>
#include <util/system.h>

CActiveMasternode activeMasternode;
CBudgetManager budget;
CMasternodeMan mnodeman;
CMasternodePayments masternodePayments;
CMasternodeSync masternodeSync;
CSporkManager sporkManager;

std::thread masternodeThread;

void InitObjects(ChainstateManager* chainman)
{
    activeMasternode.Attach(chainman);
    budget.Attach(chainman);
    mnodeman.Attach(chainman);
    masternodePayments.Attach(chainman);
    masternodeSync.Attach(chainman);
    sporkManager.Attach(chainman);
}

void MasternodeThread(CConnman* connman)
{
    bool b = false;
    unsigned int c = 0;

    while (!b)
    {
        UninterruptibleSleep(std::chrono::milliseconds{1000});

        b = ShutdownRequested();
        masternodeSync.Process(connman);
        if (masternodeSync.IsBlockchainSynced())
        {
            c++;
            if (c % MASTERNODE_PING_SECONDS == 1) {
                activeMasternode.ManageStatus(connman);
            }
            if (c % 60 == 0) {
                mnodeman.CheckAndRemove();
                mnodeman.ProcessMasternodeConnections(*connman);
                masternodePayments.CleanPaymentList();
            }
        }
    }
}
