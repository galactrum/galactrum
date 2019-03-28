// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <stakenode/activestakenode.h>
#include <addrman.h>
#include <stakenode/stakenode-sync.h>
#include <stakenode/stakenodeman.h>
#include <stakenode/stakenode.h>
#include <netfulfilledman.h>
#include <net_processing.h>
#include <script/standard.h>
#include <messagesigner.h>
#include <utilstrencodings.h>
#include <util.h>
#include <init.h>
#include <netmessagemaker.h>

/** Stakenode manager */
CStakenodeMan stakenodeman;

const std::string CStakenodeMan::SERIALIZATION_VERSION_STRING = "CStakenodeMan-Version-7";

static bool GetBlockHash(uint256 &hash, int nBlockHeight)
{
    if(auto index = chainActive[nBlockHeight])
    {
        hash = index->GetBlockHash();
        return true;
    }
    return false;
}

struct CompareByAddr

{
    bool operator()(const CStakenode* t1,
                    const CStakenode* t2) const
    {
        return t1->addr < t2->addr;
    }
};

CStakenodeMan::CStakenodeMan()
    : cs(),
      mapStakenodes(),
      mAskedUsForStakenodeList(),
      mWeAskedForStakenodeList(),
      mWeAskedForStakenodeListEntry(),
      mWeAskedForVerification(),
      mMnbRecoveryRequests(),
      mMnbRecoveryGoodReplies(),
      listScheduledMnbRequestConnections(),
      nLastWatchdogVoteTime(0),
      mapSeenStakenodeBroadcast(),
      mapSeenStakenodePing(),
      nDsqCount(0)
{}

bool CStakenodeMan::Add(CStakenode &mn)
{
    LOCK(cs);

    if (Has(mn.pubKeyStakenode)) return false;

    LogPrint(BCLog::STAKENODE, "CStakenodeMan::Add -- Adding new Stakenode: addr=%s, %i now\n", mn.addr.ToString(), size() + 1);
    mapStakenodes[mn.pubKeyStakenode] = mn;

    return true;
}

void CStakenodeMan::AskForMN(CNode* pnode, const CPubKey &pubKeyStakenode, CConnman& connman)
{
    if(!pnode) return;

    LOCK(cs);

    auto it1 = mWeAskedForStakenodeListEntry.find(pubKeyStakenode);
    if (it1 != mWeAskedForStakenodeListEntry.end()) {
        std::map<CNetAddr, int64_t>::iterator it2 = it1->second.find(pnode->addr);
        if (it2 != it1->second.end()) {
            if (GetTime() < it2->second) {
                // we've asked recently, should not repeat too often or we could get banned
                return;
            }
            // we asked this node for this outpoint but it's ok to ask again already
            LogPrintf("CStakenodeMan::AskForMN -- Asking same peer %s for missing stakenode entry again: %s\n", pnode->addr.ToString(), pubKeyStakenode.GetID().ToString());
        } else {
            // we already asked for this outpoint but not this node
            LogPrintf("CStakenodeMan::AskForMN -- Asking new peer %s for missing stakenode entry: %s\n", pnode->addr.ToString(), pubKeyStakenode.GetID().ToString());
        }
    } else {
        // we never asked any node for this outpoint
        LogPrint(BCLog::STAKENODE, "CStakenodeMan::AskForMN -- Asking peer %s for missing stakenode entry for the first time: %s\n", pnode->addr.ToString(), pubKeyStakenode.GetID().ToString());
    }
    mWeAskedForStakenodeListEntry[pubKeyStakenode][pnode->addr] = GetTime() + DSEG_UPDATE_SECONDS;

    connman.PushMessage(pnode, CNetMsgMaker(pnode->GetSendVersion()).Make(NetMsgType::STAKENODESEG, pubKeyStakenode));
}

bool CStakenodeMan::PoSeBan(const CPubKey &pubKeyStakenode)
{
    LOCK(cs);
    CStakenode* pmn = Find(pubKeyStakenode);
    if (!pmn) {
        return false;
    }
    pmn->PoSeBan();

    return true;
}

void CStakenodeMan::Check()
{
    // we need to lock in this order because function that called us uses same order, bad practice, but no other choice because of recursive mutexes.
    LOCK2(cs_main, cs);

    LogPrint(BCLog::STAKENODE, "CStakenodeMan::Check -- nLastWatchdogVoteTime=%d, IsWatchdogActive()=%d\n", nLastWatchdogVoteTime, IsWatchdogActive());

    for (auto& mnpair : mapStakenodes) {
        mnpair.second.Check();
    }
}

