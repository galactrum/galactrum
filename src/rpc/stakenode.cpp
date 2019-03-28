// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <tpos/activestakenode.h>
#include <key_io.h>
#include <init.h>
#include <netbase.h>
#include <validation.h>
#include <tpos/stakenode-sync.h>
#include <tpos/stakenodeman.h>
#include <tpos/stakenode.h>
#include <tpos/stakenodeconfig.h>
#include <rpc/server.h>
#include <util.h>
#include <utilmoneystr.h>
#ifdef ENABLE_WALLET
#include <wallet/wallet.h>
#endif
#include <core_io.h>
#include <key_io.h>

#include <fstream>
#include <iomanip>
#include <univalue.h>

static UniValue stakenodesync(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
                "stakenodesync [status|next|reset]\n"
                "Returns the sync status, updates to the next step or resets it entirely.\n"
                );

    std::string strMode = request.params[0].get_str();

    if(strMode == "status") {
        UniValue objStatus(UniValue::VOBJ);
        objStatus.push_back(Pair("AssetID", stakenodeSync.GetAssetID()));
        objStatus.push_back(Pair("AssetName", stakenodeSync.GetAssetName()));
        objStatus.push_back(Pair("AssetStartTime", stakenodeSync.GetAssetStartTime()));
        objStatus.push_back(Pair("Attempt", stakenodeSync.GetAttempt()));
        objStatus.push_back(Pair("IsBlockchainSynced", stakenodeSync.IsBlockchainSynced()));
        objStatus.push_back(Pair("IsMasternodeListSynced", stakenodeSync.IsStakenodeListSynced()));
        objStatus.push_back(Pair("IsSynced", stakenodeSync.IsSynced()));
        objStatus.push_back(Pair("IsFailed", stakenodeSync.IsFailed()));
        return objStatus;
    }

    if(strMode == "next")
    {
        stakenodeSync.SwitchToNextAsset(*g_connman);
        return "sync updated to " + stakenodeSync.GetAssetName();
    }

    if(strMode == "reset")
    {
        stakenodeSync.Reset();
        stakenodeSync.SwitchToNextAsset(*g_connman);
        return "success";
    }
    return "failure";
}


static UniValue ListOfStakeNodes(const UniValue& params, std::set<CService> myStakeNodesIps, bool showOnlyMine)
{
    std::string strMode = "status";
    std::string strFilter = "";

    if (params.size() >= 1) strMode = params[0].get_str();
    if (params.size() == 2) strFilter = params[1].get_str();

    UniValue obj(UniValue::VOBJ);

    auto mapStakenodes = stakenodeman.GetFullStakenodeMap();
    for (auto& mnpair : mapStakenodes) {

        if(showOnlyMine && myStakeNodesIps.count(mnpair.second.addr) == 0) {
            continue;
        }

        CStakenode mn = mnpair.second;
        std::string strOutpoint = HexStr(mnpair.first.GetID().ToString());
        if (strMode == "activeseconds") {
            if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, (int64_t)(mn.lastPing.sigTime - mn.sigTime)));
        } else if (strMode == "addr") {
            std::string strAddress = mn.addr.ToString();
            if (strFilter !="" && strAddress.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, strAddress));
        } else if (strMode == "full") {
            std::ostringstream streamFull;
            streamFull << std::setw(18) <<
                          mn.GetStatus() << " " <<
                          mn.nProtocolVersion << " " <<
                          CBitcoinAddress(mn.pubKeyStakenode.GetID()).ToString() << " " <<
                          mn.hashTPoSContractTx.ToString() << " " <<
                          (int64_t)mn.lastPing.sigTime << " " << std::setw(8) <<
                          (int64_t)(mn.lastPing.sigTime - mn.sigTime) << " " << std::setw(10) <<
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
                          CBitcoinAddress(mn.pubKeyStakenode.GetID()).ToString() << " " <<
                          (int64_t)mn.lastPing.sigTime << " " << std::setw(8) <<
                          (int64_t)(mn.lastPing.sigTime - mn.sigTime) << " " <<
                          (mn.lastPing.fSentinelIsCurrent ? "current" : "expired") << " " <<
                          mn.addr.ToString();
            std::string strInfo = streamInfo.str();
            if (strFilter !="" && strInfo.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, strInfo));
        } else if (strMode == "lastseen") {
            if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, (int64_t)mn.lastPing.sigTime));
        } else if (strMode == "payee") {
            CBitcoinAddress address(mn.pubKeyStakenode.GetID());
            std::string strPayee = address.ToString();
            if (strFilter !="" && strPayee.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, strPayee));
        } else if (strMode == "protocol") {
            if (strFilter !="" && strFilter != strprintf("%d", mn.nProtocolVersion) &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, (int64_t)mn.nProtocolVersion));
        } else if (strMode == "pubkey") {
            if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, HexStr(mn.pubKeyStakenode)));
        } else if (strMode == "status") {
            std::string strStatus = mn.GetStatus();
            if (strFilter !="" && strStatus.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, strStatus));
        }
    }

    return obj;
}

