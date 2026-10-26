#include "GraphicsTests.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MetadataTrust.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <utility>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

constexpr std::uint32_t Expanded = 0xfffc000fu;
constexpr std::uint64_t Writer = 0x40df900;
constexpr std::uint64_t Other = 0x40df600;

void testPendingHtile() {
    std::vector<std::uint32_t> words(0x2000 / 4u, Expanded);
    const auto address = reinterpret_cast<std::uint64_t>(words.data());
    const auto bytes = words.size() * sizeof(std::uint32_t);
    const auto end = address + bytes;
    const MetadataTrust::Key key{Writer, address, bytes};
    std::uint64_t note = 0;
    const auto clearedThenWritten = [&](std::uint64_t by, bool open) {
        const auto fill = ++note;
        const auto store = ++note;
        return std::vector<Recorder::PendingWrite>{{address, end, ++note, -1, -1, by, open}, {address, end, store, -1, Expanded}, {address, end, fill, 0, 0}};
    };
    const auto resolve = [&](std::uint64_t by, MetadataTrust* trust) { return ResolvePendingHtile(clearedThenWritten(by, false), address, bytes, trust); };

    Require(!resolve(Writer, nullptr).word.has_value() && resolve(Writer, nullptr).checks.empty(), "without the trust an unknown write over an HTILE must be read, unchecked");

    MetadataTrust trust(4);
    for (std::uint32_t check = 0; check < MetadataTrust::Confirmations; ++check) {
        const auto resolved = resolve(Writer, &trust);
        Require(!resolved.word.has_value() && resolved.checks.size() == 1 && resolved.checks[0] == key && resolved.predicted == Expanded, "an untrusted writer's unknown write over an HTILE must be read and checked against the store it follows");
        CheckPendingHtile(trust, resolved, words);
    }
    Require(trust.Trusted(key), "three reads that found the HTILE as its store left it must trust the writer");
    auto resolved = resolve(Writer, &trust);
    Require(resolved.word == Expanded && resolved.checks.empty(), "a trusted writer's unknown write over an HTILE must resolve to the store before it without a read");

    resolved = resolve(Other, &trust);
    Require(!resolved.word.has_value() && resolved.checks.size() == 1 && resolved.checks[0].writer == Other, "another writer's unknown write over the same HTILE must still be read");
    resolved = resolve(0, &trust);
    Require(!resolved.word.has_value() && resolved.checks.empty(), "an unknown write without a writer must be read and is never checked");
    auto behind = clearedThenWritten(Writer, false);
    behind.push_back(Recorder::PendingWrite{address, address + 64, ++note, -1, -1, Other, false});
    Require(!ResolvePendingHtile(behind, address, bytes, &trust).word.has_value(), "a trusted writer's write behind an untrusted one must still be read");

    Require(resolve(Writer, &trust).word == Expanded && resolve(Writer, &trust).word == Expanded, "the uses before the audit are skipped");
    resolved = resolve(Writer, &trust);
    Require(!resolved.word.has_value() && resolved.checks.size() == 1 && resolved.checks[0] == key, "every fourth use must read and check the HTILE");
    CheckPendingHtile(trust, resolved, words);
    Require(trust.Trusted(key) && resolve(Writer, &trust).word == Expanded, "a matching audit keeps the writer trusted");

    const auto half = address + bytes / 2u;
    const std::vector<Recorder::PendingWrite> submitted{{address, end, 100, -1, -1, Other, false}, {address, half, 99, -1, Expanded}};
    Require(ResolvePendingHtile(submitted, address, bytes, &trust).checks.empty(), "a submitted write over HTILE words no known write predicts must not be checked against memory");
    const std::vector<Recorder::PendingWrite> unsubmitted{{address, end, 102, -1, -1, Other, true}, {address, half, 101, -1, Expanded}};
    Require(ResolvePendingHtile(unsubmitted, address, bytes, &trust).checks.size() == 1, "an unsubmitted write leaves the memory it covers predictable");

    for (std::uint32_t use = 2; use < trust.AuditEvery(); ++use) Require(resolve(Writer, &trust).word == Expanded, "the uses up to the next audit are skipped");
    resolved = resolve(Writer, &trust);
    Require(!resolved.word.has_value() && !resolved.checks.empty(), "the next audit must read");
    words[7] = 0x1234u;
    CheckPendingHtile(trust, resolved, words);
    words[7] = Expanded;
    Require(trust.Revoked(key) && !trust.Trusted(key), "an audit that finds the HTILE changed must revoke the writer");
    resolved = resolve(Writer, &trust);
    Require(!resolved.word.has_value() && resolved.checks.empty(), "a revoked writer's unknown write must be read and is never checked again");
}

