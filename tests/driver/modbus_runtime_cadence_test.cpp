#include "modbus_runtime_cadence.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

mdv::DriverResult Result(
    mdv::DriverOperation operation,
    mdv::DriverOutcome outcome)
{
    return mdv::DriverResult{
        .address = 1U,
        .operation = operation,
        .outcome = outcome,
        .error = {},
    };
}

} // namespace

int main()
{
    try {
        const mdv::modbus::ModbusRuntimeCadence cadence{
            .pollPeriod = std::chrono::milliseconds{175},
            .commandPeriod = std::chrono::milliseconds{25},
            .retryPeriod = std::chrono::milliseconds{650},
        };

        Require(
            mdv::modbus::ModbusOperationPeriod(
                Result(mdv::DriverOperation::PollRead,
                       mdv::DriverOutcome::Success),
                cadence) == std::chrono::milliseconds{175},
            "successful poll used the wrong period");
        Require(
            mdv::modbus::ModbusOperationPeriod(
                Result(mdv::DriverOperation::SetState,
                       mdv::DriverOutcome::Success),
                cadence) == std::chrono::milliseconds{25},
            "successful write used the wrong period");
        Require(
            mdv::modbus::ModbusOperationPeriod(
                Result(mdv::DriverOperation::ConfirmRead,
                       mdv::DriverOutcome::Success),
                cadence) == std::chrono::milliseconds{25},
            "successful confirmation used the wrong period");

        for (const auto outcome : {
                 mdv::DriverOutcome::Timeout,
                 mdv::DriverOutcome::IoError,
                 mdv::DriverOutcome::InvalidResponse}) {
            Require(
                mdv::modbus::ModbusOperationPeriod(
                    Result(mdv::DriverOperation::PollRead, outcome),
                    cadence) == std::chrono::milliseconds{650},
                "failed operation did not use retry backoff");
        }

        std::cout << "MDVWB Modbus runtime cadence tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus runtime cadence tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