void CStakenodeMan::CheckAndRemove(CConnman& connman)
{
    if(!stakenodeSync.IsStakenodeListSynced()) return;

    LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckAndRemove\n");
    {
        // Need LOCK2 here to ensure consistent locking order because code below locks cs_main
        // in CheckMnbAndUpdateStakenodeList()
        LOCK2(cs_main, cs);

        Check();



        // Remove spent stakenodes, prepare structures and make requests to reasure the state of inactive ones
        // ask for up to MNB_RECOVERY_MAX_ASK_ENTRIES stakenode entries at a time
        int nAskForMnbRecovery = MNB_RECOVERY_MAX_ASK_ENTRIES;
        auto it = mapStakenodes.begin();
        while (it != mapStakenodes.end()) {
            CStakenodeBroadcast mnb = CStakenodeBroadcast(it->second);
            uint256 hash = mnb.GetHash();
            // If collateral was spent ...
            if (it->second.IsNewStartRequired()) {
                LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckAndRemove -- Removing Stakenode: %s  addr=%s  %i now\n", it->second.GetStateString(), it->second.addr.ToString(), size() - 1);

                // erase all of the broadcasts we've seen from this txin, ...
                mapSeenStakenodeBroadcast.erase(hash);
                mWeAskedForStakenodeListEntry.erase(it->first);

                // and finally remove it from the list
                mapStakenodes.erase(it++);
            } else {
                bool fAsk = (nAskForMnbRecovery > 0) &&
                        stakenodeSync.IsSynced() &&
                        !IsMnbRecoveryRequested(hash);
                if(fAsk) {
                    // this mn is in a non-recoverable state and we haven't asked other nodes yet
                    std::set<CNetAddr> setRequested;
                    // wait for mnb recovery replies for MNB_RECOVERY_WAIT_SECONDS seconds
                    mMnbRecoveryRequests[hash] = std::make_pair(GetTime() + MNB_RECOVERY_WAIT_SECONDS, setRequested);
                }
                ++it;
            }
        }

        // proces replies for STAKENODE_NEW_START_REQUIRED stakenodes
        LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckAndRemove -- mMnbRecoveryGoodReplies size=%d\n", (int)mMnbRecoveryGoodReplies.size());
        std::map<uint256, std::vector<CStakenodeBroadcast> >::iterator itMnbReplies = mMnbRecoveryGoodReplies.begin();
        while(itMnbReplies != mMnbRecoveryGoodReplies.end()){
            if(mMnbRecoveryRequests[itMnbReplies->first].first < GetTime()) {
                // all nodes we asked should have replied now
                if(itMnbReplies->second.size() >= MNB_RECOVERY_QUORUM_REQUIRED) {
                    // majority of nodes we asked agrees that this mn doesn't require new mnb, reprocess one of new mnbs
                    LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckAndRemove -- reprocessing mnb, stakenode=%s\n", itMnbReplies->second[0].pubKeyStakenode.GetID().ToString());
                    // mapSeenStakenodeBroadcast.erase(itMnbReplies->first);
                    int nDos;
                    itMnbReplies->second[0].fRecovery = true;
                    CheckMnbAndUpdateStakenodeList(NULL, itMnbReplies->second[0], nDos, connman);
                }
                LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckAndRemove -- removing mnb recovery reply, stakenode=%s, size=%d\n", itMnbReplies->second[0].pubKeyStakenode.GetID().ToString(), (int)itMnbReplies->second.size());
                mMnbRecoveryGoodReplies.erase(itMnbReplies++);
            } else {
                ++itMnbReplies;
            }
        }
    }
    {
        // no need for cm_main below
        LOCK(cs);

        std::map<uint256, std::pair< int64_t, std::set<CNetAddr> > >::iterator itMnbRequest = mMnbRecoveryRequests.begin();
        while(itMnbRequest != mMnbRecoveryRequests.end()){
            // Allow this mnb to be re-verified again after MNB_RECOVERY_RETRY_SECONDS seconds
            // if mn is still in STAKENODE_NEW_START_REQUIRED state.
            if(GetTime() - itMnbRequest->second.first > MNB_RECOVERY_RETRY_SECONDS) {
                mMnbRecoveryRequests.erase(itMnbRequest++);
            } else {
                ++itMnbRequest;
            }
        }

        // check who's asked for the Stakenode list
        std::map<CNetAddr, int64_t>::iterator it1 = mAskedUsForStakenodeList.begin();
        while(it1 != mAskedUsForStakenodeList.end()){
            if((*it1).second < GetTime()) {
                mAskedUsForStakenodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check who we asked for the Stakenode list
        it1 = mWeAskedForStakenodeList.begin();
        while(it1 != mWeAskedForStakenodeList.end()){
            if((*it1).second < GetTime()){
                mWeAskedForStakenodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check which Stakenodes we've asked for
        auto it2 = mWeAskedForStakenodeListEntry.begin();
        while(it2 != mWeAskedForStakenodeListEntry.end()){
            std::map<CNetAddr, int64_t>::iterator it3 = it2->second.begin();
            while(it3 != it2->second.end()){
                if(it3->second < GetTime()){
                    it2->second.erase(it3++);
                } else {
                    ++it3;
                }
            }
            if(it2->second.empty()) {
                mWeAskedForStakenodeListEntry.erase(it2++);
            } else {
                ++it2;
            }
        }

        std::map<CNetAddr, CStakenodeVerification>::iterator it3 = mWeAskedForVerification.begin();
        while(it3 != mWeAskedForVerification.end()){
            if(it3->second.nBlockHeight < nCachedBlockHeight - MAX_POSE_BLOCKS) {
                mWeAskedForVerification.erase(it3++);
            } else {
                ++it3;
            }
        }

        // NOTE: do not expire mapSeenStakenodeBroadcast entries here, clean them on mnb updates!

        // remove expired mapSeenStakenodePing
        std::map<uint256, CStakenodePing>::iterator it4 = mapSeenStakenodePing.begin();
        while(it4 != mapSeenStakenodePing.end()){
            if((*it4).second.IsExpired()) {
                LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckAndRemove -- Removing expired Stakenode ping: hash=%s\n", (*it4).second.GetHash().ToString());
                mapSeenStakenodePing.erase(it4++);
            } else {
                ++it4;
            }
        }

        // remove expired mapSeenStakenodeVerification
        std::map<uint256, CStakenodeVerification>::iterator itv2 = mapSeenStakenodeVerification.begin();
        while(itv2 != mapSeenStakenodeVerification.end()){
            if((*itv2).second.nBlockHeight < nCachedBlockHeight - MAX_POSE_BLOCKS){
                LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckAndRemove -- Removing expired Stakenode verification: hash=%s\n", (*itv2).first.ToString());
                mapSeenStakenodeVerification.erase(itv2++);
            } else {
                ++itv2;
            }
        }

        LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckAndRemove -- %s\n", ToString());
    }
}

void CStakenodeMan::Clear()
{
    LOCK(cs);
    mapStakenodes.clear();
    mAskedUsForStakenodeList.clear();
    mWeAskedForStakenodeList.clear();
    mWeAskedForStakenodeListEntry.clear();
    mapSeenStakenodeBroadcast.clear();
    mapSeenStakenodePing.clear();
    nDsqCount = 0;
    nLastWatchdogVoteTime = 0;
}

int CStakenodeMan::CountStakenodes(int nProtocolVersion) const
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = PROTOCOL_VERSION;

    for (auto& mnpair : mapStakenodes) {
        if(mnpair.second.nProtocolVersion < nProtocolVersion) continue;
        nCount++;
    }

    return nCount;
}

int CStakenodeMan::CountEnabled(int nProtocolVersion) const
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = PROTOCOL_VERSION;

    for (auto& mnpair : mapStakenodes) {
        if(mnpair.second.nProtocolVersion < nProtocolVersion || !mnpair.second.IsEnabled()) continue;
        nCount++;
    }

    return nCount;
}

/* Only IPv4 stakenodes are allowed in 12.1, saving this for later
int CStakenodeMan::CountByIP(int nNetworkType)
{
    LOCK(cs);
    int nNodeCount = 0;

    for (auto& mnpair : mapStakenodes)
        if ((nNetworkType == NET_IPV4 && mnpair.second.addr.IsIPv4()) ||
            (nNetworkType == NET_TOR  && mnpair.second.addr.IsTor())  ||
            (nNetworkType == NET_IPV6 && mnpair.second.addr.IsIPv6())) {
                nNodeCount++;
        }

    return nNodeCount;
}
*/

void CStakenodeMan::DsegUpdate(CNode* pnode, CConnman& connman)
{
    LOCK(cs);

    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(!(pnode->addr.IsRFC1918() || pnode->addr.IsLocal())) {
            std::map<CNetAddr, int64_t>::iterator it = mWeAskedForStakenodeList.find(pnode->addr);
            if(it != mWeAskedForStakenodeList.end() && GetTime() < (*it).second) {
                LogPrintf("CStakenodeMan::DsegUpdate -- we already asked %s for the list; skipping...\n", pnode->addr.ToString());
                return;
            }
        }
    }

    connman.PushMessage(pnode, CNetMsgMaker(pnode->GetSendVersion()).Make(NetMsgType::STAKENODESEG, CPubKey()));
    int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
    mWeAskedForStakenodeList[pnode->addr] = askAgain;

    LogPrint(BCLog::STAKENODE, "CStakenodeMan::DsegUpdate -- asked %s for the list\n", pnode->addr.ToString());
}

CStakenode* CStakenodeMan::Find(const CPubKey &pubKeyStakenode)
{
    LOCK(cs);
    auto it = mapStakenodes.find(pubKeyStakenode);
    return it == mapStakenodes.end() ? NULL : &(it->second);
}

bool CStakenodeMan::Get(const CKeyID &pubKeyID, CStakenode& stakenodeRet)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    for (auto& mnpair : mapStakenodes) {
        CKeyID keyID = mnpair.second.pubKeyStakenode.GetID();
        if (keyID == pubKeyID) {
            stakenodeRet = mnpair.second;
            return true;
        }
    }
    return false;
}

