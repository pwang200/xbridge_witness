#include <xbwd/rpc/RPCHandler.h>

#include <xbwd/app/App.h>
#include <xbwd/app/DBInit.h>
#include <xbwd/rpc/fromJSON.h>

#include <ripple/json/json_value.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/STArray.h>
#include <ripple/protocol/STBase.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/STXChainAttestationBatch.h>
#include <ripple/protocol/jss.h>

#include <fmt/core.h>

#include <functional>
#include <unordered_map>

namespace xbwd {
namespace rpc {

namespace {

void
doStop(App& app, Json::Value const& in, Json::Value& result)
{
    // TODO: This is a privilated command.
    result["request"] = in;
    result["result"] = "stopping";
    app.signalStop();
}

void
doServerInfo(App& app, Json::Value const& in, Json::Value& result)
{
    result["request"] = in;
    result["result"] = "normal";
}

void
doWitness(App& app, Json::Value const& in, Json::Value& result)
{
    result["request"] = in;
    auto optBridge = optFromJson<ripple::STXChainBridge>(in, "bridge");
    auto optAmt = optFromJson<ripple::STAmount>(in, "sending_amount");
    auto optClaimID = optFromJson<std::uint64_t>(in, "claim_id");
    auto optDoor = optFromJson<ripple::AccountID>(in, "door");
    auto optSendingAccount =
        optFromJson<ripple::AccountID>(in, "sending_account");
    auto optRewardAccount =
        optFromJson<ripple::AccountID>(in, "reward_account");
    auto optDst = optFromJson<ripple::AccountID>(in, "destination");
    {
        auto const missingOrInvalidField = [&]() -> std::string {
            if (!optBridge)
                return "bridge";
            if (!optAmt)
                return "sending_amount";
            if (!optClaimID)
                return "claim_id";
            if (!optDoor)
                return "door";
            if (!optSendingAccount)
                return "sending_account";
            if (!optRewardAccount)
                return "reward_account";
            return {};
        }();
        if (!missingOrInvalidField.empty())
        {
            result["error"] = fmt::format(
                "Missing or invalid field: {}", missingOrInvalidField);
            return;
        }
    }

    auto const& door = *optDoor;
    auto const& sendingAccount = *optSendingAccount;
    auto const& bridge = *optBridge;
    auto const& sendingAmount = *optAmt;
    auto const& rewardAccount = *optRewardAccount;
    auto const& claimID = *optClaimID;

    bool const wasLockingChainSend =
        (*optDoor == optBridge->lockingChainDoor());
    if (!wasLockingChainSend && *optDoor != optBridge->issuingChainDoor())
    {
        // TODO: Write log message
        // put expected value in the error message?
        result["error"] = fmt::format(
            "Specified door account does not match any sidechain door "
            "account.");
        return;
    }

    auto const& tblName = wasLockingChainSend
        ? db_init::xChainLockingToIssuingTableName()
        : db_init::xChainIssuingToLockingTableName();

    std::vector<std::uint8_t> const encodedBridge = [&] {
        ripple::Serializer s;
        bridge.add(s);
        return std::move(s.modData());
    }();

    auto const encodedAmt = [&]() -> std::vector<std::uint8_t> {
        ripple::Serializer s;
        sendingAmount.add(s);
        return std::move(s.modData());
    }();
    {
        auto session = app.getXChainTxnDB().checkoutDb();
        soci::blob amtBlob(*session);
        soci::blob bridgeBlob(*session);
        soci::blob sendingAccountBlob(*session);
        soci::blob rewardAccountBlob(*session);
        soci::blob otherChainAccountBlob(*session);
        soci::blob publicKeyBlob(*session);
        soci::blob signatureBlob(*session);

        convert(encodedAmt, amtBlob);
        convert(encodedBridge, bridgeBlob);
        convert(sendingAccount, sendingAccountBlob);
        if (optDst)
            convert(*optDst, otherChainAccountBlob);

        auto sql = fmt::format(
            R"sql(SELECT Signature, PublicKey, RewardAccount FROM {table_name}
                  WHERE ClaimID = :claimID and
                        Success = 1 and
                        DeliveredAmt = :amt and
                        Bridge = :bridge and
                        SendingAccount = :sendingAccount and
                        OtherChainAccount = :otherChainAccount;
            )sql",
            fmt::arg("table_name", tblName));

        *session << sql, soci::into(signatureBlob), soci::into(publicKeyBlob),
            soci::into(rewardAccountBlob), soci::use(*optClaimID),
            soci::use(amtBlob), soci::use(bridgeBlob),
            soci::use(sendingAccountBlob), soci::use(otherChainAccountBlob);

        // TODO: Check for multiple values
        if (signatureBlob.get_len() > 0 && publicKeyBlob.get_len() > 0 &&
            rewardAccountBlob.get_len() > 0)
        {
            ripple::AccountID rewardAccount;
            convert(rewardAccountBlob, rewardAccount);
            ripple::PublicKey signingPK;
            convert(publicKeyBlob, signingPK);
            ripple::Buffer sigBuf;
            convert(signatureBlob, sigBuf);

            ripple::AttestationBatch::AttestationClaim claim{
                signingPK,
                sigBuf,
                sendingAccount,
                sendingAmount,
                rewardAccount,
                wasLockingChainSend,
                claimID,
                optDst};

            ripple::STXChainAttestationBatch batch{bridge, &claim, &claim + 1};
            result["result"]["XChainAttestationBatch"] =
                batch.getJson(ripple::JsonOptions::none);
        }
        else
        {
            result["error"] = "No such transaction";
        }
    }
}

void
doWitnessAccountCreate(App& app, Json::Value const& in, Json::Value& result)
{
    result["request"] = in;
    auto optBridge = optFromJson<ripple::STXChainBridge>(in, "bridge");
    auto optAmt = optFromJson<ripple::STAmount>(in, "sending_amount");
    auto optRewardAmt = optFromJson<ripple::STAmount>(in, "reward_amount");
    auto optCreateCount = optFromJson<std::uint64_t>(in, "create_count");
    auto optDoor = optFromJson<ripple::AccountID>(in, "door");
    auto optSendingAccount =
        optFromJson<ripple::AccountID>(in, "sending_account");
    auto optRewardAccount =
        optFromJson<ripple::AccountID>(in, "reward_account");
    auto optDst = optFromJson<ripple::AccountID>(in, "destination");
    {
        auto const missingOrInvalidField = [&]() -> std::string {
            if (!optBridge)
                return "bridge";
            if (!optAmt)
                return "sending_amount";
            if (!optRewardAmt)
                return "reward_amount";
            if (!optDoor)
                return "door";
            if (!optSendingAccount)
                return "sending_account";
            if (!optRewardAccount)
                return "reward_account";
            if (!optDst)
                return "destination";
            return {};
        }();
        if (!missingOrInvalidField.empty())
        {
            result["error"] = fmt::format(
                "Missing or invalid field: {}", missingOrInvalidField);
            return;
        }
    }

    auto const& door = *optDoor;
    auto const& sendingAccount = *optSendingAccount;
    auto const& bridge = *optBridge;
    auto const& sendingAmount = *optAmt;
    auto const& rewardAmount = *optRewardAmt;
    auto const& rewardAccount = *optRewardAccount;
    auto const& createCount = *optCreateCount;
    auto const& dst = *optDst;

    bool const wasLockingChainSend =
        (*optDoor == optBridge->lockingChainDoor());
    if (!wasLockingChainSend && *optDoor != optBridge->issuingChainDoor())
    {
        // TODO: Write log message
        // put expected value in the error message?
        result["error"] = fmt::format(
            "Specified door account does not match any sidechain door "
            "account.");
        return;
    }

    auto const& tblName = wasLockingChainSend
        ? db_init::xChainCreateAccountLockingTableName()
        : db_init::xChainCreateAccountIssuingTableName();

    std::vector<std::uint8_t> const encodedBridge = [&] {
        ripple::Serializer s;
        bridge.add(s);
        return std::move(s.modData());
    }();

    auto const encodedAmt = [&]() -> std::vector<std::uint8_t> {
        ripple::Serializer s;
        sendingAmount.add(s);
        return std::move(s.modData());
    }();
    auto const encodedRewardAmt = [&]() -> std::vector<std::uint8_t> {
        ripple::Serializer s;
        rewardAmount.add(s);
        return std::move(s.modData());
    }();
    {
        auto session = app.getXChainTxnDB().checkoutDb();

        soci::blob amtBlob(*session);
        convert(encodedAmt, amtBlob);

        soci::blob rewardAmtBlob(*session);
        convert(encodedRewardAmt, rewardAmtBlob);

        soci::blob bridgeBlob(*session);
        convert(encodedBridge, bridgeBlob);

        soci::blob sendingAccountBlob(*session);
        convert(sendingAccount, sendingAccountBlob);

        soci::blob otherChainAccountBlob(*session);
        convert(dst, otherChainAccountBlob);

        soci::blob rewardAccountBlob(*session);
        soci::blob publicKeyBlob(*session);
        soci::blob signatureBlob(*session);

        auto sql = fmt::format(
            R"sql(SELECT Signature, PublicKey, RewardAccount FROM {table_name}
                  WHERE CreateCount = :createCount and
                        Success = 1 and
                        DeliveredAmt = :amt and
                        RewardAmt = :rewardAmt and
                        Bridge = :bridge and
                        SendingAccount = :sendingAccount and
                        OtherChainAccount = :otherChainAccount;
            )sql",
            fmt::arg("table_name", tblName));

        *session << sql, soci::into(signatureBlob), soci::into(publicKeyBlob),
            soci::into(rewardAccountBlob), soci::use(createCount),
            soci::use(amtBlob), soci::use(rewardAmtBlob), soci::use(bridgeBlob),
            soci::use(sendingAccountBlob), soci::use(otherChainAccountBlob);

        // TODO: Check for multiple values
        if (signatureBlob.get_len() > 0 && publicKeyBlob.get_len() > 0 &&
            rewardAccountBlob.get_len() > 0)
        {
            ripple::AccountID rewardAccount;
            convert(rewardAccountBlob, rewardAccount);
            ripple::PublicKey signingPK;
            convert(publicKeyBlob, signingPK);
            ripple::Buffer sigBuf;
            convert(signatureBlob, sigBuf);

            ripple::AttestationBatch::AttestationCreateAccount claim{
                signingPK,
                sigBuf,
                sendingAccount,
                sendingAmount,
                rewardAmount,
                rewardAccount,
                wasLockingChainSend,
                createCount,
                dst};

            ripple::AttestationBatch::AttestationClaim* nullClaim = nullptr;
            ripple::STXChainAttestationBatch batch{
                bridge, nullClaim, nullClaim, &claim, &claim + 1};
            result["result"]["XChainAttestationBatch"] =
                batch.getJson(ripple::JsonOptions::none);
        }
        else
        {
            result["error"] = "No such transaction";
        }
    }
}

using CmdFun = std::function<void(App&, Json::Value const&, Json::Value&)>;

std::unordered_map<std::string, CmdFun> const handlers = [] {
    using namespace std::literals;
    std::unordered_map<std::string, CmdFun> r;
    r.emplace("stop"s, doStop);
    r.emplace("server_info"s, doServerInfo);
    r.emplace("witness"s, doWitness);
    r.emplace("witness_account_create"s, doWitnessAccountCreate);
    return r;
}();
}  // namespace

void
doCommand(App& app, Json::Value const& in, Json::Value& result)
{
    auto const cmd = [&]() -> std::string {
        auto const cmd = in[ripple::jss::command];
        if (!cmd.isString())
        {
            return {};
        }
        return cmd.asString();
    }();
    auto it = handlers.find(cmd);
    if (it == handlers.end())
    {
        // TODO: regularize error handling
        result["error"] = fmt::format("No such method: {}", cmd);
        return;
    }
    return it->second(app, in, result);
}

}  // namespace rpc
}  // namespace xbwd
