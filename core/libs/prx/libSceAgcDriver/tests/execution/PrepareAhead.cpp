#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::DrawAheadEvent;
using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::DrawPipeline;

constexpr std::byte Kept{0x40};
constexpr std::uint32_t BaseVertexLocation = 0x8c;
constexpr std::size_t TargetBytes = 1024;
using Target = std::array<std::byte, TargetBytes>;

alignas(256) constexpr std::array<std::uint32_t, 20> VertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060208u, 0x7e0802f2u, 0x7e0a02ffu, 0x44800000u, 0xf80000d1u, 0x00000005u, 0xf80008cfu, 0x04030201u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0x7e000200u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(256) Target OpaqueReference{};
alignas(256) Target HalfReference{};
alignas(256) std::array<Target, 4> Queued{};
alignas(256) Target Overdrawn{};
alignas(256) std::array<Target, 2> Faulted{};
alignas(256) std::array<std::uint32_t, 4> Label{};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};

void Registered(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

void AppendState(std::vector<std::uint32_t>& commands) {
    const auto vertex = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(PixelCode.data());
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0x01200000}, {0x200, 0}, {0x203, 0x800},
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
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 1u << 1u},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 1u << 1u}, {0x00c, std::bit_cast<std::uint32_t>(1.0f)},
    };
    Append(commands, 0x79, {0x242, 4});
    Append(commands, 0x79, {0x24a, 0});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
}

void AppendTarget(std::vector<std::uint32_t>& commands, Target& memory) {
    const auto color = reinterpret_cast<std::uintptr_t>(memory.data());
    Append(commands, 0x69, {0x318, static_cast<std::uint32_t>(color >> 8u)});
    Append(commands, 0x69, {0x390, static_cast<std::uint32_t>(color >> 40u)});
    Append(commands, 0x76, {BaseVertexLocation, 0});
}

void AppendColor(std::vector<std::uint32_t>& commands, float value) {
    Append(commands, 0x76, {0x00c, std::bit_cast<std::uint32_t>(value)});
}

void AppendDraw(std::vector<std::uint32_t>& commands) {
    Append(commands, 0x2d, {3, 2});
}

void AppendLabel(std::vector<std::uint32_t>& commands, std::uint32_t* target, std::uint32_t value) {
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    commands.insert(commands.end(), {0xc0064900u, 0x514u, (1u << 29u) | (2u << 24u), static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), value, 0, 0});
}

void Submit(std::vector<std::uint32_t>& commands, const std::string& what) {
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, what + ": the submission failed");
}

void Flush(const Target& memory) {
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(memory.data()), memory.size());
}

std::size_t Covered(const Target& memory) {
    return static_cast<std::size_t>(std::count_if(memory.begin(), memory.end(), [](std::byte value) { return value != Kept; }));
}

void Same(const Target& memory, const Target& reference, const std::string& what) {
    for (std::size_t index = 0; index < reference.size(); ++index) {
        Require(memory[index] == reference[index], what + ": byte " + std::to_string(index) + " is " + std::to_string(static_cast<unsigned>(memory[index])) + ", the reference draw wrote " + std::to_string(static_cast<unsigned>(reference[index])));
    }
}

std::uint64_t Total(DrawAheadEvent event) {
    return AgcDriver::Graphics::DrawAheadTotals()[static_cast<std::size_t>(event)];
}

