// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <init.h>
#include <key_io.h>
#include <masternode/activemasternode.h>
#include <masternode/init.h>
#include <masternode/masternode-budget.h>
#include <masternode/masternode-payments.h>
#include <masternode/masternodeconfig.h>
#include <masternode/masternodeman.h>
#include <masternode/masternode-sync.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <script/descriptor.h>
#include <streams.h>
#include <sync.h>
#include <txdb.h>
#include <txmempool.h>
#include <undo.h>
#include <univalue.h>
#include <util/check.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/translation.h>

#include <univalue.h>

#include <boost/tokenizer.hpp>
#include <fstream>

using namespace std;

static RPCHelpMan listmasternodes()
{
    return RPCHelpMan{"listmasternodes",
	"\nGet a list of masternodes in different modes. This call is identical to 'masternode list' call\n"
	"    \"rank\": n,           (numeric) Masternode Rank (or 0 if not enabled)\n"
	"    \"txhash\": \"hash\",    (string) Collateral transaction hash\n"
	"    \"outidx\": n,         (numeric) Collateral transaction output index\n"
	"    \"pubkey\": \"key\",   (string) Masternode public key used for message broadcasting\n"
	"    \"status\": s,         (string) Status (ENABLED/EXPIRED/REMOVE/etc)\n"
	"    \"addr\": \"addr\",      (string) Masternode Myce address\n"
	"    \"version\": v,        (numeric) Masternode protocol version\n"
	"    \"lastseen\": ttt,     (numeric) The time in seconds since epoch (Jan 1 1970 GMT) of the last seen\n"
	"    \"activetime\": ttt,   (numeric) The time in seconds since epoch (Jan 1 1970 GMT) masternode has been active\n"
	"    \"lastpaid\": ttt,     (numeric) The time in seconds since epoch (Jan 1 1970 GMT) masternode was last paid\n",
	{},
        RPCResult{RPCResult::Type::ANY, "", ""},
        RPCExamples{
                HelpExampleCli("listmasternodes", "")
            + HelpExampleRpc("listmasternodes", "")
        },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::string strFilter = "";
    if (request.params.size() == 1) strFilter = request.params[0].get_str();

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    Chainstate& active_chainstate = chainman.ActiveChainstate();
    CChain& active_chain = active_chainstate.m_chain;

    UniValue ret(UniValue::VARR);
    CBlockIndex* pindex = nullptr;
    int nHeight;
    {
        LOCK(cs_main);
        pindex = active_chain.Tip();
        if(!pindex) return 0;
        nHeight = pindex->nHeight;
    }

    std::vector<std::pair<int, CMasternode> > vMasternodeRanks = mnodeman.GetMasternodeRanks(pindex, nHeight);
    for (auto& s : vMasternodeRanks) {
        UniValue obj(UniValue::VOBJ);
        string strVin = s.second.vin.prevout.ToStringShort();
        string strTxHash = s.second.vin.prevout.hash.ToString();
        uint32_t oIdx = s.second.vin.prevout.n;

        CMasternode* mn = mnodeman.Find(s.second.vin);

        if (mn != NULL) {
            if (strFilter != "" && strTxHash.find(strFilter) == string::npos &&
                mn->Status().find(strFilter) == string::npos &&
                EncodeDestination(PKHash(mn->pubKeyCollateralAddress)).find(strFilter) == string::npos) continue;

            string strStatus = mn->Status();
            string strHost;
            uint16_t port;
            SplitHostPort(mn->addr.ToString(), port, strHost);

            CNetAddr node;
            node.SetSpecial(strHost);
            string strNetwork = GetNetworkName(node.GetNetwork());

            obj.pushKV("rank", (strStatus == "ENABLED" ? s.first : 0));
            obj.pushKV("network", strNetwork);
            obj.pushKV("txhash", strTxHash);
            obj.pushKV("outidx", (uint64_t)oIdx);
            obj.pushKV("pubkey", HexStr(mn->pubKeyMasternode));
            obj.pushKV("status", strStatus);
            obj.pushKV("addr", EncodeDestination(PKHash(mn->pubKeyCollateralAddress)));
            obj.pushKV("version", mn->protocolVersion);
            obj.pushKV("lastseen", (int64_t)mn->lastPing.sigTime);
            obj.pushKV("activetime", (int64_t)(mn->lastPing.sigTime - mn->sigTime));
            obj.pushKV("lastpaid", (int64_t)mn->GetLastPaid(pindex));

            ret.push_back(obj);
        }
    }

    return ret;
},
    };
}

