#include "modbus_profile.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {

constexpr std::string_view kValidProfile = R"json(
{
  "schemaVersion": 1,
  "id": "test_vrf_gateway",
  "name": "Test VRF Gateway",
  "registerAddressing": "pdu_zero_based",
  "transport": {
    "baudRate": 9600,
    "dataBits": 8,
    "parity": "none",
    "stopBits": 1
  },
  "addressing": {
    "type": "fixed_slave_stride",
    "logicalMin": 1,
    "logicalMax": 63,
    "slaveId": 1,
    "firstLogicalAddress": 1,
    "registerStride": 91
  },
  "capabilities": {
    "power": true,
    "mode": true,
    "fanSpeed": true,
    "setTemperature": true,
    "roomTemperature": true,
    "alarm": true
  },
  "probe": {
    "read": {
      "space": "holding_register",
      "address": 25,
      "reference": "40026"
    },
    "quantity": 2
  },
  "points": {
    "power": {
      "type": "enum",
      "read": {
        "space": "holding_register",
        "address": 27,
        "reference": "40028"
      },
      "write": {
        "space": "holding_register",
        "address": 77,
        "reference": "40078"
      },
      "readMap": {
        "0": "off",
        "1": "on"
      },
      "writeMap": {
        "off": 0,
        "on": 1
      }
    },
    "mode": {
      "type": "enum",
      "read": {
        "space": "holding_register",
        "address": 28
      },
      "write": {
        "space": "holding_register",
        "address": 78
      },
      "readMap": {
        "1": "auto",
        "2": "cool",
        "4": "dry",
        "8": "fan",
        "16": "heat"
      },
      "writeMap": {
        "auto": 1,
        "cool": 2,
        "dry": 4,
        "fan": 8,
        "heat": 16
      }
    },
    "fanSpeed": {
      "type": "enum",
      "read": {
        "space": "holding_register",
        "address": 29
      },
      "write": {
        "space": "holding_register",
        "address": 79
      },
      "readMap": {
        "1": "auto",
        "2": "high",
        "4": "medium",
        "8": "low"
      },
      "writeMap": {
        "auto": 1,
        "high": 2,
        "medium": 4,
        "low": 8
      }
    },
    "setTemperature": {
      "type": "number",
      "rawType": "int16",
      "read": {
        "space": "holding_register",
        "address": 30
      },
      "write": {
        "space": "holding_register",
        "address": 80
      },
      "transform": {
        "scale": 0.1,
        "offset": 0
      },
      "limits": {
        "min": 16.0,
        "max": 30.0,
        "step": 0.5
      },
      "writeConversion": {
        "rounding": "exact"
      }
    },
    "roomTemperature": {
      "type": "number",
      "read": {
        "space": "holding_register",
        "address": 38
      },
      "transform": {
        "scale": 0.1,
        "offset": 0
      }
    },
    "alarmCode": {
      "type": "number",
      "read": {
        "space": "holding_register",
        "address": 34
      }
    }
  }
}
)json";

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void RequireProfileError(Function&& function, std::string_view expectedText)
{
    try {
        function();
    }
    catch (const mdv::modbus::ProfileError& error) {
        if (std::string_view(error.what()).find(expectedText) ==
            std::string_view::npos) {
            throw std::runtime_error(
                "profile failed for the wrong reason: " +
                std::string(error.what()));
        }
        return;
    }

    throw std::runtime_error(
        "invalid profile was accepted: " + std::string(expectedText));
}

std::string ReplaceOnce(
    std::string text,
    std::string_view from,
    std::string_view to)
{
    const auto position = text.find(from);
    if (position == std::string::npos) {
        throw std::runtime_error("test fixture replacement text was not found");
    }
    text.replace(position, from.size(), to);
    return text;
}

