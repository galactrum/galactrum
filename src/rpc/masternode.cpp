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
#include "masternodeconfig.h"
#include "masternodeman.h"
#ifdef ENABLE_WALLET
#include "privatesend-client.h"
#endif // ENABLE_WALLET
#include "privatesend-server.h"
#include "rpc/server.h"
#include "util.h"
#include "utilmoneystr.h"

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
            "cloaking \"command\"\n"
            "\nArguments:\n"
            "1. \"command\"        (string or set of strings, required) The command to execute\n"
            "\nAvailable commands:\n"
            "  start       - Start cloaking\n"
            "  stop        - Stop cloaking\n"
            "  reset       - Reset cloaking\n"
            );

    if(fMasternodeMode)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Client-side cloaking is not supported on masternodes");

    if(request.params[0].get_str() == "start") {
        {
            LOCK(pwalletMain->cs_wallet);
            if (pwalletMain->IsLocked(true))
                throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please unlock wallet for cloaking with walletpassphrase first.");
        }

        privateSendClient.fEnablePrivateSend = true;
        bool result = privateSendClient.DoAutomaticDenominating(*g_connman);
        return "Mixing " + (result ? "started successfully" : ("start failed: " + privateSendClient.GetStatus() + ", will retry"));
    }

    if(request.params[0].get_str() == "stop") {
        privateSendClient.fEnablePrivateSend = false;
        return "Mixing was stopped";
    }

    if(request.params[0].get_str() == "reset") {
        privateSendClient.ResetPool();
        return "Mixing was reset";
    }

    return "Unknown command, please see \"help cloaking\"";
}
#endif // ENABLE_WALLET

UniValue getpoolinfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getpoolinfo\n"
            "Returns an object containing cloaking pool related information.\n");

