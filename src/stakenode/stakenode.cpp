// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <stakenode/activestakenode.h>
#include <key_io.h>
#include <init.h>
#include <netbase.h>
#include <stakenode/stakenode.h>
#include <stakenode/stakenodeman.h>
#include <stakenode/stakenode-sync.h>
#include <messagesigner.h>
#include <script/standard.h>
#include <util.h>
#ifdef ENABLE_WALLET
#include <wallet/wallet.h>
#endif // ENABLE_WALLET

#include <boost/lexical_cast.hpp>


CStakenode::CStakenode() :
    stakenode_info_t{ STAKENODE_ENABLED, PROTOCOL_VERSION, GetAdjustedTime()}
{}

CStakenode::CStakenode(CService addr, CPubKey pubKeyStakenode, uint256 hashTPoSContractTxNew, int nProtocolVersionIn) :
    stakenode_info_t{ STAKENODE_ENABLED, nProtocolVersionIn, GetAdjustedTime(), addr, pubKeyStakenode, hashTPoSContractTxNew }
{}

CStakenode::CStakenode(const CStakenode& other) :
    stakenode_info_t{other},
    lastPing(other.lastPing),
    vchSig(other.vchSig),
    nPoSeBanScore(other.nPoSeBanScore),
    nPoSeBanHeight(other.nPoSeBanHeight),
    fUnitTest(other.fUnitTest)
{}

CStakenode::CStakenode(const CStakenodeBroadcast& mnb) :
    stakenode_info_t{ mnb.nActiveState, mnb.nProtocolVersion,
                         mnb.sigTime, mnb.addr,
                         mnb.pubKeyStakenode,
                         mnb.hashTPoSContractTx,
                         mnb.sigTime /*nTimeLastWatchdogVote*/},
    lastPing(mnb.lastPing),
    vchSig(mnb.vchSig)
{}

//
// When a new stakenode broadcast is sent, update our information
//
bool CStakenode::UpdateFromNewBroadcast(CStakenodeBroadcast& mnb, CConnman& connman)
{
    if(mnb.sigTime <= sigTime && !mnb.fRecovery) return false;

    pubKeyStakenode = mnb.pubKeyStakenode;
    hashTPoSContractTx = mnb.hashTPoSContractTx;
    sigTime = mnb.sigTime;
    vchSig = mnb.vchSig;
    nProtocolVersion = mnb.nProtocolVersion;
    addr = mnb.addr;
    nPoSeBanScore = 0;
    nPoSeBanHeight = 0;
    nTimeLastChecked = 0;
    int nDos = 0;
    if(mnb.lastPing == CStakenodePing() || (mnb.lastPing != CStakenodePing() && mnb.lastPing.CheckAndUpdate(this, true, nDos, connman))) {
        lastPing = mnb.lastPing;
        stakenodeman.mapSeenStakenodePing.insert(std::make_pair(lastPing.GetHash(), lastPing));
    }
    // if it matches our Stakenode privkey...
    if(fStakeNode && pubKeyStakenode == activeStakenode.pubKeyStakenode) {
        nPoSeBanScore = -STAKENODE_POSE_BAN_MAX_SCORE;
        if(nProtocolVersion == PROTOCOL_VERSION) {
            // ... and PROTOCOL_VERSION, then we've been remotely activated ...
            activeStakenode.ManageState(connman);
        } else {
            // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
            // but also do not ban the node we get this message from
            LogPrintf("CStakenode::UpdateFromNewBroadcast -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", nProtocolVersion, PROTOCOL_VERSION);
            return false;
        }
    }
    return true;
}

