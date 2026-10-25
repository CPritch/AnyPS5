#include "prx/libSceAgcDriver/Graphics/include/MetadataTrust.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

using Use = MetadataTrust::Use;

constexpr MetadataTrust::Key Htile{0x40df900, 0x247da0000, 0x48000};

void confirm(MetadataTrust& trust, const MetadataTrust::Key& key, std::uint32_t times) {
    for (std::uint32_t i = 0; i < times; ++i) trust.Check(key, true);
}

void testTrustAfterConfirmations() {
    MetadataTrust trust(64);
    Expect(!trust.Trusted(Htile) && !trust.Revoked(Htile), "an unseen key is neither trusted nor revoked");
    Expect(trust.Consult(Htile, 1) == Use::Untrusted, "an unseen key is not used");
    confirm(trust, Htile, MetadataTrust::Confirmations - 1);
    Expect(!trust.Trusted(Htile), "two confirmations do not trust a key");
    Expect(trust.Consult(Htile, 2) == Use::Untrusted, "a key short of its confirmations is not used");
    trust.Check(Htile, true);
    Expect(trust.Trusted(Htile), "the third consecutive confirmation trusts a key");
    Expect(trust.Consult(Htile, 3) == Use::Skip, "a trusted key's write is skipped");
    const MetadataTrust::Key otherRange{Htile.writer, Htile.begin, 0xf000};
    const MetadataTrust::Key otherWriter{0x40df600, Htile.begin, Htile.bytes};
    Expect(!trust.Trusted(otherRange) && !trust.Trusted(otherWriter), "trust is kept per writer and metadata range");
    const auto counts = trust.Totals();
    Expect(counts.confirmations == 3 && counts.trusted == 1 && counts.skips == 1 && counts.audits == 0 && counts.revocations == 0, "the counters follow the confirmations and the skip");
}

void testRevocation() {
    MetadataTrust trust(64);
    trust.Check(Htile, true);
    Expect(trust.Check(Htile, false), "a mismatch revokes a key");
    Expect(trust.Revoked(Htile) && !trust.Trusted(Htile), "a revoked key is not trusted");
    confirm(trust, Htile, 2 * MetadataTrust::Confirmations);
    Expect(trust.Revoked(Htile) && !trust.Trusted(Htile), "a revoked key is never trusted again");
    Expect(!trust.Check(Htile, false), "a revoked key is revoked once");
    Expect(trust.Consult(Htile, 7) == Use::Untrusted, "a revoked key is not used");

    MetadataTrust trusted(64);
    confirm(trusted, Htile, MetadataTrust::Confirmations);
    Expect(trusted.Consult(Htile, 1) == Use::Skip, "a trusted key is used");
    Expect(trusted.Check(Htile, false), "a trusted key's mismatch revokes it");
    Expect(!trusted.Trusted(Htile) && trusted.Revoked(Htile) && trusted.Consult(Htile, 2) == Use::Untrusted, "a revoked key that was trusted is no longer used");
    const auto counts = trusted.Totals();
    Expect(counts.revocations == 1 && counts.trusted == 0, "a revocation leaves no trusted key behind");

    MetadataTrust fresh(64);
    Expect(fresh.Check(Htile, false), "a mismatch on the first check revokes the key");
    confirm(fresh, Htile, MetadataTrust::Confirmations);
    Expect(!fresh.Trusted(Htile), "a key revoked before it was trusted stays untrusted");
}

void testAuditCadence() {
    constexpr std::uint32_t every = 4;
    MetadataTrust trust(every);
    confirm(trust, Htile, MetadataTrust::Confirmations);
    std::uint64_t note = 100;
    for (std::uint32_t use = 1; use < every; ++use) Expect(trust.Consult(Htile, ++note) == Use::Skip, "the uses before the audit are skipped");
    Expect(trust.Consult(Htile, ++note) == Use::Audit, "every fourth use is an audit");
    Expect(trust.Consult(Htile, note) == Use::Audit, "the audited write keeps its answer");
    Expect(trust.Consult(Htile, ++note) == Use::Audit, "an audit stays due until a check is made");
    trust.Check(Htile, true);
    Expect(trust.Trusted(Htile), "a matching audit keeps the key trusted");
    for (std::uint32_t use = 1; use < every; ++use) Expect(trust.Consult(Htile, ++note) == Use::Skip, "a check starts the next audit period");
    Expect(trust.Consult(Htile, ++note) == Use::Audit, "the next audit comes every fourth use again");
    Expect(trust.Check(Htile, false) && trust.Revoked(Htile), "a mismatching audit revokes the key");
    const auto counts = trust.Totals();
    Expect(counts.skips == 2 * (every - 1) && counts.audits == 3, "skips and audits are counted once per write");
}

void testNoteAnswers() {
    MetadataTrust trust(2);
    confirm(trust, Htile, MetadataTrust::Confirmations);
    Expect(trust.Consult(Htile, 5) == Use::Skip, "the first use is skipped");
    Expect(trust.Consult(Htile, 5) == Use::Skip && trust.Consult(Htile, 5) == Use::Skip, "a write asked about again keeps its answer");
    Expect(trust.Totals().skips == 1, "a write asked about again is one use");
    Expect(trust.Consult(Htile, 6) == Use::Audit, "the next write is the second use");
}

void testWriters() {
    MetadataTrust trust(64);
    const MetadataTrust::Key revoked{0x2000, Htile.begin, Htile.bytes};
    confirm(trust, Htile, MetadataTrust::Confirmations);
    trust.Check(revoked, false);
    const std::array<std::uint64_t, 6> writers{0x3000, Htile.writer, 0, revoked.writer, Htile.writer, 0x3000};
    Expect(trust.Writers(writers, Htile.begin, Htile.bytes, false) == std::vector<std::uint64_t>{Htile.writer}, "only the trusted writers are skipped for a use");
    Expect(trust.Writers(writers, Htile.begin, Htile.bytes, true) == std::vector<std::uint64_t>{0x3000, Htile.writer}, "every writer but the revoked ones and the unknown one is learned");
    Expect(trust.Writers(writers, Htile.begin, 0xf000, false).empty(), "a writer trusted for one range is not trusted for another");
    Expect(trust.Check({0, Htile.begin, Htile.bytes}, false) == false && trust.Totals().revocations == 1, "an unknown writer is never checked");
}

void testDefaultAudit() {
    MetadataTrust trust(0);
    Expect(trust.AuditEvery() == MetadataTrust::DefaultAudit, "an audit period of 0 takes the default");
    confirm(trust, Htile, MetadataTrust::Confirmations);
    std::uint64_t note = 0;
    for (std::uint32_t use = 1; use < MetadataTrust::DefaultAudit; ++use) Expect(trust.Consult(Htile, ++note) == Use::Skip, "the default period skips 63 uses");
    Expect(trust.Consult(Htile, ++note) == Use::Audit, "the default period audits the 64th use");
}

}

int main() {
    testTrustAfterConfirmations();
    testRevocation();
    testAuditCadence();
    testNoteAnswers();
    testWriters();
    testDefaultAudit();
    if (failures != 0) {
        std::fprintf(stderr, "%d metadata trust checks failed\n", failures);
        return 1;
    }
    std::printf("metadata trust tests passed\n");
    return 0;
}
