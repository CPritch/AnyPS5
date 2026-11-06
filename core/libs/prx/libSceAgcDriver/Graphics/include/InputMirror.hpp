#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_INPUTMIRROR_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_INPUTMIRROR_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <cstdint>
#include <functional>
#include <vector>

namespace AgcDriver::Graphics {

class Recorder;
struct HostImport;

inline constexpr std::size_t InputMirrorMinBytes = std::size_t{64} << 10u;

bool InputMirrorsEnabled(const Context& context);
bool InputMirrorsVerified();

const HostImport* MirrorDrawInput(const Context& context, std::uint64_t address, std::size_t bytes, std::size_t alignment, std::vector<std::function<void(Recorder&)>>* deferred);

struct InputMirrorCounts {
    std::uint64_t candidates = 0;
    std::uint64_t created = 0;
    std::uint64_t hits = 0;
    std::uint64_t recopies = 0;
    std::uint64_t demoted = 0;
    std::uint64_t evicted = 0;
    std::uint64_t refusedBudget = 0;
    std::uint64_t refusedAllocation = 0;
    std::uint64_t fills = 0;
    std::uint64_t stagedFills = 0;
    std::uint64_t filledBytes = 0;
    std::uint64_t verified = 0;
    std::uint64_t preludes = 0;
    std::uint64_t preludeFills = 0;
    std::uint64_t inBatchFills = 0;
    std::uint64_t liveEntries = 0;
    std::uint64_t liveBytes = 0;
};

InputMirrorCounts InputMirrorCounters();
void ClearInputMirrors();

}

#endif