static RPCHelpMan masternodeconnect()
{
    return RPCHelpMan{"masternodeconnect",
        "\nConnect to a given masternode.\n",
        {
                {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The address of the masternode to connect."},
        },
            RPCResult{RPCResult::Type::ANY, "", ""},
            RPCExamples{
                    HelpExampleCli("masternodeconnect", "1.1.1.1")
                + HelpExampleRpc("masternodeconnect", "1.1.1.1")
            },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    string strAddress = request.params[0].get_str();

    CService addr;
    addr.SetSpecial(strAddress);

    node::NodeContext& node = EnsureAnyNodeContext(request.context);
    if(!node.connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");
    // TODO: Pass CConnman instance somehow and don't use global variable.
    node.connman->OpenMasternodeConnection(CAddress(addr, NODE_NETWORK));
    CNode* pnode = node.connman->FindNode(CAddress(addr, NODE_NETWORK));
    if (pnode) {
        pnode->Release();
        return NullUniValue;
    } else {
        throw std::runtime_error("error connecting\n");
    }
},
    };
}

static RPCHelpMan getmasternodecount()
{
    return RPCHelpMan{"getmasternodecount",
        "\nConnect to a given masternode.\n",
        {},
            RPCResult{RPCResult::Type::ANY, "", ""},
            RPCExamples{
                    HelpExampleCli("getmasternodecount", "")
                + HelpExampleRpc("getmasternodecount", "")
            },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    Chainstate& active_chainstate = chainman.ActiveChainstate();
    CChain& active_chain = active_chainstate.m_chain;

    UniValue obj(UniValue::VOBJ);
    int nCount = 0;
    int ipv4 = 0, ipv6 = 0, onion = 0;

    if (active_chain.Tip())
        mnodeman.GetNextMasternodeInQueueForPayment(active_chain.Tip(), active_chain.Tip()->nHeight, true, nCount);

    mnodeman.CountNetworks(PROTOCOL_VERSION, ipv4, ipv6, onion);

    obj.pushKV("total", mnodeman.size());
    obj.pushKV("stable", mnodeman.stable_size());
    obj.pushKV("obfcompat", mnodeman.CountEnabled(PROTOCOL_VERSION));
    obj.pushKV("enabled", mnodeman.CountEnabled());
    obj.pushKV("inqueue", nCount);
    obj.pushKV("ipv4", ipv4);
    obj.pushKV("ipv6", ipv6);
    obj.pushKV("onion", onion);

    return obj;
},
    };
}

static RPCHelpMan masternodecurrent()
{
    return RPCHelpMan{"masternodecurrent",
        "\nGet current masternode winner\n",
        {},
            RPCResult{RPCResult::Type::ANY, "", ""},
            RPCExamples{
                    HelpExampleCli("masternodecurrent", "")
                + HelpExampleRpc("masternodecurrent", "")
            },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    CMasternode* winner = mnodeman.GetCurrentMasterNode(1);
    if (winner) {
        UniValue obj(UniValue::VOBJ);

        obj.pushKV("protocol", (int64_t)winner->protocolVersion);
        obj.pushKV("txhash", winner->vin.prevout.hash.ToString());
        obj.pushKV("pubkey", EncodeDestination(PKHash(winner->pubKeyCollateralAddress)));
        obj.pushKV("lastseen", (winner->lastPing == CMasternodePing()) ? winner->sigTime : (int64_t)winner->lastPing.sigTime);
        obj.pushKV("activeseconds", (winner->lastPing == CMasternodePing()) ? 0 : (int64_t)(winner->lastPing.sigTime - winner->sigTime));
        return obj;
    }

    throw std::runtime_error("unknown");
},
    };
}

