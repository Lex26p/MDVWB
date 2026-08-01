#include "mdvwb_discovery_runner.h"

#include "mdv_modbus_discovery.h"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mdvwb {
namespace {

constexpr std::string_view ResultPrefix = "FOUND_ADDRESSES=";

std::string TrimLineEnd(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    return value;
}

std::string LastNonEmptyLine(std::string_view output) {
    std::istringstream input{std::string(output)};
    std::string line;
    std::string last;
    while (std::getline(input, line)) {
        line = TrimLineEnd(std::move(line));
        if (!line.empty()) {
            last = line;
        }
    }
    return last;
}

#ifndef _WIN32

struct CapturedProcess {
    int exitCode = 125;
    std::string output;
    bool timedOut = false;
    bool outputLimitExceeded = false;
};

int DecodeExitStatus(int status) noexcept {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 125;
}

bool TryWaitForChild(pid_t child, int& status) {
    for (;;) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            return true;
        }
        if (result == 0) {
            return false;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        throw std::runtime_error(
            "cannot wait for discovery process: " +
            std::string(std::strerror(errno)));
    }
}

void WaitForChildBlocking(pid_t child, int& status) noexcept {
    for (;;) {
        const pid_t result = waitpid(child, &status, 0);
        if (result == child) {
            return;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

bool DrainOutput(
    int descriptor,
    std::string& output,
    std::size_t maximumOutputBytes,
    bool& outputLimitExceeded) {
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count > 0) {
            const std::size_t bytes = static_cast<std::size_t>(count);
            const std::size_t remaining =
                output.size() < maximumOutputBytes
                    ? maximumOutputBytes - output.size()
                    : 0U;
            const std::size_t accepted = std::min(bytes, remaining);
            output.append(buffer, accepted);
            if (accepted != bytes) {
                outputLimitExceeded = true;
            }
            continue;
        }
        if (count == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        throw std::runtime_error(
            "cannot read discovery output: " +
            std::string(std::strerror(errno)));
    }
}

void DrainOutputNoThrow(
    int descriptor,
    std::string& output,
    std::size_t maximumOutputBytes,
    bool& outputLimitExceeded) noexcept {
    try {
        static_cast<void>(DrainOutput(
            descriptor,
            output,
            maximumOutputBytes,
            outputLimitExceeded));
    } catch (...) {
    }
}

int TerminateAndReap(
    pid_t child,
    int outputDescriptor,
    std::string& output,
    std::size_t maximumOutputBytes,
    bool& outputLimitExceeded) noexcept {
    if (kill(child, SIGTERM) != 0 && errno != ESRCH) {
        // Continue to the SIGKILL fallback even if SIGTERM could not be sent.
    }

    int status = 0;
    const auto graceDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (;;) {
        DrainOutputNoThrow(
            outputDescriptor,
            output,
            maximumOutputBytes,
            outputLimitExceeded);

        const pid_t waitResult = waitpid(child, &status, WNOHANG);
        if (waitResult == child) {
            return DecodeExitStatus(status);
        }
        if (waitResult < 0 && errno != EINTR) {
            return 125;
        }
        if (std::chrono::steady_clock::now() >= graceDeadline) {
            break;
        }

        pollfd descriptor{outputDescriptor, POLLIN | POLLHUP, 0};
        static_cast<void>(poll(&descriptor, 1, 50));
    }

    if (kill(child, SIGKILL) != 0 && errno != ESRCH) {
        // waitpid below still attempts to reap the process.
    }
    WaitForChildBlocking(child, status);
    DrainOutputNoThrow(
        outputDescriptor,
        output,
        maximumOutputBytes,
        outputLimitExceeded);
    return DecodeExitStatus(status);
}

CapturedProcess ExecuteAndCapture(
    const std::string& executable,
    const std::vector<std::string>& arguments,
    std::chrono::milliseconds overallTimeout,
    std::size_t maximumOutputBytes) {
    int pipeDescriptors[2]{};
    if (pipe(pipeDescriptors) != 0) {
        throw std::runtime_error(
            "cannot create discovery output pipe: " +
            std::string(std::strerror(errno)));
    }

    const pid_t child = fork();
    if (child < 0) {
        close(pipeDescriptors[0]);
        close(pipeDescriptors[1]);
        throw std::runtime_error(
            "cannot start discovery process: " +
            std::string(std::strerror(errno)));
    }

    if (child == 0) {
        close(pipeDescriptors[0]);
        if (dup2(pipeDescriptors[1], STDOUT_FILENO) < 0 ||
            dup2(pipeDescriptors[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(pipeDescriptors[1]);

        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2U);
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execv(executable.c_str(), argv.data());
        _exit(127);
    }

    close(pipeDescriptors[1]);
    const int currentFlags = fcntl(pipeDescriptors[0], F_GETFL, 0);
    if (currentFlags < 0 ||
        fcntl(pipeDescriptors[0], F_SETFL, currentFlags | O_NONBLOCK) < 0) {
        const int savedError = errno;
        int ignoredStatus = 0;
        static_cast<void>(kill(child, SIGKILL));
        WaitForChildBlocking(child, ignoredStatus);
        close(pipeDescriptors[0]);
        throw std::runtime_error(
            "cannot configure discovery output pipe: " +
            std::string(std::strerror(savedError)));
    }

    CapturedProcess captured;
    const auto deadline = std::chrono::steady_clock::now() + overallTimeout;
    int status = 0;

    try {
        for (;;) {
            static_cast<void>(DrainOutput(
                pipeDescriptors[0],
                captured.output,
                maximumOutputBytes,
                captured.outputLimitExceeded));

            if (captured.outputLimitExceeded) {
                captured.exitCode = TerminateAndReap(
                    child,
                    pipeDescriptors[0],
                    captured.output,
                    maximumOutputBytes,
                    captured.outputLimitExceeded);
                break;
            }

            if (TryWaitForChild(child, status)) {
                captured.exitCode = DecodeExitStatus(status);
                static_cast<void>(DrainOutput(
                    pipeDescriptors[0],
                    captured.output,
                    maximumOutputBytes,
                    captured.outputLimitExceeded));
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                captured.timedOut = true;
                captured.exitCode = TerminateAndReap(
                    child,
                    pipeDescriptors[0],
                    captured.output,
                    maximumOutputBytes,
                    captured.outputLimitExceeded);
                break;
            }

            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now);
            const int waitMilliseconds = static_cast<int>(
                std::min<std::int64_t>(50, std::max<std::int64_t>(
                    1, remaining.count())));
            pollfd descriptor{pipeDescriptors[0], POLLIN | POLLHUP, 0};
            const int pollResult = poll(&descriptor, 1, waitMilliseconds);
            if (pollResult < 0 && errno != EINTR) {
                throw std::runtime_error(
                    "cannot poll discovery output: " +
                    std::string(std::strerror(errno)));
            }
        }
    } catch (...) {
        static_cast<void>(TerminateAndReap(
            child,
            pipeDescriptors[0],
            captured.output,
            maximumOutputBytes,
            captured.outputLimitExceeded));
        close(pipeDescriptors[0]);
        throw;
    }

    close(pipeDescriptors[0]);
    return captured;
}
#endif

}  // namespace

std::vector<int> ParseDiscoveryAddresses(std::string_view output) {
    std::istringstream input{std::string(output)};
    std::string line;
    std::optional<std::string> value;
    while (std::getline(input, line)) {
        line = TrimLineEnd(std::move(line));
        if (line.rfind(ResultPrefix, 0) == 0U) {
            value = line.substr(ResultPrefix.size());
        }
    }
    if (!value.has_value()) {
        throw std::runtime_error(
            "discovery output does not contain FOUND_ADDRESSES");
    }
    if (value->empty()) {
        return {};
    }

    std::set<int> unique;
    std::size_t begin = 0;
    while (begin <= value->size()) {
        const std::size_t separator = value->find(',', begin);
        const std::size_t end =
            separator == std::string::npos ? value->size() : separator;
        if (end == begin) {
            throw std::runtime_error(
                "discovery output contains an empty address");
        }
        const std::string_view token(value->data() + begin, end - begin);
        int address = -1;
        const auto parsed = std::from_chars(
            token.data(), token.data() + token.size(), address);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != token.data() + token.size() ||
            address < 0 || address > 63) {
            throw std::runtime_error(
                "discovery output contains an invalid address");
        }
        if (!unique.insert(address).second) {
            throw std::runtime_error(
                "discovery output contains a duplicate address");
        }
        if (separator == std::string::npos) {
            break;
        }
        begin = separator + 1U;
    }

    return {unique.begin(), unique.end()};
}

NativeDiscoveryRunner::NativeDiscoveryRunner(
    std::string executablePath,
    std::chrono::milliseconds overallTimeout,
    std::size_t maximumOutputBytes)
    : executablePath_(std::move(executablePath)),
      overallTimeout_(overallTimeout),
      maximumOutputBytes_(maximumOutputBytes) {
    if (executablePath_.empty()) {
        throw std::invalid_argument(
            "discovery executable path cannot be empty");
    }
    if (overallTimeout_.count() <= 0) {
        throw std::invalid_argument(
            "discovery overall timeout must be positive");
    }
    if (maximumOutputBytes_ == 0U) {
        throw std::invalid_argument(
            "discovery output limit must be positive");
    }
}

DiscoveryExecutionResult NativeDiscoveryRunner::Run(
    std::string_view port,
    int masterId,
    int periodMilliseconds,
    int responseTimeoutMilliseconds) {
    if (port.empty()) {
        throw std::invalid_argument("discovery port cannot be empty");
    }
    if (masterId < 0 || masterId > 63) {
        throw std::invalid_argument(
            "discovery master id must be in range 0..63");
    }
    if (periodMilliseconds <= 0 || responseTimeoutMilliseconds <= 0) {
        throw std::invalid_argument("discovery timing must be positive");
    }

    const auto runtime = FindDiscoveryRuntimeForPort(
        DiscoveryDefaultDirectoryFromEnvironment(),
        port);
    if (runtime.has_value() &&
        runtime->protocol == DiscoveryRuntimeProtocol::ModbusRtu) {
        if (!runtime->modbus.has_value()) {
            throw std::logic_error(
                "Modbus discovery selection omitted runtime settings");
        }
        const ModbusDiscoveryScanResult scan =
            RunModbusDiscovery(*runtime->modbus);
        return DiscoveryExecutionResult{
            .success = scan.success,
            .exitCode = scan.success ? 0 : 1,
            .addresses = scan.addresses,
            .output = scan.output,
            .message = scan.message,
        };
    }

#ifdef _WIN32
    throw std::runtime_error(
        "native discovery process is supported only on Linux");
#else
    const std::vector<std::string> arguments{
        "--discover",
        "--port", std::string(port),
        "--master-id", std::to_string(masterId),
        "--period-ms", std::to_string(periodMilliseconds),
        "--response-timeout-ms",
        std::to_string(responseTimeoutMilliseconds),
    };
    CapturedProcess captured = ExecuteAndCapture(
        executablePath_,
        arguments,
        overallTimeout_,
        maximumOutputBytes_);

    DiscoveryExecutionResult result;
    result.exitCode = captured.exitCode;
    result.output = std::move(captured.output);

    if (captured.outputLimitExceeded) {
        result.message =
            "Discovery process output exceeded limit of " +
            std::to_string(maximumOutputBytes_) + " bytes";
        return result;
    }
    if (captured.timedOut) {
        result.message =
            "Discovery process timed out after " +
            std::to_string(overallTimeout_.count()) + " ms";
        return result;
    }
    if (result.exitCode != 0) {
        result.message = LastNonEmptyLine(result.output);
        if (result.message.empty()) {
            result.message =
                "Discovery process exited with code " +
                std::to_string(result.exitCode);
        }
        return result;
    }

    try {
        result.addresses = ParseDiscoveryAddresses(result.output);
        result.success = true;
        result.message = result.addresses.empty()
            ? "Discovery completed; no devices found"
            : "Discovery completed";
    } catch (const std::exception& error) {
        result.message = error.what();
    }
    return result;
#endif
}

}  // namespace mdvwb