void testPendingKeys() {
    std::vector<std::uint32_t> metadata(0x1000 / 4u, Expanded);
    const auto address = reinterpret_cast<std::uint64_t>(metadata.data());
    const auto count = metadata.size() * sizeof(std::uint32_t);
    const auto end = address + count;
    const MetadataTrust::Key key{Writer, address, count};
    std::uint64_t note = 0;
    const auto written = [&](std::uint64_t by, bool open) { return std::vector<Recorder::PendingWrite>{{address, end, ++note, -1, -1, by, open}}; };

    MetadataTrust trust(4);
    Require(!TrustedPendingKeys(written(Writer, true), address, count, trust).has_value(), "an untrusted writer's unknown write over DCC keys must be waited for");
    Require(!PredictPendingKeys(written(Writer, false), address, count, trust).has_value(), "keys under a submitted write are no prediction");
    for (std::uint32_t check = 0; check < MetadataTrust::Confirmations; ++check) {
        const auto predicted = PredictPendingKeys(written(Writer, true), address, count, trust);
        Require(predicted.has_value() && predicted->value < 0 && predicted->writers == std::vector<std::uint64_t>{Writer}, "keys under an unsubmitted write predict the mixed keys they hold");
        CheckPendingKeys(trust, *predicted);
    }
    Require(trust.Trusted(key), "three waits that found the keys as predicted must trust the writer");
    Require(TrustedPendingKeys(written(Writer, false), address, count, trust) == DccKeys::Mixed, "a trusted writer's unknown write over DCC keys must leave the keys it found");
    Require(!TrustedPendingKeys(written(Other, false), address, count, trust).has_value(), "another writer's unknown write over the keys must still be waited for");
    Require(TrustedPendingKeys(written(Writer, false), address, count, trust) == DccKeys::Mixed && TrustedPendingKeys(written(Writer, false), address, count, trust) == DccKeys::Mixed, "the second and third uses are skipped");
    Require(!TrustedPendingKeys(written(Writer, false), address, count, trust).has_value(), "every fourth use of the keys must wait");

    std::vector<std::uint8_t> uniform(256, 0xff);
    const auto keys = reinterpret_cast<std::uint64_t>(uniform.data());
    const MetadataTrust::Key uniformKey{Writer, keys, uniform.size()};
    const std::vector<Recorder::PendingWrite> over{{keys, keys + uniform.size(), 1, -1, -1, Writer, true}};
    const auto predicted = PredictPendingKeys(over, keys, uniform.size(), trust);
    Require(predicted.has_value() && predicted->value == 0xff, "uniform keys under an unsubmitted write predict their key");
    CheckPendingKeys(trust, *predicted);
    Require(!trust.Revoked(uniformKey), "keys found as predicted confirm the writer");
    uniform[3] = 0x00;
    CheckPendingKeys(trust, *predicted);
    Require(trust.Revoked(uniformKey) && !trust.Revoked(key), "keys found changed revoke the writer for that range only");
}

void trust(MetadataTrust& registry, const MetadataTrust::Key& key) {
    for (std::uint32_t check = 0; check < MetadataTrust::Confirmations; ++check) registry.Check(key, true);
    Require(registry.Trusted(key), "three confirmations must trust the writer");
}

void testKnownHtile() {
    std::vector<std::uint32_t> words(0x2000 / 4u, 0u);
    const auto address = reinterpret_cast<std::uint64_t>(words.data());
    const auto bytes = words.size() * sizeof(std::uint32_t);
    const auto end = address + bytes;
    const MetadataTrust::Key key{Writer, address, bytes};
    std::uint64_t note = 0;
    const auto alone = [&](std::uint64_t by, bool open) { return std::vector<Recorder::PendingWrite>{{address, end, ++note, -1, -1, by, open}}; };

    MetadataTrust registry(4);
    trust(registry, key);
    Require(!ResolvePendingHtile(alone(Writer, false), address, bytes, &registry).word.has_value(), "a trusted writer's write alone, with no word known before it, must be read");
    auto resolved = ResolvePendingHtile(alone(Writer, false), address, bytes, &registry, Expanded);
    Require(resolved.word == Expanded && resolved.checks.empty(), "a trusted writer's write alone must resolve to the word known before it, whatever the memory holds");
    resolved = ResolvePendingHtile(alone(Other, false), address, bytes, &registry, Expanded);
    Require(!resolved.word.has_value(), "another writer's write must still be read");
    auto mixed = alone(Writer, false);
    mixed.push_back({address, address + 64, ++note, -1, -1, 0, false});
    Require(!ResolvePendingHtile(mixed, address, bytes, &registry, Expanded).word.has_value(), "an unknown write without a writer beside the trusted one must still be read");
    auto filled = alone(Writer, false);
    filled.insert(filled.begin(), Recorder::PendingWrite{address, end, note - 1, 0, 0, 0, false});
    Require(ResolvePendingHtile(filled, address, bytes, &registry, Expanded).word == 0u, "a known write after the remembered word must win over it");
    Require(ResolvePendingHtile(alone(Writer, true), address, bytes, &registry, Expanded).word == Expanded, "the third use before the audit is skipped");
    resolved = ResolvePendingHtile(alone(Writer, false), address, bytes, &registry, Expanded);
    Require(!resolved.word.has_value() && resolved.checks.size() == 1 && resolved.checks[0] == key && resolved.predicted == Expanded, "the audit must read the HTILE and check it against the remembered word");
    std::vector<std::uint32_t> read(words.size(), Expanded);
    CheckPendingHtile(registry, resolved, read);
    Require(registry.Trusted(key), "an audit that finds the remembered word keeps the writer trusted");
    for (std::uint32_t use = 1; use < registry.AuditEvery(); ++use) Require(ResolvePendingHtile(alone(Writer, false), address, bytes, &registry, Expanded).word == Expanded, "the uses up to the next audit are skipped");
    resolved = ResolvePendingHtile(alone(Writer, false), address, bytes, &registry, Expanded);
    Require(!resolved.checks.empty() && resolved.predicted == Expanded, "the next audit must check the remembered word");
    read[3] = 0x1234u;
    CheckPendingHtile(registry, resolved, read);
    Require(registry.Revoked(key) && !ResolvePendingHtile(alone(Writer, false), address, bytes, &registry, Expanded).word.has_value(), "an audit that finds the HTILE changed from the remembered word revokes the writer");
}

