// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <dsnotificationinterface.h>
#include <instantx.h>
#include <governance/governance.h>
#include <masternodeman.h>
#include <masternode-payments.h>
#include <masternode-sync.h>
#include <tpos/stakenode-sync.h>
#include <tpos/stakenodeman.h>

void CDSNotificationInterface::InitializeCurrentBlockTip()
{
    LOCK(cs_main);
    UpdatedBlockTip(chainActive.Tip(), NULL, IsInitialBlockDownload());
}

void CDSNotificationInterface::AcceptedBlockHeader(const CBlockIndex *pindexNew)
{
    masternodeSync.AcceptedBlockHeader(pindexNew);
    stakenodeSync.AcceptedBlockHeader(pindexNew);
}

void CDSNotificationInterface::NotifyHeaderTip(const CBlockIndex *pindexNew, bool fInitialDownload)
{
    masternodeSync.NotifyHeaderTip(pindexNew, fInitialDownload, connman);
    stakenodeSync.NotifyHeaderTip(pindexNew, fInitialDownload, connman);
}

void CDSNotificationInterface::UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload)
{
    if (pindexNew == pindexFork) // blocks were disconnected without any new ones
        return;

    masternodeSync.UpdatedBlockTip(pindexNew, fInitialDownload, connman);
    stakenodeSync.UpdatedBlockTip(pindexNew, fInitialDownload, connman);

    if (fInitialDownload)
        return;

    mnodeman.UpdatedBlockTip(pindexNew);
    stakenodeman.UpdatedBlockTip(pindexNew);
//    CPrivateSend::UpdatedBlockTip(pindexNew);
//#ifdef ENABLE_WALLET
//    privateSendClient.UpdatedBlockTip(pindexNew);
//#endif // ENABLE_WALLET
    instantsend.UpdatedBlockTip(pindexNew);
    mnpayments.UpdatedBlockTip(pindexNew, connman);
    governance.UpdatedBlockTip(pindexNew, connman);
}

void CDSNotificationInterface::TransactionAddedToMempool(const CTransactionRef &ptxn)
{
    instantsend.SyncTransaction(ptxn, nullptr);
}

void CDSNotificationInterface::BlockConnected(const std::shared_ptr<const CBlock> &block, const CBlockIndex *pindex, const std::vector<CTransactionRef> &txnConflicted)
{
    for(const auto &tx : block->vtx)
    {
        instantsend.SyncTransaction(tx, pindex);
    }

    for(const auto &tx : txnConflicted)
    {
        instantsend.SyncTransaction(tx, nullptr);
    }
}

void CDSNotificationInterface::BlockDisconnected(const std::shared_ptr<const CBlock> &block)
{
    for(const auto &tx : block->vtx)
    {
        instantsend.SyncTransaction(tx, nullptr);
    }
}