bool CStakenodeMan::Get(const CPubKey &pubKeyStakenode, CStakenode &stakenodeRet)
{
    LOCK(cs);
    auto it = mapStakenodes.find(pubKeyStakenode);
    if (it == mapStakenodes.end()) {
        return false;
    }

    stakenodeRet = it->second;
    return true;
}

bool CStakenodeMan::GetStakenodeInfo(const CPubKey& pubKeyStakenode, stakenode_info_t& mnInfoRet)
{
    LOCK(cs);
    auto it = mapStakenodes.find(pubKeyStakenode);
    if (it == mapStakenodes.end()) {
        return false;
    }
    mnInfoRet = it->second.GetInfo();
    return true;
}

bool CStakenodeMan::GetStakenodeInfo(const CKeyID &pubKeyStakenode, stakenode_info_t &mnInfoRet)
{
    LOCK(cs);
    for (auto& mnpair : mapStakenodes) {
        CKeyID keyID = mnpair.second.pubKeyStakenode.GetID();
        if (keyID == pubKeyStakenode) {
            mnInfoRet = mnpair.second.GetInfo();
            return true;
        }
    }
    return false;
}

bool CStakenodeMan::GetStakenodeInfo(const CScript& payee, stakenode_info_t& mnInfoRet)
{
    LOCK(cs);
    for (auto& mnpair : mapStakenodes) {
        CScript scriptCollateralAddress = GetScriptForDestination(mnpair.second.pubKeyStakenode.GetID());
        if (scriptCollateralAddress == payee) {
            mnInfoRet = mnpair.second.GetInfo();
            return true;
        }
    }
    return false;
}

bool CStakenodeMan::Has(const CPubKey &pubKeyStakenode)
{
    LOCK(cs);
    return mapStakenodes.find(pubKeyStakenode) != mapStakenodes.end();
}

void CStakenodeMan::ProcessStakenodeConnections(CConnman& connman)
{
    //we don't care about this for regtest
    if(Params().NetworkIDString() == CBaseChainParams::REGTEST) return;

    connman.ForEachNode([](CNode* pnode) {
        if(pnode->fStakenode) {
            LogPrintf("Closing Stakenode connection: peer=%d, addr=%s\n", pnode->GetId(), pnode->addr.ToString());
            pnode->fDisconnect = true;
        }
    });
}

std::pair<CService, std::set<uint256> > CStakenodeMan::PopScheduledMnbRequestConnection()
{
    LOCK(cs);
    if(listScheduledMnbRequestConnections.empty()) {
        return std::make_pair(CService(), std::set<uint256>());
    }

    std::set<uint256> setResult;

    listScheduledMnbRequestConnections.sort();
    std::pair<CService, uint256> pairFront = listScheduledMnbRequestConnections.front();

    // squash hashes from requests with the same CService as the first one into setResult
    std::list< std::pair<CService, uint256> >::iterator it = listScheduledMnbRequestConnections.begin();
    while(it != listScheduledMnbRequestConnections.end()) {
        if(pairFront.first == it->first) {
            setResult.insert(it->second);
            it = listScheduledMnbRequestConnections.erase(it);
        } else {
            // since list is sorted now, we can be sure that there is no more hashes left
            // to ask for from this addr
            break;
        }
    }
    return std::make_pair(pairFront.first, setResult);
}


