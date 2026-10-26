#ifndef CORE_SHADER_RECOMPILER_USERDATAUSE_HPP
#define CORE_SHADER_RECOMPILER_USERDATAUSE_HPP

#include <compare>
#include <cstdint>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

// A buffer descriptor whose base dword (dword 0) the walk took directly from user word `word`, or from the one raw
// read at `address` through user word `origin` (-1 when the read's base is no user word), the read of flat SRT slot
// `slot` when it has one; `resource` indexes the snapshot's buffers. Such an evaluation is recorded here and not in
// asValue, reads or valueSlots, so a word or a slot that appears only here is used by nothing in the walk but that
// base (a slot's read is also in reads once, for the flat SRT copy).
struct SrtBufferBase {
    std::uint32_t resource = 0;
    std::int32_t word = -1;
    std::uint64_t address = 0;
    std::int32_t origin = -1;
    std::int32_t slot = -1;
    auto operator<=>(const SrtBufferBase&) const = default;
};

struct SrtUserDataUse {
    static constexpr std::uint32_t TrackedWords = 64;
    std::uint64_t asBase = 0;
    std::uint64_t asValue = 0;
    std::vector<std::pair<std::uint64_t, std::int32_t>> reads;
    std::uint64_t asBufferBase = 0;
    std::vector<SrtBufferBase> bufferBases;
    // Flat SRT slots the walk used as a value (a ReadConst it evaluated), apart from the flat copy and buffer bases.
    std::vector<std::uint32_t> valueSlots;
};

}

#endif
