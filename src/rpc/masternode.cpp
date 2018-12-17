// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemasternode.h"
#include "base58.h"
#include "clientversion.h"
#include "init.h"
#include "netbase.h"
#include "validation.h"
#include "masternode-payments.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#ifdef ENABLE_WALLET
#include "privatesend-client.h"
#endif // ENABLE_WALLET
#include "privatesend-server.h"
#include "rpc/server.h"
#include "util.h"
#include "utilmoneystr.h"
#include "txmempool.h"

#include "evo/specialtx.h"
#include "evo/deterministicmns.h"

#include <fstream>
#include <iomanip>
#include <univalue.h>

UniValue masternodelist(const JSONRPCRequest& request);

bool EnsureWalletIsAvailable(bool avoidException);

#ifdef ENABLE_WALLET
void EnsureWalletIsUnlocked();

UniValue privatesend(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "privatesend \"command\"\n"
            "\nArguments:\n"
            "1. \"command\"        (string or set of strings, required) The command to execute\n"
            "\nAvailable commands:\n"
            "  start       - Start mixing\n"
            "  stop        - Stop mixing\n"
            "  reset       - Reset mixing\n"
            );

    if (fMasternodeMode)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Client-side mixing is not supported on masternodes");

    if (request.params[0].get_str() == "start") {
        {
            LOCK(pwalletMain->cs_wallet);
            if (pwalletMain->IsLocked(true))
                throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please unlock wallet for mixing with walletpassphrase first.");
        }

        privateSendClient.fEnablePrivateSend = true;
        bool result = privateSendClient.DoAutomaticDenominating(*g_connman);
        return "Mixing " + (result ? "started successfully" : ("start failed: " + privateSendClient.GetStatuses() + ", will retry"));
    }

    if (request.params[0].get_str() == "stop") {
        privateSendClient.fEnablePrivateSend = false;
        return "Mixing was stopped";
    }

    if (request.params[0].get_str() == "reset") {
        privateSendClient.ResetPool();
        return "Mixing was reset";
    }

    return "Unknown command, please see \"help privatesend\"";
}
#endif // ENABLE_WALLET

UniValue getpoolinfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getpoolinfo\n"
            "Returns an object containing mixing pool related information.\n");

#ifdef ENABLE_WALLET
    CPrivateSendBaseManager* pprivateSendBaseManager = fMasternodeMode ? (CPrivateSendBaseManager*)&privateSendServer : (CPrivateSendBaseManager*)&privateSendClient;

    UniValue obj(UniValue::VOBJ);
    // TODO:
    // obj.push_back(Pair("state",             pprivateSendBase->GetStateString()));
    obj.push_back(Pair("queue",             pprivateSendBaseManager->GetQueueSize()));
    // obj.push_back(Pair("entries",           pprivateSendBase->GetEntriesCount()));
    obj.push_back(Pair("status",            privateSendClient.GetStatuses()));

    std::vector<CDeterministicMNCPtr> vecDmns;
    if (privateSendClient.GetMixingMasternodesInfo(vecDmns)) {
        UniValue pools(UniValue::VARR);
        for (const auto& dmn : vecDmns) {
            UniValue pool(UniValue::VOBJ);
            pool.push_back(Pair("outpoint",      dmn->collateralOutpoint.ToStringShort()));
            pool.push_back(Pair("addr",          dmn->pdmnState->addr.ToString()));
            pools.push_back(pool);
        }
        obj.push_back(Pair("pools", pools));
    }

    if (pwalletMain) {
        obj.push_back(Pair("keys_left",     pwalletMain->nKeysLeftSinceAutoBackup));
        obj.push_back(Pair("warnings",      pwalletMain->nKeysLeftSinceAutoBackup < PRIVATESEND_KEYS_THRESHOLD_WARNING
                                                ? "WARNING: keypool is almost depleted!" : ""));
    }
#else // ENABLE_WALLET
    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("state",             privateSendServer.GetStateString()));
    obj.push_back(Pair("queue",             privateSendServer.GetQueueSize()));
    obj.push_back(Pair("entries",           privateSendServer.GetEntriesCount()));
#endif // ENABLE_WALLET

    return obj;
}