void* allocateWatched(std::size_t bytes) {
    if (!AgcDriver::GuestMemory::WriteWatched()) return nullptr;
    constexpr std::size_t alignment = 65536;
#ifdef _WIN32
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, alignment);
    if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    void* raw = mmap(nullptr, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return nullptr;
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = (begin + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned + bytes != begin + bytes + alignment) munmap(reinterpret_cast<void*>(aligned + bytes), begin + alignment - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    return block;
}

void releaseWatched(void* block, std::size_t bytes) {
#ifdef _WIN32
    GuestArena::GuestArenaReset_nid_postfix(block, bytes);
    GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
#else
    munmap(block, bytes);
    GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, bytes);
#endif
}

void testKnownHtileWords() {
    constexpr std::size_t bytes = 0x20000;
    auto* words = static_cast<std::uint32_t*>(allocateWatched(bytes));
    if (words == nullptr) {
        std::puts("guest memory has no write watch: the known HTILE word cases are skipped");
        return;
    }
    GuestAllocations::Mutation().Add(words, bytes, true, true, false);
    struct Cleanup {
        std::uint32_t* words;
        ~Cleanup() {
            NoteKnownHtileWord(reinterpret_cast<std::uint64_t>(words), bytes, std::nullopt, 0);
            GuestAllocations::Mutation().Remove(words);
            releaseWatched(words, bytes);
        }
    } cleanup{words};
    std::fill(words, words + bytes / 4u, Expanded);
    const auto address = reinterpret_cast<std::uint64_t>(words);
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> writes{{{address, address + bytes}}};
    const auto remember = [&] {
        NoteKnownHtileWord(address, bytes, Expanded, AgcDriver::GuestMemory::CollectWritesUncached(address, bytes));
        Require(KnownHtileWord(address, bytes) == Expanded, "the word just noted for an HTILE must be known");
    };
    const auto known = [&] {
        AgcDriver::GuestMemory::CollectWritesUncached(address, bytes);
        return KnownHtileWord(address, bytes);
    };
    const auto writeBy = [&](const MetadataTrust& registry, std::uint64_t writer) {
        const auto passes = TrustedHtilePasses(registry, writer, writes);
        AgcDriver::GuestMemory::MarkPossiblyWritten(address, bytes);
        PassTrustedHtiles(passes);
    };
    MetadataTrust registry(4);
    trust(registry, {Writer, address, bytes});

    remember();
    writeBy(registry, Writer);
    Require(known() == Expanded, "a trusted writer's write over the HTILE must leave its word known");
    writeBy(registry, Writer);
    Require(known() == Expanded, "a second trusted write must leave its word known");
    writeBy(registry, Other);
    Require(!known().has_value(), "another writer's write since must force the read");

    remember();
    words[5] = 0u;
    Require(!known().has_value(), "a CPU store since must force the read");
    words[5] = Expanded;

    remember();
    AgcDriver::GuestMemory::MarkWritten(address + 64, 64);
    writeBy(registry, Writer);
    Require(!known().has_value(), "a trusted writer's write after another store must not hide that store");

    remember();
    Require(KnownHtileWord(address, bytes / 2u) == std::nullopt, "a word known for one HTILE extent says nothing about another");
}

}

void RunPendingMetadataTrustTests() {
    testPendingHtile();
    testPendingKeys();
    testKnownHtile();
    testKnownHtileWords();
}