static RPCHelpMan masternodedebug()
{
    return RPCHelpMan{"masternodedebug",
        "\nPrint masternode status\n",
        {},
            RPCResult{RPCResult::Type::ANY, "", ""},
            RPCExamples{
                    HelpExampleCli("masternodedebug", "")
                + HelpExampleRpc("masternodedebug", "")
            },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (activeMasternode.status != ACTIVE_MASTERNODE_INITIAL || !masternodeSync.IsSynced())
        return activeMasternode.GetStatus();

    CTxIn vin = CTxIn();
    CPubKey pubkey;
    CKey key;
    if (!activeMasternode.GetMasterNodeVin(vin, pubkey, key))
        throw std::runtime_error("Missing masternode input, please look at the documentation for instructions on masternode creation\n");
    else
        return activeMasternode.GetStatus();
},
    };
}

static RPCHelpMan startmasternode()
{
    return RPCHelpMan{"startmasternode",
        "\nStart a given masternode.\n",
        {},
            RPCResult{RPCResult::Type::ANY, "", ""},
            RPCExamples{
                    HelpExampleCli("startmasternode", "")
                + HelpExampleRpc("startmasternode", "")
            },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    node::NodeContext& node = EnsureAnyNodeContext(request.context);
    
    string strCommand;
    if (request.params.size() >= 1) {
        strCommand =request.params[0].get_str();

        // Backwards compatibility with legacy 'masternode' super-command forwarder
        if (strCommand == "start") strCommand = "local";
        if (strCommand == "start-alias") strCommand = "alias";
        if (strCommand == "start-all") strCommand = "all";
        if (strCommand == "start-many") strCommand = "many";
        if (strCommand == "start-missing") strCommand = "missing";
        if (strCommand == "start-disabled") strCommand = "disabled";
    }

    if (request.params.size() < 2 || request.params.size() > 3 ||
        (request.params.size() == 2 && (strCommand != "local" && strCommand != "all" && strCommand != "many" && strCommand != "missing" && strCommand != "disabled")) ||
        (request.params.size() == 3 && strCommand != "alias"))
        throw std::runtime_error(
            "startmasternode \"local|all|many|missing|disabled|alias\" lockwallet ( \"alias\" )\n"
            "\nAttempts to start one or more masternode(s)\n"

            "\nArguments:\n"
            "1. set         (string, required) Specify which set of masternode(s) to start.\n"
            "2. lockwallet  (boolean, required) Lock wallet after completion.\n"
            "3. alias       (string) Masternode alias. Required if using 'alias' as the set.\n"

            "\nResult: (for 'local' set):\n"
            "\"status\"     (string) Masternode status message\n"

            "\nResult: (for other sets):\n"
            "{\n"
            "  \"overall\": \"xxxx\",     (string) Overall status message\n"
            "  \"detail\": [\n"
            "    {\n"
            "      \"node\": \"xxxx\",    (string) Node name or alias\n"
            "      \"result\": \"xxxx\",  (string) 'success' or 'failed'\n"
            "      \"error\": \"xxxx\"    (string) Error message, if failed\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("startmasternode", "\"alias\" \"0\" \"my_mn\"") + HelpExampleRpc("startmasternode", "\"alias\" \"0\" \"my_mn\""));

    bool fLock = (request.params[1].get_str() == "true" ? true : false);

    if (strCommand == "local") {
        if (!fMasterNode) throw std::runtime_error("you must set masternode=1 in the configuration\n");

        if (activeMasternode.status != ACTIVE_MASTERNODE_STARTED) {
            activeMasternode.status = ACTIVE_MASTERNODE_INITIAL; // TODO: consider better way
            activeMasternode.ManageStatus(node.connman.get());
            auto pwallet = stakeWallet.GetStakingWallet();
            if (fLock)
                pwallet->Lock();
        }

        return activeMasternode.GetStatus();
    }

    if (strCommand == "all" || strCommand == "many" || strCommand == "missing" || strCommand == "disabled") {
        if ((strCommand == "missing" || strCommand == "disabled") &&
            (masternodeSync.RequestedMasternodeAssets <= MASTERNODE_SYNC_LIST ||
                masternodeSync.RequestedMasternodeAssets == MASTERNODE_SYNC_FAILED)) {
            throw std::runtime_error("You can't use this command until masternode list is synced\n");
        }

        std::vector<CMasternodeConfig::CMasternodeEntry> mnEntries;
        mnEntries = masternodeConfig.getEntries();

        int successful = 0;
        int failed = 0;

        UniValue resultsObj(UniValue::VARR);

        for (CMasternodeConfig::CMasternodeEntry mne : masternodeConfig.getEntries()) {
            string errorMessage;
            int nIndex;
            if(!mne.castOutputIndex(nIndex))
                continue;
            CTxIn vin = CTxIn(uint256S(mne.getTxHash()), uint32_t(nIndex));
            CMasternode* pmn = mnodeman.Find(vin);
            CMasternodeBroadcast mnb;

            if (pmn != NULL) {
                if (strCommand == "missing") continue;
                if (strCommand == "disabled" && pmn->IsEnabled()) continue;
            }

            bool result = activeMasternode.CreateBroadcast(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), errorMessage, mnb, node.connman.get());

            UniValue statusObj(UniValue::VOBJ);
            statusObj.pushKV("alias", mne.getAlias());
            statusObj.pushKV("result", result ? "success" : "failed");

            if (result) {
                successful++;
                statusObj.pushKV("error", "");
            } else {
                failed++;
                statusObj.pushKV("error", errorMessage);
            }

            resultsObj.push_back(statusObj);
        }

        auto pwallet = stakeWallet.GetStakingWallet();
        if (fLock)
            pwallet->Lock();

        UniValue returnObj(UniValue::VOBJ);
        returnObj.pushKV("overall", strprintf("Successfully started %d masternodes, failed to start %d, total %d", successful, failed, successful + failed));
        returnObj.pushKV("detail", resultsObj);

        return returnObj;
    }

    if (strCommand == "alias") {
        string alias =request.params[2].get_str();

        bool found = false;
        int successful = 0;
        int failed = 0;

        UniValue resultsObj(UniValue::VARR);
        UniValue statusObj(UniValue::VOBJ);
        statusObj.pushKV("alias", alias);

        for (CMasternodeConfig::CMasternodeEntry mne : masternodeConfig.getEntries()) {
            if (mne.getAlias() == alias) {
                found = true;
                string errorMessage;
                CMasternodeBroadcast mnb;

                bool result = activeMasternode.CreateBroadcast(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), errorMessage, mnb, node.connman.get());

                statusObj.pushKV("result", result ? "successful" : "failed");

                if (result) {
                    successful++;
                    mnodeman.UpdateMasternodeList(mnb, node.connman.get());
                    mnb.Relay(node.connman.get());
                } else {
                    failed++;
                    statusObj.pushKV("errorMessage", errorMessage);
                }
                break;
            }
        }

        if (!found) {
            failed++;
            statusObj.pushKV("result", "failed");
            statusObj.pushKV("error", "could not find alias in config. Verify with list-conf.");
        }

        resultsObj.push_back(statusObj);

        auto pwallet = stakeWallet.GetStakingWallet();
        if (fLock)
            pwallet->Lock();

        UniValue returnObj(UniValue::VOBJ);
        returnObj.pushKV("overall", strprintf("Successfully started %d masternodes, failed to start %d, total %d", successful, failed, successful + failed));
        returnObj.pushKV("detail", resultsObj);

        return returnObj;
    }
    return NullUniValue;
},
    };
}