void masternode_list_help()
{
    throw std::runtime_error(
            "masternode list ( \"mode\" \"filter\" )\n"
            "Get a list of masternodes in different modes. This call is identical to masternodelist call.\n"
            "\nArguments:\n"
            "1. \"mode\"      (string, optional/required to use filter, defaults = json) The mode to run list in\n"
            "2. \"filter\"    (string, optional) Filter results. Partial match by outpoint by default in all modes,\n"
            "                                    additional matches in some modes are also available\n"
            "\nAvailable modes:\n"
            "  addr           - Print ip address associated with a masternode (can be additionally filtered, partial match)\n"
            "  full           - Print info in format 'status payee lastpaidtime lastpaidblock IP'\n"
            "                   (can be additionally filtered, partial match)\n"
            "  info           - Print info in format 'status payee IP'\n"
            "                   (can be additionally filtered, partial match)\n"
            "  json           - Print info in JSON format (can be additionally filtered, partial match)\n"
            "  lastpaidblock  - Print the last block height a node was paid on the network\n"
            "  lastpaidtime   - Print the last time a node was paid on the network\n"
            "  payee          - Print Dash address associated with a masternode (can be additionally filtered,\n"
            "                   partial match)\n"
            "  keyid          - Print the masternode (not collateral) key id\n"
            "  rank           - Print rank of a masternode based on current block\n"
            "  status         - Print masternode status: ENABED / POSE_BAN / OUTPOINT_SPENT\n"
            "                   (can be additionally filtered, partial match)\n"
        );
}

UniValue masternode_list(const JSONRPCRequest& request)
{
    if (request.fHelp)
        masternode_list_help();
    JSONRPCRequest newRequest = request;
    newRequest.params.setArray();
    // forward params but skip "list"
    for (unsigned int i = 1; i < request.params.size(); i++) {
        newRequest.params.push_back(request.params[i]);
    }
    return masternodelist(newRequest);
}

void masternode_connect_help()
{
    throw std::runtime_error(
            "masternode connect \"address\"\n"
            "Connect to given masternode\n"
            "\nArguments:\n"
            "1. \"address\"      (string, required) The address of the masternode to connect\n"
        );
}

UniValue masternode_connect(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 2)
        masternode_connect_help();

    std::string strAddress = request.params[1].get_str();

    CService addr;
    if (!Lookup(strAddress.c_str(), addr, 0, false))
        throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Incorrect masternode address %s", strAddress));

    // TODO: Pass CConnman instance somehow and don't use global variable.
    g_connman->OpenMasternodeConnection(CAddress(addr, NODE_NETWORK));
    if (!g_connman->IsConnected(CAddress(addr, NODE_NETWORK), CConnman::AllNodes))
        throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Couldn't connect to masternode %s", strAddress));

    return "successfully connected";
}

void masternode_count_help()
{
    throw std::runtime_error(
            "masternode count (\"mode\")\n"
            "  Get information about number of masternodes. Mode\n"
            "  usage is depricated, call without mode params returns\n"
            "  all values in JSON format.\n"
            "\nArguments:\n"
            "1. \"mode\"      (string, optional, DEPRICATED) Option to get number of masternodes in different states\n"
            "\nAvailable modes:\n"
            "  total         - total number of masternodes"
            "  ps            - number of PrivateSend compatible masternodes"
            "  enabled       - number of enabled masternodes"
            "  qualify       - number of qualified masternodes"
            "  all           - all above in one string"
        );
}

UniValue masternode_count(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 2)
        masternode_count_help();

    auto mnList = deterministicMNManager->GetListAtChainTip();
    int total = mnList.GetAllMNsCount();
    int enabled = mnList.GetValidMNsCount();

    if (request.params.size() == 1) {
        UniValue obj(UniValue::VOBJ);

        obj.push_back(Pair("total", total));
        obj.push_back(Pair("enabled", enabled));

        return obj;
    }

    std::string strMode = request.params[1].get_str();

    if (strMode == "total")
        return total;

    if (strMode == "enabled")
        return enabled;

    if (strMode == "all")
        return strprintf("Total: %d (Enabled: %d)",
            total, enabled);

    throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown mode value");
}

