// Temporary MDV v2 research tool. No production v2 decoder is implied.
#include "mdv_serial.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
using namespace std::chrono_literals;
volatile std::sig_atomic_t stopped = 0;
void Stop(int) { stopped = 1; }

int Integer(std::string_view text, int minimum, int maximum)
{
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() ||
        value < minimum || value > maximum) {
        throw std::runtime_error("invalid integer: " + std::string(text));
    }
    return value;
}

struct Options {
    std::string port, log = "mdv-v2-probe.log";
    int address = -1, count = 10, period = 1000, timeout = 400;
    int speed = -1, mode = -1, power = -1, temperature = -1;
};

Options Parse(int argc, char** argv)
{
    Options o;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (!seen.insert(key).second || i + 1 == argc) {
            throw std::runtime_error("duplicate option or missing value: " + key);
        }
        const std::string value = argv[++i];
        if (key == "--port") o.port = value;
        else if (key == "--address") o.address = Integer(value, 0, 63);
        else if (key == "--log") o.log = value;
        else if (key == "--count") o.count = Integer(value, 1, 10000);
        else if (key == "--period-ms") o.period = Integer(value, 150, 60000);
        else if (key == "--timeout-ms") o.timeout = Integer(value, 100, 10000);
        else if (key == "--speed") o.speed = value == "auto" ? 0 : Integer(value, 1, 7);
        else if (key == "--temp") o.temperature = Integer(value, 17, 30);
        else if (key == "--power" && (value == "on" || value == "off")) o.power = value == "on";
        else if (key == "--mode" && value == "fan") o.mode = 1;
        else if (key == "--mode" && value == "dry") o.mode = 2;
        else if (key == "--mode" && value == "heat") o.mode = 4;
        else if (key == "--mode" && value == "cool") o.mode = 8;
        else throw std::runtime_error("unknown option/value: " + key + " " + value);
    }
    if (o.port.empty() || o.log.empty() || o.address < 0 || o.timeout >= o.period) {
        throw std::runtime_error("require --port, --address, nonempty --log and timeout < period");
    }
    const bool anySet = o.speed >= 0 || o.mode >= 0 || o.power >= 0 || o.temperature >= 0;
    if (anySet && (o.speed < 0 || o.mode < 0 || o.power < 0 || o.temperature < 0)) {
        throw std::runtime_error("C3 requires all of --speed --mode --power --temp");
    }
    return o;
}

template<class Bytes> std::string Hex(const Bytes& bytes)
{
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0');
    for (const auto value : bytes) out << std::setw(2) << unsigned(value) << ' ';
    return out.str();
}

class Log {
public:
    explicit Log(const std::string& path) : file_(path, std::ios::app)
    {
        if (!file_) throw std::runtime_error("cannot open log: " + path);
        file_.exceptions(std::ios::badbit | std::ios::failbit);
    }
    void Write(const std::string& text)
    {
        const auto now = std::chrono::system_clock::now();
        const auto seconds = std::chrono::system_clock::to_time_t(now);
        const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000;
        std::ostringstream out;
        out << std::put_time(std::gmtime(&seconds), "%Y-%m-%dT%H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << millis << "Z " << text;
        file_ << out.str() << '\n';
        file_.flush();
        std::cout << out.str() << std::endl;
    }
private:
    std::ofstream file_;
};

mdv::RequestFrame Request(const Options& o, bool write)
{
    mdv::RequestFrame frame{};
    frame[0] = 0xAA;
    frame[1] = write ? 0xC3 : 0xD0;
    frame[2] = static_cast<std::uint8_t>(o.address);
    frame[4] = frame[5] = 0x80;
    frame[13] = write ? 0x3C : 0x2F;
    frame[15] = 0x55;
    if (write) {
        constexpr std::array<std::uint8_t, 8> speeds{0x80, 0x0C, 0x14, 0x1A, 0x22, 0x29, 0x31, 0x39};
        // OFF=00 is the documented experimental variant; do not infer another.
        frame[6] = o.power ? static_cast<std::uint8_t>(0x80 | o.mode) : 0;
        frame[7] = speeds.at(static_cast<std::size_t>(o.speed));
        frame[8] = static_cast<std::uint8_t>(o.temperature);
        frame[12] = 0x40;
    }
    mdv::RefreshRequestChecksum(frame);
    return frame;
}

void Describe(const mdv::ResponseFrame& frame, Log& log)
{
    std::ostringstream out;
    out << "D0 power=" << ((frame[21] & 0x80) ? "on" : "off")
        << " mode_bits=" << Hex(std::array{std::uint8_t(frame[21] & 0x0F)})
        << "speed_raw=" << Hex(std::array{frame[7]})
        << "set_temp=" << unsigned(frame[8])
        << " room_temp=" << (int(frame[9]) - 50) / 2.0
        << " alarm_raw=" << unsigned(frame[15]);
    log.Write(out.str());
}

class Probe {
public:
    Probe(const Options& options, Log& log)
        : o_(options), log_(log), pacer_({std::chrono::milliseconds(o_.period),
                                        std::chrono::milliseconds(o_.timeout)})
    {
        port_.Open(o_.port, {4800, 8, mdv::SerialParity::None, 1});
    }