static UniValue stakenodelist(const JSONRPCRequest& request)
{
    std::string strMode = "status";
    std::string strFilter = "";

    if (request.params.size() >= 1) strMode = request.params[0].get_str();
    if (request.params.size() == 2) strFilter = request.params[1].get_str();

    if (request.fHelp || (
                strMode != "activeseconds" && strMode != "addr" && strMode != "full" && strMode != "info" &&
                strMode != "lastseen" && strMode != "lastpaidtime" && strMode != "lastpaidblock" &&
                strMode != "protocol" && strMode != "payee" && strMode != "pubkey" &&
                strMode != "rank" && strMode != "status"))
    {
        throw std::runtime_error(
                    "stakenodelist ( \"mode\" \"filter\" )\n"
                    "Get a list of stakenodes in different modes\n"
                    "\nArguments:\n"
                    "1. \"mode\"      (string, optional/required to use filter, defaults = status) The mode to run list in\n"
                    "2. \"filter\"    (string, optional) Filter results. Partial match by outpoint by default in all modes,\n"
                    "                                    additional matches in some modes are also available\n"
                    "\nAvailable modes:\n"
                    "  activeseconds  - Print number of seconds stakenode recognized by the network as enabled\n"
                    "                   (since latest issued \"stakenode start/start-many/start-alias\")\n"
                    "  addr           - Print ip address associated with a stakenode (can be additionally filtered, partial match)\n"
                    "  full           - Print info in format 'status protocol payee lastseen activeseconds lastpaidtime lastpaidblock IP'\n"
                    "                   (can be additionally filtered, partial match)\n"
                    "  info           - Print info in format 'status protocol payee lastseen activeseconds sentinelversion sentinelstate IP'\n"
                    "                   (can be additionally filtered, partial match)\n"
                    "  lastpaidblock  - Print the last block height a node was paid on the network\n"
                    "  lastpaidtime   - Print the last time a node was paid on the network\n"
                    "  lastseen       - Print timestamp of when a stakenode was last seen on the network\n"
                    "  payee          - Print Galactrum address associated with a stakenode (can be additionally filtered,\n"
                    "                   partial match)\n"
                    "  protocol       - Print protocol of a stakenode (can be additionally filtered, exact match)\n"
                    "  pubkey         - Print the stakenode (not collateral) public key\n"
                    "  rank           - Print rank of a stakenode based on current block\n"
                    "  status         - Print stakenode status: PRE_ENABLED / ENABLED / EXPIRED / WATCHDOG_EXPIRED / NEW_START_REQUIRED /\n"
                    "                   UPDATE_REQUIRED / POSE_BAN / OUTPOINT_SPENT (can be additionally filtered, partial match)\n"
                    );
    }

    if (strMode == "full" || strMode == "lastpaidtime" || strMode == "lastpaidblock") {
        CBlockIndex* pindex = NULL;
        {
            LOCK(cs_main);
            pindex = chainActive.Tip();
        }
    }

    std::set<CService> myStakeNodesIps;
    return  ListOfStakeNodes(request.params, myStakeNodesIps, false);
}

