#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/VerifyCounters.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("rewind draw replay: " + what);
}

constexpr std::size_t Chunks = 9;
constexpr std::array<std::size_t, Chunks> ChunkDraws{40, 3, 25, 1, 60, 2, 33, 7, 48};
constexpr std::size_t MostDraws = 64;
constexpr std::size_t SliceBytes = 1024;

alignas(256) constexpr std::array<std::uint32_t, 16> VertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u, 0xf80008cfu, 0x04030201u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 10> PixelCode{0xf4000100u, 0xfa000000u, 0xbf8cc07fu, 0x7e000204u, 0x06000002u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u};
alignas(256) std::array<float, 4> PixelConstant{};
alignas(256) std::array<std::byte, 2 * Chunks * SliceBytes> ColorMemory{};
alignas(256) std::array<std::uint32_t, 2 * Chunks * MostDraws * 6> ListMemory{};
alignas(256) std::array<std::uint64_t, 2 * Chunks> Labels{};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};

void Register(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

std::uint32_t Low(const void* pointer) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer)); }
std::uint32_t High(const void* pointer) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer) >> 32u); }

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

void Setup(std::vector<std::uint32_t>& commands) {
    const auto vertex = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(PixelCode.data());
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000}, {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x2fe, 0}, {0x302, 0}, {0x306, 0}, {0x30a, 0},
    };
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 0u}, {0x8c, 0},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 3u << 1u},
    };
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
}

std::uint32_t Colour(std::size_t chunk, std::size_t draw) {
    return std::bit_cast<std::uint32_t>(0.0625f * static_cast<float>(1 + (chunk * 5 + draw * 3) % 15));
}

std::vector<std::uint32_t> Chain(std::size_t copy, bool indirect, std::vector<std::size_t>& controls) {
    std::vector<std::uint32_t> commands;
    Setup(commands);
    for (std::size_t chunk = 0; chunk < Chunks; ++chunk) {
        const auto* target = ColorMemory.data() + (copy * Chunks + chunk) * SliceBytes;
        const auto address = reinterpret_cast<std::uintptr_t>(target);
        Append(commands, 0x69, {0x318, static_cast<std::uint32_t>(address >> 8u)});
        Append(commands, 0x69, {0x390, static_cast<std::uint32_t>(address >> 40u)});
        for (std::size_t draw = 0; draw < ChunkDraws[chunk]; ++draw) {
            const std::array<std::pair<std::uint32_t, std::uint32_t>, 3> words{{{0x00cu, Low(PixelConstant.data())}, {0x00du, High(PixelConstant.data())}, {0x00eu, Colour(chunk, draw)}}};
            if (indirect) {
                auto* pairs = ListMemory.data() + ((copy * Chunks + chunk) * MostDraws + draw) * 6;
                for (std::size_t word = 0; word < words.size(); ++word) {
                    pairs[2 * word] = words[word].first;
                    pairs[2 * word + 1] = words[word].second;
                }
                Append(commands, 0x63, {Low(pairs), High(pairs), 0x80000000u, static_cast<std::uint32_t>(words.size())});
            } else {
                for (const auto& [offset, value] : words) Append(commands, 0x76, {offset, value});
            }
            Append(commands, 0x2d, {3, 2});
        }
        auto* label = &Labels[copy * Chunks + chunk];
        Append(commands, 0x49, {0x514u, 2u << 29u, Low(label), High(label), static_cast<std::uint32_t>(chunk + 1), 0u, 0u});
        if (chunk + 1 == Chunks) break;
        Append(commands, 0x59, {0u});
        controls.push_back(commands.size() - 1);
    }
    return commands;
}

void Await(std::size_t copy) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (std::size_t chunk = 0; chunk < Chunks; ++chunk) {
        auto& label = Labels[copy * Chunks + chunk];
        while (std::atomic_ref<std::uint64_t>(label).load(std::memory_order_acquire) != chunk + 1) {
            Require(std::chrono::steady_clock::now() < deadline, "chunk " + std::to_string(chunk) + " of copy " + std::to_string(copy) + " never released its label");
            std::this_thread::yield();
        }
    }
    AgcDriverWaitIdle_nid_postfix();
}

void Run(std::size_t copy, bool indirect, bool released) {
    std::vector<std::size_t> controls;
    auto commands = Chain(copy, indirect, controls);
    for (std::size_t chunk = 0; chunk < Chunks; ++chunk) {
        Labels[copy * Chunks + chunk] = 0;
        AgcDriver::GuestMemory::MarkWritten(reinterpret_cast<std::uintptr_t>(&Labels[copy * Chunks + chunk]), sizeof(Labels[0]));
    }
    if (released) {
        for (const auto control : controls) commands[control] = 0x80000000u;
    }
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
    if (!released) {
        for (const auto control : controls) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            std::atomic_ref<std::uint32_t>(commands[control]).store(0x80000000u, std::memory_order_release);
            AgcDriver::GuestMemory::MarkWritten(reinterpret_cast<std::uintptr_t>(&commands[control]), 4);
        }
    }
    Await(copy);
}

std::vector<std::byte> Slice(std::size_t copy, std::size_t chunk) {
    const auto* begin = ColorMemory.data() + (copy * Chunks + chunk) * SliceBytes;
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(begin), SliceBytes);
    return std::vector<std::byte>(begin, begin + SliceBytes);
}