void TestValidFixedStrideProfile()
{
    const auto profile = mdv::modbus::ParseProfile(kValidProfile);

    Require(profile.schemaVersion == 1, "schema version mismatch");
    Require(profile.id == "test_vrf_gateway", "profile id mismatch");
    Require(profile.transport.baudRate == 9600, "baud rate mismatch");
    Require(profile.transport.dataBits == 8, "data bits mismatch");
    Require(profile.transport.parity == mdv::SerialParity::None,
            "parity mismatch");
    Require(profile.points.size() == 6U, "point count mismatch");

    const auto* addressing =
        std::get_if<mdv::modbus::FixedSlaveStrideAddressing>(
            &profile.addressing);
    Require(addressing != nullptr, "fixed stride addressing was not selected");
    Require(addressing->slaveId == 1, "fixed slave ID mismatch");
    Require(addressing->registerStride == 91, "register stride mismatch");

    Require(profile.probe.read.address == 25, "probe address mismatch");
    Require(profile.probe.quantity == 2, "probe quantity mismatch");

    const auto& setPoint = profile.points.at("setTemperature");
    Require(setPoint.rawType == mdv::modbus::RawType::Int16,
            "number rawType was not loaded");
    Require(setPoint.transform.has_value(), "number transform was not loaded");
    Require(std::abs(setPoint.transform->scale - 0.1) < 1e-12,
            "number scale mismatch");
    Require(setPoint.limits.has_value(), "number limits were not loaded");
    Require(setPoint.limits->step == 0.5, "number step mismatch");

    const auto& power = profile.points.at("power");
    Require(power.enumMappings.read.at(0) == "off",
            "enum read mapping mismatch");
    Require(power.enumMappings.write.at("on") == 1,
            "enum write mapping mismatch");
}

void TestDirectSlaveProfile()
{
    auto text = ReplaceOnce(
        std::string(kValidProfile),
        R"json("addressing": {
    "type": "fixed_slave_stride",
    "logicalMin": 1,
    "logicalMax": 63,
    "slaveId": 1,
    "firstLogicalAddress": 1,
    "registerStride": 91
  })json",
        R"json("addressing": {
    "type": "direct_slave",
    "logicalMin": 1,
    "logicalMax": 63,
    "registerOffset": 0
  })json");

    const auto profile = mdv::modbus::ParseProfile(text);
    const auto* addressing =
        std::get_if<mdv::modbus::DirectSlaveAddressing>(&profile.addressing);
    Require(addressing != nullptr, "direct slave addressing was not selected");
    Require(addressing->logicalMax == 63, "direct logical max mismatch");
}

void TestExplicitProfile()
{
    auto text = ReplaceOnce(
        std::string(kValidProfile),
        R"json("addressing": {
    "type": "fixed_slave_stride",
    "logicalMin": 1,
    "logicalMax": 63,
    "slaveId": 1,
    "firstLogicalAddress": 1,
    "registerStride": 91
  })json",
        R"json("addressing": {
    "type": "explicit",
    "logicalMin": 1,
    "logicalMax": 3,
    "devices": {
      "1": {"slaveId": 1, "registerOffset": 0},
      "2": {"slaveId": 1, "registerOffset": 91},
      "3": {"slaveId": 7, "registerOffset": 0}
    }
  })json");

    const auto profile = mdv::modbus::ParseProfile(text);
    const auto* addressing =
        std::get_if<mdv::modbus::ExplicitAddressing>(&profile.addressing);
    Require(addressing != nullptr, "explicit addressing was not selected");
    Require(addressing->devices.at(2).registerOffset == 91,
            "explicit register offset mismatch");
    Require(addressing->devices.at(3).slaveId == 7,
            "explicit slave ID mismatch");
}

void TestSchemaAndIdentityValidation()
{
    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("schemaVersion": 1)json",
                R"json("schemaVersion": 2)json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "schemaVersion");

    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("id": "test_vrf_gateway")json",
                R"json("id": "Bad Profile")json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "root.id");

    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("registerAddressing": "pdu_zero_based")json",
                R"json("registerAddressing": "manufacturer_reference")json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "pdu_zero_based");
}

void TestTransportValidation()
{
    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("baudRate": 9600)json",
                R"json("baudRate": 12345)json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "baud rate");

    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("parity": "none")json",
                R"json("parity": "mark")json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "parity");
}

void TestAddressingValidation()
{
    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("logicalMax": 63)json",
                R"json("logicalMax": 64)json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "1..63");

    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("slaveId": 1)json",
                R"json("slaveId": 0)json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "1..247");

    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("registerStride": 91)json",
                R"json("registerStride": 2000)json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "overflows");
}

void TestProbeAndRegisterValidation()
{
    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("quantity": 2)json",
                R"json("quantity": 126)json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "quantity");

    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("space": "holding_register",
      "address": 25)json",
                R"json("space": "mystery_register",
      "address": 25)json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "holding_register");

    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("space": "holding_register",
        "address": 77)json",
                R"json("space": "input_register",
        "address": 77)json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "read-only");

    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("quantity": 2)json",
                R"json("quantity": 2,
    "write": {"space": "holding_register", "address": 1})json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "unknown field 'write'");
}

