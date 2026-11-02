#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <array>
#include <cstdlib>

namespace AgcDriver::DriverDetail {

PollStats& Driver::pollStats() {
    static thread_local PollStats stats;
    return stats;
}

WaitOutcomes& Driver::waitOutcomes() {
    static thread_local WaitOutcomes outcomes;
    return outcomes;
}

Graphics::Recorder::LateStatistics& Driver::lateCountsSeen() {
    static thread_local Graphics::Recorder::LateStatistics seen{};
    return seen;
}

EpochBumps& Driver::epochBumps() {
    static thread_local EpochBumps bumps;
    return bumps;
}

std::chrono::steady_clock::time_point& Driver::lastEpochBump() {
    static thread_local std::chrono::steady_clock::time_point at{};
    return at;
}

namespace {

struct LabelAtBump {
    std::uint64_t address = 0;
    std::uint64_t value = 0;
    std::uint64_t generation = 0;
    std::uint64_t bump = 0;
    std::uint32_t bytes = 0;
};

struct WaitLabels {
    std::array<LabelAtBump, 4> labels{};
    std::size_t next = 0;
    std::uint64_t bumps = 0;
};

WaitLabels& waitLabels() {
    static thread_local WaitLabels labels;
    return labels;
}

}

int Driver::waitEpochSkip() {
    static const int mode = [] {
        const char* text = std::getenv("APS5_WAIT_EPOCH_SKIP");
        return text == nullptr ? 2 : std::atoi(text);
    }();
    return mode;
}

bool Driver::waitOrdersNothingNew(std::span<const std::uint32_t> packet, std::uint64_t awaited, std::size_t bytes) {
    if (waitEpochSkip() < 2) return false;
    const auto& waits = waitLabels();
    for (const auto& label : waits.labels) {
        if (label.address != awaited || label.bytes != bytes) continue;
        if (label.bump == 0 || label.bump != waits.bumps || !Pm4::WaitComparesValue(packet, label.value)) return false;
        if (GuestMemory::CollectWritesUncached(awaited, bytes) == 0 || !GuestMemory::UnchangedSince(awaited, bytes, label.generation)) return false;
        ++epochBumps().prereadSkips;
        return true;
    }
    return false;
}

void Driver::learnWaitLabel(std::uint64_t awaited, std::size_t bytes) {
    if (waitEpochSkip() < 2 || (bytes != 4 && bytes != 8)) return;
    auto& waits = waitLabels();
    for (const auto& label : waits.labels) {
        if (label.address == awaited && label.bytes == bytes) return;
    }
    waits.labels[waits.next++ % waits.labels.size()] = {awaited, 0, 0, 0, static_cast<std::uint32_t>(bytes)};
}

void Driver::bumpEpoch(std::uint64_t EpochBumps::*counter) {
    lastEpochBump() = std::chrono::steady_clock::now();
    if (waitEpochSkip() >= 2) {
        auto& waits = waitLabels();
        ++waits.bumps;
        for (auto& label : waits.labels) {
            if (label.address == 0) continue;
            label.generation = GuestMemory::CollectWritesUncached(label.address, label.bytes);
            label.bump = label.generation != 0 ? waits.bumps : 0;
            if (label.bump == 0) continue;
            label.value = label.bytes == 8 ? *reinterpret_cast<const volatile std::uint64_t*>(label.address) : *reinterpret_cast<const volatile std::uint32_t*>(label.address);
        }
    }
    GuestMemory::BumpCollectEpoch();
    if (DrawPipeline::Active()) DrawPipeline::EpochToken().fetch_add(1, std::memory_order_relaxed);
    ++(epochBumps().*counter);
}

bool Driver::packetEpoch() {
    static const bool packet = std::getenv("APS5_PACKET_EPOCH") != nullptr;
    return packet;
}

}