static RPCHelpMan createmasternodekey()
{
    return RPCHelpMan{"createmasternodekey",
        "\nCreate a new masternode private key\n",
        {},
            RPCResult{RPCResult::Type::ANY, "", ""},
            RPCExamples{
                    HelpExampleCli("createmasternodekey", "")
                + HelpExampleRpc("createmasternodekey", "")
            },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    CKey secret;
    secret.MakeNewKey(false);

    return EncodeSecret(secret);
},
    };
}

static RPCHelpMan getmasternodeoutputs()
{
    return RPCHelpMan{"getmasternodeoutputs",
        "\nPrint all masternode transaction outputs\n",
        {},
            RPCResult{RPCResult::Type::ANY, "", ""},
            RPCExamples{
                    HelpExampleCli("getmasternodeoutputs", "")
                + HelpExampleRpc("getmasternodeoutputs", "")
            },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // Find possible candidates
    std::vector<COutput> possibleCoins = activeMasternode.SelectCoinsMasternode();

    UniValue ret(UniValue::VARR);
    for (COutput& out : possibleCoins) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("txhash", out.outpoint.hash.ToString());
        obj.pushKV("outputidx", out.outpoint.n);
        ret.push_back(obj);
    }

    return ret;
},
    };
}

static RPCHelpMan listmasternodeconf()
{
    return RPCHelpMan{"listmasternodeconf",
        "\nPrint masternode.conf in JSON format\n",
        {},
            RPCResult{RPCResult::Type::ANY, "", ""},
            RPCExamples{
                    HelpExampleCli("listmasternodeconf", "")
                + HelpExampleRpc("listmasternodeconf", "")
            },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    string strFilter = "";
    if (request.params.size() == 1) strFilter = request.params[0].get_str();

    std::vector<CMasternodeConfig::CMasternodeEntry> mnEntries;
    mnEntries = masternodeConfig.getEntries();

    UniValue ret(UniValue::VARR);

    for (CMasternodeConfig::CMasternodeEntry mne : masternodeConfig.getEntries()) {
        int nIndex;
        if(!mne.castOutputIndex(nIndex))
            continue;
        CTxIn vin = CTxIn(uint256S(mne.getTxHash()), uint32_t(nIndex));
        CMasternode* pmn = mnodeman.Find(vin);

        string strStatus = pmn ? pmn->Status() : "MISSING";

        if (strFilter != "" && mne.getAlias().find(strFilter) == string::npos &&
            mne.getIp().find(strFilter) == string::npos &&
            mne.getTxHash().find(strFilter) == string::npos &&
            strStatus.find(strFilter) == string::npos) continue;

        UniValue mnObj(UniValue::VOBJ);
        mnObj.pushKV("alias", mne.getAlias());
        mnObj.pushKV("address", mne.getIp());
        mnObj.pushKV("privateKey", mne.getPrivKey());
        mnObj.pushKV("txHash", mne.getTxHash());
        mnObj.pushKV("outputIndex", mne.getOutputIndex());
        mnObj.pushKV("status", strStatus);
        ret.push_back(mnObj);
    }

    return ret;
},
    };
}

