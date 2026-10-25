#include "GraphicsTests.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MetadataTrust.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <cstdint>
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

}

void RunPendingMetadataTrustTests() {
    testPendingHtile();
    testPendingKeys();
}