static UniValue stakenode(const JSONRPCRequest& request)
{
#ifdef ENABLE_WALLET
    auto pwallet = GetWalletForJSONRPCRequest(request);
#endif
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
                strCommand != "list" && strCommand != "list-conf" && strCommand != "list-mine" && strCommand != "count" &&
                strCommand != "debug" && strCommand != "current" && strCommand != "winner" && strCommand != "winners" && strCommand != "genkey" &&
                strCommand != "connect" && strCommand != "status"))
        throw std::runtime_error(
                "stakenode \"command\"...\n"
                "Set of commands to execute stakenode related actions\n"
                "\nArguments:\n"
                "1. \"command\"        (string or set of strings, required) The command to execute\n"
                "\nAvailable commands:\n"
                "  count        - Print number of all known stakenodes (optional: 'ps', 'enabled', 'all', 'qualify')\n"
                "  current      - Print info on current stakenode winner to be paid the next block (calculated locally)\n"
                "  genkey       - Generate new stakenodeprivkey\n"
            #ifdef ENABLE_WALLET
                "  outputs      - Print stakenode compatible outputs\n"
                "  start-alias  - Start single remote stakenode by assigned alias configured in stakenode.conf\n"
                "  start-<mode> - Start remote stakenodes configured in stakenode.conf (<mode>: 'all', 'missing', 'disabled')\n"
            #endif // ENABLE_WALLET
                "  status       - Print stakenode status information\n"
                "  list         - Print list of all known stakenodes (see stakenodelist for more info)\n"
                "  list-conf    - Print stakenode.conf in JSON format\n"
                "  list-mine    - Print own nodes"
                "  winner       - Print info on next stakenode winner to vote for\n"
                "  winners      - Print list of stakenode winners\n"
                );

    if (strCommand == "list")
    {
        UniValue newParams(UniValue::VARR);
        // forward request.params but skip "list"
        for (unsigned int i = 1; i < request.params.size(); i++) {
            newParams.push_back(request.params[i]);
        }

        auto newRequest = request;
        newRequest.params = newParams;

        return stakenodelist(newRequest);
    }

    if(strCommand == "list-mine")
    {
        UniValue newParams(UniValue::VARR);
        // forward request.params but skip "list-mine"
        for (unsigned int i = 1; i < request.params.size(); i++) {
            newParams.push_back(request.params[i]);
        }

        std::set<CService> myStakeNodesIps;
        for(auto &&mne : stakenodeConfig.getEntries())
        {
            CService service;
            Lookup(mne.getIp().c_str(), service, 0, false);

            myStakeNodesIps.insert(service);
        }

        return  ListOfStakeNodes(newParams, myStakeNodesIps, true);
    }

    if (strCommand == "list-conf")
    {
        UniValue resultObj(UniValue::VARR);

        for(auto &&mne : stakenodeConfig.getEntries())
        {
            CStakenode mn;
            CKey privKey = DecodeSecret(mne.getStakenodePrivKey());
            CPubKey pubKey = privKey.GetPubKey();
            bool fFound = stakenodeman.Get(pubKey, mn);

            std::string strStatus = fFound ? mn.GetStatus() : "MISSING";

            UniValue mnObj(UniValue::VOBJ);
            mnObj.push_back(Pair("alias", mne.getAlias()));
            mnObj.push_back(Pair("address", mne.getIp()));
            mnObj.push_back(Pair("privateKey", mne.getStakenodePrivKey()));
            mnObj.push_back(Pair("status", strStatus));
            resultObj.push_back(mnObj);
        }

        return resultObj;
    }

    if(strCommand == "connect")
    {
        if (request.params.size() < 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Stakenode address required");

        std::string strAddress = request.params[1].get_str();

        CService addr;
        if (!Lookup(strAddress.c_str(), addr, 0, false))
            throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Incorrect stakenode address %s", strAddress));

        // TODO: Pass CConnman instance somehow and don't use global variable.
        CNode *pnode = g_connman->OpenMasternodeConnection(CAddress(addr, NODE_NETWORK));
        if(!pnode)
            throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Couldn't connect to stakenode %s", strAddress));

        return "successfully connected";
    }

    if (strCommand == "count")
    {
        if (request.params.size() > 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Too many parameters");

        if (request.params.size() == 1)
            return stakenodeman.size();

        std::string strMode = request.params[1].get_str();

        if (strMode == "ps")
            return stakenodeman.CountEnabled();

        if (strMode == "enabled")
            return stakenodeman.CountEnabled();


        if (strMode == "all")
            return strprintf("Total: %d (PS Compatible: %d / Enabled: %d)",
                             stakenodeman.size(), stakenodeman.CountEnabled(),
                             stakenodeman.CountEnabled());
    }
#ifdef ENABLE_WALLET
    if (strCommand == "start-alias")
    {
        if (request.params.size() < 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Please specify an alias");

        {
            LOCK(pwallet->cs_wallet);
            EnsureWalletIsUnlocked(pwallet);
        }

        std::string strAlias = request.params[1].get_str();

        bool fFound = false;

        UniValue statusObj(UniValue::VOBJ);
        statusObj.push_back(Pair("alias", strAlias));

        for(auto && mrne : stakenodeConfig.getEntries()) {
            if(mrne.getAlias() == strAlias) {
                fFound = true;
                std::string strError;
                CStakenodeBroadcast mnb;

                bool fResult = CStakenodeBroadcast::Create(mrne.getIp(), mrne.getStakenodePrivKey(),
                                                              mrne.getContractTxID(), strError, mnb);

                statusObj.push_back(Pair("result", fResult ? "successful" : "failed"));
                if(fResult) {
                    stakenodeman.UpdateStakenodeList(mnb, *g_connman);
                    mnb.Relay(*g_connman);
                } else {
                    statusObj.push_back(Pair("errorMessage", strError));
                }

                break;
            }
        }

        if(!fFound) {
            statusObj.push_back(Pair("result", "failed"));
            statusObj.push_back(Pair("errorMessage", "Could not find alias in config. Verify with list-conf."));
        }

        return statusObj;
    }
    if (strCommand == "genkey")
    {
        CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
        EnsureWalletIsUnlocked(pwallet);

        // Generate a new key that is added to wallet
        CPubKey newKey;
        if (!pwallet->GetKeyFromPool(newKey)) {
            throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
        }
        OutputType output_type = OutputType::LEGACY;
        pwallet->LearnRelatedScripts(newKey, output_type);
        CTxDestination dest = GetDestinationForKey(newKey, output_type);

        pwallet->SetAddressBook(dest, "", "receive");

        UniValue obj(UniValue::VOBJ);

        obj.push_back(Pair("public_address", EncodeDestination(dest)));
        CKey vchSecret;
        if (!pwallet->GetKey(newKey.GetID(), vchSecret)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Unable to fetch private key");
        }
        obj.push_back(Pair("secret_key", EncodeSecret(vchSecret)));

        return obj;
    }
#endif

    if (strCommand == "status")
    {
        if (!fStakeNode)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "This is not a stakenode");

        UniValue mnObj(UniValue::VOBJ);

        mnObj.push_back(Pair("pubkey", activeStakenode.pubKeyStakenode.GetID().ToString()));
        mnObj.push_back(Pair("service", activeStakenode.service.ToString()));

        CStakenode mn;
        auto pubKey = activeStakenode.pubKeyStakenode;
        if(stakenodeman.Get(pubKey, mn)) {
            mnObj.push_back(Pair("stakenodeAddress", CBitcoinAddress(pubKey.GetID()).ToString()));
        }

        mnObj.push_back(Pair("status", activeStakenode.GetStatus()));
        return mnObj;
    }

    return NullUniValue;
}

static bool DecodeHexVecMnb(std::vector<CStakenodeBroadcast>& vecMnb, std::string strHexMnb) {

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

UniValue stakenodesentinelping(const JSONRPCRequest& request)
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

    //    activeStakenode.UpdateSentinelPing(StringVersionToInt(request.params[0].get_str()));
    return true;
}

#ifdef ENABLE_WALLET
UniValue stakecontract(const JSONRPCRequest& request)
{
    auto pwallet = GetWalletForJSONRPCRequest(request);
    std::string strCommand;
    if (request.params.size() >= 1) {
        strCommand = request.params[0].get_str();
    }

    if (request.fHelp  || (strCommand != "list" && strCommand != "create" && strCommand != "refresh" && strCommand != "cleanup"))
        throw std::runtime_error(
                "stakecontract \"command\"...\n"
                "Set of commands to execute stakenode related actions\n"
                "\nArguments:\n"
                "1. \"command\"        (string or set of strings, required) The command to execute\n"
                "\nAvailable commands:\n"
                "  create           - Create stake contract transaction\n"
                "  list             - Print list of all stake contracts that you are own or have been granted\n"
                "  refresh          - Refresh stake contract for stakenode to fetch all coins from blockchain.\n"
                );


    if (strCommand == "list")
    {
        UniValue result(UniValue::VOBJ);
        UniValue stakenodeArray(UniValue::VARR);
        UniValue ownerArray(UniValue::VARR);

        auto parseContract = [](const TPoSContract &contract) {
            UniValue object(UniValue::VOBJ);

            object.push_back(Pair("txid", contract.rawTx->GetHash().ToString()));
            object.push_back(Pair("ownerAddress", contract.tposAddress.ToString()));
            object.push_back(Pair("stakenodeAddress", contract.stakenodeAddress.ToString()));
            object.push_back(Pair("commission", 100 - contract.stakePercentage)); // show StakeNode commission
            if(contract.vchSignature.empty())
                object.push_back(Pair("deprecated", true));

            return object;
        };

        for(auto &&it : pwallet->tposStakenodeContracts)
        {
            stakenodeArray.push_back(parseContract(it.second));
        }

        for(auto &&it : pwallet->tposOwnerContracts)
        {
            ownerArray.push_back(parseContract(it.second));
        }

        result.push_back(Pair("as_stakenode", stakenodeArray));
        result.push_back(Pair("as_owner", ownerArray));

        return result;
    }
    else if(strCommand == "create")
    {
        if (request.params.size() < 4)
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "Expected format: stakecontract create owner_address stakenode_address commission");

        CBitcoinAddress tposAddress(request.params[1].get_str());
        CBitcoinAddress stakenodeAddress(request.params[2].get_str());
        int commission = std::stoi(request.params[3].get_str());

        if(!tposAddress.IsValid())
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "owner address is not valid, won't continue");

        if(!stakenodeAddress.IsValid())
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "stakenode address is not valid, won't continue");

        CReserveKey reserveKey(pwallet);

        std::string strError;
        auto transaction = MakeTransactionRef();

        if(TPoSUtils::CreateTPoSTransaction(pwallet, transaction,
                                            reserveKey, tposAddress,
                                            stakenodeAddress, commission, strError))
        {
            return EncodeHexTx(*transaction);
        }
        else
        {
            return "Failed to create stake contract transaction, reason: " + strError;
        }
    }
    else if(strCommand == "refresh")
    {
        if(request.params.size() < 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "Expected format: stakecontract refresh stakecontract_id");

        auto it = pwallet->tposStakenodeContracts.find(ParseHashV(request.params[1], "stakecontractid"));
        if(it == std::end(pwallet->tposStakenodeContracts))
            return JSONRPCError(RPC_INVALID_PARAMETER, "No Stakenode contract found");

        WalletRescanReserver reserver(pwallet);

        if (!reserver.reserve()) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
        }

        pwallet->ScanForWalletTransactions(chainActive.Genesis(), chainActive.Tip(), reserver, true);
        pwallet->ReacceptWalletTransactions();
    }
    else if(strCommand == "cleanup")
    {
        if(request.params.size() < 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "Expected format: stakecontract refresh stakecontract_id");

        auto tposContractHashID = ParseHashV(request.params[1], "stakecontractid");

        auto it = pwallet->tposStakenodeContracts.find(tposContractHashID);
        if(it == std::end(pwallet->tposStakenodeContracts))
            return "No Stakenode contract found";

        CTransactionRef tx;
        uint256 hashBlock;
        if(!GetTransaction(tposContractHashID, tx, Params().GetConsensus(), hashBlock, true))
        {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Failed to get transaction for stake contract ");
        }

        TPoSContract tmpContract = TPoSContract::FromTPoSContractTx(tx);

        if(!tmpContract.IsValid())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Contract is invalid");

            pwallet->RemoveWatchOnly(GetScriptForDestination(tmpContract.tposAddress.Get()));
    }

    return NullUniValue;
}
#endif

static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         argNames
  //  --------------------- ------------------------  -----------------------  ----------
  { "stakenode",            "stakenode",            &stakenode,            {"command"} }, /* uses wallet if enabled */
  { "stakenode",            "stakenodelist",        &stakenodelist,        {"mode", "filter"} },
  #ifdef ENABLE_WALLET
  { "stakenode",            "stakecontract",            &stakecontract,            {"command"} },
  #endif
  { "stakenode",            "stakenodesync",            &stakenodesync,            {"command"} },
};

void RegisterStakenodeCommands(CRPCTable &t)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}