void CStakenodeMan::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    if(fLiteMode) return; // disable all Galactrum specific functionality

    if (strCommand == NetMsgType::STAKENODEANNOUNCE) { //Stakenode Broadcast

        CStakenodeBroadcast mnb;
        vRecv >> mnb;

        pfrom->setAskFor.erase(mnb.GetHash());

        if(!stakenodeSync.IsBlockchainSynced()) return;

        LogPrint(BCLog::STAKENODE, "STAKENODEANNOUNCE -- Stakenode announce, stakenode=%s\n", mnb.pubKeyStakenode.GetID().ToString());

        int nDos = 0;

        if (CheckMnbAndUpdateStakenodeList(pfrom, mnb, nDos, connman)) {
            // use announced Stakenode as a peer
            connman.AddNewAddresses({CAddress(mnb.addr, NODE_NETWORK)}, pfrom->addr, 2*60*60);
        } else if(nDos > 0) {
            Misbehaving(pfrom->GetId(), nDos);
        }

    } else if (strCommand == NetMsgType::STAKENODEPING) { //Stakenode Ping

        CStakenodePing mnp;
        vRecv >> mnp;

        uint256 nHash = mnp.GetHash();

        pfrom->setAskFor.erase(nHash);

        if(!stakenodeSync.IsBlockchainSynced()) return;

        LogPrint(BCLog::STAKENODE, "STAKENODEPING -- Stakenode ping, stakenode=%s\n", mnp.stakenodePubKey.GetID().ToString());

        // Need LOCK2 here to ensure consistent locking order because the CheckAndUpdate call below locks cs_main
        LOCK2(cs_main, cs);

        if(mapSeenStakenodePing.count(nHash)) return; //seen
        mapSeenStakenodePing.insert(std::make_pair(nHash, mnp));

        LogPrint(BCLog::STAKENODE, "STAKENODEPING -- Stakenode ping, stakenode=%s new\n", mnp.stakenodePubKey.GetID().ToString());

        // see if we have this Stakenode
        CStakenode* pmn = Find(mnp.stakenodePubKey);

        // if stakenode uses sentinel ping instead of watchdog
        // we shoud update nTimeLastWatchdogVote here if sentinel
        // ping flag is actual
        if(pmn && mnp.fSentinelIsCurrent)
            UpdateWatchdogVoteTime(mnp.stakenodePubKey, mnp.sigTime);

        // too late, new STAKENODEANNOUNCE is required
        if(pmn && pmn->IsExpired()) return;

        int nDos = 0;
        if(mnp.CheckAndUpdate(pmn, false, nDos, connman)) return;

        if(nDos > 0) {
            // if anything significant failed, mark that node
            Misbehaving(pfrom->GetId(), nDos);
        } else if(pmn != NULL) {
            // nothing significant failed, mn is a known one too
            return;
        }

        // something significant is broken or mn is unknown,
        // we might have to ask for a stakenode entry once
        AskForMN(pfrom, mnp.stakenodePubKey, connman);

    } else if (strCommand == NetMsgType::STAKENODESEG) { //Get Stakenode list or specific entry
        // Ignore such requests until we are fully synced.
        // We could start processing this after stakenode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!stakenodeSync.IsSynced()) return;

        CPubKey pubKeyStakenode;
        vRecv >> pubKeyStakenode;

        LogPrint(BCLog::STAKENODE, "STAKENODESEG -- Stakenode list, stakenode=%s\n", pubKeyStakenode.GetID().ToString());

        LOCK(cs);

        if(!pubKeyStakenode.IsValid()) { //only should ask for this once
            //local network
            bool isLocal = (pfrom->addr.IsRFC1918() || pfrom->addr.IsLocal());

            if(!isLocal && Params().NetworkIDString() == CBaseChainParams::MAIN) {
                std::map<CNetAddr, int64_t>::iterator it = mAskedUsForStakenodeList.find(pfrom->addr);
                if (it != mAskedUsForStakenodeList.end() && it->second > GetTime()) {
                    Misbehaving(pfrom->GetId(), 34);
                    LogPrintf("STAKENODESEG -- peer already asked me for the list, peer=%d\n", pfrom->GetId());
                    return;
                }
                int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
                mAskedUsForStakenodeList[pfrom->addr] = askAgain;
            }
        } //else, asking for a specific node which is ok

        int nInvCount = 0;

        for (auto& mnpair : mapStakenodes) {
            if (pubKeyStakenode.IsValid() && pubKeyStakenode != mnpair.second.pubKeyStakenode) continue; // asked for specific vin but we are not there yet
            if (mnpair.second.addr.IsRFC1918() || mnpair.second.addr.IsLocal()) continue; // do not send local network stakenode
            if (mnpair.second.IsUpdateRequired()) continue; // do not send outdated stakenodes

            CStakenodeBroadcast mnb = CStakenodeBroadcast(mnpair.second);
            LogPrint(BCLog::STAKENODE, "STAKENODESEG -- Sending Stakenode entry: stakenode=%s  addr=%s\n",
                     mnb.pubKeyStakenode.GetID().ToString(), mnb.addr.ToString());
            CStakenodePing mnp = mnpair.second.lastPing;
            uint256 hashMNB = mnb.GetHash();
            uint256 hashMNP = mnp.GetHash();
            pfrom->PushInventory(CInv(MSG_STAKENODE_ANNOUNCE, hashMNB));
            pfrom->PushInventory(CInv(MSG_STAKENODE_PING, hashMNP));
            nInvCount++;

            mapSeenStakenodeBroadcast.insert(std::make_pair(hashMNB, std::make_pair(GetTime(), mnb)));
            mapSeenStakenodePing.insert(std::make_pair(hashMNP, mnp));

            if (pubKeyStakenode == mnpair.first) {
                LogPrintf("STAKENODESEG -- Sent 1 Stakenode inv to peer %d\n", pfrom->GetId());
                return;
            }
        }

        if(!pubKeyStakenode.IsValid()) {
            connman.PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(
                                    NetMsgType::MERCHANTSYNCSTATUSCOUNT, STAKENODE_SYNC_LIST, nInvCount));
            LogPrintf("STAKENODESEG -- Sent %d Stakenode invs to peer %d\n", nInvCount, pfrom->GetId());
            return;
        }
        // smth weird happen - someone asked us for vin we have no idea about?
        LogPrint(BCLog::STAKENODE, "STAKENODESEG -- No invs sent to peer %d\n", pfrom->GetId());

    } else if (strCommand == NetMsgType::STAKENODEVERIFY) { // Stakenode Verify

        // Need LOCK2 here to ensure consistent locking order because the all functions below call GetBlockHash which locks cs_main
        LOCK2(cs_main, cs);

        CStakenodeVerification mnv;
        vRecv >> mnv;

        pfrom->setAskFor.erase(mnv.GetHash());

        if(!stakenodeSync.IsStakenodeListSynced()) return;

        if(mnv.vchSig1.empty()) {
            // CASE 1: someone asked me to verify myself /IP we are using/
            SendVerifyReply(pfrom, mnv, connman);
        } else if (mnv.vchSig2.empty()) {
            // CASE 2: we _probably_ got verification we requested from some stakenode
            ProcessVerifyReply(pfrom, mnv);
        } else {
            // CASE 3: we _probably_ got verification broadcast signed by some stakenode which verified another one
            ProcessVerifyBroadcast(pfrom, mnv);
        }
    }
}

// Verification of stakenodes via unique direct requests.

void CStakenodeMan::DoFullVerificationStep(CConnman& connman)
{
    if(!activeStakenode.pubKeyStakenode.IsValid()) return;
    if(!stakenodeSync.IsSynced()) return;

#if 0
    // Need LOCK2 here to ensure consistent locking order because the SendVerifyRequest call below locks cs_main
    // through GetHeight() signal in ConnectNode
    LOCK2(cs_main, cs);

    int nCount = 0;

    // send verify requests only if we are in top MAX_POSE_RANK
    std::vector<std::pair<int, CStakenode> >::iterator it = vecStakenodeRanks.begin();
    while(it != vecStakenodeRanks.end()) {
        if(it->first > MAX_POSE_RANK) {
            LogPrint(BCLog::STAKENODE, "CStakenodeMan::DoFullVerificationStep -- Must be in top %d to send verify request\n",
                     (int)MAX_POSE_RANK);
            return;
        }
        if(it->second.vin.prevout == activeStakenode.outpoint) {
            nMyRank = it->first;
            LogPrint(BCLog::STAKENODE, "CStakenodeMan::DoFullVerificationStep -- Found self at rank %d/%d, verifying up to %d stakenodes\n",
                     nMyRank, nRanksTotal, (int)MAX_POSE_CONNECTIONS);
            break;
        }
        ++it;
    }

    // edge case: list is too short and this stakenode is not enabled
    if(nMyRank == -1) return;

    // send verify requests to up to MAX_POSE_CONNECTIONS stakenodes
    // starting from MAX_POSE_RANK + nMyRank and using MAX_POSE_CONNECTIONS as a step
    int nOffset = MAX_POSE_RANK + nMyRank - 1;
    if(nOffset >= (int)vecStakenodeRanks.size()) return;

    std::vector<CStakenode*> vSortedByAddr;
    for (auto& mnpair : mapStakenodes) {
        vSortedByAddr.push_back(&mnpair.second);
    }

    sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

    it = vecStakenodeRanks.begin() + nOffset;
    while(it != vecStakenodeRanks.end()) {
        if(it->second.IsPoSeVerified() || it->second.IsPoSeBanned()) {
            LogPrint(BCLog::STAKENODE, "CStakenodeMan::DoFullVerificationStep -- Already %s%s%s stakenode %s address %s, skipping...\n",
                     it->second.IsPoSeVerified() ? "verified" : "",
                     it->second.IsPoSeVerified() && it->second.IsPoSeBanned() ? " and " : "",
                     it->second.IsPoSeBanned() ? "banned" : "",
                     it->second.vin.prevout.ToStringShort(), it->second.addr.ToString());
            nOffset += MAX_POSE_CONNECTIONS;
            if(nOffset >= (int)vecStakenodeRanks.size()) break;
            it += MAX_POSE_CONNECTIONS;
            continue;
        }
        LogPrint(BCLog::STAKENODE, "CStakenodeMan::DoFullVerificationStep -- Verifying stakenode %s rank %d/%d address %s\n",
                 it->second.vin.prevout.ToStringShort(), it->first, nRanksTotal, it->second.addr.ToString());
        if(SendVerifyRequest(CAddress(it->second.addr, NODE_NETWORK), vSortedByAddr, connman)) {
            nCount++;
            if(nCount >= MAX_POSE_CONNECTIONS) break;
        }
        nOffset += MAX_POSE_CONNECTIONS;
        if(nOffset >= (int)vecStakenodeRanks.size()) break;
        it += MAX_POSE_CONNECTIONS;
    }


    LogPrint(BCLog::STAKENODE, "CStakenodeMan::DoFullVerificationStep -- Sent verification requests to %d stakenodes\n", nCount);
#endif
}