UniValue GetNextMasternodeForPayment(int heightShift)
{
    auto mnList = deterministicMNManager->GetListAtChainTip();
    auto payees = mnList.GetProjectedMNPayees(heightShift);
    if (payees.empty())
        return "unknown";
    auto payee = payees[heightShift - 9];
    CScript payeeScript = payee->pdmnState->scriptPayout;

    CTxDestination payeeDest;
    CBitcoinAddress payeeAddr;
    if (ExtractDestination(payeeScript, payeeDest)) {
        payeeAddr = CBitcoinAddress(payeeDest);
    }

    UniValue obj(UniValue::VOBJ);

    obj.push_back(Pair("height",        mnList.GetHeight() + heightShift));
    obj.push_back(Pair("IP:port",       payee->pdmnState->addr.ToString()));
    obj.push_back(Pair("proTxHash",     payee->proTxHash.ToString()));
    obj.push_back(Pair("outpoint",      payee->collateralOutpoint.ToStringShort()));
    obj.push_back(Pair("payee",         payeeAddr.IsValid() ? payeeAddr.ToString() : "UNKNOWN"));
    return obj;
}

void masternode_winner_help()
{
    throw std::runtime_error(
            "masternode winner\n"
            "Print info on next masternode winner to vote for\n"
        );
}

UniValue masternode_winner(const JSONRPCRequest& request)
{
    if (request.fHelp)
        masternode_winner_help();

    return GetNextMasternodeForPayment(10);
}

void masternode_current_help()
{
    throw std::runtime_error(
            "masternode current\n"
            "Print info on current masternode winner to be paid the next block (calculated locally)\n"
        );
}

UniValue masternode_current(const JSONRPCRequest& request)
{
    if (request.fHelp)
        masternode_current_help();

    return GetNextMasternodeForPayment(1);
}

#ifdef ENABLE_WALLET
void masternode_outputs_help()
{
    throw std::runtime_error(
            "masternode outputs\n"
            "Print masternode compatible outputs\n"
        );
}

UniValue masternode_outputs(const JSONRPCRequest& request)
{
    if (request.fHelp)
        masternode_outputs_help();

    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    // Find possible candidates
    std::vector<COutput> vPossibleCoins;
    pwalletMain->AvailableCoins(vPossibleCoins, true, NULL, false, ONLY_1000);

    UniValue obj(UniValue::VOBJ);
    for (const auto& out : vPossibleCoins) {
        obj.push_back(Pair(out.tx->GetHash().ToString(), strprintf("%d", out.i)));
    }

    return obj;
}

#endif // ENABLE_WALLET

void masternode_status_help()
{
    throw std::runtime_error(
            "masternode status\n"
            "Print masternode status information\n"
        );
}

UniValue masternode_status(const JSONRPCRequest& request)
{
    if (request.fHelp)
        masternode_status_help();

    if (!fMasternodeMode)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "This is not a masternode");

    UniValue mnObj(UniValue::VOBJ);

    // keep compatibility with legacy status for now (might get deprecated/removed later)
    mnObj.push_back(Pair("outpoint", activeMasternodeInfo.outpoint.ToStringShort()));
    mnObj.push_back(Pair("service", activeMasternodeInfo.service.ToString()));

    if (deterministicMNManager->IsDeterministicMNsSporkActive()) {
        auto dmn = activeMasternodeManager->GetDMN();
        if (dmn) {
            mnObj.push_back(Pair("proTxHash", dmn->proTxHash.ToString()));
            mnObj.push_back(Pair("collateralHash", dmn->collateralOutpoint.hash.ToString()));
            mnObj.push_back(Pair("collateralIndex", (int)dmn->collateralOutpoint.n));
            UniValue stateObj;
            dmn->pdmnState->ToJson(stateObj);
            mnObj.push_back(Pair("dmnState", stateObj));
        }
        mnObj.push_back(Pair("state", activeMasternodeManager->GetStateString()));
        mnObj.push_back(Pair("status", activeMasternodeManager->GetStatus()));
    }
    return mnObj;
}

