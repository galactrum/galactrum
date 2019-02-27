// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ACTIVESTAKENODE_H
#define ACTIVESTAKENODE_H

#include <chainparams.h>
#include <key.h>
#include <net.h>
#include <primitives/transaction.h>

class CActiveStakenode;

static const int ACTIVE_STAKENODE_INITIAL          = 0; // initial state
static const int ACTIVE_STAKENODE_SYNC_IN_PROCESS  = 1;
static const int ACTIVE_STAKENODE_INPUT_TOO_NEW    = 2;
static const int ACTIVE_STAKENODE_NOT_CAPABLE      = 3;
static const int ACTIVE_STAKENODE_STARTED          = 4;

extern CActiveStakenode activeStakenode;

// Responsible for activating the Stakenode and pinging the network
class CActiveStakenode
{
public:
    enum masternode_type_enum_t {
        STAKENODE_UNKNOWN = 0,
        STAKENODE_REMOTE  = 1
    };

private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    masternode_type_enum_t eType;

    bool fPingerEnabled;

    /// Ping Stakenode
    bool SendStakenodePing(CConnman &connman);

    //  sentinel ping data
    int64_t nSentinelPingTime;
    uint32_t nSentinelVersion;

public:
    // Keys for the active Stakenode
    CPubKey pubKeyStakenode;
    CKey keyStakenode;

    // Initialized while registering Stakenode
    CService service;

    int nState; // should be one of ACTIVE_STAKENODE_XXXX
    std::string strNotCapableReason;


    CActiveStakenode()
        : eType(STAKENODE_UNKNOWN),
          fPingerEnabled(false),
          pubKeyStakenode(),
          keyStakenode(),
          service(),
          nState(ACTIVE_STAKENODE_INITIAL)
    {}

    /// Manage state of active Stakenode
    void ManageState(CConnman &connman);

    std::string GetStateString() const;
    std::string GetStatus() const;
    std::string GetTypeString() const;

    bool UpdateSentinelPing(int version);

private:
    void ManageStateInitial(CConnman& connman);
    void ManageStateRemote();
};

#endif