static RPCHelpMan getmasternodestatus()
{
    return RPCHelpMan{"getmasternodestatus",
        "\nPrint masternode status\n",
        {},
            RPCResult{RPCResult::Type::ANY, "", ""},
            RPCExamples{
                    HelpExampleCli("getmasternodestatus", "")
                + HelpExampleRpc("getmasternodestatus", "")
            },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!fMasterNode) throw std::runtime_error("This is not a masternode");

    CMasternode* pmn = mnodeman.Find(activeMasternode.vin);

    if (pmn) {
        UniValue mnObj(UniValue::VOBJ);
        mnObj.pushKV("txhash", activeMasternode.vin.prevout.hash.ToString());
        mnObj.pushKV("outputidx", (uint64_t)activeMasternode.vin.prevout.n);
        mnObj.pushKV("netaddr", activeMasternode.service.ToString());
        mnObj.pushKV("addr", pmn->pubKeyCollateralAddress.GetID().ToString());
        mnObj.pushKV("status", activeMasternode.status);
        mnObj.pushKV("message", activeMasternode.GetStatus());
        return mnObj;
    }
    throw std::runtime_error("Masternode not found in the list of available masternodes. Current status: "
                        + activeMasternode.GetStatus());
},
    };
}

static RPCHelpMan getmasternodewinners()
{
    return RPCHelpMan{"getmasternodewinners",
        "\nPrint the masternode winners for the last n blocks\n",
        {
                {"blocks", RPCArg::Type::NUM, RPCArg::Optional::NO, "The number of blocks to print."},
        },
            RPCResult{RPCResult::Type::ANY, "", ""},
            RPCExamples{
                    HelpExampleCli("getmasternodewinners", "")
                + HelpExampleRpc("getmasternodewinners", "")
            },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    Chainstate& active_chainstate = chainman.ActiveChainstate();
    CChain& active_chain = active_chainstate.m_chain;

    CBlockIndex* pindex = nullptr;
    int nHeight;
    {
        LOCK(cs_main);
        pindex = active_chain.Tip();
        if(!pindex) return 0;
        nHeight = pindex->nHeight;
    }

    int nLast = 10;
    string strFilter = "";

    if (request.params.size() >= 1)
        nLast = atoi(request.params[0].get_str().c_str());

    if (request.params.size() == 2)
        strFilter = request.params[1].get_str().c_str();

    UniValue ret(UniValue::VARR);

    for (int i = nHeight - nLast; i < nHeight + 20; i++) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("nHeight", i);

        string strPayment = GetRequiredPaymentsString(i);
        if (strFilter != "" && strPayment.find(strFilter) == string::npos) continue;

        if (strPayment.find(',') != string::npos) {
            UniValue winner(UniValue::VARR);
            boost::char_separator<char> sep(",");
            boost::tokenizer< boost::char_separator<char> > tokens(strPayment, sep);
            for (const string& t : tokens) {
                UniValue addr(UniValue::VOBJ);
                std::size_t pos = t.find(":");
                string strAddress = t.substr(0,pos);
                uint64_t nVotes = atoi(t.substr(pos+1).c_str());
                addr.pushKV("address", strAddress);
                addr.pushKV("nVotes", nVotes);
                winner.push_back(addr);
            }
            obj.pushKV("winner", winner);
        } else if (strPayment.find("Unknown") == string::npos) {
            UniValue winner(UniValue::VOBJ);
            std::size_t pos = strPayment.find(":");
            string strAddress = strPayment.substr(0,pos);
            uint64_t nVotes = atoi(strPayment.substr(pos+1).c_str());
            winner.pushKV("address", strAddress);
            winner.pushKV("nVotes", nVotes);
            obj.pushKV("winner", winner);
        } else {
            UniValue winner(UniValue::VOBJ);
            winner.pushKV("address", strPayment);
            winner.pushKV("nVotes", 0);
            obj.pushKV("winner", winner);
        }

            ret.push_back(obj);
    }

    return ret;
},
    };
}

