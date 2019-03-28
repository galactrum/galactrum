// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef STAKENODEMAN_H
#define STAKENODEMAN_H

#include <stakenode/stakenode.h>
#include <sync.h>

using namespace std;

class CStakenodeMan;
class CConnman;

extern CStakenodeMan stakenodeman;

class CStakenodeMan
{
private:
    static const std::string SERIALIZATION_VERSION_STRING;

    static const int DSEG_UPDATE_SECONDS        = 1 * 30 * 60;

    static const int LAST_PAID_SCAN_BLOCKS      = 100;

    static const int MIN_POSE_PROTO_VERSION     = 70203;
    static const int MAX_POSE_CONNECTIONS       = 10;
    static const int MAX_POSE_RANK              = 10;
    static const int MAX_POSE_BLOCKS            = 10;

    static const int MNB_RECOVERY_QUORUM_TOTAL      = 10;
    static const int MNB_RECOVERY_QUORUM_REQUIRED   = 6;
    static const int MNB_RECOVERY_MAX_ASK_ENTRIES   = 10;
    static const int MNB_RECOVERY_WAIT_SECONDS      = 60;
    static const int MNB_RECOVERY_RETRY_SECONDS     = 1 * 60 * 60;


    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    // Keep track of current block height
    int nCachedBlockHeight;

    // map to hold all MNs
    std::map<CPubKey, CStakenode> mapStakenodes;
    // who's asked for the Stakenode list and the last time
    std::map<CNetAddr, int64_t> mAskedUsForStakenodeList;
    // who we asked for the Stakenode list and the last time
    std::map<CNetAddr, int64_t> mWeAskedForStakenodeList;
    // which Stakenodes we've asked for
    std::map<CPubKey, std::map<CNetAddr, int64_t> > mWeAskedForStakenodeListEntry;
    // who we asked for the masternode verification
    std::map<CNetAddr, CStakenodeVerification> mWeAskedForVerification;

    // these maps are used for masternode recovery from MASTERNODE_NEW_START_REQUIRED state
    std::map<uint256, std::pair< int64_t, std::set<CNetAddr> > > mMnbRecoveryRequests;
    std::map<uint256, std::vector<CStakenodeBroadcast> > mMnbRecoveryGoodReplies;
    std::list< std::pair<CService, uint256> > listScheduledMnbRequestConnections;

    int64_t nLastWatchdogVoteTime;

    friend class CStakenodeSync;
    /// Find an entry
    CStakenode* Find(const CPubKey &pubKeyStakenode);
public:
    // Keep track of all broadcasts I've seen
    std::map<uint256, std::pair<int64_t, CStakenodeBroadcast> > mapSeenStakenodeBroadcast;
    // Keep track of all pings I've seen
    std::map<uint256, CStakenodePing> mapSeenStakenodePing;
    // Keep track of all verifications I've seen
    std::map<uint256, CStakenodeVerification> mapSeenStakenodeVerification;
    // keep track of dsq count to prevent masternodes from gaming darksend queue
    int64_t nDsqCount;


    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        LOCK(cs);
        std::string strVersion;
        if(ser_action.ForRead()) {
            READWRITE(strVersion);
        }
        else {
            strVersion = SERIALIZATION_VERSION_STRING;
            READWRITE(strVersion);
        }

        READWRITE(mapStakenodes);
        READWRITE(mAskedUsForStakenodeList);
        READWRITE(mWeAskedForStakenodeList);
        READWRITE(mWeAskedForStakenodeListEntry);
        READWRITE(mMnbRecoveryRequests);
        READWRITE(mMnbRecoveryGoodReplies);
        READWRITE(nLastWatchdogVoteTime);
        READWRITE(nDsqCount);