template <typename Ready>
void WaitUntil(Ready ready, const std::string& what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!ready()) {
        Require(std::chrono::steady_clock::now() < deadline, "prepare-ahead: timed out waiting for " + what);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

struct Blocker {
    struct State {
        std::atomic<bool> release{false};
        std::atomic<bool> done{false};
    };
    std::shared_ptr<State> state = std::make_shared<State>();
    void Enqueue() {
        DrawPipeline::Queue0().Enqueue([state = state] {
            while (!state->release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            { std::lock_guard gpuLock(AgcDriver::GuestMemory::GpuMutex()); }
            state->done = true;
        }, {});
    }
    void Release() { state->release = true; }
    void Join() const { WaitUntil([&] { return state->done.load(); }, "the blocker"); }
};

void Finish() {
    auto idle = std::async(std::launch::async, [] { AgcDriverWaitIdle_nid_postfix(); });
    if (idle.wait_for(std::chrono::seconds(20)) != std::future_status::ready) {
        std::fputs("prepare-ahead: the queue worker and the draw pipeline deadlocked\n", stderr);
        std::fflush(stderr);
        std::_Exit(1);
    }
    idle.get();
}

struct Mode {
    bool on = false;
    bool verify = false;
};

void DrawReference(Target& memory, float color) {
    memory.fill(Kept);
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    AppendTarget(commands, memory);
    AppendColor(commands, color);
    AppendDraw(commands);
    Submit(commands, "the reference draw");
    Finish();
    Flush(memory);
    const auto covered = Covered(memory);
    Require(covered != 0 && covered != memory.size(), "the reference draw covered " + std::to_string(covered) + " of " + std::to_string(memory.size()) + " bytes, expected an edge through the target");
}

void QueuedDrawsUseTheirPreparation(const Mode& mode) {
    const auto offered = Total(DrawAheadEvent::Offered);
    const auto prepared = Total(DrawAheadEvent::Prepared);
    const auto used = Total(DrawAheadEvent::Used);
    const auto dropped = Total(DrawAheadEvent::NotStarted) + Total(DrawAheadEvent::Unfinished);
    const auto verified = Total(DrawAheadEvent::Verified);
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    for (auto& target : Queued) {
        target.fill(Kept);
        AppendTarget(commands, target);
        AppendDraw(commands);
    }
    Blocker blocker;
    blocker.Enqueue();
    Submit(commands, "the queued draws");
    if (mode.on) WaitUntil([&] { return Total(DrawAheadEvent::Prepared) >= prepared + Queued.size(); }, "the queued draws' preparation");
    else WaitUntil([] { return DrawPipeline::Queue0().Outstanding() >= 1 + Queued.size(); }, "the queued draws");
    blocker.Release();
    Finish();
    blocker.Join();
    for (std::size_t index = 0; index < Queued.size(); ++index) {
        Flush(Queued[index]);
        Same(Queued[index], OpaqueReference, "queued draw " + std::to_string(index));
    }
    if (!mode.on) {
        Require(Total(DrawAheadEvent::Offered) == offered, "prepare-ahead is off, yet draws were offered for preparation");
        return;
    }
    Require(Total(DrawAheadEvent::Offered) - offered == Queued.size(), "each queued draw is offered for preparation once");
    Require(Total(DrawAheadEvent::Used) - used == Queued.size(), "the committer used " + std::to_string(Total(DrawAheadEvent::Used) - used) + " of " + std::to_string(Queued.size()) + " preparations finished before it reached them");
    Require(Total(DrawAheadEvent::NotStarted) + Total(DrawAheadEvent::Unfinished) == dropped, "a preparation finished before the commit was dropped as not ready");
    if (mode.verify) Require(Total(DrawAheadEvent::Verified) - verified == Queued.size(), "APS5_VERIFY_PREPARE_AHEAD=1 verifies every used preparation");
}

void LaterDrawsStillWinInQueueOrder(const Mode& mode) {
    const auto prepared = Total(DrawAheadEvent::Prepared);
    Overdrawn.fill(Kept);
    Label = {};
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    AppendTarget(commands, Overdrawn);
    AppendColor(commands, 1.0f);
    AppendDraw(commands);
    AppendLabel(commands, Label.data(), 1);
    AppendColor(commands, 0.5f);
    AppendDraw(commands);
    AppendLabel(commands, Label.data(), 2);
    Blocker blocker;
    blocker.Enqueue();
    Submit(commands, "the overdrawn target");
    if (mode.on) WaitUntil([&] { return Total(DrawAheadEvent::Prepared) >= prepared + 2; }, "the overdraws' preparation");
    blocker.Release();
    Finish();
    blocker.Join();
    Flush(Overdrawn);
    Require(Label[0] == 2, "the labels behind the queued draws ended at " + std::to_string(Label[0]) + ", expected the second");
    Same(Overdrawn, HalfReference, "the target the second queued draw overdrew");
}

void ThrownPreparationsAreRedoneAtTheCommit(const Mode& mode) {
    if (!mode.on) return;
    const auto thrown = Total(DrawAheadEvent::Thrown);
    const auto used = Total(DrawAheadEvent::Used);
    AgcDriver::Graphics::ThrowInDrawAheadForTest(true);
    std::vector<std::uint32_t> commands;
    AppendState(commands);
    for (auto& target : Faulted) {
        target.fill(Kept);
        AppendTarget(commands, target);
        AppendDraw(commands);
    }
    Blocker blocker;
    blocker.Enqueue();
    Submit(commands, "the draws whose preparation throws");
    WaitUntil([&] { return Total(DrawAheadEvent::Thrown) >= thrown + Faulted.size(); }, "the failed preparations");
    AgcDriver::Graphics::ThrowInDrawAheadForTest(false);
    blocker.Release();
    Finish();
    blocker.Join();
    for (std::size_t index = 0; index < Faulted.size(); ++index) {
        Flush(Faulted[index]);
        Same(Faulted[index], OpaqueReference, "the draw " + std::to_string(index) + " whose preparation threw");
    }
    Require(Total(DrawAheadEvent::Used) == used, "a failed preparation was used at the commit");
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_PIPELINED_DRAWS", "1");
    if (std::getenv("APS5_PREPARE_AHEAD") == nullptr) _putenv_s("APS5_PREPARE_AHEAD", "2");
#else
    setenv("APS5_PIPELINED_DRAWS", "1", 1);
    setenv("APS5_PREPARE_AHEAD", "2", 0);
#endif
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        Mode mode;
        mode.on = DrawPipeline::PrepareThreads() != 0;
        const char* verify = std::getenv("APS5_VERIFY_PREPARE_AHEAD");
        mode.verify = mode.on && verify != nullptr && std::strcmp(verify, "1") == 0;
        Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
        Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
        DrawReference(OpaqueReference, 1.0f);
        DrawReference(HalfReference, 0.5f);
        Require(std::memcmp(OpaqueReference.data(), HalfReference.data(), TargetBytes) != 0, "the two reference colors wrote the same bytes");
        QueuedDrawsUseTheirPreparation(mode);
        LaterDrawsStillWinInQueueOrder(mode);
        ThrownPreparationsAreRedoneAtTheCommit(mode);
        Require(AgcDriver::Graphics::DrawAheadTotals()[static_cast<std::size_t>(DrawAheadEvent::Mismatch)] == 0, "a verified preparation differed from the commit's");
        AgcDriverShutdown_nid_postfix();
        std::printf("prepare-ahead tests passed (%s)\n", !mode.on ? "off" : mode.verify ? "on, verified" : "on");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
