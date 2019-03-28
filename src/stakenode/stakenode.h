// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef STAKENODE_H
#define STAKENODE_H

#include <key.h>
#include <validation.h>
//#include <spork.h>

class CStakenode;
class CStakenodeBroadcast;
class CConnman;

static const int STAKENODE_CHECK_SECONDS               = 20;
static const int STAKENODE_MIN_MNB_SECONDS             = 5 * 60;
static const int STAKENODE_MIN_MNP_SECONDS             = 10 * 60;
static const int STAKENODE_EXPIRATION_SECONDS          = 65 * 60;
static const int STAKENODE_WATCHDOG_MAX_SECONDS        = 120 * 60;
static const int STAKENODE_NEW_START_REQUIRED_SECONDS  = 180 * 60;
static const int STAKENODE_POSE_BAN_MAX_SCORE          = 5;

//
// The Stakenode Ping Class : Contains a different serialize method for sending pings from stakenodes throughout the network
//

// sentinel version before sentinel ping implementation
#define DEFAULT_SENTINEL_VERSION 0x010001

class CStakenodePing
{
public:
    CPubKey stakenodePubKey{};
    uint256 blockHash{};
    int64_t sigTime{}; //mnb message times
    std::vector<unsigned char> vchSig{};
    bool fSentinelIsCurrent = false; // true if last sentinel ping was actual
    // MSB is always 0, other 3 bits corresponds to x.x.x version scheme
    uint32_t nSentinelVersion{DEFAULT_SENTINEL_VERSION};

    CStakenodePing() = default;

    CStakenodePing(const CPubKey& stakenodePubKey);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(stakenodePubKey);
        READWRITE(blockHash);
        READWRITE(sigTime);
        READWRITE(vchSig);
        if(ser_action.ForRead() && (s.size() == 0))
        {
            fSentinelIsCurrent = false;
            nSentinelVersion = DEFAULT_SENTINEL_VERSION;
            return;
        }
        READWRITE(fSentinelIsCurrent);
        READWRITE(nSentinelVersion);
    }

    uint256 GetHash() const
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << stakenodePubKey;
        ss << sigTime;
        return ss.GetHash();
    }

    bool IsExpired() const { return GetAdjustedTime() - sigTime > STAKENODE_NEW_START_REQUIRED_SECONDS; }
    bool Sign(const CKey& keyStakenode, const CPubKey& pubKeyStakenode);
    bool CheckSignature(CPubKey& pubKeyStakenode, int &nDos);
    bool SimpleCheck(int& nDos);
    bool CheckAndUpdate(CStakenode* pmn, bool fFromNewBroadcast, int& nDos, CConnman& connman);
    void Relay(CConnman& connman);
};

inline bool operator==(const CStakenodePing& a, const CStakenodePing& b)
{
    return a.stakenodePubKey == b.stakenodePubKey && a.blockHash == b.blockHash;
}
inline bool operator!=(const CStakenodePing& a, const CStakenodePing& b)
{
    return !(a == b);
}

struct stakenode_info_t
{
    // Note: all these constructors can be removed once C++14 is enabled.
    // (in C++11 the member initializers wrongly disqualify this as an aggregate)
    stakenode_info_t() = default;
    stakenode_info_t(stakenode_info_t const&) = default;

    stakenode_info_t(int activeState, int protoVer, int64_t sTime) :
        nActiveState{activeState}, nProtocolVersion{protoVer}, sigTime{sTime} {}

    stakenode_info_t(int activeState, int protoVer, int64_t sTime,
                        CService const& addr, CPubKey const& pkMN, uint256 const &hashTPoSContractTxNew,
                      int64_t tWatchdogV = 0) :
        nActiveState{activeState}, nProtocolVersion{protoVer}, sigTime{sTime},
        addr{addr}, pubKeyStakenode{pkMN}, hashTPoSContractTx{hashTPoSContractTxNew},
        nTimeLastWatchdogVote{tWatchdogV} {}

    int nActiveState = 0;
    int nProtocolVersion = 0;
    int64_t sigTime = 0; //mnb message time

    CService addr{};
    CPubKey pubKeyStakenode{};
    uint256 hashTPoSContractTx{};
    int64_t nTimeLastWatchdogVote = 0;