void masternode_winners_help()
{
    throw std::runtime_error(
            "masternode winners ( count \"filter\" )\n"
            "Print list of masternode winners\n"
            "\nArguments:\n"
            "1. count        (numeric, optional) number of last winners to return\n"
            "2. filter       (string, optional) filter for returned winners\n"
        );
}

UniValue masternode_winners(const JSONRPCRequest& request)
{
    if (request.fHelp)
        masternode_winners_help();

    int nHeight;
    {
        LOCK(cs_main);
        CBlockIndex* pindex = chainActive.Tip();
        if (!pindex) return NullUniValue;

        nHeight = pindex->nHeight;
    }

    int nLast = 10;
    std::string strFilter = "";

    if (request.params.size() >= 2) {
        nLast = atoi(request.params[1].get_str());
    }

    if (request.params.size() == 3) {
        strFilter = request.params[2].get_str();
    }

    if (request.params.size() > 3)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'masternode winners ( \"count\" \"filter\" )'");

    UniValue obj(UniValue::VOBJ);
    auto mapPayments = GetRequiredPaymentsStrings(nHeight - nLast, nHeight + 20);
    for (const auto &p : mapPayments) {
        obj.push_back(Pair(strprintf("%d", p.first), p.second));
    }

    return obj;
}

[[ noreturn ]] void masternode_help()
{
    throw std::runtime_error(
        "masternode \"command\"...\n"
        "Set of commands to execute masternode related actions\n"
        "\nArguments:\n"
        "1. \"command\"        (string or set of strings, required) The command to execute\n"
        "\nAvailable commands:\n"
        "  count        - Get information about number of masternodes (DEPRECATED options: 'total', 'ps', 'enabled', 'qualify', 'all')\n"
        "  current      - Print info on current masternode winner to be paid the next block (calculated locally)\n"
#ifdef ENABLE_WALLET
        "  outputs      - Print masternode compatible outputs\n"
#endif // ENABLE_WALLET
        "  status       - Print masternode status information\n"
        "  list         - Print list of all known masternodes (see masternodelist for more info)\n"
        "  winner       - Print info on next masternode winner to vote for\n"
        "  winners      - Print list of masternode winners\n"
        );
}

UniValue masternode(const JSONRPCRequest& request)
{
    std::string strCommand;
    if (request.params.size() >= 1) {
        strCommand = request.params[0].get_str();
    }

#ifdef ENABLE_WALLET
    if (strCommand == "start-many")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "DEPRECATED, please use start-all instead");
#endif // ENABLE_WALLET

    if (request.fHelp && strCommand.empty()) {
        masternode_help();
    }

    if (strCommand == "list") {
        return masternode_list(request);
    } else if (strCommand == "connect") {
        return masternode_connect(request);
    } else if (strCommand == "count") {
        return masternode_count(request);
    } else if (strCommand == "current") {
        return masternode_current(request);
    } else if (strCommand == "winner") {
        return masternode_winner(request);
#ifdef ENABLE_WALLET
    } else if (strCommand == "outputs") {
        return masternode_outputs(request);
#endif // ENABLE_WALLET
    } else if (strCommand == "status") {
        return masternode_status(request);
    } else if (strCommand == "winners") {
        return masternode_winners(request);
    } else {
        masternode_help();
    }
}

