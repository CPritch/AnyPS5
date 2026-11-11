#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SUBMISSION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SUBMISSION_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/SubmissionProgress.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AgcDriver::DriverDetail {

inline bool FlatRegisterLists() {
    static const bool enabled = std::getenv("APS5_NO_FLAT_REGISTER_LISTS") == nullptr;
    return enabled;
}

struct ListedRegisters {
    std::span<const std::uint32_t> pairs;
    bool found = false;
    bool valid = false;
};

struct RegisterListEntry {
    std::size_t cursor = 0;
    std::size_t first = 0;
    std::size_t words = 0;
    bool valid = false;
};

struct Submission {
    std::uint64_t serial;
    std::uint32_t queue;
    std::vector<std::uint32_t> commands;
    std::shared_ptr<SubmissionProgress> progress;

    std::shared_ptr<const ShaderRegistry> shaders;
    std::map<std::size_t, std::shared_ptr<IFlipRequest>> flips;
    std::map<std::size_t, std::shared_ptr<IRenderingWait>> renderingWaits;
    std::map<std::size_t, std::size_t> conditionalEnds;
    std::map<std::size_t, std::vector<std::uint32_t>> registerLists;
    std::vector<RegisterListEntry> registerListIndex;
    std::vector<std::uint32_t> registerListWords;
    bool suspend = false;
    bool waitFree = false;
    bool holdsFlip = false;

    std::uint64_t received = 0;
    std::vector<std::uint64_t> labelWrites;
    std::set<std::size_t> heldAtSubmit;
    std::chrono::steady_clock::time_point receivedAt{};
    std::chrono::steady_clock::time_point copiedAt{};
    std::chrono::steady_clock::time_point validatedAt{};
    std::chrono::steady_clock::time_point roomReadyAt{};
    std::chrono::steady_clock::time_point enqueuedAt{};
    std::chrono::steady_clock::time_point dequeuedAt{};
    std::chrono::steady_clock::time_point orderedAt{};
    const std::uint32_t* rewindTail = nullptr;
    std::size_t rewindWords = 0;

    ListedRegisters FindRegisterList(std::size_t cursor) const;
    ListedRegisters RegisterListOf(const RegisterListEntry& entry) const { return {std::span<const std::uint32_t>(registerListWords).subspan(entry.first, entry.words), true, entry.valid}; }
};

struct QueueWorker {
    std::deque<Submission> pending;

    std::atomic<std::uint64_t> queued{0};
    std::unordered_map<std::uint64_t, std::uint32_t> unfinishedWrites;
    std::uint64_t executingWaitFree = 0;
    bool executingWaits = false;
    alignas(64) std::atomic<std::uint64_t> executingPacket{0};
    std::thread thread;
};

struct ReferencedProgramScan {
    std::vector<std::uint64_t> addresses;
    bool complete = true;
};

ReferencedProgramScan ReferencedPrograms(std::span<const std::uint32_t> commands, const std::map<std::size_t, std::vector<std::uint32_t>>& registerLists);
ReferencedProgramScan ReferencedPrograms(const Submission& submission);

}

#endif