    int64_t nLastDsq = 0; //the dsq count from the last dsq broadcast of this node
    int64_t nTimeLastChecked = 0;
    int64_t nTimeLastPing = 0; //* not in CMN
    bool fInfoValid = false; //* not in CMN
};

//
// The Stakenode Class. For managing the Darksend process. It contains the input of the 1000DRK, signature to prove
// it's the one who own that ip address and code for calculating the payment election.
//
class CStakenode : public stakenode_info_t
{
private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

public:
    enum state {
        STAKENODE_PRE_ENABLED,
        STAKENODE_ENABLED,
        STAKENODE_EXPIRED,
        STAKENODE_UPDATE_REQUIRED,
        STAKENODE_WATCHDOG_EXPIRED,
        STAKENODE_NEW_START_REQUIRED,
        STAKENODE_POSE_BAN
    };

    CStakenodePing lastPing{};
    std::vector<unsigned char> vchSig{};

    int nPoSeBanScore{};
    int nPoSeBanHeight{};
    bool fUnitTest = false;

    CStakenode();
    CStakenode(const CStakenode& other);
    CStakenode(const CStakenodeBroadcast& mnb);
    CStakenode(CService addrNew, CPubKey pubKeyStakenodeNew, uint256 hashTPoSContractTxNew, int nProtocolVersionIn);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        LOCK(cs);
        READWRITE(addr);
        READWRITE(pubKeyStakenode);
        READWRITE(hashTPoSContractTx);
        READWRITE(lastPing);
        READWRITE(vchSig);
        READWRITE(sigTime);
        READWRITE(nLastDsq);
        READWRITE(nTimeLastChecked);
        READWRITE(nTimeLastWatchdogVote);
        READWRITE(nActiveState);
        READWRITE(nProtocolVersion);
        READWRITE(nPoSeBanScore);
        READWRITE(nPoSeBanHeight);
        READWRITE(fUnitTest);
    }

    bool UpdateFromNewBroadcast(CStakenodeBroadcast& mnb, CConnman& connman);

    void Check(bool fForce = false);

    bool IsBroadcastedWithin(int nSeconds) const { return GetAdjustedTime() - sigTime < nSeconds; }

    bool IsPingedWithin(int nSeconds, int64_t nTimeToCheckAt = -1) const
    {
        if(lastPing == CStakenodePing()) return false;

        if(nTimeToCheckAt == -1) {
            nTimeToCheckAt = GetAdjustedTime();
        }
        return nTimeToCheckAt - lastPing.sigTime < nSeconds;
    }

    bool IsEnabled() const { return nActiveState == STAKENODE_ENABLED; }
    bool IsPreEnabled() const { return nActiveState == STAKENODE_PRE_ENABLED; }
    bool IsPoSeBanned() const { return nActiveState == STAKENODE_POSE_BAN; }
    // NOTE: this one relies on nPoSeBanScore, not on nActiveState as everything else here
    bool IsPoSeVerified() const { return nPoSeBanScore <= -STAKENODE_POSE_BAN_MAX_SCORE; }
    bool IsExpired() const { return nActiveState == STAKENODE_EXPIRED; }
    bool IsUpdateRequired() const { return nActiveState == STAKENODE_UPDATE_REQUIRED; }
    bool IsWatchdogExpired() const { return nActiveState == STAKENODE_WATCHDOG_EXPIRED; }
    bool IsNewStartRequired() const { return nActiveState == STAKENODE_NEW_START_REQUIRED; }

    static bool IsValidStateForAutoStart(int nActiveStateIn)
    {
        return  nActiveStateIn == STAKENODE_ENABLED ||
                nActiveStateIn == STAKENODE_PRE_ENABLED ||
                nActiveStateIn == STAKENODE_EXPIRED ||
                nActiveStateIn == STAKENODE_WATCHDOG_EXPIRED;
    }

    bool IsValidForPayment() const
    {

        if(nActiveState == STAKENODE_ENABLED && !IsPoSeBanned()) {
            return true;
        }



//        if(!sporkManager.IsSporkActive(SPORK_14_REQUIRE_SENTINEL_FLAG) &&
//           (nActiveState == STAKENODE_WATCHDOG_EXPIRED)) {
//            return true;
//        }

        return false;
    }

    bool IsValidNetAddr() const;
    static bool IsValidNetAddr(CService addrIn);

    void IncreasePoSeBanScore() { if(nPoSeBanScore < STAKENODE_POSE_BAN_MAX_SCORE) nPoSeBanScore++; }
    void DecreasePoSeBanScore() { if(nPoSeBanScore > -STAKENODE_POSE_BAN_MAX_SCORE) nPoSeBanScore--; }
    void PoSeBan() { nPoSeBanScore = STAKENODE_POSE_BAN_MAX_SCORE; }

    stakenode_info_t GetInfo() const;

    static std::string StateToString(int nStateIn);
    std::string GetStateString() const;
    std::string GetStatus() const;

    void UpdateWatchdogVoteTime(uint64_t nVoteTime = 0);

    CStakenode& operator=(CStakenode const& from)
    {
        static_cast<stakenode_info_t&>(*this)=from;
        lastPing = from.lastPing;
        vchSig = from.vchSig;
        nPoSeBanScore = from.nPoSeBanScore;
        nPoSeBanHeight = from.nPoSeBanHeight;
        fUnitTest = from.fUnitTest;
        return *this;
    }
};