    std::optional<mdv::ResponseFrame> Exchange(bool write)
    {
        static_cast<void>(pacer_.WaitForNextStart());
        if (stopped) return std::nullopt;
        port_.DiscardInput();
        const auto request = Request(o_, write);
        const auto wire = mdv::BuildWireRequest(request);
        log_.Write("TX " + Hex(wire));
        port_.WriteAll(wire);
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(o_.timeout);
        mdv::ResponseFrameCollector collector;
        std::array<std::uint8_t, 128> buffer{};
        while (!stopped && std::chrono::steady_clock::now() < deadline) {
            const auto size = port_.ReadSome(buffer);
            if (size) log_.Write("RX_RAW " + Hex(std::span(buffer.data(), size)));
            std::optional<mdv::ResponseFrame> matched;
            for (std::size_t i = 0; i < size; ++i) {
                const auto frame = collector.Push(buffer[i]);
                if (!frame) continue;
                const bool crc = mdv::HasValidResponseChecksum(*frame);
                const bool match = crc && (*frame)[1] == request[1] && (*frame)[4] == o_.address;
                log_.Write("RX_FRAME crc=" + std::string(crc ? "ok" : "bad") +
                           " match=" + (match ? "yes " : "no ") + Hex(*frame));
                if (match && !matched) matched = *frame;
            }
            if (matched) {
                if (!write) Describe(*matched, log_);
                return matched;
            }
            if (!size) std::this_thread::sleep_for(1ms);
        }
        log_.Write(stopped ? "INTERRUPTED" : "TIMEOUT (no matching valid frame)");
        return std::nullopt;
    }
private:
    Options o_;
    Log& log_;
    mdv::SerialPort port_;
    mdv::TransactionPacer pacer_;
};
} // namespace

int main(int argc, char** argv)
{
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        std::cout <<
            "MDV v2 research probe, 4800 8N1; stop other users of this serial port first.\n"
            "Read: --port /dev/ttyRS485-1 --address N [--count 10] [--log FILE]\n"
            "Write once, then read: add --power on|off --mode cool|heat|dry|fan\n"
            "                            --temp 17..30 --speed 1..7|auto\n"
            "Timing: --period-ms 1000 --timeout-ms 400 (timeout after TX completion).\n"
            "A valid initial D0 is required before C3. OFF uses mode byte 00.\n"
            "No automatic write retries, speed decoding or restoration. Ctrl+C stops.\n";
        return 0;
    }
    try {
        const auto options = Parse(argc, argv);
        Log log(options.log);
        std::signal(SIGINT, Stop);
        std::signal(SIGTERM, Stop);
        log.Write("SESSION port=" + options.port + " address=" + std::to_string(options.address) +
                  " period_ms=" + std::to_string(options.period) +
                  " timeout_ms=" + std::to_string(options.timeout));
        Probe probe(options, log);
        if (options.speed >= 0) {
            log.Write("BASELINE before C3");
            if (!probe.Exchange(false)) {
                log.Write("ABORT: no valid baseline D0; C3 not sent");
                return 2;
            }
            if (stopped) return 130;
            log.Write("SET power=" + std::to_string(options.power) +
                      " mode_bits=" + std::to_string(options.mode) +
                      " temp=" + std::to_string(options.temperature) +
                      " native_speed=" + (options.speed ? std::to_string(options.speed) : "auto"));
            const auto reply = probe.Exchange(true);
            log.Write(reply ? "C3 reply received; not factual confirmation" :
                              "C3 reply absent; not retrying; reading D0 next");
        }
        int received = 0;
        for (int i = 0; i < options.count && !stopped; ++i) {
            log.Write("POLL " + std::to_string(i + 1));
            if (probe.Exchange(false)) ++received;
        }
        log.Write("END valid_D0=" + std::to_string(received) +
                  "/" + std::to_string(options.count) + "; settings not restored");
        return stopped ? 130 : (received == options.count ? 0 : 2);
    }
    catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