void CStakenode::Check(bool fForce)
{
    LOCK2(cs_main, cs);

    if(ShutdownRequested()) return;

    if(!fForce && (GetTime() - nTimeLastChecked < STAKENODE_CHECK_SECONDS)) return;
    nTimeLastChecked = GetTime();

    LogPrint(BCLog::STAKENODE, "CStakenode::Check -- Stakenode %s is in %s state\n", pubKeyStakenode.GetID().ToString(), GetStateString());

    int nHeight = 0;
    if(!fUnitTest) {
        nHeight = chainActive.Height();
    }

    if(IsPoSeBanned()) {
        if(nHeight < nPoSeBanHeight) return; // too early?
        // Otherwise give it a chance to proceed further to do all the usual checks and to change its state.
        // Stakenode still will be on the edge and can be banned back easily if it keeps ignoring mnverify
        // or connect attempts. Will require few mnverify messages to strengthen its position in mn list.
        LogPrintf("CStakenode::Check -- Stakenode %s is unbanned and back in list now\n",
                  pubKeyStakenode.GetID().ToString());
        DecreasePoSeBanScore();
    } else if(nPoSeBanScore >= STAKENODE_POSE_BAN_MAX_SCORE) {
        nActiveState = STAKENODE_POSE_BAN;
        // ban for the whole payment cycle
        nPoSeBanHeight = 60;
        LogPrintf("CStakenode::Check -- Stakenode %s is banned till block %d now\n",
                  pubKeyStakenode.GetID().ToString(), nPoSeBanHeight);
        return;
    }

    int nActiveStatePrev = nActiveState;
    bool fOurStakenode = fStakeNode && activeStakenode.pubKeyStakenode == pubKeyStakenode;

    // stakenode doesn't meet payment protocol requirements ...
    bool fRequireUpdate =
            // or it's our own node and we just updated it to the new protocol but we are still waiting for activation ...
            (fOurStakenode && nProtocolVersion < PROTOCOL_VERSION);

    if(fRequireUpdate) {
        nActiveState = STAKENODE_UPDATE_REQUIRED;
        if(nActiveStatePrev != nActiveState) {
            LogPrint(BCLog::STAKENODE, "CStakenode::Check -- Stakenode %s is in %s state now\n",
                     pubKeyStakenode.GetID().ToString(), GetStateString());
        }
        return;
    }

    // keep old stakenodes on start, give them a chance to receive updates...
    bool fWaitForPing = !stakenodeSync.IsStakenodeListSynced() && !IsPingedWithin(STAKENODE_MIN_MNP_SECONDS);

    if(fWaitForPing && !fOurStakenode) {
        // ...but if it was already expired before the initial check - return right away
        if(IsExpired() || IsWatchdogExpired() || IsNewStartRequired()) {
            LogPrint(BCLog::STAKENODE, "CStakenode::Check -- Stakenode %s is in %s state, waiting for ping\n",
                     pubKeyStakenode.GetID().ToString(), GetStateString());
            return;
        }
    }

    // don't expire if we are still in "waiting for ping" mode unless it's our own stakenode
    if(!fWaitForPing || fOurStakenode) {

        if(!IsPingedWithin(STAKENODE_NEW_START_REQUIRED_SECONDS)) {
            nActiveState = STAKENODE_NEW_START_REQUIRED;
            if(nActiveStatePrev != nActiveState) {
                LogPrint(BCLog::STAKENODE, "CStakenode::Check -- Stakenode %s is in %s state now\n",
                         pubKeyStakenode.GetID().ToString(), GetStateString());
            }
            return;
        }

        bool fWatchdogActive = stakenodeSync.IsSynced() && stakenodeman.IsWatchdogActive();
        bool fWatchdogExpired = (fWatchdogActive && ((GetAdjustedTime() - nTimeLastWatchdogVote) > STAKENODE_WATCHDOG_MAX_SECONDS));

        LogPrint(BCLog::STAKENODE, "CStakenode::Check -- outpoint=%s, nTimeLastWatchdogVote=%d, GetAdjustedTime()=%d, fWatchdogExpired=%d\n",
                 pubKeyStakenode.GetID().ToString(), nTimeLastWatchdogVote, GetAdjustedTime(), fWatchdogExpired);

        if(fWatchdogExpired) {
            nActiveState = STAKENODE_WATCHDOG_EXPIRED;
            if(nActiveStatePrev != nActiveState) {
                LogPrint(BCLog::STAKENODE, "CStakenode::Check -- Stakenode %s is in %s state now\n",
                         pubKeyStakenode.GetID().ToString(), GetStateString());
            }
            return;
        }

        if(!IsPingedWithin(STAKENODE_EXPIRATION_SECONDS)) {
            nActiveState = STAKENODE_EXPIRED;
            if(nActiveStatePrev != nActiveState) {
                LogPrint(BCLog::STAKENODE, "CStakenode::Check -- Stakenode %s is in %s state now\n",
                         pubKeyStakenode.GetID().ToString(), GetStateString());
            }
            return;
        }
    }

    if(lastPing.sigTime - sigTime < STAKENODE_MIN_MNP_SECONDS) {
        nActiveState = STAKENODE_PRE_ENABLED;
        if(nActiveStatePrev != nActiveState) {
            LogPrint(BCLog::STAKENODE, "CStakenode::Check -- Stakenode %s is in %s state now\n",
                     pubKeyStakenode.GetID().ToString(), GetStateString());
        }
        return;
    }

    nActiveState = STAKENODE_ENABLED; // OK
    if(nActiveStatePrev != nActiveState) {
        LogPrint(BCLog::STAKENODE, "CStakenode::Check -- Stakenode %s is in %s state now\n",
                 pubKeyStakenode.GetID().ToString(), GetStateString());
    }
}

