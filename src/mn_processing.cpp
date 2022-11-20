#include <mn_processing.h>

void ProcessGetDataMasternodeTypes(CNode* pfrom, const CChainParams& chainparams, CConnman* connman, const CTxMemPool& mempool, const CInv& inv, bool& pushed)
{
    const CNetMsgMaker msgMaker(PROTOCOL_VERSION);

    if (!pushed && inv.type == MSG_SPORK) {
        if (mapSporks.count(inv.hash)) {
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::SPORK, mapSporks[inv.hash]));
            pushed = true;
        }
    }

    if (!pushed && inv.type == MSG_MASTERNODE_WINNER) {
        if (masternodePayments.mapMasternodePayeeVotes.count(inv.hash)) {
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::MNWINNER, masternodePayments.mapMasternodePayeeVotes[inv.hash]));
            pushed = true;
        }
    }

    if (!pushed && inv.type == MSG_MASTERNODE_ANNOUNCE) {
        if (mnodeman.mapSeenMasternodeBroadcast.count(inv.hash)) {
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::MNBROADCAST, mnodeman.mapSeenMasternodeBroadcast[inv.hash]));
            pushed = true;
        }
    }

    if (!pushed && inv.type == MSG_MASTERNODE_PING) {
        if (mnodeman.mapSeenMasternodePing.count(inv.hash)){
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::MNPING, mnodeman.mapSeenMasternodePing[inv.hash]));
            pushed = true;
        }
    }

    if (!pushed && inv.type == MSG_BUDGET_VOTE) {
        if (budget.mapSeenMasternodeBudgetVotes.count(inv.hash)) {
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::BUDGETVOTE, budget.mapSeenMasternodeBudgetVotes[inv.hash]));
            pushed = true;
        }
    }

    if (!pushed && inv.type == MSG_BUDGET_PROPOSAL) {
        if (budget.mapSeenMasternodeBudgetProposals.count(inv.hash)) {
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::BUDGETPROPOSAL, budget.mapSeenMasternodeBudgetProposals[inv.hash]));
            pushed = true;
        }
    }

    if (!pushed && inv.type == MSG_BUDGET_FINALIZED_VOTE) {
        if (budget.mapSeenFinalizedBudgetVotes.count(inv.hash)) {
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::FINALBUDGETVOTE, budget.mapSeenFinalizedBudgetVotes[inv.hash]));
            pushed = true;
        }
    }

    if (!pushed && inv.type == MSG_BUDGET_FINALIZED) {
        if (budget.mapSeenFinalizedBudgets.count(inv.hash)) {
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::FINALBUDGET, budget.mapSeenFinalizedBudgets[inv.hash]));
            pushed = true;
        }
    }
}