// This function tries to find stakenodes with the same addr,
// find a verified one and ban all the other. If there are many nodes
// with the same addr but none of them is verified yet, then none of them are banned.
// It could take many times to run this before most of the duplicate nodes are banned.

void CStakenodeMan::CheckSameAddr()
{
    if(!stakenodeSync.IsSynced() || mapStakenodes.empty()) return;

    std::vector<CStakenode*> vBan;
    std::vector<CStakenode*> vSortedByAddr;

    {
        LOCK(cs);

        CStakenode* pprevStakenode = NULL;
        CStakenode* pverifiedStakenode = NULL;

        for (auto& mnpair : mapStakenodes) {
            vSortedByAddr.push_back(&mnpair.second);
        }

        sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

        for(CStakenode* pmn : vSortedByAddr) {
            // check only (pre)enabled stakenodes
            if(!pmn->IsEnabled() && !pmn->IsPreEnabled()) continue;
            // initial step
            if(!pprevStakenode) {
                pprevStakenode = pmn;
                pverifiedStakenode = pmn->IsPoSeVerified() ? pmn : NULL;
                continue;
            }
            // second+ step
            if(pmn->addr == pprevStakenode->addr) {
                if(pverifiedStakenode) {
                    // another stakenode with the same ip is verified, ban this one
                    vBan.push_back(pmn);
                } else if(pmn->IsPoSeVerified()) {
                    // this stakenode with the same ip is verified, ban previous one
                    vBan.push_back(pprevStakenode);
                    // and keep a reference to be able to ban following stakenodes with the same ip
                    pverifiedStakenode = pmn;
                }
            } else {
                pverifiedStakenode = pmn->IsPoSeVerified() ? pmn : NULL;
            }
            pprevStakenode = pmn;
        }
    }

    // ban duplicates
    for(CStakenode* pmn : vBan) {
        LogPrintf("CStakenodeMan::CheckSameAddr -- increasing PoSe ban score for stakenode %s\n",
                  pmn->pubKeyStakenode.GetID().ToString());
        pmn->IncreasePoSeBanScore();
    }
}

bool CStakenodeMan::SendVerifyRequest(const CAddress& addr, const std::vector<CStakenode*>& vSortedByAddr, CConnman& connman)
{
    if(netfulfilledman.HasFulfilledRequest(addr, strprintf("%s", NetMsgType::STAKENODEVERIFY)+"-request")) {
        // we already asked for verification, not a good idea to do this too often, skip it
        LogPrint(BCLog::STAKENODE, "CStakenodeMan::SendVerifyRequest -- too many requests, skipping... addr=%s\n", addr.ToString());
        return false;
    }

    CNode* pnode = connman.OpenStakenodeConnection(addr);
    if(!pnode) {
        LogPrintf("CStakenodeMan::SendVerifyRequest -- can't connect to node to verify it, addr=%s\n", addr.ToString());
        return false;
    }

    netfulfilledman.AddFulfilledRequest(addr, strprintf("%s", NetMsgType::STAKENODEVERIFY)+"-request");
    // use random nonce, store it and require node to reply with correct one later
    CStakenodeVerification mnv(addr, GetRandInt(999999), nCachedBlockHeight - 1);
    mWeAskedForVerification[addr] = mnv;
    LogPrintf("CStakenodeMan::SendVerifyRequest -- verifying node using nonce %d addr=%s\n", mnv.nonce, addr.ToString());
    connman.PushMessage(pnode, CNetMsgMaker(pnode->GetSendVersion()).Make(NetMsgType::STAKENODEVERIFY, mnv));

    return true;
}

void CStakenodeMan::SendVerifyReply(CNode* pnode, CStakenodeVerification& mnv, CConnman& connman)
{
    // only stakenodes can sign this, why would someone ask regular node?
    if(!fStakeNode) {
        // do not ban, malicious node might be using my IP
        // and trying to confuse the node which tries to verify it
        return;
    }

    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::STAKENODEVERIFY)+"-reply")) {
        // peer should not ask us that often
        LogPrintf("StakenodeMan::SendVerifyReply -- ERROR: peer already asked me recently, peer=%d\n", pnode->GetId());
        Misbehaving(pnode->GetId(), 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        LogPrintf("StakenodeMan::SendVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->GetId());
        return;
    }

    std::string strMessage = strprintf("%s%d%s", activeStakenode.service.ToString(false), mnv.nonce, blockHash.ToString());

    if(!CMessageSigner::SignMessage(strMessage, mnv.vchSig1, activeStakenode.keyStakenode, CPubKey::InputScriptType::SPENDP2PKH)) {
        LogPrintf("StakenodeMan::SendVerifyReply -- SignMessage() failed\n");
        return;
    }

    std::string strError;

    if(!CMessageSigner::VerifyMessage(activeStakenode.pubKeyStakenode.GetID(), mnv.vchSig1, strMessage, strError)) {
        LogPrintf("StakenodeMan::SendVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
        return;
    }

    connman.PushMessage(pnode, CNetMsgMaker(pnode->GetSendVersion()).Make(NetMsgType::STAKENODEVERIFY, mnv));
    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::STAKENODEVERIFY)+"-reply");
}

