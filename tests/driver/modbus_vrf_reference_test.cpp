#include "json.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef MDVWB_SOURCE_DIR
#error MDVWB_SOURCE_DIR must point to the repository source directory
#endif

namespace {

using mdvwb::json::Array;
using mdvwb::json::Object;
using mdvwb::json::Value;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

const Object& RequireObject(const Value& value, std::string_view path)
{
    if (!value.IsObject()) {
        throw std::runtime_error(std::string(path) + " must be an object");
    }
    return value.AsObject();
}

const Array& RequireArray(const Value& value, std::string_view path)
{
    if (!value.IsArray()) {
        throw std::runtime_error(std::string(path) + " must be an array");
    }
    return value.AsArray();
}

const Value& Field(
    const Object& object,
    std::string_view name,
    std::string_view path)
{
    const auto iterator = object.find(name);
    if (iterator == object.end()) {
        throw std::runtime_error(
            std::string(path) + " is missing field '" +
            std::string(name) + "'");
    }
    return iterator->second;
}

std::string_view StringField(
    const Object& object,
    std::string_view name,
    std::string_view path)
{
    const auto& value = Field(object, name, path);
    if (!value.IsString()) {
        throw std::runtime_error(
            std::string(path) + "." + std::string(name) +
            " must be a string");
    }
    return value.AsString();
}

std::int64_t IntegerField(
    const Object& object,
    std::string_view name,
    std::string_view path)
{
    const auto& value = Field(object, name, path);
    if (!value.IsInteger()) {
        throw std::runtime_error(
            std::string(path) + "." + std::string(name) +
            " must be an integer");
    }
    return value.AsInteger();
}

bool BooleanField(
    const Object& object,
    std::string_view name,
    std::string_view path)
{
    const auto& value = Field(object, name, path);
    if (!value.IsBoolean()) {
        throw std::runtime_error(
            std::string(path) + "." + std::string(name) +
            " must be a boolean");
    }
    return value.AsBoolean();
}

bool ContainsKeyRecursively(
    const Value& value,
    std::string_view forbidden)
{
    if (value.IsObject()) {
        for (const auto& [key, child] : value.AsObject()) {
            if (key == forbidden || ContainsKeyRecursively(child, forbidden)) {
                return true;
            }
        }
    }
    else if (value.IsArray()) {
        for (const auto& child : value.AsArray()) {
            if (ContainsKeyRecursively(child, forbidden)) {
                return true;
            }
        }
    }
    return false;
}

void RequireStringArrayContains(
    const Array& values,
    std::string_view expected,
    std::string_view path)
{
    for (const auto& value : values) {
        if (value.IsString() && value.AsString() == expected) {
            return;
        }
    }
    throw std::runtime_error(
        std::string(path) + " does not contain '" +
        std::string(expected) + "'");
}

void TestReferenceFixture()
{
    const auto path =
        std::filesystem::path(MDVWB_SOURCE_DIR) /
        "tests/fixtures/modbus/vrf_add_controller_source.json";

    const auto rootValue = mdvwb::json::ParseFile(path);
    const auto& root = RequireObject(rootValue, "root");

    Require(IntegerField(root, "fixtureVersion", "root") == 1,
            "unexpected fixture version");
    Require(StringField(root, "kind", "root") ==
                "manufacturer_source_reference",
            "fixture kind mismatch");
    Require(!BooleanField(root, "productionProfile", "root"),
            "reference fixture must never masquerade as a production profile");

    const auto& wireAddressing = RequireObject(
        Field(root, "wireAddressing", "root"),
        "root.wireAddressing");
    Require(StringField(
                wireAddressing,
                "status",
                "root.wireAddressing") == "verified_live_wirenboard",
            "wire address status does not record live verification");
    Require(BooleanField(
                wireAddressing,
                "pduAddressingResolved",
                "root.wireAddressing"),
            "PDU addressing must be resolved after live WirenBoard verification");
    Require(StringField(
                wireAddressing,
                "convention",
                "root.wireAddressing") == "literal_source_address",
            "wire-address convention mismatch");

    const auto& liveVerification = RequireObject(
        Field(root, "liveVerification", "root"),
        "root.liveVerification");
    const auto& liveAddressing = RequireObject(
        Field(liveVerification, "addressing", "root.liveVerification"),
        "root.liveVerification.addressing");
    Require(StringField(
                liveAddressing,
                "client",
                "root.liveVerification.addressing") == "WirenBoard",
            "live addressing client mismatch");
    Require(IntegerField(
                liveAddressing,
                "verifiedBaseRegister",
                "root.liveVerification.addressing") == 40028,
            "live addressing base register mismatch");

    const auto& scanPresence = RequireObject(
        Field(liveVerification, "scanPresence", "root.liveVerification"),
        "root.liveVerification.scanPresence");
    Require(StringField(
                scanPresence,
                "sourceReference",
                "root.liveVerification.scanPresence") == "40039+(91*Y)",
            "scan presence source reference mismatch");
    Require(IntegerField(
                scanPresence,
                "absentRawValue",
                "root.liveVerification.scanPresence") == 0,
            "scan absent raw value mismatch");
    Require(StringField(
                scanPresence,
                "presenceRule",
                "root.liveVerification.scanPresence") == "any_nonzero",
            "scan presence rule mismatch");

    const auto& transport = RequireObject(
        Field(root, "transport", "root"),
        "root.transport");
    Require(IntegerField(transport, "baudRate", "root.transport") == 9600,
            "baud rate mismatch");
    Require(IntegerField(transport, "dataBits", "root.transport") == 8,
            "data bits mismatch");
    Require(StringField(transport, "parity", "root.transport") == "none",
            "parity mismatch");
    Require(IntegerField(transport, "stopBits", "root.transport") == 1,
            "stop bits mismatch");
    Require(IntegerField(transport, "defaultSlaveId", "root.transport") == 1,
            "default slave ID mismatch");

    const auto& functions = RequireArray(
        Field(transport, "functionCodes", "root.transport"),
        "root.transport.functionCodes");
    Require(functions.size() == 2U, "unexpected function-code count");
    Require(functions[0].IsInteger() && functions[0].AsInteger() == 3,
            "FC03 source fact missing");
    Require(functions[1].IsInteger() && functions[1].AsInteger() == 16,
            "FC10 source fact missing");

    const auto& index = RequireObject(
        Field(root, "indoorUnitIndex", "root"),
        "root.indoorUnitIndex");
    Require(IntegerField(index, "minimum", "root.indoorUnitIndex") == 0,
            "manufacturer Y minimum mismatch");
    Require(IntegerField(index, "maximum", "root.indoorUnitIndex") == 159,
            "manufacturer Y maximum mismatch");
    Require(IntegerField(index, "registerStride", "root.indoorUnitIndex") == 91,
            "manufacturer register stride mismatch");

    const auto& points = RequireObject(
        Field(root, "firstProfilePoints", "root"),
        "root.firstProfilePoints");

    const auto& power = RequireObject(
        Field(points, "power", "root.firstProfilePoints"),
        "root.firstProfilePoints.power");
    const auto& powerRead = RequireObject(
        Field(power, "read", "root.firstProfilePoints.power"),
        "root.firstProfilePoints.power.read");
    const auto& powerWrite = RequireObject(
        Field(power, "write", "root.firstProfilePoints.power"),
        "root.firstProfilePoints.power.write");
    Require(StringField(
                powerRead,
                "sourceReference",
                "root.firstProfilePoints.power.read") == "40028+(91*Y)",
            "power status source reference mismatch");
    Require(StringField(
                powerWrite,
                "sourceReference",
                "root.firstProfilePoints.power.write") == "40078+(91*Y)",
            "power control source reference mismatch");

    const auto& mode = RequireObject(
        Field(points, "mode", "root.firstProfilePoints"),
        "root.firstProfilePoints.mode");
    Require(StringField(
                mode,
                "readSourceReference",
                "root.firstProfilePoints.mode") == "40029+(91*Y)",
            "mode status source reference mismatch");
    Require(StringField(
                mode,
                "writeSourceReference",
                "root.firstProfilePoints.mode") == "40079+(91*Y)",
            "mode control source reference mismatch");

    const auto& modeBits = RequireObject(
        Field(mode, "bitPositions", "root.firstProfilePoints.mode"),
        "root.firstProfilePoints.mode.bitPositions");
    Require(IntegerField(
                modeBits,
                "auto",
                "root.firstProfilePoints.mode.bitPositions") == 0,
            "Auto mode bit mismatch");
    Require(IntegerField(
                modeBits,
                "heat",
                "root.firstProfilePoints.mode.bitPositions") == 4,
            "Heat mode bit mismatch");

    const auto& setTemperature = RequireObject(
        Field(points, "setTemperature", "root.firstProfilePoints"),
        "root.firstProfilePoints.setTemperature");
    Require(StringField(
                setTemperature,
                "integerReadSourceReference",
                "root.firstProfilePoints.setTemperature") == "40031+(91*Y)",
            "set-temperature integer status reference mismatch");
    Require(StringField(
                setTemperature,
                "halfDegreeReadSourceReference",
                "root.firstProfilePoints.setTemperature") == "40037+(91*Y)",
            "set-temperature half-degree status reference mismatch");
    Require(StringField(
                setTemperature,
                "integerWriteSourceReference",
                "root.firstProfilePoints.setTemperature") == "40081+(91*Y)",
            "set-temperature integer control reference mismatch");
    Require(StringField(
                setTemperature,
                "halfDegreeWriteSourceReference",
                "root.firstProfilePoints.setTemperature") == "40085+(91*Y)",
            "set-temperature half-degree control reference mismatch");

    const auto& deferred = RequireArray(
        Field(root, "deferredItems", "root"),
        "root.deferredItems");
    RequireStringArrayContains(
        deferred,
        "logical_identity_stability_after_topology_change",
        "root.deferredItems");
    RequireStringArrayContains(
        deferred,
        "set_temperature_composite_behavior",
        "root.deferredItems");
    RequireStringArrayContains(
        deferred,
        "room_temperature_scale_offset_signedness",
        "root.deferredItems");

    // This file remains a source/evidence fixture rather than an executable
    // profile. The resolved wire convention is promoted only in the dedicated
    // production profile under profiles/modbus.
    Require(!ContainsKeyRecursively(rootValue, "address"),
            "reference fixture must not contain executable address fields");
    Require(!ContainsKeyRecursively(rootValue, "pduAddress"),
            "reference fixture must not contain PDU address fields");
    Require(!ContainsKeyRecursively(rootValue, "registerAddressing"),
            "reference fixture must not select a register-addressing convention");
}

} // namespace

int main()
{
    try {
        TestReferenceFixture();
        std::cout << "MDVWB VRF source reference tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB VRF source reference tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
