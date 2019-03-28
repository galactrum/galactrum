#include <tpos/tposutils.h>

#include <wallet/wallet.h>
#include <utilmoneystr.h>
#include <policy/policy.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <tpos/stakenode-sync.h>
#include <tpos/stakenodeman.h>
#include <tpos/activestakenode.h>
#include <consensus/validation.h>
#include <messagesigner.h>
#include <spork.h>
#include <sstream>
#include <numeric>

static const std::string TPOSEXPORTHEADER("STAKEOWNERINFO");
static const int TPOSEXPORTHEADERWIDTH = 40;

static const int TPOS_CONTRACT_COLATERAL = 1 * COIN;

std::string ParseAddressFromMetadata(std::string str)
{
    auto tposAddressRaw = ParseHex(str);
    std::string addressAsStr(tposAddressRaw.size(), '0');

    for(size_t i = 0; i < tposAddressRaw.size(); ++i)
        addressAsStr[i] = static_cast<char>(tposAddressRaw[i]);

    return addressAsStr;
}

bool TPoSUtils::IsTPoSContract(const CTransactionRef &tx)
{
    return TPoSContract::FromTPoSContractTx(tx).IsValid();
}

#ifdef ENABLE_WALLET

bool TPoSUtils::GetTPoSPayments(const CWallet *wallet,
                                const CTransactionRef &tx,
                                CAmount &stakeAmount,
                                CAmount &commissionAmount,
                                CTxDestination &tposAddress,
                                CTxDestination &stakenodeAddress)
{
    if(!tx->IsCoinStake())
        return false;

    CAmount nCredit = wallet->GetCredit(*tx, ISMINE_ALL);
    CAmount nDebit = wallet->GetDebit(*tx, ISMINE_ALL);
    CAmount nNet = nCredit - nDebit;

    std::vector<TPoSContract> tposContracts;

    for(auto &&pair : wallet->tposOwnerContracts)
        tposContracts.emplace_back(pair.second);

    for(auto &&pair : wallet->tposStakenodeContracts)
        tposContracts.emplace_back(pair.second);

    CTxDestination address;
    auto scriptKernel = tx->vout.at(1).scriptPubKey;
    commissionAmount = stakeAmount = 0;
    if(ExtractDestination(scriptKernel, address))
    {
        CBitcoinAddress tmpAddress(address);

        auto it = std::find_if(std::begin(tposContracts), std::end(tposContracts), [tmpAddress](const TPoSContract &entry) {
            return entry.tposAddress == tmpAddress;
        });

        if(it != std::end(tposContracts))
        {
            auto stakenodeScript = GetScriptForDestination(it->stakenodeAddress.Get());
            auto commissionIt = std::find_if(std::begin(tx->vout), std::end(tx->vout), [stakenodeScript](const CTxOut &txOut) {
                return txOut.scriptPubKey == stakenodeScript;
            });

            if(commissionIt != tx->vout.end())
            {
                stakeAmount = nNet;
                commissionAmount = commissionIt->nValue;
                tposAddress = tmpAddress.Get();
                stakenodeAddress = it->stakenodeAddress.Get();

                return true;
            }
        }
    }

    return false;

}

bool TPoSUtils::IsTPoSStakenodeContract(CWallet *wallet, const CTransactionRef &tx)
{
    TPoSContract contract = TPoSContract::FromTPoSContractTx(tx);

    bool isStakeNode = GetScriptForDestination(contract.stakenodeAddress.Get()) ==
            GetScriptForDestination(activeStakenode.pubKeyStakenode.GetID());

    return contract.IsValid() && (isStakeNode ||
                                  IsMine(*wallet, contract.stakenodeAddress.Get()) == ISMINE_SPENDABLE);
}

bool TPoSUtils::IsTPoSOwnerContract(CWallet *wallet, const CTransactionRef &tx)
{
    TPoSContract contract = TPoSContract::FromTPoSContractTx(tx);

    return contract.IsValid() &&
            IsMine(*wallet, contract.tposAddress.Get()) == ISMINE_SPENDABLE;
}