        READWRITE(mapSeenStakenodeBroadcast);
        READWRITE(mapSeenStakenodePing);
        if(ser_action.ForRead() && (strVersion != SERIALIZATION_VERSION_STRING)) {
            Clear();
        }
    }

    CStakenodeMan();

    /// Add an entry
    bool Add(CStakenode &mn);

    /// Ask (source) node for mnb
    void AskForMN(CNode *pnode, const CPubKey &pubKeyStakenode, CConnman& connman);
    void AskForMnb(CNode *pnode, const uint256 &hash);

    bool PoSeBan(const CPubKey &pubKeyStakenode);

    /// Check all Stakenodes
    void Check();

    /// Check all Stakenodes and remove inactive
    void CheckAndRemove(CConnman& connman);
    /// This is dummy overload to be used for dumping/loading mncache.dat
    void CheckAndRemove() {}

    /// Clear Stakenode vector
    void Clear();

    /// Count Stakenodes filtered by nProtocolVersion.
    /// Stakenode nProtocolVersion should match or be above the one specified in param here.
    int CountStakenodes(int nProtocolVersion = -1) const;
    /// Count enabled Stakenodes filtered by nProtocolVersion.
    /// Stakenode nProtocolVersion should match or be above the one specified in param here.
    int CountEnabled(int nProtocolVersion = -1) const;

    /// Count Stakenodes by network type - NET_IPV4, NET_IPV6, NET_TOR
    // int CountByIP(int nNetworkType);

    void DsegUpdate(CNode* pnode, CConnman& connman);

    /// Versions of Find that are safe to use from outside the class
    bool Get(const CKeyID &pubKeyID, CStakenode& masternodeRet);
    bool Get(const CPubKey &pubKeyStakenode, CStakenode& stakenodeRet);
    bool Has(const CPubKey &pubKeyStakenode);

    bool GetStakenodeInfo(const CPubKey& pubKeyStakenode, stakenode_info_t& mnInfoRet);
    bool GetStakenodeInfo(const CKeyID& pubKeyStakenode, stakenode_info_t& mnInfoRet);
    bool GetStakenodeInfo(const CScript& payee, stakenode_info_t& mnInfoRet);

    std::map<CPubKey, CStakenode> GetFullStakenodeMap() { return mapStakenodes; }

    void ProcessStakenodeConnections(CConnman& connman);
    std::pair<CService, std::set<uint256> > PopScheduledMnbRequestConnection();

    void ProcessMessage(CNode* pfrom, const string &strCommand, CDataStream& vRecv, CConnman& connman);

    void DoFullVerificationStep(CConnman& connman);
    void CheckSameAddr();
    bool SendVerifyRequest(const CAddress& addr, const std::vector<CStakenode*>& vSortedByAddr, CConnman& connman);
    void SendVerifyReply(CNode* pnode, CStakenodeVerification& mnv, CConnman& connman);
    void ProcessVerifyReply(CNode* pnode, CStakenodeVerification& mnv);
    void ProcessVerifyBroadcast(CNode* pnode, const CStakenodeVerification& mnv);

    /// Return the number of (unique) Stakenodes
    int size() { return mapStakenodes.size(); }

    std::string ToString() const;

    /// Update masternode list and maps using provided CStakenodeBroadcast
    void UpdateStakenodeList(CStakenodeBroadcast mnb, CConnman& connman);
    /// Perform complete check and only then update list and maps
    bool CheckMnbAndUpdateStakenodeList(CNode* pfrom, CStakenodeBroadcast mnb, int& nDos, CConnman& connman);
    bool IsMnbRecoveryRequested(const uint256& hash) { return mMnbRecoveryRequests.count(hash); }

    bool IsWatchdogActive();
    void UpdateWatchdogVoteTime(const CPubKey &pubKeyStakenode, uint64_t nVoteTime = 0);

    void CheckStakenode(const CPubKey& pubKeyStakenode, bool fForce);

    bool IsStakenodePingedWithin(const CPubKey &pubKeyStakenode, int nSeconds, int64_t nTimeToCheckAt = -1);
    void SetStakenodeLastPing(const CPubKey &pubKeyStakenode, const CStakenodePing& mnp);

    void UpdatedBlockTip(const CBlockIndex *pindex);
};

void ThreadStakenodeCheck(CConnman& connman);

#endif
