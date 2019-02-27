// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <tpos/activestakenode.h>
#include <checkpoints.h>
#include <governance/governance.h>
#include <validation.h>
#include <tpos/stakenode-sync.h>
#include <tpos/stakenode.h>
#include <tpos/stakenodeman.h>
#include <netfulfilledman.h>
#include <spork.h>
#include <ui_interface.h>
#include <util.h>

class CStakenodeSync;
CStakenodeSync stakenodeSync;

void CStakenodeSync::Fail()
{
    nTimeLastFailure = GetTime();
    nRequestedStakenodeAssets = STAKENODE_SYNC_FAILED;
}

void CStakenodeSync::Reset()
{
    nRequestedStakenodeAssets = STAKENODE_SYNC_INITIAL;
    nRequestedStakenodeAttempt = 0;
    nTimeAssetSyncStarted = GetTime();
    nTimeLastBumped = GetTime();
    nTimeLastFailure = 0;
}

void CStakenodeSync::BumpAssetLastTime(std::string strFuncName)
{
    if(IsSynced() || IsFailed()) return;
    nTimeLastBumped = GetTime();
    LogPrint(BCLog::MNSYNC, "CStakenodeSync::BumpAssetLastTime -- %s\n", strFuncName);
}

std::string CStakenodeSync::GetAssetName()
{
    switch(nRequestedStakenodeAssets)
    {
        case(STAKENODE_SYNC_INITIAL):      return "STAKENODE_SYNC_INITIAL";
        case(STAKENODE_SYNC_WAITING):      return "STAKENODE_SYNC_WAITING";
        case(STAKENODE_SYNC_LIST):         return "STAKENODE_SYNC_LIST";
        case(STAKENODE_SYNC_FAILED):       return "STAKENODE_SYNC_FAILED";
        case STAKENODE_SYNC_FINISHED:      return "STAKENODE_SYNC_FINISHED";
        default:                            return "UNKNOWN";
    }
}