inline bool operator==(const CStakenode& a, const CStakenode& b)
{
    return a.addr == b.addr && a.pubKeyStakenode == b.pubKeyStakenode;
}
inline bool operator!=(const CStakenode& a, const CStakenode& b)
{
    return !(a == b);
}


//
// The Stakenode Broadcast Class : Contains a different serialize method for sending stakenodes through the network
//

class CStakenodeBroadcast : public CStakenode
{
public:

    bool fRecovery;

    CStakenodeBroadcast() : CStakenode(), fRecovery(false) {}
    CStakenodeBroadcast(const CStakenode& mn) : CStakenode(mn), fRecovery(false) {}
    CStakenodeBroadcast(CService addrNew, CPubKey pubKeyStakenodeNew, uint256 hashTPoSContractTxNew, int nProtocolVersionIn) :
        CStakenode(addrNew, pubKeyStakenodeNew, hashTPoSContractTxNew, nProtocolVersionIn), fRecovery(false) {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(addr);
        READWRITE(pubKeyStakenode);
        READWRITE(hashTPoSContractTx);
        READWRITE(vchSig);
        READWRITE(sigTime);
        READWRITE(nProtocolVersion);
        READWRITE(lastPing);
    }

    uint256 GetHash() const
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << pubKeyStakenode;
        ss << hashTPoSContractTx;

        ss << sigTime;
        return ss.GetHash();
    }

    /// Create Stakenode broadcast, needs to be relayed manually after that
    static bool Create(const CService& service, const CKey& keyStakenodeNew,
                       const CPubKey& pubKeyStakenodeNew, const uint256 &hashTPoSContractTx,
                       std::string &strErrorRet, CStakenodeBroadcast &mnbRet);

    static bool Create(std::string strService, std::string strStakenodeAddress,
                       std::string strHashTPoSContractTx, std::string& strErrorRet,
                       CStakenodeBroadcast &mnbRet, bool fOffline = false);

    bool SimpleCheck(int& nDos);
    bool Update(CStakenode* pmn, int& nDos, CConnman& connman);
    bool CheckStakenode(int &nDos);

    bool Sign(const CKey& keyCollateralAddress);
    bool CheckSignature(int& nDos);
    void Relay(CConnman& connman);
};

class CStakenodeVerification
{
public:
    CPubKey pubKeyStakenode1{};
    CPubKey pubKeyStakenode2{};
    CService addr{};
    int nonce{};
    int nBlockHeight{};
    std::vector<unsigned char> vchSig1{};
    std::vector<unsigned char> vchSig2{};

    CStakenodeVerification() = default;

    CStakenodeVerification(CService addr, int nonce, int nBlockHeight) :
        addr(addr),
        nonce(nonce),
        nBlockHeight(nBlockHeight)
    {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(pubKeyStakenode1);
        READWRITE(pubKeyStakenode2);
        READWRITE(addr);
        READWRITE(nonce);
        READWRITE(nBlockHeight);
        READWRITE(vchSig1);
        READWRITE(vchSig2);
    }

    uint256 GetHash() const
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << pubKeyStakenode1;
        ss << pubKeyStakenode2;
        ss << addr;
        ss << nonce;
        ss << nBlockHeight;
        return ss.GetHash();
    }

    void Relay() const;
};

#endif