bool CStakenode::IsValidNetAddr() const
{
    return IsValidNetAddr(addr);
}

bool CStakenode::IsValidNetAddr(CService addrIn)
{
    // TODO: regtest is fine with any addresses for now,
    // should probably be a bit smarter if one day we start to implement tests for this
    return Params().NetworkIDString() == CBaseChainParams::REGTEST ||
            (addrIn.IsIPv4() && IsReachable(addrIn) && addrIn.IsRoutable());
}

stakenode_info_t CStakenode::GetInfo() const
{
    stakenode_info_t info{*this};
    info.nTimeLastPing = lastPing.sigTime;
    info.fInfoValid = true;
    return info;
}

std::string CStakenode::StateToString(int nStateIn)
{
    switch(nStateIn) {
    case STAKENODE_PRE_ENABLED:            return "PRE_ENABLED";
    case STAKENODE_ENABLED:                return "ENABLED";
    case STAKENODE_EXPIRED:                return "EXPIRED";
    case STAKENODE_UPDATE_REQUIRED:        return "UPDATE_REQUIRED";
    case STAKENODE_WATCHDOG_EXPIRED:       return "WATCHDOG_EXPIRED";
    case STAKENODE_NEW_START_REQUIRED:     return "NEW_START_REQUIRED";
    case STAKENODE_POSE_BAN:               return "POSE_BAN";
    default:                                return "UNKNOWN";
    }
}

std::string CStakenode::GetStateString() const
{
    return StateToString(nActiveState);
}

std::string CStakenode::GetStatus() const
{
    // TODO: return smth a bit more human readable here
    return GetStateString();
}

