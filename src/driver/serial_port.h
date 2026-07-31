#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace mdv {

enum class SerialParity {
    None,
    Even,
    Odd,
};

struct SerialSettings {
    std::uint32_t baudRate = 4800;
    std::uint8_t dataBits = 8;
    SerialParity parity = SerialParity::None;
    std::uint8_t stopBits = 1;
};

void ValidateSerialSettings(const SerialSettings& settings);

class SerialPort {
public:
    SerialPort() = default;
    ~SerialPort();

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    SerialPort(SerialPort&& other) noexcept;
    SerialPort& operator=(SerialPort&& other) noexcept;

    void Open(std::string_view portName, SerialSettings settings = {});
    void Close() noexcept;

    [[nodiscard]] bool IsOpen() const noexcept;
    [[nodiscard]] const std::string& PortName() const noexcept;
    [[nodiscard]] const SerialSettings& Settings() const noexcept;

    void DiscardInput();
    void WriteAll(std::span<const std::uint8_t> data);
    [[nodiscard]] std::size_t ReadSome(std::span<std::uint8_t> buffer);

    [[nodiscard]] static std::string NormalizePortName(std::string_view portName);

private:
#ifdef _WIN32
    void* handle_ = reinterpret_cast<void*>(-1);
#else
    int handle_ = -1;
#endif
    std::string portName_;
    SerialSettings settings_{};
};

} // namespace mdv
