// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The WaykiChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#include "coinutxotx.h"
#include "main.h"
#include <string>
#include <cstdarg>

bool GetUtxoTxFromChain(TxID &txid, std::shared_ptr<CCoinUtxoTx> pTx) {
    if (!SysCfg().IsTxIndex()) 
        return false;
    
    CDiskTxPos txPos;
    if (pCdMan->pBlockCache->ReadTxIndex(txid, txPos)) {
        LOCK(cs_main);
        CAutoFile file(OpenBlockFile(txPos, true), SER_DISK, CLIENT_VERSION);
        CBlockHeader header;

        try {
            file >> header;
            fseek(file, txPos.nTxOffset, SEEK_CUR);
            file >> pTx;
            
        } catch (std::exception &e) {
            throw runtime_error(tfm::format("%s : Deserialize or I/O error - %s", __func__, e.what()).c_str());
        }
    }
    return true;
}

inline bool CheckUtxoCondition( const bool isCheckInput, const CTxExecuteContext &context, 
                                const CUserID &prevUtxoTxUid, const CUserID &txUid,
                                const CUtxoInput &input, CUtxoCond &cond) {
    CValidationState &state = *context.pState;

    switch (cond.cond_type) {
        case UtxoCondType::P2SA : {
            CSingleAddressCondOut& theCond = dynamic_cast< CSingleAddressCondOut& > (cond);

            if (isCheckInput) {
                if (theCond.uid != txUid) {
                    return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, uid mismatches error!"), REJECT_INVALID, 
                                    "uid-mismatches-err");
                }
            } else {
                if (theCond.uid.IsEmpty()) {
                    return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, uid empty error!"), REJECT_INVALID, 
                                    "uid-empty-err");
                }
            }
            break;
        }
        case UtxoCondType::P2MA : {
            CMultiSignAddressCondOut& theCond = dynamic_cast< CMultiSignAddressCondOut& > (cond);

            if (isCheckInput) {
                //TODO must verify signatures one by one below!!!



            } else {
                if (theCond.uid.IsEmpty()) {
                    return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, uid empty error!"), REJECT_INVALID, 
                                    "uid-empty-err");
                }
            }
            break;
        }
        case UtxoCondType::P2PH : {
            CPasswordHashLockCondOut& theCond = dynamic_cast< CPasswordHashLockCondOut& > (cond);

            if (isCheckInput) {
                bool found = false;
                for (auto cond : input.conds) {
                    if (cond.cond_type == UtxoCondType::P2PH) {
                        found = true;
                        CPasswordHashLockCondIn& inCond = dynamic_cast< CPasswordHashLockCondIn& > (cond);
                        string text = strprintf("%s%s", inCond.password, txUid.ToString());
                        uint256 hash = Hash(text); 
                        if (theCond.password_hash != hash) {
                            return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, secret mismatches error!"), REJECT_INVALID, 
                                            "secret-mismatches-err");
                        }
                    } else
                        continue;
                }
                if (!found) {
                     return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, cond mismatches error!"), REJECT_INVALID, 
                                    "cond-mismatches-err");
                }
            } else { //output cond
                if (theCond.password_hash == uint256()) {
                    return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, empty hash lock error!"), REJECT_INVALID, 
                                    "empty-hash-lock-err");
                }
            }
            break;
        }
        case UtxoCondType::CLAIM_LOCK : { 
            CClaimLockCondOut& theCond = dynamic_cast< CClaimLockCondOut& > (cond);
            
            if (isCheckInput) {
                if (context.height <= theCond.height) {
                    return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, too early to claim error!"), REJECT_INVALID, 
                                    "too-early-to-claim-err");
                }
            } else { //output cond
                if (theCond.height == 0) {
                    return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, claim lock empty error!"), REJECT_INVALID, 
                                    "claim-lock-empty-err");
                }
            }
            break; 
        }
        case UtxoCondType::RECLAIM_LOCK : {
            CReClaimLockCondOut& theCond = dynamic_cast< CReClaimLockCondOut& > (cond);

            if (isCheckInput) {
                if (prevUtxoTxUid == txUid) { // for reclaiming the coins
                    if (theCond.height == 0 || context.height <= theCond.height) {
                        return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, too early to reclaim error!"), REJECT_INVALID, 
                                        "too-early-to-claim-err");
                    } 
                }
            } else { //output cond
                if (theCond.height == 0) {
                    return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, reclaim lock empty error!"), REJECT_INVALID, 
                                    "reclaim-lock-empty-err");
                }
            }
            break; 
        }
        default: {
            string strInOut = isCheckInput ? "input" : "output";
            return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, %s cond type error!", strInOut), REJECT_INVALID, 
                                "cond-type-err");
        }
    }
}