#ifdef ENABLE_WALLET
    CPrivateSendBase* pprivateSendBase = fMasternodeMode ? (CPrivateSendBase*)&privateSendServer : (CPrivateSendBase*)&privateSendClient;

    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("state",             pprivateSendBase->GetStateString()));
    obj.push_back(Pair("cloaking_mode",       (!fMasternodeMode && privateSendClient.fPrivateSendMultiSession) ? "multi-session" : "normal"));
    obj.push_back(Pair("queue",             pprivateSendBase->GetQueueSize()));
    obj.push_back(Pair("entries",           pprivateSendBase->GetEntriesCount()));
    obj.push_back(Pair("status",            privateSendClient.GetStatus()));

    masternode_info_t mnInfo;
    if (privateSendClient.GetMixingMasternodeInfo(mnInfo)) {
        obj.push_back(Pair("outpoint",      mnInfo.outpoint.ToStringShort()));
        obj.push_back(Pair("addr",          mnInfo.addr.ToString()));
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

    if (request.fHelp  ||
        (
#ifdef ENABLE_WALLET
            strCommand != "start-alias" && strCommand != "start-all" && strCommand != "start-missing" &&
         strCommand != "start-disabled" && strCommand != "outputs" &&
#endif // ENABLE_WALLET
         strCommand != "list" && strCommand != "list-conf" && strCommand != "count" &&
         strCommand != "debug" && strCommand != "current" && strCommand != "winner" && strCommand != "winners" && strCommand != "genkey" &&
         strCommand != "connect" && strCommand != "status"))
            throw std::runtime_error(
                "masternode \"command\"...\n"
                "Set of commands to execute masternode related actions\n"
                "\nArguments:\n"
                "1. \"command\"        (string or set of strings, required) The command to execute\n"
                "\nAvailable commands:\n"
                "  count        - Get information about number of masternodes (DEPRECATED options: 'total', 'ps', 'enabled', 'qualify', 'all')\n"
                "  current      - Print info on current masternode winner to be paid the next block (calculated locally)\n"
                "  genkey       - Generate new masternodeprivkey\n"
#ifdef ENABLE_WALLET
                "  outputs      - Print masternode compatible outputs\n"
                "  start-alias  - Start single remote masternode by assigned alias configured in masternode.conf\n"
                "  start-<mode> - Start remote masternodes configured in masternode.conf (<mode>: 'all', 'missing', 'disabled')\n"
#endif // ENABLE_WALLET
                "  status       - Print masternode status information\n"
                "  list         - Print list of all known masternodes (see masternodelist for more info)\n"
                "  list-conf    - Print masternode.conf in JSON format\n"
                "  winner       - Print info on next masternode winner to vote for\n"
                "  winners      - Print list of masternode winners\n"
                );

    if (strCommand == "list")
    {
        JSONRPCRequest newRequest = request;
        newRequest.params.setArray();
        // forward params but skip "list"
        for (unsigned int i = 1; i < request.params.size(); i++) {
            newRequest.params.push_back(request.params[i]);
        }
        return masternodelist(newRequest);
    }

    if(strCommand == "connect")
    {
        if (request.params.size() < 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Masternode address required");

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

    if (strCommand == "count")
    {
        if (request.params.size() > 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Too many parameters");

        int nCount;
        masternode_info_t mnInfo;
        mnodeman.GetNextMasternodeInQueueForPayment(true, nCount, mnInfo);

        int total = mnodeman.size();
        int ps = mnodeman.CountEnabled(MIN_PRIVATESEND_PEER_PROTO_VERSION);
        int enabled = mnodeman.CountEnabled();

        if (request.params.size() == 1) {
            UniValue obj(UniValue::VOBJ);

            obj.push_back(Pair("total", total));
            obj.push_back(Pair("ps_compatible", ps));
            obj.push_back(Pair("enabled", enabled));
            obj.push_back(Pair("qualify", nCount));

            return obj;
        }

        std::string strMode = request.params[1].get_str();

        if (strMode == "total")
            return total;

        if (strMode == "ps")
            return ps;

        if (strMode == "enabled")
            return enabled;

        if (strMode == "qualify")
            return nCount;

        if (strMode == "all")
            return strprintf("Total: %d (PS Compatible: %d / Enabled: %d / Qualify: %d)",
                total, ps, enabled, nCount);
    }

    if (strCommand == "current" || strCommand == "winner")
    {
        int nCount;
        int nHeight;
        masternode_info_t mnInfo;
        CBlockIndex* pindex = NULL;
        {
            LOCK(cs_main);
            pindex = chainActive.Tip();
        }
        nHeight = pindex->nHeight + (strCommand == "current" ? 1 : 10);
        mnodeman.UpdateLastPaid(pindex);

        if(!mnodeman.GetNextMasternodeInQueueForPayment(nHeight, true, nCount, mnInfo))
            return "unknown";

        UniValue obj(UniValue::VOBJ);

        obj.push_back(Pair("height",        nHeight));
        obj.push_back(Pair("IP:port",       mnInfo.addr.ToString()));
        obj.push_back(Pair("protocol",      mnInfo.nProtocolVersion));
        obj.push_back(Pair("outpoint",      mnInfo.outpoint.ToStringShort()));
        obj.push_back(Pair("payee",         CBitcoinAddress(mnInfo.pubKeyCollateralAddress.GetID()).ToString()));
        obj.push_back(Pair("lastseen",      mnInfo.nTimeLastPing));
        obj.push_back(Pair("activeseconds", mnInfo.nTimeLastPing - mnInfo.sigTime));
        return obj;
    }

#ifdef ENABLE_WALLET
    if (strCommand == "start-alias")
    {
        if (!EnsureWalletIsAvailable(request.fHelp))
            return NullUniValue;

        if (request.params.size() < 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Please specify an alias");

        {
            LOCK(pwalletMain->cs_wallet);
            EnsureWalletIsUnlocked();
        }

        std::string strAlias = request.params[1].get_str();

        bool fFound = false;

        UniValue statusObj(UniValue::VOBJ);
        statusObj.push_back(Pair("alias", strAlias));

        for (const auto& mne : masternodeConfig.getEntries()) {
            if(mne.getAlias() == strAlias) {
                fFound = true;
                std::string strError;
                CMasternodeBroadcast mnb;

                bool fResult = CMasternodeBroadcast::Create(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), strError, mnb);

                int nDoS;
                if (fResult && !mnodeman.CheckMnbAndUpdateMasternodeList(NULL, mnb, nDoS, *g_connman)) {
                    strError = "Failed to verify MNB";
                    fResult = false;
                }

                statusObj.push_back(Pair("result", fResult ? "successful" : "failed"));
                if(!fResult) {
                    statusObj.push_back(Pair("errorMessage", strError));
                }
                mnodeman.NotifyMasternodeUpdates(*g_connman);
                break;
            }
        }

        if(!fFound) {
            statusObj.push_back(Pair("result", "failed"));
            statusObj.push_back(Pair("errorMessage", "Could not find alias in config. Verify with list-conf."));
        }

        return statusObj;

    }

    if (strCommand == "start-all" || strCommand == "start-missing" || strCommand == "start-disabled")
    {
        if (!EnsureWalletIsAvailable(request.fHelp))
            return NullUniValue;

        {
            LOCK(pwalletMain->cs_wallet);
            EnsureWalletIsUnlocked();
        }

        if((strCommand == "start-missing" || strCommand == "start-disabled") && !masternodeSync.IsMasternodeListSynced()) {
            throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "You can't use this command until masternode list is synced");
        }

        int nSuccessful = 0;
        int nFailed = 0;

        UniValue resultsObj(UniValue::VOBJ);

        for (const auto& mne : masternodeConfig.getEntries()) {
            std::string strError;

            COutPoint outpoint = COutPoint(uint256S(mne.getTxHash()), (uint32_t)atoi(mne.getOutputIndex()));
            CMasternode mn;
            bool fFound = mnodeman.Get(outpoint, mn);
            CMasternodeBroadcast mnb;

            if(strCommand == "start-missing" && fFound) continue;
            if(strCommand == "start-disabled" && fFound && mn.IsEnabled()) continue;

            bool fResult = CMasternodeBroadcast::Create(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), strError, mnb);

            int nDoS;
            if (fResult && !mnodeman.CheckMnbAndUpdateMasternodeList(NULL, mnb, nDoS, *g_connman)) {
                strError = "Failed to verify MNB";
                fResult = false;
            }

            UniValue statusObj(UniValue::VOBJ);
            statusObj.push_back(Pair("alias", mne.getAlias()));
            statusObj.push_back(Pair("result", fResult ? "successful" : "failed"));

            if (fResult) {
                nSuccessful++;
            } else {
                nFailed++;
                statusObj.push_back(Pair("errorMessage", strError));
            }

            resultsObj.push_back(Pair("status", statusObj));
        }
        mnodeman.NotifyMasternodeUpdates(*g_connman);

        UniValue returnObj(UniValue::VOBJ);
        returnObj.push_back(Pair("overall", strprintf("Successfully started %d masternodes, failed to start %d, total %d", nSuccessful, nFailed, nSuccessful + nFailed)));
        returnObj.push_back(Pair("detail", resultsObj));

        return returnObj;
    }
#endif // ENABLE_WALLET

    if (strCommand == "genkey")
    {
        CKey secret;
        secret.MakeNewKey(false);

        return CBitcoinSecret(secret).ToString();
    }

    if (strCommand == "list-conf")
    {
        UniValue resultObj(UniValue::VOBJ);

        for (const auto& mne : masternodeConfig.getEntries()) {
            COutPoint outpoint = COutPoint(uint256S(mne.getTxHash()), (uint32_t)atoi(mne.getOutputIndex()));
            CMasternode mn;
            bool fFound = mnodeman.Get(outpoint, mn);

            std::string strStatus = fFound ? mn.GetStatus() : "MISSING";

            UniValue mnObj(UniValue::VOBJ);
            mnObj.push_back(Pair("alias", mne.getAlias()));
            mnObj.push_back(Pair("address", mne.getIp()));
            mnObj.push_back(Pair("privateKey", mne.getPrivKey()));
            mnObj.push_back(Pair("txHash", mne.getTxHash()));
            mnObj.push_back(Pair("outputIndex", mne.getOutputIndex()));
            mnObj.push_back(Pair("status", strStatus));
            resultObj.push_back(Pair("masternode", mnObj));
        }

        return resultObj;
    }

#ifdef ENABLE_WALLET
    if (strCommand == "outputs") {
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

    if (strCommand == "status")
    {
        if (!fMasternodeMode)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "This is not a masternode");

        UniValue mnObj(UniValue::VOBJ);

        mnObj.push_back(Pair("outpoint", activeMasternode.outpoint.ToStringShort()));
        mnObj.push_back(Pair("service", activeMasternode.service.ToString()));

        CMasternode mn;
        if(mnodeman.Get(activeMasternode.outpoint, mn)) {
            mnObj.push_back(Pair("payee", CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString()));
        }

        mnObj.push_back(Pair("status", activeMasternode.GetStatus()));
        return mnObj;
    }

    if (strCommand == "winners")
    {
        int nHeight;
        {
            LOCK(cs_main);
            CBlockIndex* pindex = chainActive.Tip();
            if(!pindex) return NullUniValue;

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

        for(int i = nHeight - nLast; i < nHeight + 20; i++) {
            std::string strPayment = GetRequiredPaymentsString(i);
            if (strFilter !="" && strPayment.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strprintf("%d", i), strPayment));
        }

        return obj;
    }

    return NullUniValue;
}

UniValue masternodelist(const JSONRPCRequest& request)
{
    std::string strMode = "json";
    std::string strFilter = "";

    if (request.params.size() >= 1) strMode = request.params[0].get_str();
    if (request.params.size() == 2) strFilter = request.params[1].get_str();

    if (request.fHelp || (
                strMode != "activeseconds" && strMode != "addr" && strMode != "daemon" && strMode != "full" && strMode != "info" && strMode != "json" &&
                strMode != "lastseen" && strMode != "lastpaidtime" && strMode != "lastpaidblock" &&
                strMode != "protocol" && strMode != "payee" && strMode != "pubkey" &&
                strMode != "rank" && strMode != "sentinel" && strMode != "status"))
    {
        throw std::runtime_error(
                "masternodelist ( \"mode\" \"filter\" )\n"
                "Get a list of masternodes in different modes\n"
                "\nArguments:\n"
                "1. \"mode\"      (string, optional/required to use filter, defaults = json) The mode to run list in\n"
                "2. \"filter\"    (string, optional) Filter results. Partial match by outpoint by default in all modes,\n"
                "                                    additional matches in some modes are also available\n"
                "\nAvailable modes:\n"
                "  activeseconds  - Print number of seconds masternode recognized by the network as enabled\n"
                "                   (since latest issued \"masternode start/start-many/start-alias\")\n"
                "  addr           - Print ip address associated with a masternode (can be additionally filtered, partial match)\n"
                "  daemon         - Print daemon version of a masternode (can be additionally filtered, exact match)\n"
                "  full           - Print info in format 'status protocol payee lastseen activeseconds lastpaidtime lastpaidblock IP'\n"
                "                   (can be additionally filtered, partial match)\n"
                "  info           - Print info in format 'status protocol payee lastseen activeseconds sentinelversion sentinelstate IP'\n"
                "                   (can be additionally filtered, partial match)\n"
                "  json           - Print info in JSON format (can be additionally filtered, partial match)\n"
                "  lastpaidblock  - Print the last block height a node was paid on the network\n"
                "  lastpaidtime   - Print the last time a node was paid on the network\n"
                "  lastseen       - Print timestamp of when a masternode was last seen on the network\n"
                "  payee          - Print Galactrum address associated with a masternode (can be additionally filtered,\n"
                "                   partial match)\n"
                "  protocol       - Print protocol of a masternode (can be additionally filtered, exact match)\n"
                "  pubkey         - Print the masternode (not collateral) public key\n"
                "  rank           - Print rank of a masternode based on current block\n"
                "  sentinel       - Print sentinel version of a masternode (can be additionally filtered, exact match)\n"
                "  status         - Print masternode status: PRE_ENABLED / ENABLED / EXPIRED / SENTINEL_PING_EXPIRED / NEW_START_REQUIRED /\n"
                "                   UPDATE_REQUIRED / POSE_BAN / OUTPOINT_SPENT (can be additionally filtered, partial match)\n"
                );
    }

    if (strMode == "full" || strMode == "json" || strMode == "lastpaidtime" || strMode == "lastpaidblock") {
        CBlockIndex* pindex = NULL;
        {
            LOCK(cs_main);
            pindex = chainActive.Tip();
        }
        mnodeman.UpdateLastPaid(pindex);
    }

    UniValue obj(UniValue::VOBJ);
    if (strMode == "rank") {
        CMasternodeMan::rank_pair_vec_t vMasternodeRanks;
        mnodeman.GetMasternodeRanks(vMasternodeRanks);
        for (const auto& rankpair : vMasternodeRanks) {
            std::string strOutpoint = rankpair.second.outpoint.ToStringShort();
            if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, rankpair.first));
        }
    } else {
        std::map<COutPoint, CMasternode> mapMasternodes = mnodeman.GetFullMasternodeMap();
        for (const auto& mnpair : mapMasternodes) {
            CMasternode mn = mnpair.second;
            std::string strOutpoint = mnpair.first.ToStringShort();
            if (strMode == "activeseconds") {
                if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, (int64_t)(mn.lastPing.sigTime - mn.sigTime)));
            } else if (strMode == "addr") {
                std::string strAddress = mn.addr.ToString();
                if (strFilter !="" && strAddress.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, strAddress));
            } else if (strMode == "daemon") {
                std::string strDaemon = mn.lastPing.nDaemonVersion > DEFAULT_DAEMON_VERSION ? FormatVersion(mn.lastPing.nDaemonVersion) : "Unknown";
                if (strFilter !="" && strDaemon.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, strDaemon));
            } else if (strMode == "sentinel") {
                std::string strSentinel = mn.lastPing.nSentinelVersion > DEFAULT_SENTINEL_VERSION ? SafeIntVersionToString(mn.lastPing.nSentinelVersion) : "Unknown";
                if (strFilter !="" && strSentinel.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, strSentinel));
            } else if (strMode == "full") {
                std::ostringstream streamFull;
                streamFull << std::setw(18) <<
                               mn.GetStatus() << " " <<
                               mn.nProtocolVersion << " " <<
                               CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString() << " " <<
                               (int64_t)mn.lastPing.sigTime << " " << std::setw(8) <<
                               (int64_t)(mn.lastPing.sigTime - mn.sigTime) << " " << std::setw(10) <<
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
                               mn.nProtocolVersion << " " <<
                               CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString() << " " <<
                               (int64_t)mn.lastPing.sigTime << " " << std::setw(8) <<
                               (int64_t)(mn.lastPing.sigTime - mn.sigTime) << " " <<
                               SafeIntVersionToString(mn.lastPing.nSentinelVersion) << " "  <<
                               (mn.lastPing.fSentinelIsCurrent ? "current" : "expired") << " " <<
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
                               mn.nProtocolVersion << " " <<
                               mn.lastPing.nDaemonVersion << " " <<
                               SafeIntVersionToString(mn.lastPing.nSentinelVersion) << " " <<
                               (mn.lastPing.fSentinelIsCurrent ? "current" : "expired") << " " <<
                               (int64_t)mn.lastPing.sigTime << " " <<
                               (int64_t)(mn.lastPing.sigTime - mn.sigTime) << " " <<
                               mn.GetLastPaidTime() << " " <<
                               mn.GetLastPaidBlock();
                std::string strInfo = streamInfo.str();
                if (strFilter !="" && strInfo.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                UniValue objMN(UniValue::VOBJ);
                objMN.push_back(Pair("address", mn.addr.ToString()));
                objMN.push_back(Pair("payee", CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString()));
                objMN.push_back(Pair("status", mn.GetStatus()));
                objMN.push_back(Pair("protocol", mn.nProtocolVersion));
                objMN.push_back(Pair("daemonversion", mn.lastPing.nDaemonVersion > DEFAULT_DAEMON_VERSION ? FormatVersion(mn.lastPing.nDaemonVersion) : "Unknown"));
                objMN.push_back(Pair("sentinelversion", mn.lastPing.nSentinelVersion > DEFAULT_SENTINEL_VERSION ? SafeIntVersionToString(mn.lastPing.nSentinelVersion) : "Unknown"));
                objMN.push_back(Pair("sentinelstate", (mn.lastPing.fSentinelIsCurrent ? "current" : "expired")));
                objMN.push_back(Pair("lastseen", (int64_t)mn.lastPing.sigTime));
                objMN.push_back(Pair("activeseconds", (int64_t)(mn.lastPing.sigTime - mn.sigTime)));
                objMN.push_back(Pair("lastpaidtime", mn.GetLastPaidTime()));
                objMN.push_back(Pair("lastpaidblock", mn.GetLastPaidBlock()));
                obj.push_back(Pair(strOutpoint, objMN));
            } else if (strMode == "lastpaidblock") {
                if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, mn.GetLastPaidBlock()));
            } else if (strMode == "lastpaidtime") {
                if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, mn.GetLastPaidTime()));
            } else if (strMode == "lastseen") {
                if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, (int64_t)mn.lastPing.sigTime));
            } else if (strMode == "payee") {
                CBitcoinAddress address(mn.pubKeyCollateralAddress.GetID());
                std::string strPayee = address.ToString();
                if (strFilter !="" && strPayee.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, strPayee));
            } else if (strMode == "protocol") {
                if (strFilter !="" && strFilter != strprintf("%d", mn.nProtocolVersion) &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, mn.nProtocolVersion));
            } else if (strMode == "pubkey") {
                if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, HexStr(mn.pubKeyMasternode)));
            } else if (strMode == "status") {
                std::string strStatus = mn.GetStatus();
                if (strFilter !="" && strStatus.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, strStatus));
            }
        }
    }
    return obj;
}

