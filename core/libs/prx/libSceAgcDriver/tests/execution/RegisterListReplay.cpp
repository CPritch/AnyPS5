#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRegisterKey.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace AgcDriver::DriverDetail {

struct DriverTestAccess {
    static void ReadRegisterLists(Submission& submission) { Driver::readRegisterLists(submission); }
    static std::uint64_t RegisterKey(const QueueState& queue, const ShaderRegistry& registry, std::uint64_t* shape) { return Driver::drawRegisterKey(queue, registry, 7, shape); }
    static std::uint64_t IdentityKey(const QueueState& queue, const ShaderRegistry& registry) { return Driver::drawIdentityKey(queue, registry); }
};

}

namespace {

using namespace AgcDriver;
using namespace AgcDriver::DriverDetail;

void Require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("register list replay: " + what);
}

class InstructionCounter {
public:
    InstructionCounter() {
#if defined(__linux__)
        perf_event_attr attr{};
        attr.type = PERF_TYPE_HARDWARE;
        attr.size = sizeof(attr);
        attr.config = PERF_COUNT_HW_INSTRUCTIONS;
        attr.disabled = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        descriptor = static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0));
#endif
    }
    ~InstructionCounter() {
#if defined(__linux__)
        if (descriptor >= 0) close(descriptor);
#endif
    }
    bool Available() const { return descriptor >= 0; }
    void Start() {
#if defined(__linux__)
        if (descriptor < 0) return;
        ioctl(descriptor, PERF_EVENT_IOC_RESET, 0);
        ioctl(descriptor, PERF_EVENT_IOC_ENABLE, 0);
#endif
    }
    std::uint64_t Stop() {
#if defined(__linux__)
        if (descriptor < 0) return 0;
        ioctl(descriptor, PERF_EVENT_IOC_DISABLE, 0);
        std::uint64_t count = 0;
        if (read(descriptor, &count, sizeof(count)) != sizeof(count)) return 0;
        return count;
#else
        return 0;
#endif
    }

private:
    int descriptor = -1;
};

constexpr std::uint32_t Type3(std::uint32_t opcode, std::size_t words) {
    return (3u << 30u) | (static_cast<std::uint32_t>(words - 2) << 16u) | (opcode << 8u);
}

constexpr std::size_t Materials = 96;
constexpr std::size_t DrawsPerFrame = 1500;
constexpr std::size_t ShaderPairs = 3;

struct Material {
    std::vector<std::uint32_t> shaderPairs;
    std::vector<std::uint32_t> contextValues;
    std::uint32_t contextFirst = 0;
};

struct Stream {
    std::vector<Material> materials;
    std::vector<std::uint32_t> commands;
    std::vector<std::size_t> draws;
    ShaderRegistry registry;
};

Stream BuildStream(std::uint32_t seed) {
    std::mt19937 random(seed);
    Stream stream;
    stream.materials.resize(Materials);
    for (std::size_t i = 0; i < 4096; ++i) stream.registry.emplace(0x100000000ull + i * 0x4000ull, {});
    for (std::size_t m = 0; m < Materials; ++m) {
        auto& material = stream.materials[m];
        const auto vertex = 0x100000000ull + (random() % 4096) * 0x4000ull + 0x100;
        const auto pixel = 0x100000000ull + (random() % 4096) * 0x4000ull + 0x200;
        const std::array<std::pair<std::uint32_t, std::uint32_t>, 10> shader{{
            {0x088u, static_cast<std::uint32_t>(vertex >> 8u)}, {0x089u, static_cast<std::uint32_t>(vertex >> 40u)},
            {0x08au, 0x002c0041u}, {0x08bu, 0x00000010u | (8u << 1u)},
            {0x008u, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009u, static_cast<std::uint32_t>(pixel >> 40u)},
            {0x00au, 0x002c0003u}, {0x00bu, 0x00000010u | (6u << 1u)},
            {0x082u, 0x1u}, {0x102u, 0x0u},
        }};
        for (const auto& [offset, value] : shader) {
            material.shaderPairs.push_back(offset);
            material.shaderPairs.push_back(value);
        }
        for (std::uint32_t word = 0; word < 8; ++word) {
            material.shaderPairs.push_back(0x08cu + word);
            material.shaderPairs.push_back(static_cast<std::uint32_t>(random()));
        }
        material.contextFirst = 0x1e0u;
        for (std::uint32_t word = 0; word < 16; ++word) material.contextValues.push_back(static_cast<std::uint32_t>(random()));
    }
    const auto address = [](const std::vector<std::uint32_t>& words) { return reinterpret_cast<std::uint64_t>(words.data()); };
    for (std::size_t draw = 0; draw < DrawsPerFrame; ++draw) {
        const auto& material = stream.materials[(draw * 7 + draw / 13) % Materials];
        const auto pairs = address(material.shaderPairs);
        stream.commands.insert(stream.commands.end(), {Type3(0x63, 5), static_cast<std::uint32_t>(pairs), static_cast<std::uint32_t>(pairs >> 32u), 0x80000000u, static_cast<std::uint32_t>(material.shaderPairs.size() / 2)});
        const auto values = address(material.contextValues);
        stream.commands.insert(stream.commands.end(), {Type3(0x9f, 5), static_cast<std::uint32_t>(values), static_cast<std::uint32_t>(values >> 32u), material.contextFirst, static_cast<std::uint32_t>(material.contextValues.size())});
        stream.commands.insert(stream.commands.end(), {Type3(0x76, 6), 0x00cu, static_cast<std::uint32_t>(random()), static_cast<std::uint32_t>(random()), static_cast<std::uint32_t>(random()), static_cast<std::uint32_t>(random())});
        stream.draws.push_back(stream.commands.size());
        stream.commands.insert(stream.commands.end(), {Type3(0x2d, 3), 3u, 2u});
    }
    return stream;
}