bool TPoSUtils::CreateTPoSTransaction(CWallet *wallet,
                                      CTransactionRef &transactionOut,
                                      CReserveKey& reservekey,
                                      const CBitcoinAddress &tposAddress,
                                      const CBitcoinAddress &stakenodeAddress,
                                      int stakenodeCommission,
                                      std::string &strError)
{
    auto tposAddressAsStr = tposAddress.ToString();
    auto stakenodeAddressAsStr = stakenodeAddress.ToString();

    CScript metadataScriptPubKey;
    metadataScriptPubKey << OP_RETURN
                         << std::vector<unsigned char>(tposAddressAsStr.begin(), tposAddressAsStr.end())
                         << std::vector<unsigned char>(stakenodeAddressAsStr.begin(), stakenodeAddressAsStr.end())
                         << (100 - stakenodeCommission);


    if(wallet->IsLocked())
    {
        strError = "Error: Wallet is locked";
        return false;
    }

    CKey key;
    CKeyID keyID;
    if(!tposAddress.GetKeyID(keyID))
    {
        strError = "Error: stake contract address is not P2PKH";
        return false;
    }
    if (!wallet->GetKey(keyID, key))
    {
        strError = "Error: Failed to get private key associated with stake contract address";
        return false;
    }
    std::vector<unsigned char> vchSignature;
    key.SignCompact(SerializeHash(COutPoint()), vchSignature);
    metadataScriptPubKey << vchSignature;

    std::vector<CRecipient> vecSend {
        { metadataScriptPubKey, 0, false },
        { GetScriptForDestination(tposAddress.Get()), TPOS_CONTRACT_COLATERAL, false }
    };


    CAmount nFeeRequired;

    // this delegate will be executed right before signing. This will allow us to tweak transaction and do
    // some tpos specific thing, like signing contract.
    auto txModifier = [&strError, &key](CMutableTransaction &tx, std::vector<unsigned char> vchSignature) {
        auto firstInput = tx.vin.front().prevout;

        auto it = std::find_if(tx.vout.begin(), tx.vout.end(), [](const CTxOut &txOut) {
            return txOut.scriptPubKey.IsUnspendable();
        });

        auto vchSignatureCopy = vchSignature;
        vchSignature.clear();
        auto hashMessage = SerializeHash(firstInput);
        if(!key.SignCompact(hashMessage, vchSignature))
        {
            strError = "Error: Failed to sign stake contract";
        }
        it->scriptPubKey.FindAndDelete(CScript(vchSignatureCopy));
        it->scriptPubKey << vchSignature;
    };

    auto txModifierBinded = std::bind(txModifier, std::placeholders::_1, vchSignature);

    int nChangePos = -1;
    if (!wallet->CreateTransaction(vecSend, transactionOut, reservekey, nFeeRequired, nChangePos, strError, {}, true, txModifierBinded))
    {
        if (TPOS_CONTRACT_COLATERAL + nFeeRequired > wallet->GetBalance())
            strError = strprintf("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds!", FormatMoney(nFeeRequired));
        LogPrintf("Error() : %s\n", strError);
        return false;
    }

    if(!strError.empty())
        return false;

    std::string reason;
    if(!IsStandardTx(*transactionOut, reason))
    {
        strError = strprintf("Error: Not standard tx: %s\n", reason.c_str());
        LogPrintf(strError.c_str());
        return false;
    }

    return true;
}

bool TPoSUtils::CreateCancelContractTransaction(CWallet *wallet, CTransactionRef &txOut, CReserveKey &reserveKey, const TPoSContract &contract, string &strError)
{
    if(wallet->IsLocked())
    {
        strError = "Error: Wallet is locked";
        return false;
    }

    COutPoint prevOutpoint = GetContractCollateralOutpoint(contract);
    if(prevOutpoint.IsNull())
    {
        strError = "Error: Contract collateral is invalid";
        return false;
    }

    Coin coin;
    if(!pcoinsTip->GetCoin(prevOutpoint, coin) || coin.IsSpent())
    {
        strError = "Error: Collateral is already spent";
        return false;
    }

    auto &prevOutput = contract.rawTx->vout.at(prevOutpoint.n);

    CAmount nFeeRet;
    int nChangePosRet;
    CCoinControl coinControl;
    //    coinControl.fUsePrivateSend = false;
    coinControl.nCoinType = ONLY_STAKENODE_COLLATERAL;
    coinControl.Select(prevOutpoint);
    if(!wallet->CreateTransaction({ { prevOutput.scriptPubKey, prevOutput.nValue, true } }, txOut,
                                  reserveKey, nFeeRet, nChangePosRet,
                                  strError, coinControl, true))
    {
        LogPrintf("Error() : %s\n", strError.c_str());
        return false;
    }

    return true;
}

COutPoint TPoSUtils::GetContractCollateralOutpoint(const TPoSContract &contract)
{
    COutPoint result;
    if(!contract.rawTx)
    {
        return result;
    }


    const auto &vout = contract.rawTx->vout;
    for(size_t i = 0; i < vout.size(); ++i)
    {
        if(vout[i].scriptPubKey == GetScriptForDestination(contract.tposAddress.Get()) &&
                vout[i].nValue == TPOS_CONTRACT_COLATERAL)
        {
            result = COutPoint(contract.rawTx->GetHash(), i);
            break;
        }
    }

    return result;
}