bool DecodeHexVecMnb(std::vector<CMasternodeBroadcast>& vecMnb, std::string strHexMnb) {

    if (!IsHex(strHexMnb))
        return false;

    std::vector<unsigned char> mnbData(ParseHex(strHexMnb));
    CDataStream ssData(mnbData, SER_NETWORK, PROTOCOL_VERSION);
    try {
        ssData >> vecMnb;
    }
    catch (const std::exception&) {
        return false;
    }

    return true;
}

UniValue masternodebroadcast(const JSONRPCRequest& request)
{
    std::string strCommand;
    if (request.params.size() >= 1)
        strCommand = request.params[0].get_str();

    if (request.fHelp  ||
        (
#ifdef ENABLE_WALLET
            strCommand != "create-alias" && strCommand != "create-all" &&
#endif // ENABLE_WALLET
            strCommand != "decode" && strCommand != "relay"))
        throw std::runtime_error(
                "masternodebroadcast \"command\"...\n"
                "Set of commands to create and relay masternode broadcast messages\n"
                "\nArguments:\n"
                "1. \"command\"        (string or set of strings, required) The command to execute\n"
                "\nAvailable commands:\n"
#ifdef ENABLE_WALLET
                "  create-alias  - Create single remote masternode broadcast message by assigned alias configured in masternode.conf\n"
                "  create-all    - Create remote masternode broadcast messages for all masternodes configured in masternode.conf\n"
#endif // ENABLE_WALLET
                "  decode        - Decode masternode broadcast message\n"
                "  relay         - Relay masternode broadcast message to the network\n"
                );

#ifdef ENABLE_WALLET
    if (strCommand == "create-alias")
    {
        if (!EnsureWalletIsAvailable(request.fHelp))
            return NullUniValue;

        // wait for reindex and/or import to finish
        if (fImporting || fReindex)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Wait for reindex and/or import to finish");

        if (request.params.size() < 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Please specify an alias");

        {
            LOCK(pwalletMain->cs_wallet);
            EnsureWalletIsUnlocked();
        }

        bool fFound = false;
        std::string strAlias = request.params[1].get_str();

        UniValue statusObj(UniValue::VOBJ);
        std::vector<CMasternodeBroadcast> vecMnb;

        statusObj.push_back(Pair("alias", strAlias));

        for (const auto& mne : masternodeConfig.getEntries()) {
            if(mne.getAlias() == strAlias) {
                fFound = true;
                std::string strError;
                CMasternodeBroadcast mnb;

                bool fResult = CMasternodeBroadcast::Create(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), strError, mnb, true);

                statusObj.push_back(Pair("result", fResult ? "successful" : "failed"));
                if(fResult) {
                    vecMnb.push_back(mnb);
                    CDataStream ssVecMnb(SER_NETWORK, PROTOCOL_VERSION);
                    ssVecMnb << vecMnb;
                    statusObj.push_back(Pair("hex", HexStr(ssVecMnb)));
                } else {
                    statusObj.push_back(Pair("errorMessage", strError));
                }
                break;
            }
        }

        if(!fFound) {
            statusObj.push_back(Pair("result", "not found"));
            statusObj.push_back(Pair("errorMessage", "Could not find alias in config. Verify with list-conf."));
        }

        return statusObj;

    }

    if (strCommand == "create-all")
    {
        if (!EnsureWalletIsAvailable(request.fHelp))
            return NullUniValue;

        // wait for reindex and/or import to finish
        if (fImporting || fReindex)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Wait for reindex and/or import to finish");

        {
            LOCK(pwalletMain->cs_wallet);
            EnsureWalletIsUnlocked();
        }

        int nSuccessful = 0;
        int nFailed = 0;

        UniValue resultsObj(UniValue::VOBJ);
        std::vector<CMasternodeBroadcast> vecMnb;

        for (const auto& mne : masternodeConfig.getEntries()) {
            std::string strError;
            CMasternodeBroadcast mnb;

            bool fResult = CMasternodeBroadcast::Create(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), strError, mnb, true);

            UniValue statusObj(UniValue::VOBJ);
            statusObj.push_back(Pair("alias", mne.getAlias()));
            statusObj.push_back(Pair("result", fResult ? "successful" : "failed"));

            if(fResult) {
                nSuccessful++;
                vecMnb.push_back(mnb);
            } else {
                nFailed++;
                statusObj.push_back(Pair("errorMessage", strError));
            }

            resultsObj.push_back(Pair("status", statusObj));
        }

        CDataStream ssVecMnb(SER_NETWORK, PROTOCOL_VERSION);
        ssVecMnb << vecMnb;
        UniValue returnObj(UniValue::VOBJ);
        returnObj.push_back(Pair("overall", strprintf("Successfully created broadcast messages for %d masternodes, failed to create %d, total %d", nSuccessful, nFailed, nSuccessful + nFailed)));
        returnObj.push_back(Pair("detail", resultsObj));
        returnObj.push_back(Pair("hex", HexStr(ssVecMnb.begin(), ssVecMnb.end())));

        return returnObj;
    }
