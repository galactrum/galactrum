// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <stakenode/activestakenode.h>
#include <stakenode/stakenode.h>
#include <stakenode/stakenode-sync.h>
#include <stakenode/stakenodeman.h>
#include <protocol.h>
#include <utilstrencodings.h>

// Keep track of the active Stakenode
CActiveStakenode activeStakenode;

void CActiveStakenode::ManageState(CConnman& connman)
{
    LogPrint(BCLog::STAKENODE, "CActiveStakenode::ManageState -- Start\n");
    if(!fStakeNode) {
        LogPrint(BCLog::STAKENODE, "CActiveStakenode::ManageState -- Not a masternode, returning\n");
        return;
    }

    if(Params().NetworkIDString() != CBaseChainParams::REGTEST && !stakenodeSync.IsBlockchainSynced()) {
        nState = ACTIVE_STAKENODE_SYNC_IN_PROCESS;
        LogPrintf("CActiveStakenode::ManageState -- %s: %s\n", GetStateString(), GetStatus());
        return;
    }

    if(nState == ACTIVE_STAKENODE_SYNC_IN_PROCESS) {
        nState = ACTIVE_STAKENODE_INITIAL;
    }

    LogPrint(BCLog::STAKENODE, "CActiveStakenode::ManageState -- status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);

    if(eType == STAKENODE_UNKNOWN) {
        ManageStateInitial(connman);
    }

    if(eType == STAKENODE_REMOTE) {
        ManageStateRemote();
    }

    SendStakenodePing(connman);
}

std::string CActiveStakenode::GetStateString() const
{
    switch (nState) {
        case ACTIVE_STAKENODE_INITIAL:         return "INITIAL";
        case ACTIVE_STAKENODE_SYNC_IN_PROCESS: return "SYNC_IN_PROCESS";
        case ACTIVE_STAKENODE_INPUT_TOO_NEW:   return "INPUT_TOO_NEW";
        case ACTIVE_STAKENODE_NOT_CAPABLE:     return "NOT_CAPABLE";
        case ACTIVE_STAKENODE_STARTED:         return "STARTED";
        default:                                return "UNKNOWN";
    }
}

std::string CActiveStakenode::GetStatus() const
{
    switch (nState) {
        case ACTIVE_STAKENODE_INITIAL:         return "Node just started, not yet activated";
        case ACTIVE_STAKENODE_SYNC_IN_PROCESS: return "Sync in progress. Must wait until sync is complete to start Stakenode";
        case ACTIVE_STAKENODE_INPUT_TOO_NEW:   return strprintf("Stakenode input must have at least %d confirmations", Params().GetConsensus().nStakenodeMinimumConfirmations);
        case ACTIVE_STAKENODE_NOT_CAPABLE:     return "Not capable stakenode: " + strNotCapableReason;
        case ACTIVE_STAKENODE_STARTED:         return "Stakenode successfully started";
        default:                                return "Unknown";
    }
}

std::string CActiveStakenode::GetTypeString() const
{
    std::string strType;
    switch(eType) {
    case STAKENODE_REMOTE:
        strType = "REMOTE";
        break;
    default:
        strType = "UNKNOWN";
        break;
    }
    return strType;
}

bool CActiveStakenode::SendStakenodePing(CConnman& connman)
{
    if(!fPingerEnabled) {
        LogPrint(BCLog::STAKENODE, "CActiveStakenode::SendStakenodePing -- %s: masternode ping service is disabled, skipping...\n", GetStateString());
        return false;
    }

    if(!stakenodeman.Has(pubKeyStakenode)) {
        strNotCapableReason = "Stakenode not in stakenode list";
        nState = ACTIVE_STAKENODE_NOT_CAPABLE;
        LogPrintf("CActiveStakenode::SendStakenodePing -- %s: %s\n", GetStateString(), strNotCapableReason);
        return false;
    }

    CStakenodePing mnp(pubKeyStakenode);
    mnp.nSentinelVersion = nSentinelVersion;
    mnp.fSentinelIsCurrent =
            (abs(GetAdjustedTime() - nSentinelPingTime) < STAKENODE_WATCHDOG_MAX_SECONDS);
    if(!mnp.Sign(keyStakenode, pubKeyStakenode)) {
        LogPrintf("CActiveStakenode::SendStakenodePing -- ERROR: Couldn't sign Stakenode Ping\n");
        return false;
    }

    // Update lastPing for our masternode in Stakenode list
    if(stakenodeman.IsStakenodePingedWithin(pubKeyStakenode, STAKENODE_MIN_MNP_SECONDS, mnp.sigTime)) {
        LogPrintf("CActiveStakenode::SendStakenodePing -- Too early to send Stakenode Ping\n");
        return false;
    }

    stakenodeman.SetStakenodeLastPing(pubKeyStakenode, mnp);

    LogPrintf("%s -- Relaying ping, collateral=%s\n", __func__, HexStr(pubKeyStakenode.GetID().ToString()));
    mnp.Relay(connman);

    return true;
}

bool CActiveStakenode::UpdateSentinelPing(int version)
{
    nSentinelVersion = version;
    nSentinelPingTime = GetAdjustedTime();

    return true;
}

void CActiveStakenode::ManageStateInitial(CConnman& connman)
{
    LogPrint(BCLog::STAKENODE, "CActiveStakenode::ManageStateInitial -- status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);

    // Check that our local network configuration is correct
    if (!fListen) {
        // listen option is probably overwritten by smth else, no good
        nState = ACTIVE_STAKENODE_NOT_CAPABLE;
        strNotCapableReason = "Stakenode must accept connections from outside. Make sure listen configuration option is not overwritten by some another parameter.";
        LogPrintf("CActiveStakenode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    // First try to find whatever local address is specified by externalip option
    bool fFoundLocal = GetLocal(service) && CStakenode::IsValidNetAddr(service);
    if(!fFoundLocal) {
        bool empty = true;
        // If we have some peers, let's try to find our local address from one of them
        connman.ForEachNode([&fFoundLocal, &empty, this](CNode* pnode) {
            empty = false;
            if (!fFoundLocal && pnode->addr.IsIPv4())
                fFoundLocal = GetLocal(service, &pnode->addr) && CStakenode::IsValidNetAddr(service);
        });
        // nothing and no live connections, can't do anything for now
        if (empty) {
            nState = ACTIVE_STAKENODE_NOT_CAPABLE;
            strNotCapableReason = "Can't detect valid external address. Will retry when there are some connections available.";
            LogPrintf("CActiveStakenode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
    }

    if(!fFoundLocal) {
        nState = ACTIVE_STAKENODE_NOT_CAPABLE;
        strNotCapableReason = "Can't detect valid external address. Please consider using the externalip configuration option if problem persists. Make sure to use IPv4 address only.";
        LogPrintf("CActiveStakenode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    auto mainChainParams = CreateChainParams(CBaseChainParams::MAIN);
    int mainnetDefaultPort = mainChainParams->GetDefaultPort();
    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(service.GetPort() != mainnetDefaultPort) {
            nState = ACTIVE_STAKENODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Invalid port: %u - only %d is supported on mainnet.", service.GetPort(), mainnetDefaultPort);
            LogPrintf("CActiveStakenode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
    } else if(service.GetPort() == mainnetDefaultPort) {
        nState = ACTIVE_STAKENODE_NOT_CAPABLE;
        strNotCapableReason = strprintf("Invalid port: %u - %d is only supported on mainnet.", service.GetPort(), mainnetDefaultPort);
        LogPrintf("CActiveStakenode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    LogPrintf("CActiveStakenode::ManageStateInitial -- Checking inbound connection to '%s'\n", service.ToString());

    if(!connman.OpenStakenodeConnection(CAddress(service, NODE_NETWORK))) {
        nState = ACTIVE_STAKENODE_NOT_CAPABLE;
        strNotCapableReason = "Could not connect to " + service.ToString();
        LogPrintf("CActiveStakenode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    // Default to REMOTE
    eType = STAKENODE_REMOTE;

    LogPrint(BCLog::STAKENODE, "CActiveStakenode::ManageStateInitial -- End status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);
}

void CActiveStakenode::ManageStateRemote()
{
    LogPrint(BCLog::STAKENODE, "CActiveStakenode::ManageStateRemote -- Start status = %s, type = %s, pinger enabled = %d, pubKeyStakenode.GetID() = %s\n",
             GetStatus(), GetTypeString(), fPingerEnabled, pubKeyStakenode.GetID().ToString());

    stakenodeman.CheckStakenode(pubKeyStakenode, true);
    stakenode_info_t infoMn;
    if(stakenodeman.GetStakenodeInfo(pubKeyStakenode, infoMn)) {
        if(infoMn.nProtocolVersion != PROTOCOL_VERSION) {
            nState = ACTIVE_STAKENODE_NOT_CAPABLE;
            strNotCapableReason = "Invalid protocol version";
            LogPrintf("CActiveStakenode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if(service != infoMn.addr) {
            nState = ACTIVE_STAKENODE_NOT_CAPABLE;
            strNotCapableReason = "Broadcasted IP doesn't match our external address. Make sure you issued a new broadcast if IP of this masternode changed recently.";
            LogPrintf("CActiveStakenode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if(!CStakenode::IsValidStateForAutoStart(infoMn.nActiveState)) {
            nState = ACTIVE_STAKENODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Stakenode in %s state", CStakenode::StateToString(infoMn.nActiveState));
            LogPrintf("CActiveStakenode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if(nState != ACTIVE_STAKENODE_STARTED) {
            LogPrintf("CActiveStakenode::ManageStateRemote -- STARTED!\n");
            pubKeyStakenode = infoMn.pubKeyStakenode;
            service = infoMn.addr;
            fPingerEnabled = true;
            nState = ACTIVE_STAKENODE_STARTED;
        }
    }
    else {
        nState = ACTIVE_STAKENODE_NOT_CAPABLE;
        strNotCapableReason = "Stakenode not in masternode list";
        LogPrintf("CActiveStakenode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
    }
}
