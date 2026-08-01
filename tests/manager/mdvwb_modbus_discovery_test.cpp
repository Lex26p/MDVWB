#include "mdv_modbus_discovery.h"

#include "modbus_rtu.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef MDVWB_SOURCE_DIR
#error MDVWB_SOURCE_DIR must point to the repository source directory
#endif

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void RequireThrows(Function&& function, std::string_view text)
{
    try {
        function();
    }
    catch (const std::exception& error) {
        Require(
            std::string_view(error.what()).find(text) != std::string_view::npos,
            "exception did not contain expected text");
        return;
    }
    throw std::runtime_error("expected exception was not thrown");
}

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        static unsigned long long counter = 0;
        const auto token =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("mdvwb-modbus-discovery-" + std::to_string(token) + "-" +
             std::to_string(++counter));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& Path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void Write(const std::filesystem::path& path, std::string_view content)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) {
        throw std::runtime_error("cannot write test file");
    }
}

std::string ManagedModbusConfig(
    std::string_view port,
    const std::filesystem::path& profileDirectory)
{
    return
        "# Managed by mdvwb-manager from buses.json.\n"
        "MDVWB_PROTOCOL=\"modbus_rtu\"\n"
        "MDVWB_PORT=\"" + std::string(port) + "\"\n"
        "MDVWB_MODBUS_PROFILE=\"vrf_add_controller\"\n"
        "MDVWB_MODBUS_PROFILE_DIR=\"" +
            profileDirectory.generic_string() + "\"\n"
        "MDVWB_MODBUS_BAUD_RATE=\"9600\"\n"
        "MDVWB_MODBUS_DATA_BITS=\"8\"\n"
        "MDVWB_MODBUS_PARITY=\"none\"\n"
        "MDVWB_MODBUS_STOP_BITS=\"1\"\n"
        "MDVWB_MODBUS_RESPONSE_TIMEOUT_MS=\"200\"\n";
}

std::uint16_t RequestAddress(const mdv::modbus::RtuAdu& request)
{
    Require(request.size() == 8U, "unexpected Modbus request size");
    Require(request[1] == 0x03U, "scan used a non-read function");
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(request[2]) << 8U) |
        static_cast<std::uint16_t>(request[3]));
}

mdv::modbus::TransactionResult Success(
    const mdv::modbus::RtuAdu& request,
    std::uint16_t value)
{
    mdv::modbus::ParsedResponse response;
    response.status = mdv::modbus::ResponseStatus::Success;
    response.slaveId = request[0];
    response.function = mdv::modbus::Function::ReadHoldingRegisters;
    response.registers = {value};

    mdv::modbus::TransactionResult result;
    result.status = mdv::modbus::TransactionStatus::Success;
    result.response = std::move(response);
    return result;
}

class ScanTransport final : public mdv::modbus::ITransactionTransport {
public:
    [[nodiscard]] mdv::modbus::TransactionResult Execute(
        const mdv::modbus::RtuAdu& request) override
    {
        const std::uint16_t address = RequestAddress(request);
        requests.push_back(address);
        if (failAt.has_value() && address == *failAt) {
            mdv::modbus::TransactionResult result;
            result.status = mdv::modbus::TransactionStatus::IoError;
            result.error = "serial disconnected";
            return result;
        }

        const bool found = address == 40039U || address == 40221U;
        return Success(request, found ? 1U : 0U);
    }

    std::vector<std::uint16_t> requests;
    std::optional<std::uint16_t> failAt;
};

mdvwb::ModbusDiscoveryRuntime ProductionRuntime()
{
    mdvwb::ModbusDiscoveryRuntime runtime;
    runtime.port = "/dev/ttyRS485-2";
    runtime.profileDirectory =
        std::filesystem::path(MDVWB_SOURCE_DIR) / "profiles/modbus";
    runtime.profileId = "vrf_add_controller";
    runtime.serial = mdv::SerialSettings{
        .baudRate = 9600,
        .dataBits = 8,
        .parity = mdv::SerialParity::None,
        .stopBits = 1,
    };
    runtime.responseTimeout = std::chrono::milliseconds(200);
    return runtime;
}