UniValue masternodelist(const JSONRPCRequest& request)
{
    std::string strMode = "json";
    std::string strFilter = "";

    if (request.params.size() >= 1) strMode = request.params[0].get_str();
    if (request.params.size() == 2) strFilter = request.params[1].get_str();

    if (request.fHelp || (
                strMode != "addr" && strMode != "full" && strMode != "info" && strMode != "json" &&
                strMode != "lastpaidtime" && strMode != "lastpaidblock" &&
                strMode != "payee" && strMode != "pubkey" &&
                strMode != "status"))
    {
        masternode_list_help();
    }

    UniValue obj(UniValue::VOBJ);
    std::map<COutPoint, CMasternode> mapMasternodes = mnodeman.GetFullMasternodeMap();
    for (const auto& mnpair : mapMasternodes) {
        CMasternode mn = mnpair.second;
        std::string strOutpoint = mnpair.first.ToStringShort();

        CScript payeeScript;
        if (deterministicMNManager->IsDeterministicMNsSporkActive()) {
            auto dmn = deterministicMNManager->GetListAtChainTip().GetMNByCollateral(mn.outpoint);
            if (dmn) {
                payeeScript = dmn->pdmnState->scriptPayout;
            }
        } else {
            payeeScript = GetScriptForDestination(mn.keyIDCollateralAddress);
        }

        CTxDestination payeeDest;
        std::string payeeStr = "UNKOWN";
        if (ExtractDestination(payeeScript, payeeDest)) {
            payeeStr = CBitcoinAddress(payeeDest).ToString();
        }

        if (strMode == "addr") {
            std::string strAddress = mn.addr.ToString();
            if (strFilter !="" && strAddress.find(strFilter) == std::string::npos &&
                strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, strAddress));
        } else if (strMode == "full") {
            std::ostringstream streamFull;
            streamFull << std::setw(18) <<
                           mn.GetStatus() << " " <<
                           payeeStr << " " <<
                           mn.GetLastPaidTime() << " "  << std::setw(6) <<
                           mn.GetLastPaidBlock() << " " <<
                           mn.addr.ToString();
            std::string strFull = streamFull.str();
            if (strFilter !="" && strFull.find(strFilter) == std::string::npos &&
                strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, strFull));
        } else if (strMode == "info") {
            std::ostringstream streamInfo;
            streamInfo << std::setw(18) <<
                           mn.GetStatus() << " " <<
                           payeeStr << " " <<
                           mn.addr.ToString();
            std::string strInfo = streamInfo.str();
            if (strFilter !="" && strInfo.find(strFilter) == std::string::npos &&
                strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, strInfo));
        } else if (strMode == "json") {
            std::ostringstream streamInfo;
            streamInfo <<  mn.addr.ToString() << " " <<
                           CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString() << " " <<
                           mn.GetStatus() << " " <<
                           mn.GetLastPaidTime() << " " <<
                           mn.GetLastPaidBlock();
            std::string strInfo = streamInfo.str();
            if (strFilter !="" && strInfo.find(strFilter) == std::string::npos &&
                strOutpoint.find(strFilter) == std::string::npos) continue;
            UniValue objMN(UniValue::VOBJ);
            objMN.push_back(Pair("address", mn.addr.ToString()));
            objMN.push_back(Pair("payee", CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString()));
            objMN.push_back(Pair("status", mn.GetStatus()));
            objMN.push_back(Pair("lastpaidtime", mn.GetLastPaidTime()));
            objMN.push_back(Pair("lastpaidblock", mn.GetLastPaidBlock()));
            obj.push_back(Pair(strOutpoint, objMN));
        } else if (strMode == "lastpaidblock") {
            if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, mn.GetLastPaidBlock()));
        } else if (strMode == "lastpaidtime") {
            if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, mn.GetLastPaidTime()));
        } else if (strMode == "payee") {
            if (strFilter !="" && payeeStr.find(strFilter) == std::string::npos &&
                strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, payeeStr));
        } else if (strMode == "keyIDOwner") {
            if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, HexStr(mn.keyIDOwner)));
        } else if (strMode == "keyIDOperator") {
            if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, HexStr(mn.legacyKeyIDOperator)));
        } else if (strMode == "keyIDVoting") {
            if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, HexStr(mn.keyIDVoting)));
        } else if (strMode == "status") {
            std::string strStatus = mn.GetStatus();
            if (strFilter !="" && strStatus.find(strFilter) == std::string::npos &&
                strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, strStatus));
        }
    }

    return obj;
}

static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         okSafe argNames
  //  --------------------- ------------------------  -----------------------  ------ ----------
    { "dash",               "masternode",             &masternode,             true,  {} },
    { "dash",               "masternodelist",         &masternodelist,         true,  {} },
    { "dash",               "getpoolinfo",            &getpoolinfo,            true,  {} },
#ifdef ENABLE_WALLET
    { "dash",               "privatesend",            &privatesend,            false, {} },
#endif // ENABLE_WALLET
};

void RegisterMasternodeRPCCommands(CRPCTable &t)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