void TestNumericValidation()
{
    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("scale": 0.1)json",
                R"json("scale": 0.0)json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "scale must not be zero");

    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("min": 16.0,
        "max": 30.0)json",
                R"json("min": 31.0,
        "max": 30.0)json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "must not exceed");

    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("step": 0.5)json",
                R"json("step": 0.0)json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "step must be positive");
}

void TestEnumValidation()
{
    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("readMap": {
        "0": "off",
        "1": "on"
      })json",
                R"json("readMap": {})json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "readMap is required");

    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("writeMap": {
        "off": 0,
        "on": 1
      })json",
                R"json("writeMap": {})json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "writeMap is required");
}

void TestCapabilitiesRequirePoints()
{
    RequireProfileError(
        [] {
            auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("roomTemperature": true)json",
                R"json("roomTemperature": false)json");
            text = ReplaceOnce(
                std::move(text),
                R"json(,
    "roomTemperature": {
      "type": "number",
      "read": {
        "space": "holding_register",
        "address": 38
      },
      "transform": {
        "scale": 0.1,
        "offset": 0
      }
    })json",
                "");
            // Turning the capability back on proves the cross-field check.
            text = ReplaceOnce(
                std::move(text),
                R"json("roomTemperature": false)json",
                R"json("roomTemperature": true)json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "points.roomTemperature");
}



void TestFixedStrideAnchorValidation()
{
    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("firstLogicalAddress": 1)json",
                R"json("firstLogicalAddress": 2)json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "must equal logicalMin");
}

void TestRawTypeValidation()
{
    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("rawType": "int16")json",
                R"json("rawType": "uint32")json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "uint16, int16");

    RequireProfileError(
        [] {
            auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("power": {
      "type": "enum")json",
                R"json("power": {
      "type": "enum",
      "rawType": "int16")json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "allowed only for number");
}


void TestSemanticPointVocabularyValidation()
{
    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("16": "heat")json",
                R"json("16": "turbo")json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "unsupported semantic value 'turbo'");

    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("mode": {
      "type": "enum")json",
                R"json("mode": {
      "type": "number")json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "readMap/writeMap are allowed only for enum points");

    RequireProfileError(
        [] {
            const auto text = ReplaceOnce(
                std::string(kValidProfile),
                R"json("roomTemperature": {
      "type": "number")json",
                R"json("roomTemperature": {
      "type": "boolean")json");
            static_cast<void>(mdv::modbus::ParseProfile(text));
        },
        "transform is allowed only for number points");
}

void TestFileLoading()
{
    const auto path =
        std::filesystem::temp_directory_path() /
        "mdvwb-modbus-profile-test.json";

    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("could not create profile fixture");
        }
        output << kValidProfile;
    }

    try {
        const auto profile = mdv::modbus::LoadProfileFile(path);
        Require(profile.id == "test_vrf_gateway",
                "profile file was not loaded");
    }
    catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}


class TemporaryProfileDirectory final {
public:
    TemporaryProfileDirectory()
    {
        const auto suffix =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ =
            std::filesystem::temp_directory_path() /
            ("mdvwb-modbus-profile-catalog-" + std::to_string(suffix));

        std::error_code error;
        if (!std::filesystem::create_directories(path_, error) || error) {
            throw std::runtime_error(
                "could not create temporary profile directory");
        }
    }

    ~TemporaryProfileDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryProfileDirectory(const TemporaryProfileDirectory&) = delete;
    TemporaryProfileDirectory& operator=(const TemporaryProfileDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept
    {
        return path_;
    }

    void Write(std::string_view name, std::string_view contents) const
    {
        const auto file = path_ / std::string(name);
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error(
                "could not create profile fixture " + file.string());
        }
        output << contents;
        if (!output) {
            throw std::runtime_error(
                "could not write profile fixture " + file.string());
        }
    }

private:
    std::filesystem::path path_;
};

std::string WithProfileId(std::string_view id)
{
    return ReplaceOnce(
        std::string(kValidProfile),
        R"json("id": "test_vrf_gateway")json",
        "\"id\": \"" + std::string(id) + "\"");
}