void CStakenodeMan::ProcessVerifyReply(CNode* pnode, CStakenodeVerification& mnv)
{
    std::string strError;

    // did we even ask for it? if that's the case we should have matching fulfilled request
    if(!netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::STAKENODEVERIFY)+"-request")) {
        LogPrintf("CStakenodeMan::ProcessVerifyReply -- ERROR: we didn't ask for verification of %s, peer=%d\n", pnode->addr.ToString(), pnode->GetId());
        Misbehaving(pnode->GetId(), 20);
        return;
    }

    // Received nonce for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nonce != mnv.nonce) {
        LogPrintf("CStakenodeMan::ProcessVerifyReply -- ERROR: wrong nounce: requested=%d, received=%d, peer=%d\n",
                  mWeAskedForVerification[pnode->addr].nonce, mnv.nonce, pnode->GetId());
        Misbehaving(pnode->GetId(), 20);
        return;
    }

    // Received nBlockHeight for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nBlockHeight != mnv.nBlockHeight) {
        LogPrintf("CStakenodeMan::ProcessVerifyReply -- ERROR: wrong nBlockHeight: requested=%d, received=%d, peer=%d\n",
                  mWeAskedForVerification[pnode->addr].nBlockHeight, mnv.nBlockHeight, pnode->GetId());
        Misbehaving(pnode->GetId(), 20);
        return;
    }



    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("StakenodeMan::ProcessVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->GetId());
        return;
    }

    // we already verified this address, why node is spamming?
    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::STAKENODEVERIFY)+"-done")) {
        LogPrintf("CStakenodeMan::ProcessVerifyReply -- ERROR: already verified %s recently\n", pnode->addr.ToString());
        Misbehaving(pnode->GetId(), 20);
        return;
    }

    {
        LOCK(cs);

        CStakenode* prealStakenode = NULL;
        std::vector<CStakenode*> vpStakenodesToBan;
        std::string strMessage1 = strprintf("%s%d%s", pnode->addr.ToString(false), mnv.nonce, blockHash.ToString());
        for (auto& mnpair : mapStakenodes) {
            if(CAddress(mnpair.second.addr, NODE_NETWORK) == pnode->addr) {
                if(CMessageSigner::VerifyMessage(mnpair.second.pubKeyStakenode.GetID(), mnv.vchSig1, strMessage1, strError)) {
                    // found it!
                    prealStakenode = &mnpair.second;
                    if(!mnpair.second.IsPoSeVerified()) {
                        mnpair.second.DecreasePoSeBanScore();
                    }
                    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::STAKENODEVERIFY)+"-done");

                    // we can only broadcast it if we are an activated stakenode
                    if(!activeStakenode.pubKeyStakenode.IsValid()) continue;
                    // update ...
                    mnv.addr = mnpair.second.addr;
                    mnv.pubKeyStakenode1 = mnpair.second.pubKeyStakenode;
                    mnv.pubKeyStakenode2 = activeStakenode.pubKeyStakenode;
                    std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(false), mnv.nonce, blockHash.ToString(),
                                                        HexStr(mnv.pubKeyStakenode1.Raw()), HexStr(mnv.pubKeyStakenode2.Raw()));
                    // ... and sign it
                    if(!CMessageSigner::SignMessage(strMessage2, mnv.vchSig2, activeStakenode.keyStakenode, CPubKey::InputScriptType::SPENDP2PKH)) {
                        LogPrintf("StakenodeMan::ProcessVerifyReply -- SignMessage() failed\n");
                        return;
                    }

                    std::string strError;

                    if(!CMessageSigner::VerifyMessage(activeStakenode.pubKeyStakenode.GetID(), mnv.vchSig2, strMessage2, strError)) {
                        LogPrintf("StakenodeMan::ProcessVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
                        return;
                    }

                    mWeAskedForVerification[pnode->addr] = mnv;
                    mapSeenStakenodeVerification.insert(std::make_pair(mnv.GetHash(), mnv));
                    mnv.Relay();

                } else {
                    vpStakenodesToBan.push_back(&mnpair.second);
                }
            }
        }
        // no real stakenode found?...
        if(!prealStakenode) {
            // this should never be the case normally,
            // only if someone is trying to game the system in some way or smth like that
            LogPrintf("CStakenodeMan::ProcessVerifyReply -- ERROR: no real stakenode found for addr %s\n", pnode->addr.ToString());
            Misbehaving(pnode->GetId(), 20);
            return;
        }
        LogPrintf("CStakenodeMan::ProcessVerifyReply -- verified real stakenode %s for addr %s\n",
                  prealStakenode->pubKeyStakenode.GetID().ToString(), pnode->addr.ToString());
        // increase ban score for everyone else
        for(CStakenode* pmn : vpStakenodesToBan) {
            pmn->IncreasePoSeBanScore();
            LogPrint(BCLog::STAKENODE, "CStakenodeMan::ProcessVerifyReply -- increased PoSe ban score for %s addr %s, new score %d\n",
                     prealStakenode->pubKeyStakenode.GetID().ToString(), pnode->addr.ToString(), pmn->nPoSeBanScore);
        }
        if(!vpStakenodesToBan.empty())
            LogPrintf("CStakenodeMan::ProcessVerifyReply -- PoSe score increased for %d fake stakenodes, addr %s\n",
                      (int)vpStakenodesToBan.size(), pnode->addr.ToString());
    }
}