void CStakenodeSync::SwitchToNextAsset(CConnman& connman)
{
    switch(nRequestedStakenodeAssets)
    {
        case(STAKENODE_SYNC_FAILED):
            throw std::runtime_error("Can't switch to next asset from failed, should use Reset() first!");
            break;
        case(STAKENODE_SYNC_INITIAL):
            ClearFulfilledRequests(connman);
            nRequestedStakenodeAssets = STAKENODE_SYNC_WAITING;
            LogPrintf("CStakenodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
            break;
        case(STAKENODE_SYNC_WAITING):
            ClearFulfilledRequests(connman);
            LogPrintf("CStakenodeSync::SwitchToNextAsset -- Completed %s in %llds\n", GetAssetName(), GetTime() - nTimeAssetSyncStarted);
            nRequestedStakenodeAssets = STAKENODE_SYNC_LIST;
            LogPrintf("CStakenodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
            break;
        case(STAKENODE_SYNC_LIST):
            LogPrintf("CStakenodeSync::SwitchToNextAsset -- Completed %s in %llds\n", GetAssetName(), GetTime() - nTimeAssetSyncStarted);
            nRequestedStakenodeAssets = STAKENODE_SYNC_FINISHED;
            uiInterface.NotifyAdditionalDataSyncProgressChanged(1);
            //try to activate our masternode if possible
            activeStakenode.ManageState(connman);

            // TODO: Find out whether we can just use LOCK instead of:
            // TRY_LOCK(cs_vNodes, lockRecv);
            // if(lockRecv) { ... }

            connman.ForEachNode([](CNode* pnode) {
                netfulfilledman.AddFulfilledRequest(pnode->addr, "full-mrnsync");
            });
            LogPrintf("CStakenodeSync::SwitchToNextAsset -- Sync has finished\n");

            break;
    }
    nRequestedStakenodeAttempt = 0;
    nTimeAssetSyncStarted = GetTime();
    BumpAssetLastTime("CStakenodeSync::SwitchToNextAsset");
}

std::string CStakenodeSync::GetSyncStatus()
{
    switch (stakenodeSync.nRequestedStakenodeAssets) {
        case STAKENODE_SYNC_INITIAL:       return _("Synchroning blockchain...");
        case STAKENODE_SYNC_WAITING:       return _("Synchronization pending...");
        case STAKENODE_SYNC_LIST:          return _("Synchronizing masternodes...");
        case STAKENODE_SYNC_FAILED:        return _("Synchronization failed");
        case STAKENODE_SYNC_FINISHED:      return _("Synchronization finished");
        default:                            return "";
    }
}

void CStakenodeSync::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv)
{
    if (strCommand == NetMsgType::MERCHANTSYNCSTATUSCOUNT) { //Sync status count

        //do not care about stats if sync process finished or failed
        if(IsSynced() || IsFailed()) return;

        int nItemID;
        int nCount;
        vRecv >> nItemID >> nCount;

        LogPrint(BCLog::MNSYNC, "MERCHANTSYNCSTATUSCOUNT -- got inventory count: nItemID=%d  nCount=%d  peer=%d\n", nItemID, nCount, pfrom->GetId());
    }
}

void CStakenodeSync::ClearFulfilledRequests(CConnman& connman)
{
    // TODO: Find out whether we can just use LOCK instead of:
    // TRY_LOCK(cs_vNodes, lockRecv);
    // if(!lockRecv) return;

    connman.ForEachNode([](CNode* pnode) {
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "stakenode-list-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "full-mrnsync");
    });
}

void CStakenodeSync::ProcessTick(CConnman& connman)
{
    static int nTick = 0;
    if(nTick++ % STAKENODE_SYNC_TICK_SECONDS != 0) return;

    // reset the sync process if the last call to this function was more than 60 minutes ago (client was in sleep mode)
    static int64_t nTimeLastProcess = GetTime();
    if(GetTime() - nTimeLastProcess > 60*60) {
        LogPrintf("CStakenodeSync::HasSyncFailures -- WARNING: no actions for too long, restarting sync...\n");
        Reset();
        SwitchToNextAsset(connman);
        nTimeLastProcess = GetTime();
        return;
    }
    nTimeLastProcess = GetTime();

    // reset sync status in case of any other sync failure
    if(IsFailed()) {
        if(nTimeLastFailure + (1*60) < GetTime()) { // 1 minute cooldown after failed sync
            LogPrintf("CStakenodeSync::HasSyncFailures -- WARNING: failed to sync, trying again...\n");
            Reset();
            SwitchToNextAsset(connman);
        }
        return;
    }

    // gradually request the rest of the votes after sync finished
    if(IsSynced()) {
        std::vector<CNode*> vNodesCopy = connman.CopyNodeVector();
        governance.RequestGovernanceObjectVotes(vNodesCopy, connman);
        connman.ReleaseNodeVector(vNodesCopy);
        return;
    }

    // Calculate "progress" for LOG reporting / GUI notification
    double nSyncProgress = double(nRequestedStakenodeAttempt + (nRequestedStakenodeAssets - 1) * 8) / (8*4);
    LogPrint(BCLog::STAKENODE, "CStakenodeSync::ProcessTick -- nTick %d nRequestedStakenodeAssets %d nRequestedStakenodeAttempt %d nSyncProgress %f\n", nTick, nRequestedStakenodeAssets, nRequestedStakenodeAttempt, nSyncProgress);
    uiInterface.NotifyAdditionalDataSyncProgressChanged(nSyncProgress);

    std::vector<CNode*> vNodesCopy = connman.CopyNodeVector();

    for(CNode* pnode : vNodesCopy)
    {
        // Don't try to sync any data from outbound "stakenode" connections -
        // they are temporary and should be considered unreliable for a sync process.
        // Inbound connection this early is most likely a "stakenode" connection
        // initiated from another node, so skip it too.
        if(pnode->fStakenode || (fStakeNode && pnode->fInbound)) continue;

        // NORMAL NETWORK MODE - TESTNET/MAINNET
        {
            if(netfulfilledman.HasFulfilledRequest(pnode->addr, "full-mrnsync")) {
                // We already fully synced from this node recently,
                // disconnect to free this connection slot for another peer.
                pnode->fDisconnect = true;
                LogPrintf("CStakenodeSync::ProcessTick -- disconnecting from recently synced peer %d\n", pnode->GetId());
                continue;
            }

            // INITIAL TIMEOUT

            if(nRequestedStakenodeAssets == STAKENODE_SYNC_WAITING) {
                if(GetTime() - nTimeLastBumped > STAKENODE_SYNC_TIMEOUT_SECONDS) {
                    // At this point we know that:
                    // a) there are peers (because we are looping on at least one of them);
                    // b) we waited for at least STAKENODE_SYNC_TIMEOUT_SECONDS since we reached
                    //    the headers tip the last time (i.e. since we switched from
                    //     STAKENODE_SYNC_INITIAL to STAKENODE_SYNC_WAITING and bumped time);
                    // c) there were no blocks (UpdatedBlockTip, NotifyHeaderTip) or headers (AcceptedBlockHeader)
                    //    for at least STAKENODE_SYNC_TIMEOUT_SECONDS.
                    // We must be at the tip already, let's move to the next asset.
                    SwitchToNextAsset(connman);
                }
            }

            // MNLIST : SYNC STAKENODE LIST FROM OTHER CONNECTED CLIENTS

            if(nRequestedStakenodeAssets == STAKENODE_SYNC_LIST) {
                LogPrint(BCLog::STAKENODE, "CStakenodeSync::ProcessTick -- nTick %d nRequestedStakenodeAssets %d nTimeLastBumped %lld GetTime() %lld diff %lld\n", nTick, nRequestedStakenodeAssets, nTimeLastBumped, GetTime(), GetTime() - nTimeLastBumped);
                // check for timeout first
                if(GetTime() - nTimeLastBumped > STAKENODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrint(BCLog::STAKENODE, "CStakenodeSync::ProcessTick -- nTick %d nRequestedStakenodeAssets %d -- timeout\n", nTick, nRequestedStakenodeAssets);
                    if (nRequestedStakenodeAttempt == 0) {
                        LogPrintf("CStakenodeSync::ProcessTick -- ERROR: failed to sync %s\n", GetAssetName());
                        // there is no way we can continue without masternode list, fail here and try later
                        Fail();
                        connman.ReleaseNodeVector(vNodesCopy);
                        return;
                    }
                    SwitchToNextAsset(connman);
                    connman.ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // only request once from each peer
                if(netfulfilledman.HasFulfilledRequest(pnode->addr, "stakenode-list-sync")) continue;
                netfulfilledman.AddFulfilledRequest(pnode->addr, "stakenode-list-sync");

//                if (pnode->nVersion < PROTOCOL_VERSION) continue;
                nRequestedStakenodeAttempt++;

                stakenodeman.DsegUpdate(pnode, connman);

                connman.ReleaseNodeVector(vNodesCopy);
                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }
        }
    }
    // looped through all nodes, release them
    connman.ReleaseNodeVector(vNodesCopy);
}


void CStakenodeSync::AcceptedBlockHeader(const CBlockIndex *pindexNew)
{
    LogPrint(BCLog::MNSYNC, "CStakenodeSync::AcceptedBlockHeader -- pindexNew->nHeight: %d\n", pindexNew->nHeight);

    if (!IsBlockchainSynced()) {
        // Postpone timeout each time new block header arrives while we are still syncing blockchain
        BumpAssetLastTime("CStakenodeSync::AcceptedBlockHeader");
    }
}

void CStakenodeSync::NotifyHeaderTip(const CBlockIndex *pindexNew, bool fInitialDownload, CConnman& connman)
{
    LogPrint(BCLog::MNSYNC, "CStakenodeSync::NotifyHeaderTip -- pindexNew->nHeight: %d fInitialDownload=%d\n", pindexNew->nHeight, fInitialDownload);

    if (IsFailed() || IsSynced() || !pindexBestHeader)
        return;

    if (!IsBlockchainSynced()) {
        // Postpone timeout each time new block arrives while we are still syncing blockchain
        BumpAssetLastTime("CStakenodeSync::NotifyHeaderTip");
    }
}

void CStakenodeSync::UpdatedBlockTip(const CBlockIndex *pindexNew, bool fInitialDownload, CConnman& connman)
{
    LogPrint(BCLog::MNSYNC, "CStakenodeSync::UpdatedBlockTip -- pindexNew->nHeight: %d fInitialDownload=%d\n", pindexNew->nHeight, fInitialDownload);

    if (IsFailed() || IsSynced() || !pindexBestHeader)
        return;

    if (!IsBlockchainSynced()) {
        // Postpone timeout each time new block arrives while we are still syncing blockchain
        BumpAssetLastTime("CStakenodeSync::UpdatedBlockTip");
    }

    if (fInitialDownload) {
        // switched too early
        if (IsBlockchainSynced()) {
            Reset();
        }

        // no need to check any further while still in IBD mode
        return;
    }

    // Note: since we sync headers first, it should be ok to use this
    static bool fReachedBestHeader = false;
    bool fReachedBestHeaderNew = pindexNew->GetBlockHash() == pindexBestHeader->GetBlockHash();

    if (fReachedBestHeader && !fReachedBestHeaderNew) {
        // Switching from true to false means that we previousely stuck syncing headers for some reason,
        // probably initial timeout was not enough,
        // because there is no way we can update tip not having best header
        Reset();
        fReachedBestHeader = false;
        return;
    }

    fReachedBestHeader = fReachedBestHeaderNew;

    LogPrint(BCLog::MNSYNC, "CStakenodeSync::UpdatedBlockTip -- pindexNew->nHeight: %d pindexBestHeader->nHeight: %d fInitialDownload=%d fReachedBestHeader=%d\n",
                pindexNew->nHeight, pindexBestHeader->nHeight, fInitialDownload, fReachedBestHeader);

    if (!IsBlockchainSynced() && fReachedBestHeader) {
        // Reached best header while being in initial mode.
        // We must be at the tip already, let's move to the next asset.
        SwitchToNextAsset(connman);
    }
}