static RPCHelpMan getmasternodescores()
{
    return RPCHelpMan{"getmasternodescores",
        "\nPrint list of winning masternode by score\n",
        {
                {"blocks", RPCArg::Type::NUM, RPCArg::Optional::NO, "The number of blocks to print."},
        },
            RPCResult{RPCResult::Type::ANY, "", ""},
            RPCExamples{
                    HelpExampleCli("getmasternodescores", "")
                + HelpExampleRpc("getmasternodescores", "")
            },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    Chainstate& active_chainstate = chainman.ActiveChainstate();
    CChain& active_chain = active_chainstate.m_chain;

    CBlockIndex* pindex = nullptr;
    int nHeight;
    {
        LOCK(cs_main);
        pindex = active_chain.Tip();
        if(!pindex) return 0;
        nHeight = pindex->nHeight;
    }

    int nLast = 10;
    if (request.params.size() == 1) {
        try {
            nLast = std::stoi(request.params[0].get_str().c_str());
        } catch (const std::invalid_argument&) {
            throw std::runtime_error("Exception on param 2");
        }
    }

    UniValue obj(UniValue::VOBJ);
    std::vector<CMasternode> vMasternodes = mnodeman.GetFullMasternodeVector();
    for (int nHeight = active_chain.Tip()->nHeight - nLast; nHeight < active_chain.Tip()->nHeight + 20; nHeight++) {
        uint256 nHigh = uint256();
        CMasternode* pBestMasternode = NULL;
        for (CMasternode& mn : vMasternodes) {
            uint256 n = mn.CalculateScore(1, nHeight - 100);
            if (UintToArith256(n) > UintToArith256(nHigh)) {
                nHigh = n;
                pBestMasternode = &mn;
            }
        }
        if (pBestMasternode)
            obj.pushKV(strprintf("%d", nHeight), pBestMasternode->vin.prevout.hash.ToString().c_str());
    }

    return obj;
},
    };
}

void RegisterMasternodeRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"masternode", &listmasternodes},
        {"masternode", &masternodeconnect},
        {"masternode", &getmasternodecount},
        {"masternode", &masternodecurrent},
        {"masternode", &masternodedebug},
        {"masternode", &startmasternode},
        {"masternode", &createmasternodekey},
        {"masternode", &getmasternodeoutputs},
        {"masternode", &listmasternodeconf},
        {"masternode", &getmasternodestatus},
        {"masternode", &getmasternodewinners},
        {"masternode", &getmasternodescores},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