std::uint64_t WorkerCpuNs() {
#if defined(__linux__)
    std::uint64_t total = 0;
    for (const auto& task : std::filesystem::directory_iterator("/proc/self/task")) {
        std::ifstream comm(task.path() / "comm");
        std::string name;
        std::getline(comm, name);
        if (name.find("queue") == std::string::npos) continue;
        std::ifstream schedstat(task.path() / "schedstat");
        std::uint64_t ns = 0;
        schedstat >> ns;
        total += ns;
    }
    return total;
#else
    return 0;
#endif
}

void Compare(const char* what) {
    for (std::size_t chunk = 0; chunk < Chunks; ++chunk) {
        const auto direct = Slice(0, chunk);
        const auto listed = Slice(1, chunk);
        Require(std::any_of(direct.begin(), direct.end(), [](std::byte value) { return value != std::byte{0}; }), std::string(what) + ": chunk " + std::to_string(chunk) + " drew nothing");
        Require(direct == listed, std::string(what) + ": chunk " + std::to_string(chunk) + " drawn with indirect register lists differs from the same draws with direct register writes");
    }
}

void RequireVerifyCounters() {
    using namespace AgcDriver::DriverDetail;
    struct Expectation {
        VerifyCount which;
        const char* disabledBy;
        std::optional<VerifyCount> skippedWhenDisabled;
    };
    const std::array<Expectation, 5> expectations{{
        {VerifyCount::PacketOverlapChecked, "APS5_NO_PACKET_OVERLAP_MEMO", VerifyCount::PacketOverlapSkipped},
        {VerifyCount::HeldWritesChecked, "APS5_NO_RECYCLED_HELD_WRITES", VerifyCount::HeldWritesSkipped},
        {VerifyCount::PendingWriteIndexChecked, "APS5_NO_PENDING_WRITE_INDEX", std::nullopt},
        {VerifyCount::RegisterKeyMemoChecked, "APS5_NO_REGISTRY_LOOKUP_MEMO", std::nullopt},
        {VerifyCount::RegisterKeyHashChecked, "APS5_NO_INCREMENTAL_REGISTER_KEY", VerifyCount::RegisterKeySkipped},
    }};
    const auto totals = VerifyTotals();
    for (std::size_t index = 0; index < totals.size(); ++index) {
        const auto which = static_cast<VerifyCount>(index);
        const bool set = std::getenv(VerifySwitchName(which)) != nullptr;
        Require(set == VerifyRequested(which), std::string(VerifyFieldName(which)) + " is requested differently from its switch " + VerifySwitchName(which));
        if (!set) Require(totals[index] == 0, std::string(VerifyFieldName(which)) + " moved to " + std::to_string(totals[index]) + " with " + VerifySwitchName(which) + " unset");
        std::printf("rewind draw replay: %s %llu (%s)\n", VerifyFieldName(which), static_cast<unsigned long long>(totals[index]), set ? "switch set" : "switch unset");
    }
    const char* pipeline = std::getenv("APS5_DRAW_PIPELINE");
    if (pipeline == nullptr || std::strcmp(pipeline, "1") != 0) return;
    for (const auto& expectation : expectations) {
        if (!VerifyRequested(expectation.which)) continue;
        const auto checked = totals[static_cast<std::size_t>(expectation.which)];
        if (std::getenv(expectation.disabledBy) == nullptr) {
            Require(checked != 0, std::string(VerifyFieldName(expectation.which)) + " stayed at 0 with " + VerifySwitchName(expectation.which) + " set");
        } else {
            Require(checked == 0, std::string(VerifyFieldName(expectation.which)) + " moved with " + expectation.disabledBy + " set");
            if (expectation.skippedWhenDisabled) Require(totals[static_cast<std::size_t>(*expectation.skippedWhenDisabled)] != 0, std::string(VerifyFieldName(*expectation.skippedWhenDisabled)) + " stayed at 0 with " + expectation.disabledBy + " set");
        }
    }
}

}

int main() {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        Register(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
        Register(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
        for (int round = 0; round < 3; ++round) {
            const bool released = round != 1;
            ColorMemory.fill(std::byte{0});
            AgcDriver::GuestMemory::MarkWritten(reinterpret_cast<std::uintptr_t>(ColorMemory.data()), ColorMemory.size());
            Run(0, false, released);
            Run(1, true, released);
            Compare(released ? "released chain" : "chain released while it runs");
        }
        std::printf("rewind draw replay: %zu chunks of %zu to %zu draws, three rounds, indirect register lists match direct writes\n", Chunks, *std::min_element(ChunkDraws.begin(), ChunkDraws.end()), *std::max_element(ChunkDraws.begin(), ChunkDraws.end()));
        constexpr int Measured = 12;
        std::size_t draws = 0;
        for (const auto count : ChunkDraws) draws += count;
        std::uint64_t best = ~std::uint64_t{0};
        for (int round = 0; round < Measured; ++round) {
            const auto before = WorkerCpuNs();
            Run(1, true, true);
            best = std::min(best, WorkerCpuNs() - before);
        }
        if (best != 0) std::printf("rewind draw replay: queue worker CPU %.0f ns a draw (best of %d released chains of %zu draws with indirect register lists)\n", static_cast<double>(best) / static_cast<double>(draws), Measured, draws);
        RequireVerifyCounters();
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
