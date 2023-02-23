#ifndef MN_PROCESSING_H
#define MN_PROCESSING_H

#include <chainparams.h>
#include <masternode/init.h>
#include <masternode/activemasternode.h>
#include <masternode/masternode.h>
#include <masternode/masternodeman.h>
#include <masternode/masternodesigner.h>
#include <masternode/masternode-payments.h>
#include <masternode/masternode-sync.h>
#include <masternode/spork.h>
#include <net.h>
#include <net_processing.h>
#include <netmessagemaker.h>
#include <txmempool.h>

void ProcessGetDataMasternodeTypes(CNode* pfrom, const CChainParams& chainparams, CConnman* connman, const CTxMemPool& mempool, const CInv& inv, bool& pushed) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

#endif // MN_PROCESSING_H

