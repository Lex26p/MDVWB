#include "modbus_driver.h"
#include "modbus_runtime_profile.h"
#include "modbus_semantic.h"
#include "modbus_value.h"
#include "modbus_write_block.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace mdv::modbus;

namespace {
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
template<class F> void Reject(F action)
{
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid operation was accepted");
}
ModbusProfile Profile()
{
    return LoadProfileFile(std::filesystem::path(MDVWB_SOURCE_DIR) / "profiles/modbus/gw3_mod.json");
}
std::uint16_t Word(const RtuAdu& bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>((bytes.at(offset) << 8U) | bytes.at(offset + 1));
}
void Crc(RtuAdu& bytes)
{
    const auto crc = CalculateCrc(bytes);
    bytes.push_back(static_cast<std::uint8_t>(crc));
    bytes.push_back(static_cast<std::uint8_t>(crc >> 8));
}
struct Gateway final : ITransactionTransport {
    bool online = true;
    bool power = true;
    std::vector<std::uint16_t> snapshot{2, 2, 235, 260, 200, 245, 0, 14, 1};
    std::vector<RtuAdu> requests;
    std::vector<std::uint16_t> lastWrite;
    TransactionResult Execute(const RtuAdu& request) override
    {
        Require(HasValidCrc(request) && request[0] == 1, "invalid request CRC/slave");
        requests.push_back(request);
        RtuAdu reply{1, request[1]};
        const auto address = Word(request, 2);
        const auto count = Word(request, 4);
        if (request[1] == 2) {
            Require(count == 1, "unexpected discrete read size");
            reply.push_back(1);
            reply.push_back(static_cast<std::uint8_t>(address % 8 == 2 ? online : power));
        }
        else if (request[1] == 4) {
            reply.push_back(static_cast<std::uint8_t>(count * 2));
            for (unsigned int i = 0; i < count; ++i) {
                const auto value = snapshot.at(address % 32 + i);
                reply.push_back(static_cast<std::uint8_t>(value >> 8));
                reply.push_back(static_cast<std::uint8_t>(value));
            }
        }
        else {
            Require(request[1] == 16 && count == 7 && address % 25 == 1, "wrong grouped write");
            lastWrite.clear();
            for (unsigned int i = 0; i < count; ++i) lastWrite.push_back(Word(request, 7 + i * 2));
            reply.insert(reply.end(), request.begin() + 2, request.begin() + 6);
        }
        Crc(reply);
        auto parsed = ParseResponse(reply, 1, static_cast<Function>(request[1]));
        Require(parsed.status == ResponseStatus::Success, "fake response parse failed");
        TransactionResult result;
        result.status = TransactionStatus::Success;
        result.response = std::move(parsed);
        return result;
    }
};

void TestFactsAndValidation()
{
    auto p = Profile();
    ValidateModbusRuntimeProfile(p);
    Require(p.transport.baudRate == 9600 && p.transport.dataBits == 8 &&
        p.transport.parity == mdv::SerialParity::None && p.transport.stopBits == 1, "wrong transport");
    const auto plan = BuildScanPlan(p);
    Require(plan.size() == 64 && plan.front().logicalAddress == 0 && plan.back().logicalAddress == 63, "scan loses address zero");
    Require(plan[12].probe->address == 98 && plan[12].probe->slaveId == 1, "wrong Online probe");
    Require(ResolveRegisterLocation(p, 12, *p.points.at("mode").read)->address == 384, "wrong read stride");
    Require(ResolveRegisterLocation(p, 12, *p.points.at("setTemperature").write)->address == 303, "wrong write stride");
    Require(std::get<double>(DecodePointValue(p.points.at("setTemperature"), 150)) == 15.0, "read was clamped");
    Require(EncodePointValue(p.points.at("setTemperature"), 23.0) == 23, "write scale mismatch");
    Reject([&] { static_cast<void>(EncodePointValue(p.points.at("setTemperature"), 15.0)); });
    Reject([&] { static_cast<void>(EncodePointValue(p.points.at("setTemperature"), 23.5)); });
    Require(EncodeSemanticWrite(p, mdv::DriverControl::FanSpeed, mdv::HvacFanSpeed::Medium).rawValue == 4, "medium != 4");
    Require(EncodeSemanticWrite(p, mdv::DriverControl::FanSpeed, mdv::HvacFanSpeed::High).rawValue == 7, "high != 7");
    mdv::DriverDeviceState state;
    ApplySemanticRead(state, p, "mode", 0);
    ApplySemanticRead(state, p, "fanSpeed", 0);
    Require(!state.mode && !state.fanSpeed, "unknown off values invented facts");
    p.writeBlock->snapshot.address = 65535;
    Reject([&] { ValidateModbusRuntimeProfile(p); });
    p = Profile();
    p.writeBlock->fields[0].sourceOffset = 100;
    Reject([&] { ValidateModbusRuntimeProfile(p); });
    p = Profile();
    p.addressing = DirectSlaveAddressing{};
    Reject([&] { ValidateModbusRuntimeProfile(p); });

    const auto request = BuildReadRequest(Function::ReadInputRegisters, 1, 32, 3);
    Require(request == RtuAdu({1,4,0,32,0,3,0xB1,0xC1}), "FC04 differs from manual example");
    RtuAdu response{1,4,6,0,2,0,4,0,200}; Crc(response);
    ResponseCollector collector;
    std::optional<RtuAdu> frame;
    for (auto byte : response) frame = collector.Push(byte);
    Require(frame && ParseResponse(*frame,1,Function::ReadInputRegisters).registers.size() == 3, "FC04 collection failed");
    Require(ParseResponse(response,1,Function::ReadHoldingRegisters).status == ResponseStatus::Invalid, "wrong function accepted");
    response.back() ^= 1;
    Require(ParseResponse(response,1,Function::ReadInputRegisters).status == ResponseStatus::Invalid, "bad CRC accepted");
    RtuAdu exception{1,0x84,2}; Crc(exception);
    Require(ParseResponse(exception,1,Function::ReadInputRegisters).status == ResponseStatus::Exception, "exception lost");

    RtuAdu bits{1,2,2,0x85,0x01}; Crc(bits);
    const auto bitResponse = ParseResponse(bits,1,Function::ReadDiscreteInputs);
    Require(bitResponse.status == ResponseStatus::Success && bitResponse.registers.size() == 16 &&
        bitResponse.registers[0] == 1 && bitResponse.registers[1] == 0 &&
        bitResponse.registers[2] == 1 && bitResponse.registers[7] == 1 &&
        bitResponse.registers[8] == 1, "FC02 bit order wrong");
    RtuAdu badBits{1,2,2,0x85}; Crc(badBits);
    Require(ParseResponse(badBits,1,Function::ReadDiscreteInputs).status == ResponseStatus::Invalid,
        "FC02 byte-count mismatch accepted");
    Reject([] { static_cast<void>(BuildReadRequest(Function::ReadDiscreteInputs, 0, 0, 1)); });
    Reject([] { static_cast<void>(BuildReadRequest(Function::ReadInputRegisters, 1, 65535, 2)); });
}

void TestDriver()
{
    const auto p = Profile();
    Gateway io;
    ModbusDriver driver({0,12}, p, io);
    Require(driver.ProcessNext().outcome == mdv::DriverOutcome::Success, "poll 0 failed");
    Require(driver.ProcessNext().outcome == mdv::DriverOutcome::Success, "poll 12 failed");
    Require(driver.DeviceStateByAddress(0).roomTemperature == 24.5, "room temperature wrong");
    driver.ApplyCommand({.address=0, .control=mdv::DriverControl::SetTemperature, .value=24.0});
    driver.ApplyCommand({.address=0, .control=mdv::DriverControl::FanSpeed, .value=mdv::HvacFanSpeed::High});
    Require(driver.ProcessNext().outcome == mdv::DriverOutcome::Success, "write failed");
    Require(io.lastWrite == std::vector<std::uint16_t>({66,7,24,26,20,14,1}), "neighbor settings not preserved");
    Require(driver.DeviceStateByAddress(0).setTemperature == 23.5, "write ACK invented factual state");
    const auto writes = [&] { std::size_t n=0; for (const auto& r:io.requests) n += r[1] == 16; return n; };
    for (int i=0;i<8;++i) static_cast<void>(driver.ProcessNext());
    Require(writes() == 1, "stale gateway state caused repeated writes");
    Require(io.requests.back()[1] != 16, "poll fairness lost");
    io.snapshot[2] = 240; io.snapshot[1] = 7;
    for (int i=0;i<2;++i) static_cast<void>(driver.ProcessNext());
    Require(driver.DeviceStateByAddress(0).setTemperature == 24.0, "factual confirmation failed");
    Require(!driver.HasQueuedWork(), "confirmed commands remain pending");
    io.online = false;
    for (int i=0;i<6;++i) static_cast<void>(driver.ProcessNext());
    Require(!driver.DeviceStateByAddress(0).online, "gateway ACK mistaken for IDU Online");
    Reject([&] { driver.ApplyCommand({.address=0,.control=mdv::DriverControl::Power,.value=false}); });
    io.online=true;
    static_cast<void>(driver.ProcessNext());
    Require(driver.DeviceStateByAddress(0).online, "online recovery failed");
}

void TestPreservationAndTimeout()
{
    auto p = Profile();
    Gateway io;
    const auto preserved = EncodeWriteBlockSnapshot(*p.writeBlock, io.snapshot, {});
    Require(preserved[1] == 2 && preserved[2] == 151, "native speed/half degree was rounded");
    io.snapshot[7] = 0;
    Reject([&] { static_cast<void>(EncodeWriteBlockSnapshot(*p.writeBlock, io.snapshot, {})); });
    ModbusDriver refused({0},p,io);
    static_cast<void>(refused.ProcessNext());
    refused.ApplyCommand({.address=0,.control=mdv::DriverControl::SetTemperature,.value=24.0});
    Require(refused.ProcessNext().outcome != mdv::DriverOutcome::Success && io.lastWrite.empty(), "unknown neighbor sent to device");
    io.snapshot[7] = 14;
    p.confirmationTimeoutMs = 1;
    ModbusDriver timed({0},p,io);
    static_cast<void>(timed.ProcessNext());
    timed.ApplyCommand({.address=0,.control=mdv::DriverControl::Power,.value=false});
    static_cast<void>(timed.ProcessNext());
    io.online = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Require(timed.ProcessNext().outcome == mdv::DriverOutcome::Timeout && !timed.HasQueuedWork(), "pending timeout not bounded");
}

void TestPowerAndMode()
{
    const auto p = Profile();
    Gateway io;
    io.power = false; // Gateway may retain the last active Mode while off.
    ModbusDriver driver({0}, p, io);
    static_cast<void>(driver.ProcessNext());
    driver.ApplyCommand({.address=0,.control=mdv::DriverControl::SetTemperature,.value=24.0});
    Require(driver.ProcessNext().outcome == mdv::DriverOutcome::Success, "off temperature write failed");
    Require(io.lastWrite[0] == 159, "temperature command switched the unit on");
    driver.ApplyCommand({.address=0,.control=mdv::DriverControl::Mode,.value=mdv::HvacMode::Heat});
    driver.ApplyCommand({.address=0,.control=mdv::DriverControl::Power,.value=true});
    Require(driver.ProcessNext().outcome == mdv::DriverOutcome::Success, "mode/on write failed");
    Require(io.lastWrite[0] == 67, "Power=on discarded pending heat mode");
    driver.ApplyCommand({.address=0,.control=mdv::DriverControl::Power,.value=false});
    Require(driver.ProcessNext().outcome == mdv::DriverOutcome::Success, "reverse power write failed");
    Require(io.lastWrite[0] == 159, "stale off state suppressed reversing a sent on command");
}
}

int main()
{
    try { TestFactsAndValidation(); TestDriver(); TestPreservationAndTimeout(); TestPowerAndMode(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "GW3-MOD tests passed\n";
}