void TestProfileDirectoryLoadsValidFilesAndIsolatesInvalidFiles()
{
    TemporaryProfileDirectory directory;
    directory.Write("10-alpha.json", WithProfileId("alpha_profile"));
    directory.Write("20-broken.json", R"json({"schemaVersion":1,)json");
    directory.Write(
        "30-schema.json",
        ReplaceOnce(
            WithProfileId("schema_profile"),
            R"json("schemaVersion": 1)json",
            R"json("schemaVersion": 9)json"));
    directory.Write("40-beta.json", WithProfileId("beta_profile"));
    directory.Write("README.txt", "not a profile");

    const auto catalog =
        mdv::modbus::LoadProfileDirectory(directory.Path());

    Require(catalog.profiles.size() == 2U,
            "valid profiles were not isolated from invalid files");
    Require(catalog.Find("alpha_profile") != nullptr,
            "first valid profile is missing from the catalog");
    Require(catalog.Find("beta_profile") != nullptr,
            "second valid profile is missing from the catalog");
    Require(catalog.Find("schema_profile") == nullptr,
            "invalid schema entered the profile catalog");
    Require(catalog.Find("missing_profile") == nullptr,
            "unknown profile lookup returned a result");
    Require(catalog.HasErrors(),
            "invalid profile files did not produce diagnostics");
    Require(catalog.issues.size() == 2U,
            "unexpected number of profile directory diagnostics");
    Require(
        catalog.issues[0].path.filename() == "20-broken.json",
        "profile diagnostics are not deterministic by file path");
    Require(
        catalog.issues[1].path.filename() == "30-schema.json",
        "schema diagnostic is not in deterministic order");
}

void TestDuplicateIdsRejectWholeDuplicateGroup()
{
    TemporaryProfileDirectory directory;
    directory.Write("10-duplicate-a.json", WithProfileId("duplicate_profile"));
    directory.Write("20-unrelated.json", WithProfileId("unrelated_profile"));
    directory.Write("30-duplicate-b.json", WithProfileId("duplicate_profile"));

    const auto catalog =
        mdv::modbus::LoadProfileDirectory(directory.Path());

    Require(catalog.profiles.size() == 1U,
            "duplicate profile IDs left an ambiguous catalog entry");
    Require(catalog.Find("duplicate_profile") == nullptr,
            "one duplicate profile won based on directory ordering");
    Require(catalog.Find("unrelated_profile") != nullptr,
            "duplicate profile IDs broke an unrelated valid profile");
    Require(catalog.issues.size() == 2U,
            "duplicate profile group did not diagnose every conflicting file");
    Require(
        catalog.issues[0].path.filename() == "10-duplicate-a.json",
        "first duplicate diagnostic path mismatch");
    Require(
        catalog.issues[1].path.filename() == "30-duplicate-b.json",
        "second duplicate diagnostic path mismatch");

    for (const auto& issue : catalog.issues) {
        Require(
            issue.error.find("duplicate Modbus profile id 'duplicate_profile'") !=
                std::string::npos,
            "duplicate profile diagnostic does not contain the conflicting id");
    }
}

void TestEmptyProfileDirectoryIsValid()
{
    TemporaryProfileDirectory directory;

    const auto catalog =
        mdv::modbus::LoadProfileDirectory(directory.Path());

    Require(catalog.profiles.empty(),
            "empty profile directory unexpectedly produced profiles");
    Require(catalog.issues.empty(),
            "empty profile directory unexpectedly produced diagnostics");
    Require(!catalog.HasErrors(),
            "empty profile directory was marked erroneous");
}

void TestMissingProfileDirectoryFailsClearly()
{
    const auto path =
        std::filesystem::temp_directory_path() /
        "mdvwb-modbus-profile-directory-that-does-not-exist";

    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);

    RequireProfileError(
        [&] {
            static_cast<void>(
                mdv::modbus::LoadProfileDirectory(path));
        },
        "does not exist");
}

} // namespace

int main()
{
    try {
        TestValidFixedStrideProfile();
        TestDirectSlaveProfile();
        TestExplicitProfile();
        TestSchemaAndIdentityValidation();
        TestTransportValidation();
        TestAddressingValidation();
        TestFixedStrideAnchorValidation();
        TestProbeAndRegisterValidation();
        TestNumericValidation();
        TestEnumValidation();
        TestCapabilitiesRequirePoints();
        TestRawTypeValidation();
        TestSemanticPointVocabularyValidation();
        TestFileLoading();
        TestProfileDirectoryLoadsValidFilesAndIsolatesInvalidFiles();
        TestDuplicateIdsRejectWholeDuplicateGroup();
        TestEmptyProfileDirectoryIsValid();
        TestMissingProfileDirectoryFailsClearly();

        std::cout << "MDVWB Modbus profile tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus profile tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