#ifdef ENABLE_WALLET
bool CStakenodeBroadcast::Create(std::string strService, std::string strStakenodePrivKey,
                                    std::string strHashTPoSContractTx, std::string& strErrorRet,
                                    CStakenodeBroadcast &mnbRet, bool fOffline)
{
    CPubKey pubKeyStakenodeNew;
    CKey keyStakenodeNew;

    auto Log = [&strErrorRet](std::string sErr)->bool
    {
        strErrorRet = sErr;
        LogPrintf("CStakenodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    };

    //need correct blocks to send ping
    if (!fOffline && !stakenodeSync.IsBlockchainSynced())
        return Log("Sync in progress. Must wait until sync is complete to start Stakenode");

    if (!CMessageSigner::GetKeysFromSecret(strStakenodePrivKey, keyStakenodeNew, pubKeyStakenodeNew))
        return Log(strprintf("Invalid stakenode key %s", strStakenodePrivKey));

    CService service;
    if (!Lookup(strService.c_str(), service, 0, false))
        return Log(strprintf("Invalid address %s for stakenode.", strService));
    int mainnetDefaultPort = CreateChainParams(CBaseChainParams::MAIN)->GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (service.GetPort() != mainnetDefaultPort)
            return Log(strprintf("Invalid port %u for stakenode %s, only %d is supported on mainnet.", service.GetPort(), strService, mainnetDefaultPort));
    } else if (service.GetPort() == mainnetDefaultPort)
        return Log(strprintf("Invalid port %u for stakenode %s, %d is the only supported on mainnet.", service.GetPort(), strService, mainnetDefaultPort));

    return Create(service, keyStakenodeNew, pubKeyStakenodeNew, uint256S(strHashTPoSContractTx), strErrorRet, mnbRet);
}

bool CStakenodeBroadcast::Create(const CService& service, const CKey& keyStakenodeNew,
                                    const CPubKey& pubKeyStakenodeNew, const uint256 &hashTPoSContractTx,
                                    std::string &strErrorRet, CStakenodeBroadcast &mnbRet)
{
    // wait for reindex and/or import to finish
    if (fImporting || fReindex) return false;

    LogPrint(BCLog::STAKENODE, "CStakenodeBroadcast::Create -- pubKeyStakenodeNew.GetID() = %s\n",
             pubKeyStakenodeNew.GetID().ToString());

    auto Log = [&strErrorRet,&mnbRet](std::string sErr)->bool
    {
        strErrorRet = sErr;
        LogPrintf("CStakenodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CStakenodeBroadcast();
        return false;
    };

    CStakenodePing mnp(pubKeyStakenodeNew);
    if (!mnp.Sign(keyStakenodeNew, pubKeyStakenodeNew))
        return Log(strprintf("Failed to sign ping, stakenode=%s",
                             pubKeyStakenodeNew.GetID().ToString()));

    mnbRet = CStakenodeBroadcast(service, pubKeyStakenodeNew, hashTPoSContractTx, PROTOCOL_VERSION);

    if (!mnbRet.IsValidNetAddr())
        return Log(strprintf("Invalid IP address, stakenode=%s",
                             pubKeyStakenodeNew.GetID().ToString()));

    mnbRet.lastPing = mnp;
    if (!mnbRet.Sign(keyStakenodeNew))
        return Log(strprintf("Failed to sign broadcast, stakenode=%s",
                             pubKeyStakenodeNew.GetID().ToString()));

    return true;
}
#endif // ENABLE_WALLET

bool CStakenodeBroadcast::SimpleCheck(int& nDos)
{
    nDos = 0;

    // make sure addr is valid
    if(!IsValidNetAddr()) {
        LogPrintf("CStakenodeBroadcast::SimpleCheck -- Invalid addr, rejected: stakenode=%s  addr=%s\n",
                  pubKeyStakenode.GetID().ToString(), addr.ToString());
        return false;
    }

    // make sure signature isn't in the future (past is OK)
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CStakenodeBroadcast::SimpleCheck -- Signature rejected, too far into the future: stakenode=%s\n",
                  pubKeyStakenode.GetID().ToString());
        nDos = 1;
        return false;
    }

    // empty ping or incorrect sigTime/unknown blockhash
    if(lastPing == CStakenodePing() || !lastPing.SimpleCheck(nDos)) {
        // one of us is probably forked or smth, just mark it as expired and check the rest of the rules
        nActiveState = STAKENODE_EXPIRED;
    }

    if(nProtocolVersion < PRESEGWIT_PROTO_VERSION) {
        LogPrintf("CStakenodeBroadcast::SimpleCheck -- ignoring outdated Stakenode: stakenode=%s  nProtocolVersion=%d\n",
                  pubKeyStakenode.GetID().ToString(), nProtocolVersion);
        return false;
    }

    CScript pubkeyScript;
    pubkeyScript = GetScriptForDestination(pubKeyStakenode.GetID());

    if(pubkeyScript.size() != 25) {
        LogPrintf("CStakenodeBroadcast::SimpleCheck -- pubKeyCollateralAddress has the wrong size\n");
        nDos = 100;
        return false;
    }

    int mainnetDefaultPort = CreateChainParams(CBaseChainParams::MAIN)->GetDefaultPort();
    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(addr.GetPort() != mainnetDefaultPort) return false;
    } else if(addr.GetPort() == mainnetDefaultPort) return false;

    return true;
}

