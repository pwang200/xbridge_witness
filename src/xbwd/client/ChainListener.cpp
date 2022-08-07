//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2021 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xbwd/client/ChainListener.h>

#include <xbwd/client/WebsocketClient.h>
#include <xbwd/federator/Federator.h>
#include <xbwd/federator/FederatorEvents.h>

#include <ripple/basics/Log.h>
#include <ripple/basics/XRPAmount.h>
#include <ripple/basics/strHex.h>
#include <ripple/json/Output.h>
#include <ripple/json/json_get_or_throw.h>
#include <ripple/json/json_writer.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/TER.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>

#include <type_traits>

namespace xbwd {

class Federator;

ChainListener::ChainListener(
    IsMainchain isMainchain,
    ripple::STXChainBridge const sidechain,
    std::weak_ptr<Federator>&& federator,
    beast::Journal j)
    : isMainchain_{isMainchain == IsMainchain::yes}
    , bridge_{sidechain}
    , federator_{std::move(federator)}
    , j_{j}
{
}

// destructor must be defined after WebsocketClient size is known (i.e. it can
// not be defaulted in the header or the unique_ptr declration of
// WebsocketClient won't work)
ChainListener::~ChainListener() = default;

void
ChainListener::init(boost::asio::io_service& ios, beast::IP::Endpoint const& ip)
{
    wsClient_ = std::make_unique<WebsocketClient>(
        [self = shared_from_this()](Json::Value const& msg) {
            self->onMessage(msg);
        },
        ios,
        ip,
        /*headers*/ std::unordered_map<std::string, std::string>{},
        j_);

    Json::Value params;
    params[ripple::jss::account_history_tx_stream] = Json::objectValue;
    params[ripple::jss::account_history_tx_stream][ripple::jss::account] =
        ripple::toBase58(
            isMainchain_ ? bridge_.lockingChainDoor()
                         : bridge_.issuingChainDoor());
    send("subscribe", params);
}

void
ChainListener::shutdown()
{
    if (wsClient_)
        wsClient_->shutdown();
}

std::uint32_t
ChainListener::send(std::string const& cmd, Json::Value const& params)
{
    return wsClient_->send(cmd, params);
}

void
ChainListener::stopHistoricalTxns()
{
    Json::Value params;
    params[ripple::jss::account_history_tx_stream] = Json::objectValue;
    params[ripple::jss::account_history_tx_stream]
          [ripple::jss::stop_history_tx_only] = true;
    params[ripple::jss::account_history_tx_stream][ripple::jss::account] =
        ripple::toBase58(
            isMainchain_ ? bridge_.lockingChainDoor()
                         : bridge_.issuingChainDoor());
    send("unsubscribe", params);
}

void
ChainListener::send(
    std::string const& cmd,
    Json::Value const& params,
    RpcCallback onResponse)
{
    JLOGV(
        j_.trace(),
        "ChainListener send",
        ripple::jv("command", cmd),
        ripple::jv("params", params));

    auto id = wsClient_->send(cmd, params);
    std::lock_guard lock(callbacksMtx_);
    callbacks_.emplace(id, onResponse);
}

std::string const&
ChainListener::chainName() const
{
    // Note: If this function is ever changed to return a value instead of a
    // ref, review the code to ensure the "jv" functions don't bind to temps
    static const std::string m("Mainchain");
    static const std::string s("Sidechain");
    return isMainchain_ ? m : s;
}

template <class E>
void
ChainListener::pushEvent(E&& e)
{
    static_assert(std::is_rvalue_reference_v<decltype(e)>, "");

    if (auto f = federator_.lock())
    {
        f->push(std::move(e));
    }
}

void
ChainListener::onMessage(Json::Value const& msg)
{
    auto callbackOpt = [&]() -> std::optional<RpcCallback> {
        if (msg.isMember(ripple::jss::id) && msg[ripple::jss::id].isIntegral())
        {
            auto callbackId = msg[ripple::jss::id].asUInt();
            std::lock_guard lock(callbacksMtx_);
            auto i = callbacks_.find(callbackId);
            if (i != callbacks_.end())
            {
                auto cb = i->second;
                callbacks_.erase(i);
                return cb;
            }
        }
        return {};
    }();

    if (callbackOpt)
    {
        JLOGV(
            j_.trace(),
            "ChainListener onMessage, reply to a callback",
            ripple::jv("msg", msg));
        assert(msg.isMember(ripple::jss::result));
        (*callbackOpt)(msg[ripple::jss::result]);
    }
    else
    {
        processMessage(msg);
    }
}

void
ChainListener::processMessage(Json::Value const& msg)
{
    // Even though this lock has a large scope, this function does very little
    // processing and should run relatively quickly
    std::lock_guard l{m_};

    JLOGV(
        j_.trace(),
        "chain listener message",
        ripple::jv("msg", msg),
        ripple::jv("isMainchain", isMainchain_));

    if (!msg.isMember(ripple::jss::validated) ||
        !msg[ripple::jss::validated].asBool())
    {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            ripple::jv("reason", "not validated"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName()));
        return;
    }