void TestRuntimeSelection()
{
    TemporaryDirectory temporary;
    const auto profileDirectory =
        std::filesystem::path(MDVWB_SOURCE_DIR) / "profiles/modbus";

    Write(
        temporary.Path() / "mdvwb-1",
        "# Managed by mdvwb-manager from buses.json.\n"
        "MDVWB_PORT=\"/dev/ttyRS485-1\"\n"
        "MDVWB_PROTOCOL=\"mdv\"\n");
    Write(
        temporary.Path() / "mdvwb-2",
        ManagedModbusConfig("/dev/ttyRS485-2", profileDirectory));
    Write(
        temporary.Path() / "unrelated",
        "MDVWB_PORT=\"/dev/ttyRS485-2\"\n");

    const auto mdv = mdvwb::FindDiscoveryRuntimeForPort(
        temporary.Path(), "/dev/ttyRS485-1");
    Require(mdv.has_value(), "managed MDV runtime was not found");
    Require(
        mdv->protocol == mdvwb::DiscoveryRuntimeProtocol::Mdv,
        "managed MDV runtime changed protocol");
    Require(!mdv->modbus.has_value(), "MDV runtime acquired Modbus settings");

    const auto modbus = mdvwb::FindDiscoveryRuntimeForPort(
        temporary.Path(), "/dev/ttyRS485-2");
    Require(modbus.has_value(), "managed Modbus runtime was not found");
    Require(
        modbus->protocol == mdvwb::DiscoveryRuntimeProtocol::ModbusRtu,
        "managed Modbus runtime changed protocol");
    Require(modbus->modbus.has_value(), "Modbus runtime omitted settings");
    Require(
        modbus->modbus->profileId == "vrf_add_controller",
        "Modbus runtime profile mismatch");
    Require(
        modbus->modbus->serial.baudRate == 9600U &&
        modbus->modbus->serial.dataBits == 8U &&
        modbus->modbus->serial.stopBits == 1U,
        "Modbus runtime serial settings mismatch");

    const auto missing = mdvwb::FindDiscoveryRuntimeForPort(
        temporary.Path(), "/dev/ttyRS485-9");
    Require(!missing.has_value(), "unknown port was matched");
}

void TestDuplicateManagedPortRejected()
{
    TemporaryDirectory temporary;
    const auto profileDirectory =
        std::filesystem::path(MDVWB_SOURCE_DIR) / "profiles/modbus";
    const std::string content =
        ManagedModbusConfig("/dev/ttyRS485-2", profileDirectory);
    Write(temporary.Path() / "mdvwb-2", content);
    Write(temporary.Path() / "mdvwb-3", content);

    RequireThrows(
        [&] {
            static_cast<void>(mdvwb::FindDiscoveryRuntimeForPort(
                temporary.Path(), "/dev/ttyRS485-2"));
        },
        "multiple managed bus configurations");
}

void TestProfileDrivenScan()
{
    ScanTransport transport;
    const auto result = mdvwb::ExecuteModbusDiscovery(
        ProductionRuntime(), transport);

    Require(result.success, "valid Modbus discovery failed");
    Require(
        result.addresses == std::vector<int>({1, 3}),
        "found logical addresses mismatch");
    Require(
        transport.requests.size() == 63U,
        "Modbus discovery did not evaluate logical addresses 1..63");
    Require(
        transport.requests.front() == 40039U,
        "logical address 1 probe mismatch");
    Require(
        transport.requests[1] == 40130U,
        "logical address 2 did not use profile stride");
    Require(
        result.output.find("FOUND_ADDRESSES=1,3") != std::string::npos,
        "machine-readable discovery result is missing");
}

void TestTransportErrorRejectsPartialResult()
{
    ScanTransport transport;
    transport.failAt = 40130U;
    const auto result = mdvwb::ExecuteModbusDiscovery(
        ProductionRuntime(), transport);

    Require(!result.success, "I/O error produced successful discovery");
    Require(result.addresses.empty(), "I/O error returned partial addresses");
    Require(
        result.message.find("logical address 2") != std::string::npos,
        "I/O error omitted logical address context");
    Require(
        transport.requests.size() == 2U,
        "scan continued after a factual transport error");
}

void TestSerialMismatchRejectedBeforeTraffic()
{
    ScanTransport transport;
    auto runtime = ProductionRuntime();
    runtime.serial.baudRate = 19200;

    RequireThrows(
        [&] {
            static_cast<void>(
                mdvwb::ExecuteModbusDiscovery(runtime, transport));
        },
        "do not match profile");
    Require(
        transport.requests.empty(),
        "serial mismatch generated Modbus traffic");
}

} // namespace

int main()
{
    try {
        TestRuntimeSelection();
        TestDuplicateManagedPortRejected();
        TestProfileDrivenScan();
        TestTransportErrorRejectsPartialResult();
        TestSerialMismatchRejectedBeforeTraffic();

        std::cout << "MDVWB Modbus discovery tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus discovery tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
