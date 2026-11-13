#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace AgcDriver::GuestMemory;
constexpr std::size_t Unit = 65536;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Range {
public:
    Range() {
        data = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(2 * Unit, Unit));
        Require(data != nullptr, "cannot reserve mapped-copy fixture");
        GuestArena::GuestArenaCommit_nid_postfix(data, 2 * Unit, 0x04, Unit);
        std::memset(data, 0x5b, 2 * Unit);
    }

    ~Range() {
        GuestArena::GuestArenaReset_nid_postfix(data, 2 * Unit);
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(Address(), 2 * Unit);
        GuestArena::GuestArenaRelease_nid_postfix(data, 2 * Unit);
    }

    std::uintptr_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
    std::byte* Data() const { return data; }

private:
    std::byte* data = nullptr;
};

unsigned readGuards = 0;
unsigned flushes = 0;

void NoteRead(std::uint64_t, std::size_t) { ++readGuards; }
void NoteFlush(std::uint64_t, std::size_t) { ++flushes; }

void Check(std::string_view mode) {
    std::array<std::byte, 256> out{};
    if (mode == "boundaries") {
        Require(CopyMapped(0, {}) == Compare::Equal && CopyMapped(std::numeric_limits<std::uintptr_t>::max(), {}) == Compare::Equal, "an empty copy requires mapped memory");
        Require(CopyMapped(0, out) == Compare::Unmapped, "a null source was reported copied");
        Require(CopyMapped(std::numeric_limits<std::uintptr_t>::max() - 7u, out) == Compare::Unmapped, "an overflowing source range was reported copied");
        return;
    }
    Range range;
    Require(Accessible(range.Data(), 2 * Unit), "readable fixture cannot prime the access cache");
    if (mode == "hooks") {
        SetReadGuard(&NoteRead);
        SetFlushHook(&NoteFlush);
        const auto speculative = CopyMapped(range.Address(), out);
        const auto speculativeReads = readGuards;
        const auto speculativeFlushes = flushes;
        const bool ordinary = GuestCopyIn_nid_no_patch(out.data(), range.Data(), out.size());
        const auto ordinaryReads = readGuards;
        const auto ordinaryFlushes = flushes;
        SetReadGuard(nullptr);
        SetFlushHook(nullptr);
        Require(speculative == Compare::Equal, "a speculative copy with read hooks cannot read valid bytes");
        Require(speculativeReads == 0 && speculativeFlushes == 0, "a speculative mapped copy ran read guards or flushed GPU writes");
        Require(ordinary && ordinaryReads == 1 && ordinaryFlushes == 1, "an ordinary checked copy lost its read preparation");
        return;
    }
    if (mode == "reset") {
        GuestArena::GuestArenaReset_nid_postfix(range.Data(), 2 * Unit);
        errno = E2BIG;
        Require(CopyMapped(range.Address(), out) == Compare::Unmapped, "a reset source was reported copied through cached readable pages");
        Require(errno == E2BIG, "a failed speculative copy changed errno");
        return;
    }
    if (mode == "partial") {
        GuestArena::GuestArenaReset_nid_postfix(range.Data() + Unit, Unit);
        errno = E2BIG;
        Require(CopyMapped(range.Address() + Unit - 128u, out) == Compare::Unmapped, "a partly reset source was reported fully copied");
        Require(errno == E2BIG, "a partial speculative copy changed errno");
        return;
    }
    if (mode == "reuse") {
        GuestArena::GuestArenaReset_nid_postfix(range.Data(), 2 * Unit);
        GuestArena::GuestArenaCommit_nid_postfix(range.Data(), 2 * Unit, 0x04, Unit);
        std::memset(range.Data(), 0x6c, 2 * Unit);
        Require(CopyMapped(range.Address(), out) == Compare::Equal, "a recommitted source cannot be copied");
        Require(std::all_of(out.begin(), out.end(), [](std::byte byte) { return byte == std::byte{0x6c}; }), "a recommitted source returned old bytes");
        return;
    }
    if (mode == "runs") {
        std::array<std::byte, 64> first{};
        std::array<std::byte, 64> second{};
        std::array<std::byte, 64> third{};
        const auto filled = [](const auto& bytes, std::byte value) { return std::all_of(bytes.begin(), bytes.end(), [&](std::byte byte) { return byte == value; }); };
        const std::array<CopyRun, 3> runs{{{range.Address(), first}, {range.Address() + Unit - 32u, second}, {range.Address() + Unit + 64u, third}}};
        errno = E2BIG;
        Require(CopyMappedRuns(runs) == Compare::Equal, "readable runs, one across an arena unit, cannot be copied together");
        Require(filled(first, std::byte{0x5b}) && filled(second, std::byte{0x5b}) && filled(third, std::byte{0x5b}), "copied runs returned different bytes");
        Require(errno == E2BIG, "a successful run copy changed errno");
        Require(CopyMappedRuns({}) == Compare::Equal, "no runs require mapped memory");
        std::array<std::uint32_t, 100> words{};
        std::vector<CopyRun> many;
        for (std::size_t index = 0; index < words.size(); ++index) many.push_back({range.Address() + index * 512u, std::as_writable_bytes(std::span(words).subspan(index, 1))});
        Require(CopyMappedRuns(many) == Compare::Equal && std::all_of(words.begin(), words.end(), [](std::uint32_t word) { return word == 0x5b5b5b5bu; }), "more runs than one kernel call takes were not all copied");
        const std::array<CopyRun, 1> null{{{0, first}}};
        Require(CopyMappedRuns(null) == Compare::Unmapped, "a null run source was reported copied");
        GuestArena::GuestArenaReset_nid_postfix(range.Data() + Unit, Unit);
        const std::array<CopyRun, 2> partly{{{range.Address(), first}, {range.Address() + Unit + 64u, third}}};
        Require(CopyMappedRuns(partly) == Compare::Unmapped, "runs with one in a reset unit were reported copied");
        Require(errno == E2BIG, "a failed run copy changed errno");
        return;
    }
    Require(mode == "valid", "unknown mapped-copy fixture mode");
    errno = E2BIG;
    Require(CopyMapped(range.Address() + Unit - 128u, out) == Compare::Equal, "a readable source crossing an arena unit cannot be copied");
    Require(std::all_of(out.begin(), out.end(), [](std::byte byte) { return byte == std::byte{0x5b}; }), "a readable source returned different bytes");
    Require(errno == E2BIG, "a successful speculative copy changed errno");
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "mapped-copy fixture requires a mode");
        Check(argv[1]);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