#endif // ENABLE_WALLET

    if (strCommand == "decode")
    {
        if (request.params.size() != 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'masternodebroadcast decode \"hexstring\"'");

        std::vector<CMasternodeBroadcast> vecMnb;

        if (!DecodeHexVecMnb(vecMnb, request.params[1].get_str()))
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Masternode broadcast message decode failed");

        int nSuccessful = 0;
        int nFailed = 0;
        int nDos = 0;
        UniValue returnObj(UniValue::VOBJ);

        for (const auto& mnb : vecMnb) {
            UniValue resultObj(UniValue::VOBJ);

            if(mnb.CheckSignature(nDos)) {
                nSuccessful++;
                resultObj.push_back(Pair("outpoint", mnb.outpoint.ToStringShort()));
                resultObj.push_back(Pair("addr", mnb.addr.ToString()));
                resultObj.push_back(Pair("pubKeyCollateralAddress", CBitcoinAddress(mnb.pubKeyCollateralAddress.GetID()).ToString()));
                resultObj.push_back(Pair("pubKeyMasternode", CBitcoinAddress(mnb.pubKeyMasternode.GetID()).ToString()));
                resultObj.push_back(Pair("vchSig", EncodeBase64(&mnb.vchSig[0], mnb.vchSig.size())));
                resultObj.push_back(Pair("sigTime", mnb.sigTime));
                resultObj.push_back(Pair("protocolVersion", mnb.nProtocolVersion));
                resultObj.push_back(Pair("nLastDsq", mnb.nLastDsq));

                UniValue lastPingObj(UniValue::VOBJ);
                lastPingObj.push_back(Pair("outpoint", mnb.lastPing.masternodeOutpoint.ToStringShort()));
                lastPingObj.push_back(Pair("blockHash", mnb.lastPing.blockHash.ToString()));
                lastPingObj.push_back(Pair("sigTime", mnb.lastPing.sigTime));
                lastPingObj.push_back(Pair("vchSig", EncodeBase64(&mnb.lastPing.vchSig[0], mnb.lastPing.vchSig.size())));

                resultObj.push_back(Pair("lastPing", lastPingObj));
            } else {
                nFailed++;
                resultObj.push_back(Pair("errorMessage", "Masternode broadcast signature verification failed"));
            }

            returnObj.push_back(Pair(mnb.GetHash().ToString(), resultObj));
        }

        returnObj.push_back(Pair("overall", strprintf("Successfully decoded broadcast messages for %d masternodes, failed to decode %d, total %d", nSuccessful, nFailed, nSuccessful + nFailed)));

        return returnObj;
    }

    if (strCommand == "relay")
    {
        if (request.params.size() < 2 || request.params.size() > 3)
            throw JSONRPCError(RPC_INVALID_PARAMETER,   "masternodebroadcast relay \"hexstring\"\n"
                                                        "\nArguments:\n"
                                                        "1. \"hex\"      (string, required) Broadcast messages hex string\n");

        std::vector<CMasternodeBroadcast> vecMnb;

        if (!DecodeHexVecMnb(vecMnb, request.params[1].get_str()))
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Masternode broadcast message decode failed");

        int nSuccessful = 0;
        int nFailed = 0;
        UniValue returnObj(UniValue::VOBJ);

        // verify all signatures first, bailout if any of them broken
        for (const auto& mnb : vecMnb) {
            UniValue resultObj(UniValue::VOBJ);

            resultObj.push_back(Pair("outpoint", mnb.outpoint.ToStringShort()));
            resultObj.push_back(Pair("addr", mnb.addr.ToString()));

            int nDos = 0;
            bool fResult;
            if (mnb.CheckSignature(nDos)) {
                fResult = mnodeman.CheckMnbAndUpdateMasternodeList(NULL, mnb, nDos, *g_connman);
                mnodeman.NotifyMasternodeUpdates(*g_connman);
            } else fResult = false;

            if(fResult) {
                nSuccessful++;
                resultObj.push_back(Pair(mnb.GetHash().ToString(), "successful"));
            } else {
                nFailed++;
                resultObj.push_back(Pair("errorMessage", "Masternode broadcast signature verification failed"));
            }

            returnObj.push_back(Pair(mnb.GetHash().ToString(), resultObj));
        }

        returnObj.push_back(Pair("overall", strprintf("Successfully relayed broadcast messages for %d masternodes, failed to relay %d, total %d", nSuccessful, nFailed, nSuccessful + nFailed)));

        return returnObj;
    }

    return NullUniValue;
}