bool CStakenodeBroadcast::Update(CStakenode* pmn, int& nDos, CConnman& connman)
{
    nDos = 0;

    if(pmn->sigTime == sigTime && !fRecovery) {
        // mapSeenStakenodeBroadcast in CStakenodeMan::CheckMnbAndUpdateStakenodeList should filter legit duplicates
        // but this still can happen if we just started, which is ok, just do nothing here.
        return false;
    }

    // this broadcast is older than the one that we already have - it's bad and should never happen
    // unless someone is doing something fishy
    if(pmn->sigTime > sigTime) {
        LogPrintf("CStakenodeBroadcast::Update -- Bad sigTime %d (existing broadcast is at %d) for Stakenode %s %s\n",
                  sigTime, pmn->sigTime, pubKeyStakenode.GetID().ToString(), addr.ToString());
        return false;
    }

    pmn->Check();

    // stakenode is banned by PoSe
    if(pmn->IsPoSeBanned()) {
        LogPrintf("CStakenodeBroadcast::Update -- Banned by PoSe, stakenode=%s\n",
                  pubKeyStakenode.GetID().ToString());
        return false;
    }

    // IsVnAssociatedWithPubkey is validated once in CheckOutpoint, after that they just need to match
    if(pmn->pubKeyStakenode != pubKeyStakenode) {
        LogPrintf("CStakenodeBroadcast::Update -- Got mismatched pubKeyStakenode");
        nDos = 33;
        return false;
    }

    if (!CheckSignature(nDos)) {
        LogPrintf("CStakenodeBroadcast::Update -- CheckSignature() failed, stakenode=%s\n",
                  pubKeyStakenode.GetID().ToString());
        return false;
    }

    // if ther was no stakenode broadcast recently or if it matches our Stakenode privkey...
    if(!pmn->IsBroadcastedWithin(STAKENODE_MIN_MNB_SECONDS) || (fStakeNode && pubKeyStakenode == activeStakenode.pubKeyStakenode)) {
        // take the newest entry
        LogPrintf("CStakenodeBroadcast::Update -- Got UPDATED Stakenode entry: addr=%s\n", addr.ToString());
        if(pmn->UpdateFromNewBroadcast(*this, connman)) {
            pmn->Check();
            Relay(connman);
        }
        stakenodeSync.BumpAssetLastTime("CStakenodeBroadcast::Update");
    }

    return true;
}

bool CStakenodeBroadcast::CheckStakenode(int &nDos)
{
    nDos = 0;
    return CheckSignature(nDos);
}