bool CCoinUtxoTx::CheckTx(CTxExecuteContext &context) {
    IMPLEMENT_DEFINE_CW_STATE;
    IMPLEMENT_DISABLE_TX_PRE_STABLE_COIN_RELEASE;
    IMPLEMENT_CHECK_TX_MEMO;
    IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(txUid);
    if (!CheckFee(context)) return false;

    if ((txUid.is<CPubKey>()) && !txUid.get<CPubKey>().IsFullyValid())
        return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, public key is invalid"), REJECT_INVALID,
                        "bad-publickey");

    CAccount srcAccount;
    if (!cw.accountCache.GetAccount(txUid, srcAccount)) //unrecorded account not allowed to participate
        return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, read account failed"), REJECT_INVALID,
                        "bad-getaccount");

    if (vins.size() > 100) //FIXME: need to use sysparam to replace 100
        return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, vins size > 100 error"), REJECT_INVALID,
                        "vins-size-too-large");

    if (vouts.size() > 100) //FIXME: need to use sysparam to replace 100
        return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, vouts size > 100 error"), REJECT_INVALID,
                        "vouts-size-too-large");

    if (vins.size() == 0 && vouts.size() == 0)
        return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, empty utxo error"), REJECT_INVALID,
                        "utxo-empty-err");

    uint64_t minFee;
    if (!GetTxMinFee(nTxType, context.height, fee_symbol, minFee)) { assert(false); }
    uint64_t minerMinFees = (2 * vins.size() + vouts.size()) * minFee;
    if (llFees < minerMinFees)
        return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, tx fee too small!"), REJECT_INVALID, 
                        "bad-tx-fee-toosmall");

    uint64_t totalInAmount = 0;
    uint64_t totalOutAmount = 0;
    for (auto input : vins) {
        //load prevUtxoTx from blockchain
        std::shared_ptr<CCoinUtxoTx> pPrevUtxoTx;
        if (!GetUtxoTxFromChain(input.prev_utxo_txid, pPrevUtxoTx))
            return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, failed to load prev utxo from chain!"), REJECT_INVALID, 
                            "failed-to-load-prev-utxo-err");

        if (pPrevUtxoTx->vouts.size() < input.prev_utxo_out_index + 1)
            return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, prev utxo index OOR error!"), REJECT_INVALID, 
                            "prev-utxo-index-OOR-err");

        //enumerate the prev tx out conditions to check if current input meets 
        //the output conditions of the previous Tx
        for (auto cond : pPrevUtxoTx->vouts[input.prev_utxo_out_index].conds)
            CheckUtxoCondition(true, context, pPrevUtxoTx->txUid, txUid, input, cond);
    
        totalInAmount += pPrevUtxoTx->vouts[input.prev_utxo_out_index].coin_amount;
    }

    for (auto output : vouts) {
        if (output.coin_amount == 0)
            return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, zeror output amount error!"), REJECT_INVALID, 
                            "zero-output-amount-err");

        //check each cond's validity
        for (auto cond : output.conds)
            CheckUtxoCondition(false, context, CUserID(), txUid, CUtxoInput(), cond);

        totalOutAmount += output.coin_amount;
    }

    uint64_t accountBalance = srcAccount.GetBalance(coin_symbol, BalanceType::FREE_VALUE);
    if (accountBalance + totalInAmount < totalOutAmount + llFees) {
        return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, account balance coin_amount insufficient!"), REJECT_INVALID, 
                        "insufficient-account-coin-amount");
    }

    CPubKey pubKey = (txUid.is<CPubKey>() ? txUid.get<CPubKey>() : srcAccount.owner_pubkey);
    IMPLEMENT_CHECK_TX_SIGNATURE(pubKey);

    return true;
}

/**
 * only deal with account balance states change...nothing on UTXO
 */