bool TPoSUtils::CheckContract(const uint256 &hashContractTx, TPoSContract &contract, bool fCheckSignature, bool fCheckContractOutpoint, std::string &strError)
{
    CTransactionRef tx;
    uint256 hashBlock;
    if(!GetTransaction(hashContractTx, tx, Params().GetConsensus(), hashBlock, true))
    {
        strError = strprintf("%s : failed to get transaction for stake contract %s", __func__,
                             hashContractTx.ToString());

        return error(strError.c_str());
    }

    return CheckContract(tx, contract, fCheckSignature, fCheckContractOutpoint, strError);
}

bool TPoSUtils::CheckContract(const CTransactionRef &txContract, TPoSContract &contract, bool fCheckSignature, bool fCheckContractOutpoint, std::string &strError)
{
    TPoSContract tmpContract = TPoSContract::FromTPoSContractTx(txContract);

    if(!tmpContract.IsValid())
    {
        strError = "CheckContract() : invalid transaction for stake contract";
        return error(strError.c_str());
    }

    if(fCheckSignature)
    {
        auto hashMessage = SerializeHash(tmpContract.rawTx->vin.front().prevout);
        std::string verifyHashError;
        if(!CHashSigner::VerifyHash(hashMessage, tmpContract.tposAddress.Get(), tmpContract.vchSignature, verifyHashError))
        {
            strError = strprintf("%s : stake contract signature is invalid %s", __func__, verifyHashError);
            return error(strError.c_str());
        }
    }

    if(fCheckContractOutpoint)
    {
        auto tposContractOutpoint = TPoSUtils::GetContractCollateralOutpoint(tmpContract);
        Coin coin;
        if(!pcoinsTip->GetCoin(tposContractOutpoint, coin) || coin.IsSpent())
        {
            strError = "CheckContract() : stake contract invalid, collateral is spent";
            return error(strError.c_str());
        }
    }

    contract = tmpContract;

    return true;
}

bool TPoSUtils::IsStakenodePaymentValid(CValidationState &state, const CBlock &block, int nBlockHeight, CAmount expectedReward, CAmount actualReward)
{
    auto contract = TPoSContract::FromTPoSContractTx(block.txTPoSContract);
    CBitcoinAddress stakenodeAddress = contract.stakenodeAddress;
    CScript scriptStakenodePubKey = GetScriptForDestination(stakenodeAddress.Get());

    const auto &coinstake = block.vtx[1];

    if(coinstake->vout[1].scriptPubKey != GetScriptForDestination(contract.tposAddress.Get()))
    {
        CTxDestination dest;
        if(!ExtractDestination(coinstake->vout[1].scriptPubKey, dest))
            return state.DoS(100, error("IsStakenodePaymentValid -- ERROR: coinstake extract destination failed"), REJECT_INVALID, "bad-stakenode-payee");

        // ban him, something is incorrect completely
        return state.DoS(100, error("IsStakenodePaymentValid -- ERROR: coinstake is invalid expected: %s, actual %s\n",
                                    contract.tposAddress.ToString().c_str(), CBitcoinAddress(dest).ToString().c_str()), REJECT_INVALID, "bad-stakenode-payee");
    }

    CAmount stakenodePayment = 0;
    stakenodePayment = std::accumulate(std::begin(coinstake->vout) + 2, std::end(coinstake->vout), CAmount(0), [scriptStakenodePubKey](CAmount accum, const CTxOut &txOut) {
            return txOut.scriptPubKey == scriptStakenodePubKey ? accum + txOut.nValue : accum;
});

    if(stakenodePayment > 0)
    {
        auto maxAllowedValue = (expectedReward / 100) * (100 - contract.stakePercentage);
        // ban, we know fur sure that stakenode tries to get more than he is allowed
        if(stakenodePayment > maxAllowedValue)
            return state.DoS(100, error("IsStakenodePaymentValid -- ERROR: stakenode was paid more than allowed: %s\n", contract.stakenodeAddress.ToString().c_str()),
                             REJECT_INVALID, "bad-stakenode-payee");
    }
    else
    {
        LogPrintf("IsStakenodePaymentValid -- WARNING: stakenode wasn't paid, this is weird, but totally acceptable. Shouldn't happen.\n");
    }

    if(!stakenodeSync.IsSynced())
    {
        //there is no stakenode node info to check anything, let's just accept the longest chain
        //        if(fDebug)
        LogPrintf("IsStakenodePaymentValid -- WARNING: Client not synced, skipping block payee checks\n");

        return true;
    }

    if(!sporkManager.IsSporkActive(Spork::SPORK_15_STAKENODES_ENABLED))
    {
        return state.DoS(0, error("IsBlockPayeeValid -- ERROR: Invalid stakenode payment detected at height %d\n", nBlockHeight),
                         REJECT_INVALID, "bad-stakenode-payee", true);
    }

    CKeyID coinstakeKeyID;
    if(!stakenodeAddress.GetKeyID(coinstakeKeyID))
        return state.DoS(0, error("IsStakenodePaymentValid -- ERROR: coin stake was paid to invalid address\n"),
                         REJECT_INVALID, "bad-stakenode-payee", true);

    CStakenode StakeNode;
    if(!stakenodeman.Get(coinstakeKeyID, StakeNode))
    {
        return state.DoS(0, error("IsStakenodePaymentValid -- ERROR: failed to find stakenode with address: %s\n", stakenodeAddress.ToString().c_str()),
                         REJECT_INVALID, "bad-stakenode-payee", true);
    }

    if(StakeNode.hashTPoSContractTx != block.hashTPoSContractTx)
    {
        return state.DoS(100, error("IsStakenodePaymentValid -- ERROR: stakenode contract is invalid expected: %s, actual %s\n",
                                    block.hashTPoSContractTx.ToString().c_str(), StakeNode.hashTPoSContractTx.ToString().c_str()),
                         REJECT_INVALID, "bad-stakenode-payee");
    }

    if(!StakeNode.IsValidForPayment())
    {
        return state.DoS(0, error("IsStakenodePaymentValid -- ERROR: stakenode with address: %s is not valid for payment\n", stakenodeAddress.ToString().c_str()),
                         REJECT_INVALID, "bad-stakenode-payee", true);
    }

    return true;
}