struct Totals {
    std::uint64_t key = 0;
    std::uint64_t ns = 0;
    std::uint64_t instructions = 0;
};

Totals ReplayFrame(const Stream& stream, QueueState& queue, InstructionCounter& counter) {
    Totals totals;
    Submission submission{};
    submission.commands = stream.commands;
    const auto begin = std::chrono::steady_clock::now();
    counter.Start();
    DriverTestAccess::ReadRegisterLists(submission);
    std::size_t at = 0;
    std::size_t nextDraw = 0;
    for (std::size_t cursor = 0; cursor < submission.commands.size();) {
        const auto header = submission.commands[cursor];
        const auto count = Pm4::PacketWords(header);
        const auto packet = std::span<const std::uint32_t>(submission.commands).subspan(cursor, count);
        const auto opcode = (header >> 8u) & 0xffu;
        if (Pm4::IndirectRegisterOpcode(opcode)) {
            ListedRegisters listed;
            if (FlatRegisterLists()) {
                const auto& index = submission.registerListIndex;
                while (at < index.size() && index[at].cursor < cursor) ++at;
                listed = submission.RegisterListOf(index.at(at));
            } else {
                listed = submission.FindRegisterList(cursor);
            }
            Require(listed.found && listed.valid, "register list missing or invalid");
            if (FlatRegisterLists()) Pm4::ExecuteValidatedIndirectRegisters(packet, listed.pairs, queue);
            else Pm4::ExecuteIndirectRegisters(packet, listed.pairs, queue);
        } else if (nextDraw < stream.draws.size() && cursor == stream.draws[nextDraw]) {
            ++nextDraw;
            std::uint64_t shape = 0;
            const auto identity = DriverTestAccess::IdentityKey(queue, stream.registry);
            const auto key = DriverTestAccess::RegisterKey(queue, stream.registry, &shape);
            totals.key = (totals.key ^ key ^ (identity << 1u) ^ (shape << 2u)) * 0x100000001b3ull;
        } else {
            Pm4::Execute(packet, queue);
        }
        cursor += count;
    }
    totals.instructions = counter.Stop();
    totals.ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
    return totals;
}

void ReferenceFrame(const Stream& stream, QueueState& queue) {
    for (std::size_t cursor = 0; cursor < stream.commands.size();) {
        const auto header = stream.commands[cursor];
        const auto count = Pm4::PacketWords(header);
        const auto opcode = (header >> 8u) & 0xffu;
        if (opcode != 0x2d) Pm4::Execute(std::span<const std::uint32_t>(stream.commands).subspan(cursor, count), queue);
        cursor += count;
    }
}

void MeasureKeyWaits(InstructionCounter& counter) {
    auto& finisher = DrawFinisher::Get();
    constexpr int Rounds = 200;
    std::uint64_t instructions = 0, waited = 0, ns = 0;
    for (int round = 0; round < Rounds; ++round) {
        DrawFinisher::Pending held;
        held.unknown = false;
        held.key = 91;
        held.inserts = true;
        finisher.EnqueuePrepared([] {
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(120);
            while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
        }, std::move(held));
        const auto begin = std::chrono::steady_clock::now();
        counter.Start();
        waited += finisher.WaitPendingKey(91, 0) ? 1u : 0u;
        instructions += counter.Stop();
        ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
        finisher.Drain();
    }
    std::printf("draw key waits: %llu of %d waited, %.1f us and %.0f instructions a wait\n", static_cast<unsigned long long>(waited), Rounds, static_cast<double>(ns) / Rounds / 1000.0, static_cast<double>(instructions) / Rounds);
}

}

int main() {
    try {
        const auto stream = BuildStream(5);
        constexpr int frames = 40;
        InstructionCounter counter;
        QueueState queue;
        QueueState reference;
        std::uint64_t key = 0, bestNs = ~std::uint64_t{0}, instructions = 0, measured = 0;
        for (int frame = 0; frame < frames + 4; ++frame) {
            const auto totals = ReplayFrame(stream, queue, counter);
            ReferenceFrame(stream, reference);
            Require(queue.shader == reference.shader && queue.context == reference.context && queue.userConfig == reference.userConfig, "replayed register state differs from the reference execution");
            key = frame == 0 ? totals.key : key;
            Require(totals.key == key, "draw keys differ between identical frames");
            if (frame < 4) continue;
            bestNs = std::min(bestNs, totals.ns);
            instructions += totals.instructions;
            ++measured;
        }
        const double draws = static_cast<double>(stream.draws.size());
        std::printf("register list replay: %zu draws a frame, %llu frames; best %.1f ns a draw", stream.draws.size(), static_cast<unsigned long long>(measured), static_cast<double>(bestNs) / draws);
        if (counter.Available()) std::printf(", %.1f instructions a draw", static_cast<double>(instructions) / static_cast<double>(measured) / draws);
        std::printf(" (key 0x%016llx)\n", static_cast<unsigned long long>(key));
        MeasureKeyWaits(counter);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