bool CCoinUtxoTx::ExecuteTx(CTxExecuteContext &context) {
    CCacheWrapper &cw       = *context.pCw;
    CValidationState &state = *context.pState;

    CAccount srcAccount;
    if (!cw.accountCache.GetAccount(txUid, srcAccount))
        return state.DoS(100, ERRORMSG("CCoinUtxoTx::ExecuteTx, read txUid %s account info error",
                        txUid.ToString()), READ_ACCOUNT_FAIL, "bad-read-accountdb");

    if (!GenerateRegID(context, srcAccount)) {
        return false;
    }

    vector<CReceipt> receipts;

    uint64_t totalInAmount = 0;
    uint64_t totalOutAmount = 0;
    for (auto input : vins) {
        if (!context.pCw->txUtxoCache.GetUtxoTx(std::make_pair(input.prev_utxo_txid, input.prev_utxo_out_index)))
            return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, prev utxo already spent error!"), REJECT_INVALID, 
                            "double-spend-prev-utxo-err");

        //load prevUtxoTx from blockchain
        std::shared_ptr<CCoinUtxoTx> pPrevUtxoTx;
        if (!GetUtxoTxFromChain(input.prev_utxo_txid, pPrevUtxoTx))
            return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, failed to load prev utxo from chain!"), REJECT_INVALID, 
                            "failed-to-load-prev-utxo-err");

        totalInAmount += pPrevUtxoTx->vouts[input.prev_utxo_out_index].coin_amount;

        if (!context.pCw->txUtxoCache.DelUtoxTx(std::make_pair(input.prev_utxo_txid, input.prev_utxo_out_index)))
            return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, del prev utxo error!"), REJECT_INVALID, 
                            "del-prev-utxo-err");
    }

    for (int i = 0; i < vouts.size(); i++) {
        CUtxoOutput output = vouts[i];
        totalOutAmount += output.coin_amount;

        if (!context.pCw->txUtxoCache.SetUtxoTx(std::make_pair(GetHash(), i)))
            return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, set utxo error!"), REJECT_INVALID, 
                            "set-utxo-err");
    }

    uint64_t accountBalance = srcAccount.GetBalance(coin_symbol, BalanceType::FREE_VALUE);
    if (accountBalance + totalInAmount < totalOutAmount + llFees) {
        return state.DoS(100, ERRORMSG("CCoinUtxoTx::CheckTx, account balance coin_amount insufficient!"), REJECT_INVALID, 
                        "insufficient-account-coin-amount");
    }
    int diff = totalInAmount - totalOutAmount - llFees;
    if (diff < 0) {
        if (!srcAccount.OperateBalance(coin_symbol, SUB_FREE, abs(diff))) {
            return state.DoS(100, ERRORMSG("CCoinUtxoTx::ExecuteTx, failed to deduct coin_amount in txUid %s account",
                            txUid.ToString()), UPDATE_ACCOUNT_FAIL, "insufficient-fund-utxo");
        }
    } else if (diff > 0) {
        if (!srcAccount.OperateBalance(coin_symbol, ADD_FREE, diff)) {
            return state.DoS(100, ERRORMSG("CCoinUtxoTx::ExecuteTx, failed to add coin_amount in txUid %s account",
                            txUid.ToString()), UPDATE_ACCOUNT_FAIL, "insufficient-fund-utxo");
        }
    }
    
    receipts.emplace_back(txUid, utxo.to_uid, utxo.coin_symbol, abs(diff), ReceiptCode::TRANSFER_UTXO_COINS);
    
    if (!cw.accountCache.SaveAccount(srcAccount))
        return state.DoS(100, ERRORMSG("CCoinUtxoTx::ExecuteTx, write source addr %s account info error",
                        txUid.ToString()), UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");

    if (!receipts.empty() && !cw.txReceiptCache.SetTxReceipts(GetHash(), receipts))
        return state.DoS(100, ERRORMSG("CCDPStakeTx::ExecuteTx, set tx receipts failed!! txid=%s",
                        GetHash().ToString()), REJECT_INVALID, "set-tx-receipt-failed");

    return true;
}

string CCoinUtxoTx::ToString(CAccountDBCache &accountCache) {
    return strprintf(
        "txType=%s, hash=%s, ver=%d, txUid=%s, fee_symbol=%s, llFees=%llu, "
        "valid_height=%d, priorUtxoTxId=%s, priorUtxoSecret=%s, utxo=[%s], memo=%s",
        GetTxType(nTxType), GetHash().ToString(), nVersion, txUid.ToString(), fee_symbol, llFees,
        valid_height, prior_utxo_txid.ToString(), prior_utxo_secret, utxo.ToString(), HexStr(memo));
}

Object CCoinUtxoTx::ToJson(const CAccountDBCache &accountCache) const {
    Object result = CBaseTx::ToJson(accountCache);

    result.push_back(Pair("prior_utxo_txid", prior_utxo_txid.ToString()));
    result.push_back(Pair("prior_utxo_secret", prior_utxo_secret));

    if (!utxo.is_null) {
        Array utxoArray;
        utxoArray.push_back(utxo.ToJson());
        result.push_back(Pair("utxo", utxoArray));
    }

    result.push_back(Pair("memo", memo));

    return result;
}