UniValue sentinelping(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "sentinelping version\n"
            "\nSentinel ping.\n"
            "\nArguments:\n"
            "1. version           (string, required) Sentinel version in the form \"x.x.x\"\n"
            "\nResult:\n"
            "state                (boolean) Ping result\n"
            "\nExamples:\n"
            + HelpExampleCli("sentinelping", "1.0.2")
            + HelpExampleRpc("sentinelping", "1.0.2")
        );
    }

    activeMasternode.UpdateSentinelPing(StringVersionToInt(request.params[0].get_str()));
    return true;
}

static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         okSafe argNames
  //  --------------------- ------------------------  -----------------------  ------ ----------
    { "galactrum",               "masternode",             &masternode,             true,  {} },
    { "galactrum",               "masternodelist",         &masternodelist,         true,  {} },
    { "galactrum",               "masternodebroadcast",    &masternodebroadcast,    true,  {} },
    { "galactrum",               "getpoolinfo",            &getpoolinfo,            true,  {} },
    { "galactrum",               "sentinelping",           &sentinelping,           true,  {} },
#ifdef ENABLE_WALLET
    { "galactrum",               "cloaking",            &privatesend,            false, {} },
#endif // ENABLE_WALLET
};

void RegisterMasternodeRPCCommands(CRPCTable &t)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