void CStakenodeMan::ProcessVerifyBroadcast(CNode* pnode, const CStakenodeVerification& mnv)
{
    std::string strError;

    if(mapSeenStakenodeVerification.find(mnv.GetHash()) != mapSeenStakenodeVerification.end()) {
        // we already have one
        return;
    }
    mapSeenStakenodeVerification[mnv.GetHash()] = mnv;

    // we don't care about history
    if(mnv.nBlockHeight < nCachedBlockHeight - MAX_POSE_BLOCKS) {
        LogPrint(BCLog::STAKENODE, "CStakenodeMan::ProcessVerifyBroadcast -- Outdated: current block %d, verification block %d, peer=%d\n",
                 nCachedBlockHeight, mnv.nBlockHeight, pnode->GetId());
        return;
    }

    if(mnv.pubKeyStakenode1 == mnv.pubKeyStakenode2) {
        LogPrint(BCLog::STAKENODE, "CStakenodeMan::ProcessVerifyBroadcast -- ERROR: same vins %s, peer=%d\n",
                 mnv.pubKeyStakenode1.GetID().ToString(), pnode->GetId());
        // that was NOT a good idea to cheat and verify itself,
        // ban the node we received such message from
        Misbehaving(pnode->GetId(), 100);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("CStakenodeMan::ProcessVerifyBroadcast -- Can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->GetId());
        return;
    }

    int nRank;

#if 0
    if (!GetStakenodeRank(mnv.vin2.prevout, nRank, mnv.nBlockHeight, MIN_POSE_PROTO_VERSION)) {
        LogPrint(BCLog::STAKENODE, "CStakenodeMan::ProcessVerifyBroadcast -- Can't calculate rank for stakenode %s\n",
                 mnv.vin2.prevout.ToStringShort());
        return;
    }
#endif

    if(nRank > MAX_POSE_RANK) {
        LogPrint(BCLog::STAKENODE, "CStakenodeMan::ProcessVerifyBroadcast -- Stakenode %s is not in top %d, current rank %d, peer=%d\n",
                 mnv.pubKeyStakenode2.GetID().ToString(), (int)MAX_POSE_RANK, nRank, pnode->GetId());
        return;
    }

    {
        LOCK(cs);

        std::string strMessage1 = strprintf("%s%d%s", mnv.addr.ToString(false), mnv.nonce, blockHash.ToString());
        std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(false), mnv.nonce, blockHash.ToString(),
                                            HexStr(mnv.pubKeyStakenode1.Raw()), HexStr(mnv.pubKeyStakenode2.Raw()));

        CStakenode* pmn1 = Find(mnv.pubKeyStakenode1);
        if(!pmn1) {
            LogPrintf("CStakenodeMan::ProcessVerifyBroadcast -- can't find stakenode1 %s\n",
                      mnv.pubKeyStakenode1.GetID().ToString());
            return;
        }

        CStakenode* pmn2 = Find(mnv.pubKeyStakenode2);
        if(!pmn2) {
            LogPrintf("CStakenodeMan::ProcessVerifyBroadcast -- can't find stakenode2 %s\n",
                      mnv.pubKeyStakenode2.GetID().ToString());
            return;
        }

        if(pmn1->addr != mnv.addr) {
            LogPrintf("CStakenodeMan::ProcessVerifyBroadcast -- addr %s does not match %s\n", mnv.addr.ToString(), pmn1->addr.ToString());
            return;
        }

        if(!CMessageSigner::VerifyMessage(pmn1->pubKeyStakenode.GetID(), mnv.vchSig1, strMessage1, strError)) {
            LogPrintf("CStakenodeMan::ProcessVerifyBroadcast -- VerifyMessage() for stakenode1 failed, error: %s\n", strError);
            return;
        }

        if(!CMessageSigner::VerifyMessage(pmn2->pubKeyStakenode.GetID(), mnv.vchSig2, strMessage2, strError)) {
            LogPrintf("CStakenodeMan::ProcessVerifyBroadcast -- VerifyMessage() for stakenode2 failed, error: %s\n", strError);
            return;
        }

        if(!pmn1->IsPoSeVerified()) {
            pmn1->DecreasePoSeBanScore();
        }
        mnv.Relay();

        LogPrintf("CStakenodeMan::ProcessVerifyBroadcast -- verified stakenode %s for addr %s\n",
                  pmn1->pubKeyStakenode.GetID().ToString(), pmn1->addr.ToString());

        // increase ban score for everyone else with the same addr
        int nCount = 0;
        for (auto& mnpair : mapStakenodes) {
            if(mnpair.second.addr != mnv.addr || mnpair.first == mnv.pubKeyStakenode1) continue;
            mnpair.second.IncreasePoSeBanScore();
            nCount++;
            LogPrint(BCLog::STAKENODE, "CStakenodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                     mnpair.first.GetID().ToString(), mnpair.second.addr.ToString(), mnpair.second.nPoSeBanScore);
        }
        if(nCount)
            LogPrintf("CStakenodeMan::ProcessVerifyBroadcast -- PoSe score increased for %d fake stakenodes, addr %s\n",
                      nCount, pmn1->addr.ToString());
    }
}

std::string CStakenodeMan::ToString() const
{
    std::ostringstream info;

    info << "Stakenodes: " << (int)mapStakenodes.size() <<
            ", peers who asked us for Stakenode list: " << (int)mAskedUsForStakenodeList.size() <<
            ", peers we asked for Stakenode list: " << (int)mWeAskedForStakenodeList.size() <<
            ", entries in Stakenode list we asked for: " << (int)mWeAskedForStakenodeListEntry.size() <<
            ", nDsqCount: " << (int)nDsqCount;

    return info.str();
}

void CStakenodeMan::UpdateStakenodeList(CStakenodeBroadcast mnb, CConnman& connman)
{
    LOCK2(cs_main, cs);
    mapSeenStakenodePing.insert(std::make_pair(mnb.lastPing.GetHash(), mnb.lastPing));
    mapSeenStakenodeBroadcast.insert(std::make_pair(mnb.GetHash(), std::make_pair(GetTime(), mnb)));

    LogPrintf("CStakenodeMan::UpdateStakenodeList -- stakenode=%s  addr=%s\n", mnb.pubKeyStakenode.GetID().ToString(), mnb.addr.ToString());

    CStakenode* pmn = Find(mnb.pubKeyStakenode);
    if(pmn == NULL) {

        if(Add(mnb)) {
            stakenodeSync.BumpAssetLastTime("CStakenodeMan::UpdateStakenodeList - new");
        }
    } else {
        CStakenodeBroadcast mnbOld = mapSeenStakenodeBroadcast[CStakenodeBroadcast(*pmn).GetHash()].second;
        if(pmn->UpdateFromNewBroadcast(mnb, connman)) {
            stakenodeSync.BumpAssetLastTime("CStakenodeMan::UpdateStakenodeList - seen");
            mapSeenStakenodeBroadcast.erase(mnbOld.GetHash());
        }
    }
}

bool CStakenodeMan::CheckMnbAndUpdateStakenodeList(CNode* pfrom, CStakenodeBroadcast mnb, int& nDos, CConnman& connman)
{
    {
        // we need to lock in this order because function that called us uses same order, bad practice, but no other choice because of recursive mutexes.
        LOCK2(cs_main, cs);
        nDos = 0;
        LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckMnbAndUpdateStakenodeList -- stakenode=%s\n", mnb.pubKeyStakenode.GetID().ToString());

        uint256 hash = mnb.GetHash();
        if(mapSeenStakenodeBroadcast.count(hash) && !mnb.fRecovery) { //seen
            LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckMnbAndUpdateStakenodeList -- stakenode=%s seen\n",
                     mnb.pubKeyStakenode.GetID().ToString());
            // less then 2 pings left before this MN goes into non-recoverable state, bump sync timeout
            if(GetTime() - mapSeenStakenodeBroadcast[hash].first > STAKENODE_NEW_START_REQUIRED_SECONDS - STAKENODE_MIN_MNP_SECONDS * 2) {
                LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckMnbAndUpdateStakenodeList -- stakenode=%s seen update\n",
                         mnb.pubKeyStakenode.GetID().ToString());
                mapSeenStakenodeBroadcast[hash].first = GetTime();
                stakenodeSync.BumpAssetLastTime("CStakenodeMan::CheckMnbAndUpdateStakenodeList - seen");
            }
            // did we ask this node for it?
            if(pfrom && IsMnbRecoveryRequested(hash) && GetTime() < mMnbRecoveryRequests[hash].first) {
                LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckMnbAndUpdateStakenodeList -- mnb=%s seen request\n", hash.ToString());
                if(mMnbRecoveryRequests[hash].second.count(pfrom->addr)) {
                    LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckMnbAndUpdateStakenodeList -- mnb=%s seen request, addr=%s\n", hash.ToString(), pfrom->addr.ToString());
                    // do not allow node to send same mnb multiple times in recovery mode
                    mMnbRecoveryRequests[hash].second.erase(pfrom->addr);
                    // does it have newer lastPing?
                    if(mnb.lastPing.sigTime > mapSeenStakenodeBroadcast[hash].second.lastPing.sigTime) {
                        // simulate Check
                        CStakenode mnTemp = CStakenode(mnb);
                        mnTemp.Check();
                        LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckMnbAndUpdateStakenodeList -- mnb=%s seen request, addr=%s, better lastPing: %d min ago, projected mn state: %s\n", hash.ToString(), pfrom->addr.ToString(), (GetAdjustedTime() - mnb.lastPing.sigTime)/60, mnTemp.GetStateString());
                        if(mnTemp.IsValidStateForAutoStart(mnTemp.nActiveState)) {
                            // this node thinks it's a good one
                            LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckMnbAndUpdateStakenodeList -- stakenode=%s seen good\n",
                                     mnb.pubKeyStakenode.GetID().ToString());
                            mMnbRecoveryGoodReplies[hash].push_back(mnb);
                        }
                    }
                }
            }
            return true;
        }
        mapSeenStakenodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), mnb)));

        LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckMnbAndUpdateStakenodeList -- stakenode=%s new\n",
                 mnb.pubKeyStakenode.GetID().ToString());

        {
            // Need to lock cs_main here to ensure consistent locking order because the SimpleCheck call below locks cs_main
//            LOCK(cs_main);
            if(!mnb.SimpleCheck(nDos)) {
                LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckMnbAndUpdateStakenodeList -- SimpleCheck() failed, stakenode=%s\n",
                         mnb.pubKeyStakenode.GetID().ToString());
                return false;
            }
        }

        // search Stakenode list
        CStakenode* pmn = Find(mnb.pubKeyStakenode);
        if(pmn) {
            CStakenodeBroadcast mnbOld = mapSeenStakenodeBroadcast[CStakenodeBroadcast(*pmn).GetHash()].second;
            if(!mnb.Update(pmn, nDos, connman)) {
                LogPrint(BCLog::STAKENODE, "CStakenodeMan::CheckMnbAndUpdateStakenodeList -- Update() failed, stakenode=%s\n",
                         mnb.pubKeyStakenode.GetID().ToString());
                return false;
            }
            if(hash != mnbOld.GetHash()) {
                mapSeenStakenodeBroadcast.erase(mnbOld.GetHash());
            }
            return true;
        }
    }

    if(mnb.CheckStakenode(nDos)) {

        Add(mnb);
        stakenodeSync.BumpAssetLastTime("CStakenodeMan::CheckMnbAndUpdateStakenodeList - new");
        // if it matches our Stakenode privkey...
        if(fStakeNode && mnb.pubKeyStakenode == activeStakenode.pubKeyStakenode) {
            mnb.nPoSeBanScore = -STAKENODE_POSE_BAN_MAX_SCORE;
            if(mnb.nProtocolVersion == PROTOCOL_VERSION) {
                // ... and PROTOCOL_VERSION, then we've been remotely activated ...
                LogPrintf("CStakenodeMan::CheckMnbAndUpdateStakenodeList -- Got NEW Stakenode entry: stakenode=%s  sigTime=%lld  addr=%s\n",
                          mnb.pubKeyStakenode.GetID().ToString(), mnb.sigTime, mnb.addr.ToString());
                activeStakenode.ManageState(connman);
            } else {
                // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
                // but also do not ban the node we get this message from
                LogPrintf("CStakenodeMan::CheckMnbAndUpdateStakenodeList -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", mnb.nProtocolVersion, PROTOCOL_VERSION);
                return false;
            }
        }
        mnb.Relay(connman);
    } else {
        LogPrintf("CStakenodeMan::CheckMnbAndUpdateStakenodeList -- Rejected Stakenode entry: %s  addr=%s\n",
                  mnb.pubKeyStakenode.GetID().ToString(), mnb.addr.ToString());
        return false;
    }

    return true;
}

