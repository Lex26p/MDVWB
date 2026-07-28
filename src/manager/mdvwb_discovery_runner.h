#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mdvwb {

struct DiscoveryExecutionResult {
    bool success = false;
    int exitCode = -1;
    std::vector<int> addresses;
    std::string output;
    std::string message;
};

class DiscoveryRunner {
public:
    virtual ~DiscoveryRunner() = default;
    [[nodiscard]] virtual DiscoveryExecutionResult Run(
        std::string_view port,
        int masterId,
        int periodMilliseconds,
        int responseTimeoutMilliseconds) = 0;
};

class NativeDiscoveryRunner final : public DiscoveryRunner {
public:
    explicit NativeDiscoveryRunner(
        std::string executablePath,
        std::chrono::milliseconds overallTimeout =
            std::chrono::milliseconds{45000},
        std::size_t maximumOutputBytes = 256U * 1024U);

    [[nodiscard]] DiscoveryExecutionResult Run(
        std::string_view port,
        int masterId,
        int periodMilliseconds,
        int responseTimeoutMilliseconds) override;

private:
    std::string executablePath_;
    std::chrono::milliseconds overallTimeout_;
    std::size_t maximumOutputBytes_ = 0;
};

[[nodiscard]] std::vector<int> ParseDiscoveryAddresses(std::string_view output);

}  // namespace mdvwb