    if (!msg.isMember(ripple::jss::engine_result_code))
    {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            ripple::jv("reason", "no engine result code"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName()));
        return;
    }

    if (!msg.isMember(ripple::jss::account_history_tx_index))
    {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            ripple::jv("reason", "no account history tx index"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName()));
        return;
    }

    if (!msg.isMember(ripple::jss::meta))
    {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            ripple::jv("reason", "tx meta"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName()));
        return;
    }

    auto fieldMatchesStr =
        [](Json::Value const& val, char const* field, char const* toMatch) {
            if (!val.isMember(field))
                return false;
            auto const f = val[field];
            if (!f.isString())
                return false;
            return f.asString() == toMatch;
        };

    ripple::TER const txnTER = [&msg] {
        return ripple::TER::fromInt(
            msg[ripple::jss::engine_result_code].asInt());
    }();

    bool const txnSuccess = ripple::isTesSuccess(txnTER);

    // values < 0 are historical txns. values >= 0 are new transactions. Only
    // the initial sync needs historical txns.
    int const txnHistoryIndex =
        msg[ripple::jss::account_history_tx_index].asInt();

    auto const meta = msg[ripple::jss::meta];

    auto const txnBridge = [&]() -> std::optional<ripple::STXChainBridge> {
        if (msg.isMember(ripple::jss::XChainBridge))
        {
            try
            {
                if (!msg.isMember(ripple::jss::transaction))
                    return {};
                auto const txn = msg[ripple::jss::transaction];
                return ripple::STXChainBridge(txn[ripple::jss::XChainBridge]);
            }
            catch (...)
            {
            }
        }
        return std::nullopt;
    }();

    enum class TxnType { xChainCommit, xChainClaim, xChainCreateAccount };
    auto txnTypeOpt = [&]() -> std::optional<TxnType> {
        // Only keep transactions to or from the door account.
        // Transactions to the account are initiated by users and are are cross
        // chain transactions. Transaction from the account are initiated by
        // federators and need to be monitored for errors. There are two types
        // of transactions that originate from the door account: the second half
        // of a cross chain payment and a refund of a failed cross chain
        // payment.

        if (!fieldMatchesStr(msg, ripple::jss::type, ripple::jss::transaction))
            return {};

        if (!msg.isMember(ripple::jss::transaction))
            return {};
        auto const txn = msg[ripple::jss::transaction];

        auto const isXChainCommit = fieldMatchesStr(
            txn, ripple::jss::TransactionType, ripple::jss::XChainCommit);

        auto const isXChainClaim = !isXChainCommit &&
            fieldMatchesStr(
                txn, ripple::jss::TransactionType, ripple::jss::XChainClaim);

        auto const isXChainCreateAccount = !isXChainCommit && !isXChainClaim &&
            fieldMatchesStr(
                txn,
                ripple::jss::TransactionType,
                ripple::jss::SidechainXChainAccountCreate);

        if (!isXChainCommit && !isXChainClaim && !isXChainCreateAccount)
            return {};

        if (!txnBridge)
        {
            JLOGV(
                j_.trace(),
                "ignoring listener message",
                ripple::jv("reason", "invalid txn: Missing bridge"),
                ripple::jv("msg", msg),
                ripple::jv("chain_name", chainName()));
            return {};
        }

        if (*txnBridge != bridge_)
        {
            // TODO: It is a mistake to filter out based on sidechain.
            // This server should support multiple sidechains
            // Note: the federator stores a hard-coded sidechain in the
            // database, if we remove this filter we need to remove sidechain
            // from the app and listener as well
            JLOGV(
                j_.trace(),
                "ignoring listener message",
                ripple::jv("reason", "Sidechain mismatch"),
                ripple::jv("msg", msg),
                ripple::jv("chain_name", chainName()));
            return {};
        }

        if (isXChainClaim)
            return TxnType::xChainClaim;
        else if (isXChainCommit)
            return TxnType::xChainCommit;
        else if (isXChainCreateAccount)
            return TxnType::xChainCreateAccount;
        return {};
    }();

    if (!txnTypeOpt)
    {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            ripple::jv("reason", "not a sidechain transaction"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName()));
        return;
    }

    auto const txnHash = [&]() -> std::optional<ripple::uint256> {
        try
        {
            ripple::uint256 result;
            if (result.parseHex(msg[ripple::jss::transaction][ripple::jss::hash]
                                    .asString()))
                return result;
        }
        catch (...)
        {
        }
        // TODO: this is an insane input stream
        // Detect and connect to another server
        return {};
    }();
    if (!txnHash)
    {
        JLOGV(
            j_.warn(),
            "ignoring listener message",
            ripple::jv("reason", "no tx hash"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName()));
        return;
    }

    auto const txnSeq = [&]() -> std::optional<std::uint32_t> {
        try
        {
            return msg[ripple::jss::transaction][ripple::jss::Sequence]
                .asUInt();
        }
        catch (...)
        {
            // TODO: this is an insane input stream
            // Detect and connect to another server
            return {};
        }
    }();
    if (!txnSeq)
    {
        JLOGV(
            j_.warn(),
            "ignoring listener message",
            ripple::jv("reason", "no txnSeq"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName()));
        return;
    }
    auto const lgrSeq = [&]() -> std::optional<std::uint32_t> {
        try
        {
            if (msg.isMember(ripple::jss::ledger_index))
            {
                return msg[ripple::jss::ledger_index].asUInt();
            }
        }
        catch (...)
        {
        }
        // TODO: this is an insane input stream
        // Detect and connect to another server
        return {};
    }();
    if (!lgrSeq)
    {
        JLOGV(
            j_.warn(),
            "ignoring listener message",
            ripple::jv("reason", "no lgrSeq"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName()));
        return;
    }
    TxnType const txnType = *txnTypeOpt;

    std::optional<ripple::STAmount> deliveredAmt;
    if (meta.isMember(ripple::jss::delivered_amount))
    {
        deliveredAmt = amountFromJson(
            ripple::sfGeneric,
            msg[ripple::jss::transaction][ripple::jss::delivered_amount]);
    }
    // TODO: Add delivered amount to the txn data; for now override with amount
    if (msg[ripple::jss::transaction].isMember(ripple::jss::Amount))
    {
        deliveredAmt = amountFromJson(
            ripple::sfGeneric,
            msg[ripple::jss::transaction][ripple::jss::Amount]);
    }

    auto const src = [&]() -> std::optional<ripple::AccountID> {
        try
        {
            return ripple::parseBase58<ripple::AccountID>(
                msg[ripple::jss::transaction][ripple::jss::Account].asString());
        }
        catch (...)
        {
        }
        // TODO: this is an insane input stream
        // Detect and connect to another server
        return {};
    }();
    if (!src)
    {
        JLOGV(
            j_.warn(),
            "ignoring listener message",
            ripple::jv("reason", "no account src"),
            ripple::jv("msg", msg),
            ripple::jv("chain_name", chainName()));
        return;
    }
    auto const dst = [&]() -> std::optional<ripple::AccountID> {
        try
        {
            switch (txnType)
            {
                case TxnType::xChainCreateAccount:
                    [[fallthrough]];
                case TxnType::xChainClaim:
                    return ripple::parseBase58<ripple::AccountID>(
                        msg[ripple::jss::transaction]
                           [ripple::sfDestination.getJsonName()]
                               .asString());
                case TxnType::xChainCommit:
                    return ripple::parseBase58<ripple::AccountID>(
                        msg[ripple::jss::transaction]
                           [ripple::sfOtherChainAccount.getJsonName()]
                               .asString());
            }
        }
        catch (...)
        {
        }
        return {};
    }();

    switch (txnType)
    {
        case TxnType::xChainClaim: {
            auto const claimID = Json::getOptional<std::uint64_t>(
                msg[ripple::jss::transaction], ripple::sfXChainClaimID);

            if (!claimID)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv("reason", "no xChainSeq"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName()));
                return;
            }
            if (!dst)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv("reason", "no dst in xchain claim"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName()));
                return;
            }
            using namespace event;
            XChainTransferResult e{
                isMainchain_ ? Dir::issuingToLocking : Dir::lockingToIssuing,
                *dst,
                deliveredAmt,
                *claimID,
                *lgrSeq,
                *txnHash,
                txnTER,
                txnHistoryIndex};
            pushEvent(std::move(e));
        }
        break;
        case TxnType::xChainCommit: {
            auto const claimID = Json::getOptional<std::uint64_t>(
                msg[ripple::jss::transaction], ripple::sfXChainClaimID);

            if (!claimID)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv("reason", "no xChainSeq"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName()));
                return;
            }
            if (!txnBridge)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv("reason", "no bridge in xchain commit"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName()));
                return;
            }
            using namespace event;
            XChainCommitDetected e{
                isMainchain_ ? Dir::lockingToIssuing : Dir::issuingToLocking,
                *src,
                *txnBridge,
                deliveredAmt,
                *claimID,
                dst,
                *lgrSeq,
                *txnHash,
                txnTER,
                txnHistoryIndex};
            pushEvent(std::move(e));
        }
        break;
        case TxnType::xChainCreateAccount: {
            // TODO: Get create count from the metadata
            std::optional<std::uint64_t> const createCount;

            if (!createCount)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv("reason", "no createCount"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName()));
                return;
            }
            if (!txnBridge)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv("reason", "no bridge in xchain commit"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName()));
                return;
            }
            auto const rewardAmt = [&]() -> std::optional<ripple::STAmount> {
                if (msg[ripple::jss::transaction].isMember(
                        ripple::sfSignatureReward.getJsonName()))
                {
                    return amountFromJson(
                        ripple::sfGeneric,
                        msg[ripple::jss::transaction]
                           [ripple::sfSignatureReward.getJsonName()]);
                }
                return {};
            }();
            if (!rewardAmt)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv(
                        "reason", "no reward amt in xchain create account"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName()));
                return;
            }
            if (!dst)
            {
                JLOGV(
                    j_.warn(),
                    "ignoring listener message",
                    ripple::jv("reason", "no dst in xchain create account"),
                    ripple::jv("msg", msg),
                    ripple::jv("chain_name", chainName()));
                return;
            }
            using namespace event;
            XChainAccountCreateCommitDetected e{
                isMainchain_ ? Dir::lockingToIssuing : Dir::issuingToLocking,
                *src,
                *txnBridge,
                deliveredAmt,
                *rewardAmt,
                *createCount,
                *dst,
                *lgrSeq,
                *txnHash,
                txnTER,
                txnHistoryIndex};
            pushEvent(std::move(e));
        }
        break;
    }
}

Json::Value
ChainListener::getInfo() const
{
    std::lock_guard l{m_};

    // TODO
    Json::Value ret{Json::objectValue};
    return ret;
}

}  // namespace xbwd