void CStakenodeMan::UpdateWatchdogVoteTime(const CPubKey &pubKeyStakenode, uint64_t nVoteTime)
{
    LOCK(cs);
    CStakenode* pmn = Find(pubKeyStakenode);
    if(!pmn) {
        return;
    }
    pmn->UpdateWatchdogVoteTime(nVoteTime);
    nLastWatchdogVoteTime = GetTime();
}

bool CStakenodeMan::IsWatchdogActive()
{
    LOCK(cs);
    // Check if any stakenodes have voted recently, otherwise return false
    return (GetTime() - nLastWatchdogVoteTime) <= STAKENODE_WATCHDOG_MAX_SECONDS;
}

void CStakenodeMan::CheckStakenode(const CPubKey& pubKeyStakenode, bool fForce)
{
    LOCK2(cs_main, cs);
    for (auto& mnpair : mapStakenodes) {
        if (mnpair.second.pubKeyStakenode == pubKeyStakenode) {
            mnpair.second.Check(fForce);
            return;
        }
    }
}

bool CStakenodeMan::IsStakenodePingedWithin(const CPubKey &pubKeyStakenode, int nSeconds, int64_t nTimeToCheckAt)
{
    LOCK(cs);
    CStakenode* pmn = Find(pubKeyStakenode);
    return pmn ? pmn->IsPingedWithin(nSeconds, nTimeToCheckAt) : false;
}

void CStakenodeMan::SetStakenodeLastPing(const CPubKey &pubKeyStakenode, const CStakenodePing& mnp)
{
    LOCK(cs);
    CStakenode* pmn = Find(pubKeyStakenode);
    if(!pmn) {
        return;
    }
    pmn->lastPing = mnp;
    // if stakenode uses sentinel ping instead of watchdog
    // we shoud update nTimeLastWatchdogVote here if sentinel
    // ping flag is actual
    if(mnp.fSentinelIsCurrent) {
        UpdateWatchdogVoteTime(mnp.stakenodePubKey, mnp.sigTime);
    }
    mapSeenStakenodePing.insert(std::make_pair(mnp.GetHash(), mnp));

    CStakenodeBroadcast mnb(*pmn);
    uint256 hash = mnb.GetHash();
    if(mapSeenStakenodeBroadcast.count(hash)) {
        mapSeenStakenodeBroadcast[hash].second.lastPing = mnp;
    }
}

void CStakenodeMan::UpdatedBlockTip(const CBlockIndex *pindex)
{
    nCachedBlockHeight = pindex->nHeight;
    LogPrint(BCLog::STAKENODE, "CStakenodeMan::UpdatedBlockTip -- nCachedBlockHeight=%d\n", nCachedBlockHeight);

    CheckSameAddr();
}

void ThreadStakenodeCheck(CConnman &connman)
{
    if(fLiteMode) return; // disable all Galactrum specific functionality

    static bool fOneThread;
    if(fOneThread) return;
    fOneThread = true;

    RenameThread("galactrum-tpos");

    unsigned int nTick = 0;

    while (true)
    {
        MilliSleep(1000);

        // try to sync from all available nodes, one step at a time
        stakenodeSync.ProcessTick(connman);

        if(stakenodeSync.IsBlockchainSynced() && !ShutdownRequested()) {

            nTick++;

            // make sure to check all stakenodes first
            stakenodeman.Check();

            // check if we should activate or ping every few minutes,
            // slightly postpone first run to give net thread a chance to connect to some peers
            if(nTick % STAKENODE_MIN_MNP_SECONDS == 15)
                activeStakenode.ManageState(connman);

            if(nTick % 60 == 0) {
                stakenodeman.ProcessStakenodeConnections(connman);
                stakenodeman.CheckAndRemove(connman);
            }
            if(fStakeNode && (nTick % (60 * 5) == 0)) {
                stakenodeman.DoFullVerificationStep(connman);
            }
        }
    }

}