bool CStakenodeBroadcast::Sign(const CKey& keyCollateralAddress)
{
    std::string strError;
    std::string strMessage;

    sigTime = GetAdjustedTime();

    strMessage = addr.ToString(false) + boost::lexical_cast<std::string>(sigTime) +
            pubKeyStakenode.GetID().ToString() +
            boost::lexical_cast<std::string>(nProtocolVersion);

    if(!CMessageSigner::SignMessage(strMessage, vchSig, keyCollateralAddress, CPubKey::InputScriptType::SPENDP2PKH)) {
        LogPrintf("CStakenodeBroadcast::Sign -- SignMessage() failed\n");
        return false;
    }

    if(!CMessageSigner::VerifyMessage(pubKeyStakenode.GetID(), vchSig, strMessage, strError)) {
        LogPrintf("CStakenodeBroadcast::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CStakenodeBroadcast::CheckSignature(int& nDos)
{
    std::string strMessage;
    std::string strError = "";
    nDos = 0;

    strMessage = addr.ToString(false) + boost::lexical_cast<std::string>(sigTime) +
            pubKeyStakenode.GetID().ToString() +
            boost::lexical_cast<std::string>(nProtocolVersion);

    LogPrint(BCLog::STAKENODE, "CStakenodeBroadcast::CheckSignature -- strMessage: %s  pubKeyStakenode address: %s  sig: %s\n", strMessage, CBitcoinAddress(pubKeyStakenode.GetID()).ToString(), EncodeBase64(&vchSig[0], vchSig.size()));

    if(!CMessageSigner::VerifyMessage(pubKeyStakenode.GetID(), vchSig, strMessage, strError)){
        LogPrintf("CStakenodeBroadcast::CheckSignature -- Got bad Stakenode announce signature, error: %s\n", strError);
        nDos = 100;
        return false;
    }

    return true;
}

void CStakenodeBroadcast::Relay(CConnman& connman)
{
    // Do not relay until fully synced
    if(!stakenodeSync.IsSynced()) {
        LogPrint(BCLog::STAKENODE, "CStakenodeBroadcast::Relay -- won't relay until fully synced\n");
        return;
    }

    CInv inv(MSG_STAKENODE_ANNOUNCE, GetHash());
    connman.ForEachNode([&inv](CNode* pnode)
    {
        pnode->PushInventory(inv);
    });
}

CStakenodePing::CStakenodePing(const CPubKey &stakenodePubKey)
{
    LOCK(cs_main);
    if (!chainActive.Tip() || chainActive.Height() < 12) return;

    this->stakenodePubKey = stakenodePubKey;
    blockHash = chainActive[chainActive.Height() - 12]->GetBlockHash();
    sigTime = GetAdjustedTime();
}

bool CStakenodePing::Sign(const CKey& keyStakenode, const CPubKey& pubKeyStakenode)
{
    std::string strError;
    std::string strMasterNodeSignMessage;

    // TODO: add sentinel data
    sigTime = GetAdjustedTime();
    std::string strMessage = stakenodePubKey.GetID().ToString() + blockHash.ToString() + boost::lexical_cast<std::string>(sigTime);

    if(!CMessageSigner::SignMessage(strMessage, vchSig, keyStakenode, CPubKey::InputScriptType::SPENDP2PKH)) {
        LogPrintf("CStakenodePing::Sign -- SignMessage() failed\n");
        return false;
    }

    if(!CMessageSigner::VerifyMessage(pubKeyStakenode.GetID(), vchSig, strMessage, strError)) {
        LogPrintf("CStakenodePing::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CStakenodePing::CheckSignature(CPubKey& pubKeyStakenode, int &nDos)
{
    // TODO: add sentinel data
    std::string strMessage = stakenodePubKey.GetID().ToString() + blockHash.ToString() + boost::lexical_cast<std::string>(sigTime);
    std::string strError = "";
    nDos = 0;

    if(!CMessageSigner::VerifyMessage(pubKeyStakenode.GetID(), vchSig, strMessage, strError)) {
        LogPrintf("CStakenodePing::CheckSignature -- Got bad Stakenode ping signature, stakenode=%s, error: %s\n",
                  stakenodePubKey.GetID().ToString(), strError);
        nDos = 33;
        return false;
    }
    return true;
}

bool CStakenodePing::SimpleCheck(int& nDos)
{
    // don't ban by default
    nDos = 0;

    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CStakenodePing::SimpleCheck -- Signature rejected, too far into the future, stakenode=%s\n",
                  stakenodePubKey.GetID().ToString());
        nDos = 1;
        return false;
    }

    {
        AssertLockHeld(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if (mi == mapBlockIndex.end()) {
            LogPrint(BCLog::STAKENODE, "CStakenodePing::SimpleCheck -- Stakenode ping is invalid, unknown block hash: stakenode=%s blockHash=%s\n",
                     stakenodePubKey.GetID().ToString(), blockHash.ToString());
            // maybe we stuck or forked so we shouldn't ban this node, just fail to accept this ping
            // TODO: or should we also request this block?
            return false;
        }
    }
    LogPrint(BCLog::STAKENODE, "CStakenodePing::SimpleCheck -- Stakenode ping verified: stakenode=%s  blockHash=%s  sigTime=%d\n",
             stakenodePubKey.GetID().ToString(), blockHash.ToString(), sigTime);
    return true;
}

bool CStakenodePing::CheckAndUpdate(CStakenode* pmn, bool fFromNewBroadcast, int& nDos, CConnman& connman)
{
    // don't ban by default
    nDos = 0;

    {
        LOCK(cs_main);
        if (!SimpleCheck(nDos)) {
            return false;
        }
    }

    if (pmn == NULL) {
        LogPrint(BCLog::STAKENODE, "CStakenodePing::CheckAndUpdate -- Couldn't find Stakenode entry, stakenode=%s\n",
                 stakenodePubKey.GetID().ToString());
        return false;
    }

    if(!fFromNewBroadcast) {
        if (pmn->IsUpdateRequired()) {
            LogPrint(BCLog::STAKENODE, "CStakenodePing::CheckAndUpdate -- stakenode protocol is outdated, stakenode=%s\n",
                     stakenodePubKey.GetID().ToString());
            return false;
        }

        if (pmn->IsNewStartRequired()) {
            LogPrint(BCLog::STAKENODE, "CStakenodePing::CheckAndUpdate -- stakenode is completely expired, new start is required, stakenode=%s\n",
                     stakenodePubKey.GetID().ToString());
            return false;
        }
    }

    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if ((*mi).second && (*mi).second->nHeight < chainActive.Height() - 24) {
            LogPrintf("CStakenodePing::CheckAndUpdate -- Stakenode ping is invalid, block hash is too old: stakenode=%s  blockHash=%s\n",
                      stakenodePubKey.GetID().ToString(), blockHash.ToString());
            // nDos = 1;
            return false;
        }
    }

    LogPrint(BCLog::STAKENODE, "CStakenodePing::CheckAndUpdate -- New ping: stakenode=%s  blockHash=%s  sigTime=%d\n",
             stakenodePubKey.GetID().ToString(), blockHash.ToString(), sigTime);

    // LogPrintf("mnping - Found corresponding mn for vin: %s\n", HexStr(pubKeyStakenode.Raw()));
    // update only if there is no known ping for this stakenode or
    // last ping was more then STAKENODE_MIN_MNP_SECONDS-60 ago comparing to this one
    if (pmn->IsPingedWithin(STAKENODE_MIN_MNP_SECONDS - 60, sigTime)) {
        LogPrint(BCLog::STAKENODE, "CStakenodePing::CheckAndUpdate -- Stakenode ping arrived too early, stakenode=%s\n",
                 stakenodePubKey.GetID().ToString());
        //nDos = 1; //disable, this is happening frequently and causing banned peers
        return false;
    }

    if (!CheckSignature(pmn->pubKeyStakenode, nDos)) return false;

    // so, ping seems to be ok

    // if we are still syncing and there was no known ping for this mn for quite a while
    // (NOTE: assuming that STAKENODE_EXPIRATION_SECONDS/2 should be enough to finish mn list sync)
    if(!stakenodeSync.IsStakenodeListSynced() && !pmn->IsPingedWithin(STAKENODE_EXPIRATION_SECONDS/2)) {
        // let's bump sync timeout
        LogPrint(BCLog::STAKENODE, "CStakenodePing::CheckAndUpdate -- bumping sync timeout, stakenode=%s\n",
                 stakenodePubKey.GetID().ToString());
        stakenodeSync.BumpAssetLastTime("CStakenodePing::CheckAndUpdate");
    }

    // let's store this ping as the last one
    LogPrint(BCLog::STAKENODE, "CStakenodePing::CheckAndUpdate -- Stakenode ping accepted, stakenode=%s\n",
             stakenodePubKey.GetID().ToString());
    pmn->lastPing = *this;

    // and update stakenodeman.mapSeenStakenodeBroadcast.lastPing which is probably outdated
    CStakenodeBroadcast mnb(*pmn);
    uint256 hash = mnb.GetHash();
    if (stakenodeman.mapSeenStakenodeBroadcast.count(hash)) {
        stakenodeman.mapSeenStakenodeBroadcast[hash].second.lastPing = *this;
    }

    // force update, ignoring cache
    pmn->Check(true);
    // relay ping for nodes in ENABLED/EXPIRED/WATCHDOG_EXPIRED state only, skip everyone else
    if (!pmn->IsEnabled() && !pmn->IsExpired() && !pmn->IsWatchdogExpired()) return false;

    LogPrint(BCLog::STAKENODE, "CStakenodePing::CheckAndUpdate -- Stakenode ping acceepted and relayed, stakenode=%s\n",
             stakenodePubKey.GetID().ToString());
    Relay(connman);

    return true;
}

void CStakenodePing::Relay(CConnman& connman)
{
    // Do not relay until fully synced
    if(!stakenodeSync.IsSynced()) {
        LogPrint(BCLog::STAKENODE, "CStakenodePing::Relay -- won't relay until fully synced\n");
        return;
    }

    CInv inv(MSG_STAKENODE_PING, GetHash());
    connman.ForEachNode([&inv](CNode* pnode)
    {
        pnode->PushInventory(inv);
    });
}

void CStakenode::UpdateWatchdogVoteTime(uint64_t nVoteTime)
{
    LOCK(cs);
    nTimeLastWatchdogVote = (nVoteTime == 0) ? GetAdjustedTime() : nVoteTime;
}

void CStakenodeVerification::Relay() const
{
    CInv inv(MSG_STAKENODE_VERIFY, GetHash());
    g_connman->ForEachNode([&inv](CNode* pnode)
    {
        pnode->PushInventory(inv);
    });
}