#endif

TPoSContract::TPoSContract(CTransactionRef tx, CBitcoinAddress stakenodeAddress, CBitcoinAddress tposAddress, short stakePercentage, std::vector<unsigned char> vchSignature)
{
    this->rawTx = tx;
    this->stakenodeAddress = stakenodeAddress;
    this->tposAddress = tposAddress;
    this->vchSignature = vchSignature;
    this->stakePercentage = stakePercentage;
}

bool TPoSContract::IsValid() const
{
    return rawTx && !rawTx->IsNull() && stakenodeAddress.IsValid() && tposAddress.IsValid() &&
            stakePercentage > 0 && stakePercentage < 100;
}

TPoSContract TPoSContract::FromTPoSContractTx(const CTransactionRef tx)
{
    try
    {
        if(tx->vout.size() >= 2 && tx->vout.size() <= 3 )
        {
            const CTxOut *metadataOutPtr = nullptr;
            bool colateralFound = false;
            for(const CTxOut &txOut : tx->vout)
            {
                if(txOut.scriptPubKey.IsUnspendable())
                {
                    metadataOutPtr = &txOut;
                }
                else if(txOut.nValue == TPOS_CONTRACT_COLATERAL)
                {
                    colateralFound = true;
                }
            }

            if(metadataOutPtr && colateralFound)
            {
                const auto &metadataOut = *metadataOutPtr;
                std::vector<std::vector<unsigned char>> vSolutions;
                txnouttype whichType;
                if (Solver(metadataOut.scriptPubKey, whichType, vSolutions) && whichType == TX_NULL_DATA)
                {
                    // Here we can have a chance that it is transaction which is a tpos contract, let's check if it has
                    std::stringstream stringStream(metadataOut.scriptPubKey.ToString());

                    std::string tokens[5];
                    for(auto &token : tokens)
                    {
                        stringStream >> token;
                    }

                    CBitcoinAddress tposAddress(ParseAddressFromMetadata(tokens[1]));
                    CBitcoinAddress stakenodeAddress(ParseAddressFromMetadata(tokens[2]));
                    int commission = std::stoi(tokens[3]);
                    std::vector<unsigned char> vchSignature = ParseHex(tokens[4]);
                    if(tokens[0] == GetOpName(OP_RETURN) && tposAddress.IsValid() && stakenodeAddress.IsValid() &&
                            commission > 0 && commission < 100)
                    {

                        // if we get to this point, it means that we have found tpos contract that was created for us to act as stakenode.
                        return TPoSContract(tx, stakenodeAddress, tposAddress, commission, vchSignature);
                    }
                }
            }
        }
    }
    catch(std::exception &ex)
    {
        LogPrintf("Failed to parse stake contract, %s\n", ex.what());
    }

    return TPoSContract();
}
