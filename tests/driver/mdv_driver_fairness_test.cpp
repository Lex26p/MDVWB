#include "mdv_driver.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using mdv::RequestFrame;
using mdv::ResponseFrame;

bool Check(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

ResponseFrame MakeResponse(
    std::uint8_t address,
    mdv::Command command,
    bool modeLocked = false,
    std::uint8_t setTemperature = 23)
{
    ResponseFrame response{
        0xAA, 0xC0, 0x80, 0x00, 0x00, 0x00, 0xE0, 0x14,
        0x88, 0x84, 0x17, 0x58, 0x3C, 0x4C, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x55};
    response[1] = static_cast<std::uint8_t>(command);
    response[4] = address;
    response[8] = static_cast<std::uint8_t>(
        modeLocked ? 0xA8 : 0x88);
    response[10] = setTemperature;

    std::uint8_t sum = 0;
    for (std::size_t index = 1; index <= 29; ++index) {
        sum = static_cast<std::uint8_t>(sum + response[index]);
    }
    response[30] = static_cast<std::uint8_t>(0U - sum);
    return response;
}

mdv::TransactionResult Success(ResponseFrame response)
{
    return mdv::TransactionResult{
        .status = mdv::TransactionStatus::Success,
        .response = std::move(response),
        .error = {},
    };
}

class IgnoringDeviceTransport final : public mdv::ITransactionTransport {
public:
    mdv::TransactionResult Execute(const RequestFrame& request) override
    {
        requests.push_back(request);
        const auto command = static_cast<mdv::Command>(request[1]);
        const auto address = request[2];
        switch (command) {
        case mdv::Command::Read:
            // The device always reports its original temperature and unlocked
            // state, so neither C3 nor CC can ever be confirmed.
            return Success(MakeResponse(address, mdv::Command::Read));
        case mdv::Command::Set:
            return Success(MakeResponse(address, mdv::Command::Set));
        case mdv::Command::Lock:
            return Success(MakeResponse(address, mdv::Command::Lock));
        case mdv::Command::Unlock:
            return Success(MakeResponse(address, mdv::Command::Unlock));
        }
        return mdv::TransactionResult{
            .status = mdv::TransactionStatus::IoError,
            .response = std::nullopt,
            .error = "unexpected command",
        };
    }

    std::vector<RequestFrame> requests;
};

std::size_t CountRequests(
    const std::vector<RequestFrame>& requests,
    mdv::Command command,
    std::uint8_t address)
{
    return static_cast<std::size_t>(std::count_if(
        requests.begin(), requests.end(),
        [command, address](const RequestFrame& request) {
            return request[1] == static_cast<std::uint8_t>(command) &&
                request[2] == address;
        }));
}

bool HasPollForAddress(
    const std::vector<mdv::DriverResult>& results,
    std::uint8_t address)
{
    return std::any_of(
        results.begin(), results.end(),
        [address](const mdv::DriverResult& result) {
            return result.operation == mdv::DriverOperation::PollRead &&
                result.address == address;
        });
}

std::size_t LongestPriorityBurst(const std::vector<mdv::DriverResult>& results)
{
    std::size_t longest = 0;
    std::size_t current = 0;
    for (const auto& result : results) {
        if (result.operation == mdv::DriverOperation::PollRead) {
            current = 0;
            continue;
        }
        ++current;
        longest = std::max(longest, current);
    }
    return longest;
}

bool TestSetRetryLimitAndFairPolling()
{
    IgnoringDeviceTransport transport;
    mdv::MdvDriver driver({1, 2}, transport);

    static_cast<void>(driver.ProcessNext());
    static_cast<void>(driver.ProcessNext());
    driver.SetTemperature(1, 24);

    std::vector<mdv::DriverResult> results;
    for (int index = 0; index < 16; ++index) {
        results.push_back(driver.ProcessNext());
    }

    const auto& runtime = driver.DeviceByAddress(1);
    return Check(
               CountRequests(transport.requests, mdv::Command::Set, 1) ==
                   mdv::kMaxSetCommandAttempts,
               "C3 retries stop at the configured attempt limit") &&
        Check(runtime.setAttempts == mdv::kMaxSetCommandAttempts,
              "runtime records all C3 attempts") &&
        Check(runtime.setRetryExhausted,
              "runtime exposes exhausted C3 confirmation") &&
        Check(runtime.device.HasPendingField(mdv::PendingField::SetTemperature),
              "unconfirmed desired value remains pending") &&
        Check(HasPollForAddress(results, 2),
              "another address continues to receive ordinary polls") &&
        Check(LongestPriorityBurst(results) <=
                  mdv::kMaxPriorityOperationsBeforePoll,
              "priority work is interrupted by bounded round-robin polling") &&
        Check(!driver.HasQueuedWork(),
              "exhausted C3 retry no longer occupies the service queues");
}

bool TestBlockRetryLimitAndFairPolling()
{
    IgnoringDeviceTransport transport;
    mdv::MdvDriver driver({3, 4}, transport);

    static_cast<void>(driver.ProcessNext());
    static_cast<void>(driver.ProcessNext());
    driver.SetBlocked(3, true);

    std::vector<mdv::DriverResult> results;
    for (int index = 0; index < 16; ++index) {
        results.push_back(driver.ProcessNext());
    }

    const auto& runtime = driver.DeviceByAddress(3);
    return Check(
               CountRequests(transport.requests, mdv::Command::Lock, 3) ==
                   mdv::kMaxBlockCommandAttempts,
               "CC retries stop at the configured attempt limit") &&
        Check(runtime.blockAttempts == mdv::kMaxBlockCommandAttempts,
              "runtime records all CC attempts") &&
        Check(runtime.blockPending,
              "unconfirmed block desire remains pending") &&
        Check(runtime.blockRetryExhausted,
              "runtime exposes exhausted block confirmation") &&
        Check(HasPollForAddress(results, 4),
              "another address continues to receive polls during CC retries") &&
        Check(LongestPriorityBurst(results) <=
                  mdv::kMaxPriorityOperationsBeforePoll,
              "block retries also obey the bounded priority burst") &&
        Check(!driver.HasQueuedWork(),
              "exhausted block retry no longer occupies the service queues");
}

bool TestNewCommandResetsRetryBudget()
{
    IgnoringDeviceTransport transport;
    mdv::MdvDriver driver({5}, transport);

    static_cast<void>(driver.ProcessNext());
    driver.SetTemperature(5, 24);
    for (int index = 0; index < 12; ++index) {
        static_cast<void>(driver.ProcessNext());
    }

    const auto beforeRevision =
        driver.DeviceByAddress(5).device.DesiredRevision();
    driver.SetTemperature(5, 25);
    const auto& reset = driver.DeviceByAddress(5);
    const bool resetState = reset.device.DesiredRevision() > beforeRevision &&
        reset.setAttempts == 0 && !reset.setRetryExhausted &&
        driver.HasQueuedWork();

    static_cast<void>(driver.ProcessNext());
    return Check(resetState, "a new desired revision resets the retry budget") &&
        Check(driver.DeviceByAddress(5).setAttempts == 1,
              "new desired revision starts a fresh first attempt");
}

} // namespace

int main()
{
    const bool ok = TestSetRetryLimitAndFairPolling() &&
        TestBlockRetryLimitAndFairPolling() &&
        TestNewCommandResetsRetryBudget();
    if (!ok) {
        return 1;
    }
    std::cout << "MDVWB driver fairness and retry test: OK\n";
    return 0;
}
