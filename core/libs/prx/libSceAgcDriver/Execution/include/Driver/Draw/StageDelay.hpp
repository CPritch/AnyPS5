#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_STAGEDELAY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_STAGEDELAY_HPP

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace AgcDriver::DriverDetail {

enum class DelayedStage : std::uint8_t { Committer, Worker, Finisher, Preparer, Resolver, Count };

using StageDelays = std::array<std::uint64_t, static_cast<std::size_t>(DelayedStage::Count)>;

inline constexpr std::array<std::string_view, static_cast<std::size_t>(DelayedStage::Count)> DelayedStageNames{"committer", "worker", "finisher", "preparer", "resolver"};

inline StageDelays ParseStageDelays(std::string_view text) {
    StageDelays delays{};
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto pair = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        const auto colon = pair.find(':');
        if (colon == std::string_view::npos) continue;
        const auto value = pair.substr(colon + 1);
        std::uint64_t ns = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), ns);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) continue;
        const auto name = std::find(DelayedStageNames.begin(), DelayedStageNames.end(), pair.substr(0, colon));
        if (name == DelayedStageNames.end()) continue;
        delays[static_cast<std::size_t>(name - DelayedStageNames.begin())] = ns;
    }
    return delays;
}

extern const StageDelays stageDelays;
void SpinStageDelay(std::uint64_t ns);

inline void DelayStage(DelayedStage stage) {
    const auto ns = stageDelays[static_cast<std::size_t>(stage)];
    if (ns != 0) [[unlikely]] SpinStageDelay(ns);
}

}

#endif
