#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace mdv {

constexpr std::size_t kRequestSize = 16;
constexpr std::size_t kResponseSize = 32;
constexpr std::uint8_t kFrameStart = 0xAA;
constexpr std::uint8_t kFrameEnd = 0x55;
constexpr std::uint8_t kTransportPadding = 0xFE;
constexpr std::uint8_t kMaxDeviceAddress = 0x3F;

using RequestFrame = std::array<std::uint8_t, kRequestSize>;
using ResponseFrame = std::array<std::uint8_t, kResponseSize>;

enum class Command : std::uint8_t {
    Read = 0xC0,
    Set = 0xC3,
    Lock = 0xCC,
    Unlock = 0xCD,
};

enum class Mode : std::uint8_t {
    Fan = 0x01,
    Dry = 0x02,
    Heat = 0x04,
    Cool = 0x08,
    Auto = 0x10,
};

enum class FanSpeed : std::uint8_t {
    High = 0x01,
    Medium = 0x02,
    Low = 0x04,
    Auto = 0x80,
};

struct SetState {
    bool power = false;
    Mode mode = Mode::Auto;
    FanSpeed fanSpeed = FanSpeed::Auto;
    std::uint8_t setTemperature = 21;

    // Only bits 0..3 are known. Blinds are bit 2.
    std::uint8_t additionalFunctions = 0;
};

struct DeviceState {
    Command command = Command::Read;
    std::uint8_t address = 0;
    std::uint8_t masterId = 0;

    bool power = false;
    bool modeLocked = false;
    std::optional<Mode> mode;
    std::optional<Mode> activeMode;
    std::optional<FanSpeed> fanSpeed;
    std::optional<FanSpeed> activeFanSpeed;

    std::uint8_t setTemperature = 0;
    std::optional<double> roomTemperature;

    std::uint8_t additionalFunctions = 0;
    bool blinds = false;

    std::uint8_t statusBits = 0;
    std::uint8_t errorsE0E7 = 0;
    std::uint8_t errorsE8EF = 0;
    std::uint8_t protectionsP0P7 = 0;
    std::uint8_t protectionsP8PF = 0;
    std::uint8_t communicationErrors = 0;
};

struct ParseResult {
    bool ok = false;
    DeviceState state{};
    std::string error;
};

[[nodiscard]] bool IsKnownCommand(std::uint8_t value) noexcept;
[[nodiscard]] std::uint8_t CalculateRequestChecksum(const RequestFrame& frame) noexcept;
[[nodiscard]] bool HasValidRequestChecksum(const RequestFrame& frame) noexcept;
[[nodiscard]] bool HasValidResponseChecksum(const ResponseFrame& frame) noexcept;

void RefreshRequestChecksum(RequestFrame& frame) noexcept;

[[nodiscard]] RequestFrame BuildReadRequest(std::uint8_t address, std::uint8_t masterId = 0);
[[nodiscard]] RequestFrame BuildLockRequest(std::uint8_t address, std::uint8_t masterId = 0);
[[nodiscard]] RequestFrame BuildUnlockRequest(std::uint8_t address, std::uint8_t masterId = 0);
[[nodiscard]] RequestFrame BuildSetRequest(
    std::uint8_t address,
    const SetState& state,
    std::uint8_t masterId = 0);

// Safe modifications of an already initialized C3 frame. Each function changes
// only its own field and then refreshes checksum.
void SetRequestPower(RequestFrame& frame, bool power);
void SetRequestMode(RequestFrame& frame, Mode mode);
void SetRequestFanSpeed(RequestFrame& frame, FanSpeed speed);
void SetRequestTemperature(RequestFrame& frame, std::uint8_t temperature);
void SetRequestBlinds(RequestFrame& frame, bool enabled);

[[nodiscard]] ParseResult ParseResponse(
    const ResponseFrame& frame,
    std::optional<std::uint8_t> expectedAddress = std::nullopt,
    std::uint8_t expectedMasterId = 0);

// Collects exactly 32 bytes beginning with 0xAA. Bytes outside a frame are
// ignored. A 0x55 inside payload does not end the frame.
class ResponseFrameCollector {
public:
    [[nodiscard]] std::optional<ResponseFrame> Push(std::uint8_t byte) noexcept;
    void Reset() noexcept;

private:
    void Resynchronize() noexcept;

    ResponseFrame buffer_{};
    std::size_t size_ = 0;
};

} // namespace mdv
